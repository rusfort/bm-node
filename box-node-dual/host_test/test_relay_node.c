/*
 * Хост-тест relay_node + store: несколько модельных телефонов обмениваются только через узел.
 * Модель телефона повторяет FileSyncWorker/Queue: BOX -> CBB/CDB, CBB/CDB про свои коробки -> ACB,
 * ACB снимает свои CBB/CDB, unit_count +1 при приёме.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relay_node.h"
#include "store.h"

/* ---------- окружение ---------- */

#define FL_SECTORS 64
static uint8_t fl[FL_SECTORS * ST_SECTOR];
static uint8_t mem[ST_DELETED_RING * 8 + FL_SECTORS * 12 + 4000 * sizeof(st_ent_t)];
static uint32_t clk_ms = 1000;

static int f_read(void *c, uint32_t o, void *b, size_t n) { (void)c; memcpy(b, fl + o, n); return 0; }
static int f_write(void *c, uint32_t o, const void *b, size_t n)
{
    (void)c;
    for (size_t i = 0; i < n; i++) { assert(fl[o + i] == 0xff); fl[o + i] = ((const uint8_t *)b)[i]; }
    return 0;
}
static int f_erase(void *c, uint32_t s) { (void)c; memset(fl + s * ST_SECTOR, 0xff, ST_SECTOR); return 0; }
static uint32_t now_s(void) { return clk_ms / 1000; }
static uint32_t now_ms(void) { return clk_ms; }
static uint32_t rnd(void) { return (uint32_t)rand() ^ ((uint32_t)rand() << 16); }
static int verbose;
static void lg(const char *s) { if (verbose) printf("    %s\n", s); }

#define NODE_SFX "aabbccddeeff"

static void node_boot(void)
{
    st_env_t e = { f_read, f_write, f_erase, NULL, sizeof(fl), now_s, lg, mem, sizeof(mem) };
    assert(st_init(&e) == 0);
    rn_env_t r = { rnd, now_ms, lg, NODE_SFX };
    rn_init(&r);
}

/* ---------- модель телефона ---------- */

#define PH_MAX_MSG 300
typedef struct {
    char id[64], ttl[24], recv[40], type[8], bytes[120];
    int  uc;
    char box[37];            /* box_id в очереди телефона (стабилен) */
    bool node_has;           /* узел подтвердил CBB/CDB */
} pmsg_t;

typedef struct {
    char   id[13];
    pmsg_t m[PH_MAX_MSG];
    int    n;
    char   cbb[256][37]; int ncbb;     /* box_id, на которые надо ответить CBB */
    char   cdb[256][37]; int ncdb;
    char   cb_out_id[2][37];          /* id своих CBB/CDB (ждут ACB): 0 — CBB, 1 — CDB */
    char   acb[256][37]; int nacb;     /* ACB на чужие колбэки */
    int    got_acb;
} phone_t;

static int ph_find(phone_t *p, const char *mid)
{
    for (int i = 0; i < p->n; i++) if (!strcmp(p->m[i].id, mid)) return i;
    return -1;
}

static void ph_new_msg(phone_t *p, const char *text)
{
    pmsg_t *m = &p->m[p->n++];
    box_uuid_v4(m->id, rnd);
    strcpy(m->ttl, "2026-12-31 23:59:59");
    strcpy(m->recv, "R+"); strcat(m->recv, p->id); strcat(m->recv, "/abc=");
    strcpy(m->type, "text");
    snprintf(m->bytes, sizeof(m->bytes), "%s", text);
    m->uc = 0;
    box_uuid_v4(m->box, rnd);
}

/* телефон -> узел: узел подключается к GATT server телефона */
static e2e_rx_action_t feed(const char *pkt)
{
    return rn_on_rx((const uint8_t *)pkt, strlen(pkt));
}

static int phone_serve(phone_t *p)
{
    static char b[BOX_MAX_PACKET + 1];
    char sid[37];
    box_uuid_v4(sid, rnd);
    rn_rx_begin(p->id);
    box_build_session(b, sizeof(b), BOX_PKT_SID, sid);
    assert(feed(b) == E2E_RX_CONTINUE);
    if (p->nacb) {
        const char *ids[10];
        int k = 0;
        for (int i = 0; i < p->nacb && k < 10; i++) ids[k++] = p->acb[i];
        assert(box_build_acb(b, sizeof(b), ids, k) > 0);
        feed(b);
        p->nacb = 0;                      /* ACB_TTL у Android 60 с — здесь шлём один раз */
    }
    for (int kind = 0; kind < 2; kind++) {
        int n = kind ? p->ncdb : p->ncbb;
        if (!n) continue;
        const char *ids[10];
        for (int i = 0; i < n && i < 10; i++) ids[i] = kind ? p->cdb[i] : p->cbb[i];
        if (!p->cb_out_id[kind][0]) box_uuid_with_suffix(p->cb_out_id[kind], p->id, rnd);
        assert(box_build_callback(b, sizeof(b), kind ? BOX_PKT_CDB : BOX_PKT_CBB, p->cb_out_id[kind], ids,
                                  n < 10 ? n : 10) > 0);
        feed(b);
    }
    int sent = 0;
    for (int i = 0; i < p->n; i++) {
        pmsg_t *m = &p->m[i];
        if (m->node_has) continue;
        box_msg_t bm = { m->id, m->ttl, m->uc, m->recv, m->type, m->bytes };
        assert(box_build_box(b, sizeof(b), m->box, &bm, 1) > 0);
        feed(b);
        sent++;
    }
    box_build_session(b, sizeof(b), BOX_PKT_TRM, sid);
    assert(feed(b) == E2E_RX_CLOSE);
    assert(feed(b) == E2E_RX_CLOSE);
    /* повтор той же сессии другим соединением: узел закрывает сразу */
    rn_rx_begin(p->id);
    box_build_session(b, sizeof(b), BOX_PKT_SID, sid);
    assert(feed(b) == E2E_RX_CLOSE);
    return sent;
}

/* узел -> телефон через TX: телефон обрабатывает пакеты как FileSyncWorker */
typedef struct { int boxes, new_msgs, dups, cbb, cdb, acb; } got_t;

static got_t node_serve(phone_t *p, bool phone_known, int deliver_limit)
{
    static e2e_session_t s;
    int n = rn_build_session(&s, 509, phone_known ? p->id : NULL);
    assert(n >= 3 && n <= E2E_MAX_SESSION_PKTS);
    assert(s.pkt[0].kind == E2E_PK_SID && s.pkt[n - 1].kind == E2E_PK_TRM && s.pkt[n - 2].kind == E2E_PK_TRM);
    int delivered = deliver_limit >= 0 && deliver_limit < n ? deliver_limit : n;
    got_t g = {0};
    static box_pkt_t pk;
    for (int i = 0; i < delivered; i++) {
        assert(s.pkt[i].len <= 509);
        assert(box_parse((const uint8_t *)s.pkt[i].data, s.pkt[i].len, &pk) == 0);
        switch (pk.type) {
        case BOX_PKT_ACB:
            g.acb++;
            for (int k = 0; k < box_pkt_id_count(&pk); k++)
                for (int t = 0; t < 2; t++)
                    if (!strcmp(p->cb_out_id[t], box_pkt_id_at(&pk, k))) {
                        p->cb_out_id[t][0] = '\0';
                        if (t) p->ncdb = 0; else p->ncbb = 0;
                        p->got_acb++;
                    }
            break;
        case BOX_PKT_CBB:
        case BOX_PKT_CDB: {
            if (pk.type == BOX_PKT_CBB) g.cbb++; else g.cdb++;
            const char *cb = box_pkt_primary_id(&pk);
            assert(!strcmp(box_uuid_suffix(cb), NODE_SFX));
            int mine = 0;
            for (int k = 0; k < box_pkt_id_count(&pk); k++)
                for (int i2 = 0; i2 < p->n; i2++)
                    if (!strcmp(p->m[i2].box, box_pkt_id_at(&pk, k))) { p->m[i2].node_has = true; mine++; }
            if (mine) strcpy(p->acb[p->nacb++], cb);
            break;
        }
        case BOX_PKT_BOX: {
            g.boxes++;
            assert(box_pkt_msg_count(&pk) == 1);
            box_msg_t m;
            assert(box_pkt_msg_at(&pk, 0, &m) == 0);
            const char *bid = box_pkt_primary_id(&pk);
            int j = ph_find(p, m.message_id);
            if (j < 0) {
                pmsg_t *q = &p->m[p->n++];
                strcpy(q->id, m.message_id); strcpy(q->ttl, m.ttl); strcpy(q->recv, m.receiver);
                strcpy(q->type, m.type); strcpy(q->bytes, m.bytes);
                q->uc = m.unit_count + 1;
                box_uuid_v4(q->box, rnd);
                g.new_msgs++;
                strcpy(p->cbb[p->ncbb++], bid);
            } else {
                g.dups++;
                strcpy(p->cdb[p->ncdb++], bid);
            }
            break;
        }
        default: break;
        }
    }
    rn_commit_session(&s, delivered);
    return g;
}

/* ---------- сценарии ---------- */

static phone_t A, B, C, D;

static void setup_phone(phone_t *p, const char *id) { memset(p, 0, sizeof(*p)); strcpy(p->id, id); }

static void t_relay_a_to_b(void)
{
    memset(fl, 0xff, sizeof(fl));
    node_boot();
    setup_phone(&A, "72389525976d");
    setup_phone(&B, "70ce4cf097ea");
    for (int i = 0; i < 5; i++) { char t[32]; snprintf(t, sizeof(t), "hello-%d", i); ph_new_msg(&A, t); }

    assert(phone_serve(&A) == 5);
    assert(st_stats()->entries == 5 && rn_stats()->rx_msgs_new == 5);
    assert(rn_pending_callbacks() == 1);

    /* узел -> A: CBB про коробки A, своих сообщений A не получает */
    got_t g = node_serve(&A, true, -1);
    assert(g.cbb == 1 && g.boxes == 0);
    for (int i = 0; i < A.n; i++) assert(A.m[i].node_has);
    assert(A.nacb == 1);
    /* A -> узел: ACB снимает CBB узла; новых BOX нет */
    assert(phone_serve(&A) == 0);
    assert(rn_pending_callbacks() == 0);

    /* B не встречал A: получает всё через узел */
    g = node_serve(&B, true, -1);
    assert(g.boxes == 5 && g.new_msgs == 5 && g.cbb == 0);
    for (int i = 0; i < 5; i++) {
        int j = ph_find(&B, A.m[i].id);
        assert(j >= 0 && !strcmp(B.m[j].bytes, A.m[i].bytes));
        assert(B.m[j].uc == 2);                     /* +1 на узле, +1 на B */
    }
    /* B -> узел: CBB про коробки узла -> узел отметит, что у B есть, и ответит ACB */
    for (int i = 0; i < B.n; i++) B.m[i].node_has = true;   /* B знает от кого получил (putBoxIDinMap) */
    phone_serve(&B);
    assert(rn_stats()->rx_cb_matched == 1 && rn_pending_acbs() == 1);
    g = node_serve(&B, true, -1);
    assert(g.acb == 1 && g.boxes == 0 && B.got_acb == 1 && B.ncbb == 0);
    assert(rn_pending_acbs() == 0);
    printf("A -> node -> B         ok\n");
}

static void t_dup_from_b(void)
{
    /* B возвращает узлу те же сообщения (как при первом контакте) — узел отвечает CDB */
    for (int i = 0; i < B.n; i++) B.m[i].node_has = false;
    uint32_t dup0 = rn_stats()->rx_msgs_dup;
    assert(phone_serve(&B) == 5);
    assert(rn_stats()->rx_msgs_dup - dup0 == 5 && st_stats()->entries == 5);
    got_t g = node_serve(&B, true, -1);
    assert(g.cdb == 1 && g.boxes == 0);
    for (int i = 0; i < B.n; i++) assert(B.m[i].node_has);
    phone_serve(&B);                                  /* ACB на CDB */
    assert(rn_pending_callbacks() == 0);
    printf("dup -> CDB -> ACB      ok\n");
}

static void t_reboot(void)
{
    node_boot();                                      /* перезагрузка узла */
    assert(st_stats()->entries == 5);
    setup_phone(&C, "0123456789ab");
    got_t g = node_serve(&C, true, -1);
    assert(g.boxes == 5 && g.new_msgs == 5);
    for (int i = 0; i < 5; i++) {
        int j = ph_find(&C, A.m[i].id);
        assert(j >= 0 && C.m[j].uc == 2);
    }
    /* A после перезагрузки узла получит свои же? Нет: источник сохранён в журнале */
    g = node_serve(&A, true, -1);
    assert(g.boxes == 0);
    printf("node reboot            ok\n");
}

static void t_unknown_and_partial(void)
{
    /* Телефон неизвестен (старое приложение без HELLO): отдаём всё, частичная доставка. */
    setup_phone(&D, "ddddeeeeffff");
    got_t g = node_serve(&D, false, 3);               /* SID + 2 BOX */
    assert(g.boxes == 2 && g.new_msgs == 2);
    /* при неизвестном телефоне «у кого есть» не отмечаем, но sends растёт -> следующими идут другие */
    g = node_serve(&D, false, -1);
    assert(g.boxes == 5 && g.new_msgs == 3 && g.dups == 2);
    printf("unknown phone/partial  ok\n");
}

static void t_many(void)
{
    /* 100 сообщений: сессия ограничена 40 пакетами, за несколько сессий доходит всё. */
    memset(fl, 0xff, sizeof(fl));
    node_boot();
    setup_phone(&A, "a00000000001");
    setup_phone(&B, "b00000000002");
    for (int i = 0; i < 100; i++) { char t[64]; snprintf(t, sizeof(t), "bulk message number %d", i); ph_new_msg(&A, t); }
    phone_serve(&A);
    assert(st_stats()->entries == 100 && rn_pending_callbacks() == 10);
    int sessions = 0;
    while (B.n < 100 && sessions < 10) {
        got_t g = node_serve(&B, true, -1);
        assert(g.dups == 0);
        sessions++;
    }
    assert(B.n == 100 && sessions <= 3);
    /* A получает 10 CBB (по 10 box_id) за одну-две сессии и подтверждает */
    int s2 = 0;
    while (rn_pending_callbacks() && s2 < 5) { node_serve(&A, true, -1); phone_serve(&A); s2++; }
    assert(rn_pending_callbacks() == 0);
    for (int i = 0; i < A.n; i++) assert(A.m[i].node_has);
    printf("100 messages           ok (%d sessions to B)\n", sessions);
}

static void t_expiry(void)
{
    clk_ms += ST_MAX_AGE_S * 1000u + 5000u;
    for (int i = 0; i < 3; i++) { clk_ms += 1100; rn_tick(); }
    assert(st_stats()->entries == 0);
    setup_phone(&C, "c00000000003");
    got_t g = node_serve(&C, true, -1);
    assert(g.boxes == 0);
    printf("7-day expiry           ok\n");
}

static void t_callback_giveup(void)
{
    memset(fl, 0xff, sizeof(fl));
    node_boot();
    setup_phone(&A, "a0000000000a");
    ph_new_msg(&A, "lonely");
    phone_serve(&A);
    assert(rn_pending_callbacks() == 1);
    for (int i = 0; i < 20; i++) { clk_ms += 60000; rn_tick(); }
    assert(rn_pending_callbacks() == 0 && rn_stats()->cb_given_up == 1);
    printf("callback give-up       ok\n");
}

static void t_empty_fields(void)
{
    /* Сообщение с пустыми ttl/receiver/type (как может прислать Android) проходит через узел. */
    node_boot();
    static phone_t E, F;
    setup_phone(&E, "eeee00001111");
    setup_phone(&F, "ffff00002222");
    ph_new_msg(&E, "hello-empty");
    E.m[0].ttl[0] = 0; E.m[0].recv[0] = 0; E.m[0].type[0] = 0;
    uint32_t rej0 = rn_stats()->rx_msgs_rej;
    assert(phone_serve(&E) == 1);
    assert(rn_stats()->rx_msgs_rej == rej0 && st_find_msg(E.m[0].id) >= 0);
    got_t g = node_serve(&F, false, -1);
    int j = ph_find(&F, E.m[0].id);
    assert(g.new_msgs >= 1 && j >= 0);
    assert(F.m[j].ttl[0] == 0 && F.m[j].recv[0] == 0 && !strcmp(F.m[j].bytes, "hello-empty"));
    printf("empty ttl/receiver     ok\n");
}

static void t_multipart(void)
{
    /* Большое сообщение A: части «uuid_4_0..uuid_4_3» (BoxChatActivity.createMultipart), каждая в своём BOX — так, как в прошлом
     * тесте пришло 47519810 (в dual-0.4 такие id отклонялись как «не UUID»). */
    node_boot();
    static phone_t P, Q;
    setup_phone(&P, "0a0a0a0a0a0a");
    setup_phone(&Q, "0b0b0b0b0b0b");
    char base[37];
    box_uuid_v4(base, rnd);
    uint32_t new0 = rn_stats()->rx_msgs_new;
    for (int i = 0; i < 4; i++) {
        ph_new_msg(&P, "part");
        snprintf(P.m[i].id, sizeof(P.m[i].id), "%s_4_%d", base, i);
    }
    assert(phone_serve(&P) == 4);
    assert(rn_stats()->rx_msgs_new - new0 == 4);
    for (int i = 0; i < 4; i++) assert(st_find_msg(P.m[i].id) >= 0);
    /* Q получает все 4 части за одну сессию, unit_count +1 */
    got_t g = node_serve(&Q, false, -1);
    assert(g.boxes == (int)st_stats()->entries && g.new_msgs == g.boxes);   /* + 2 от прошлых тестов */
    for (int i = 0; i < 4; i++) { int j = ph_find(&Q, P.m[i].id); assert(j >= 0 && Q.m[j].uc == 2); }
    printf("multipart A->node->B   ok\n");
}

static void t_session_without_id(void)
{
    /* S24: LocalCache.currentSessionID == "" -> SID "V1\nSID\n" и TRM "V1\nTRM\n" (7 байт). */
    node_boot();
    static phone_t S;
    setup_phone(&S, "545e05cfd599");
    ph_new_msg(&S, "from-s24");
    box_pkt_t pk;
    assert(box_parse((const uint8_t *)"V1\nSID\n", 7, &pk) == 0 && pk.type == BOX_PKT_SID &&
           box_pkt_primary_id(&pk)[0] == 0);
    assert(box_parse((const uint8_t *)"V1\nTRM", 6, &pk) == 0 && pk.type == BOX_PKT_TRM);
    assert(box_parse((const uint8_t *)"V1\nCBB\n", 7, &pk) != 0);      /* остальным id нужен */
    for (int round = 0; round < 2; round++) {          /* дважды подряд: без id повтор не закрываем */
        uint32_t s0 = rn_stats()->rx_sessions;
        rn_rx_begin(S.id);
        assert(feed("V1\nSID\n") == E2E_RX_CONTINUE);
        static char b[BOX_MAX_PACKET + 1];
        box_msg_t bm = { S.m[0].id, S.m[0].ttl, 0, S.m[0].recv, S.m[0].type, S.m[0].bytes };
        assert(box_build_box(b, sizeof(b), S.m[0].box, &bm, 1) > 0);
        assert(feed(b) == E2E_RX_CONTINUE);
        assert(feed("V1\nTRM\n") == E2E_RX_CLOSE);
        assert(rn_stats()->rx_sessions == s0 + 1);
    }
    assert(rn_stats()->rx_sessions_noid >= 2 && st_find_msg(S.m[0].id) >= 0);
    printf("SID/TRM without id     ok\n");
}

int main(int argc, char **argv)
{
    (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    verbose = argc > 1;
    srand(4242);
    t_relay_a_to_b();
    t_dup_from_b();
    t_reboot();
    t_unknown_and_partial();
    t_many();
    t_expiry();
    t_callback_giveup();
    t_empty_fields();
    t_multipart();
    t_session_without_id();
    printf("ALL RELAY NODE TESTS PASSED\n");
    return 0;
}
