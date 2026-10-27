/*
 * Хост-тест relay.c вместе с настоящим автоматом e2e_test.c (ASan/UBSan).
 * Транспорт между CORE и TX здесь — прямые вызовы с возможностью отказа:
 * надёжность самого UART-канала проверяется отдельно в box-node-link.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "relay.h"
#include "e2e_test.h"
#include "box_proto.h"

static uint32_t now;
static uint32_t t_now(void) { return now; }
static uint32_t r32(void) { return (uint32_t)rand() ^ ((uint32_t)rand() << 16); }
static int verbose;
static void lg(const char *s) { if (verbose) printf("    %s\n", s); }

/* ---- захват того, что CORE построил и что TX собрал ---- */
static e2e_session_t built;
static int commits, last_commit_delivered = -99;
static int build_w(e2e_session_t *s, uint16_t mp, const char *ph) { (void)ph; int n = e2e_build_session(s, mp); built = *s; return n; }
static void commit_w(const e2e_session_t *s, int d) { commits++; last_commit_delivered = d; e2e_commit_session(s, d); }

static rt_asm_t tx_asm;
static int tx_complete, tx_errors;
static int send_fail_at = -1, send_count;
static uint8_t last_type;
static int sync_reqs;

static int core_send(uint8_t type, const uint8_t *d, uint16_t len)
{
    last_type = type;
    if (type == RL_SYNC_REQ) { sync_reqs++; return 0; }
    assert(type == RL_SESS_PKT);
    if (send_fail_at >= 0 && send_count++ == send_fail_at) return -1;
    rt_result_t r = rt_on_pkt(&tx_asm, d, len);
    if (r == RT_COMPLETE) tx_complete++;
    if (r == RT_ERROR) tx_errors++;
    return 0;
}

static void tx_says(uint8_t type, const uint8_t *d, size_t n) { rc_on_msg(type, d, (uint16_t)n); }

static void subscribe(uint16_t conn, uint16_t mtu)
{
    uint8_t b[4];
    tx_says(RL_SUBSCRIBED, b, rl_enc_conn_mtu(b, conn, mtu));
}

static void done(uint16_t token, uint16_t conn, int delivered, int count, int16_t rc)
{
    uint8_t b[8];
    tx_says(RL_SESS_DONE, b, rl_enc_sess_done(b, token, conn, (uint8_t)delivered, (uint8_t)count, rc));
}

static void setup(void)
{
    srand(7);
    now = 1000;
    e2e_env_t ee = { r32, t_now, lg, "aabbccddeeff", E2E_TTL_DEFAULT, 15u * 60u * 1000u };
    e2e_init(&ee);
    rc_env_t re = { core_send, t_now, lg, build_w, commit_w, NULL };
    rc_init(&re);
    rt_init(&tx_asm);
    commits = 0; tx_complete = tx_errors = 0; send_fail_at = -1; send_count = 0;
}

static void same_as_built(void)
{
    assert(tx_asm.s.count == built.n);
    for (int i = 0; i < built.n; i++) {
        assert(tx_asm.s.pkt[i].kind == (uint8_t)built.pkt[i].kind);
        assert(tx_asm.s.pkt[i].len == built.pkt[i].len);
        assert(memcmp(tx_asm.s.pkt[i].data, built.pkt[i].data, built.pkt[i].len) == 0);
    }
}

static void t_normal(void)
{
    setup();
    subscribe(1, 517);
    rc_tick(true);
    assert(tx_complete == 1 && tx_errors == 0);
    same_as_built();
    assert(built.n >= 4);                       /* SID, BOX X1, TRM, TRM */
    int has_x1 = 0;
    for (int i = 0; i < built.n; i++) has_x1 |= built.pkt[i].kind == E2E_PK_BOX_X1;
    assert(has_x1);
    assert(tx_asm.s.conn == 1);
    done(tx_asm.s.token, 1, built.n, built.n, 0);
    assert(commits == 1 && last_commit_delivered == built.n);
    assert(rc_stats()->sessions_done == 1);
    /* следующий подписчик получает новую сессию; X1 уже отдан, но CBB(X1) нет — снова X1 */
    subscribe(2, 185);
    rc_tick(true);
    assert(tx_complete == 2 && tx_asm.s.conn == 2);
    same_as_built();
    for (int i = 0; i < built.n; i++) assert(built.pkt[i].len <= 182);
    done(tx_asm.s.token, 2, built.n, built.n, 0);
    assert(commits == 2);
    printf("normal                 ok\n");
}

static void t_partial_and_stale(void)
{
    setup();
    subscribe(5, 517);
    rc_tick(true);
    uint16_t tok = tx_asm.s.token;
    done(tok, 5, 2, built.n, 7);
    assert(commits == 1 && last_commit_delivered == 2);
    done(tok, 5, built.n, built.n, 0);          /* повтор — устаревший */
    assert(commits == 1 && rc_stats()->stale_done == 1);
    done((uint16_t)(tok + 5), 5, 1, 1, 0);       /* чужой token */
    assert(commits == 1 && rc_stats()->stale_done == 2);
    printf("partial+stale          ok\n");
}

static void t_restart(void)
{
    setup();
    subscribe(1, 517);
    subscribe(2, 517);
    rc_tick(true);                               /* активна conn=1, conn=2 в очереди */
    uint8_t a[6];
    assert(!rc_tx_addr(a));
    uint8_t info[7], addr[6] = {1, 2, 3, 4, 5, 6};
    tx_says(RL_TX_INFO, info, rl_enc_tx_info(info, 0, addr));
    assert(rc_tx_addr(a) && memcmp(a, addr, 6) == 0);
    rc_on_peer_restart();
    assert(commits == 1 && last_commit_delivered == 0);
    rc_tick(true);
    assert(tx_complete == 1);                    /* conn=2 сброшен вместе с перезагрузкой */
    assert(rc_tx_addr(a) && memcmp(a, addr, 6) == 0);   /* адрес TX не забывается */
    rc_on_link_up();
    assert(sync_reqs >= 1);
    printf("restart                ok\n");
}

static void t_timeout(void)
{
    setup();
    subscribe(3, 517);
    rc_tick(true);
    uint16_t tok = tx_asm.s.token;
    now += RL_SESS_TIMEOUT_MS - 1; rc_tick(true);
    assert(commits == 0);
    now += 2; rc_tick(true);
    assert(commits == 1 && last_commit_delivered == 0 && rc_stats()->aborted_timeout == 1);
    done(tok, 3, built.n, built.n, 0);
    assert(commits == 1 && rc_stats()->stale_done == 1);
    printf("timeout                ok\n");
}

static void t_send_fail(void)
{
    setup();
    send_fail_at = 2;
    subscribe(4, 517);
    rc_tick(true);
    assert(commits == 1 && last_commit_delivered == 0 && rc_stats()->aborted_send == 1);
    assert(tx_complete == 0);
    send_fail_at = -1;
    subscribe(4, 517);
    rc_tick(true);                               /* недособранная сессия на TX сбрасывается idx=0 */
    assert(tx_complete == 1 && tx_errors == 0);
    same_as_built();
    printf("send fail+resume       ok\n");
}

static void t_queue(void)
{
    setup();
    subscribe(1, 517);
    rc_tick(true);
    subscribe(1, 517);                           /* та же conn во время сессии — игнор */
    for (int c = 2; c <= 7; c++) subscribe((uint16_t)c, 517);
    assert(rc_stats()->subscribed == 8);
    done(tx_asm.s.token, 1, built.n, built.n, 0);
    rc_tick(true);
    /* очередь на 4: остались 4..7 */
    assert(tx_asm.s.conn == 4);
    uint8_t b[2];
    tx_says(RL_UNSUBSCRIBED, b, rl_enc_conn(b, 5));
    done(tx_asm.s.token, 4, built.n, built.n, 0);
    rc_tick(true);
    assert(tx_asm.s.conn == 6);
    rc_tick(false);                              /* без канала ничего не шлём */
    printf("queue                  ok\n");
}

static void t_link_down_no_send(void)
{
    setup();
    subscribe(1, 517);
    rc_tick(false);
    assert(tx_complete == 0 && commits == 0);
    rc_tick(true);
    assert(tx_complete == 1);
    printf("link down hold         ok\n");
}

static const uint8_t phone_a_addr[6] = {0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0x7a};
static const char *by_addr(const uint8_t a[6]) { return memcmp(a, phone_a_addr, 6) == 0 ? "aaaaaaaaaaaa" : NULL; }

static void t_sub_addr(void)
{
    setup();
    rc_env_t re = { core_send, t_now, lg, build_w, commit_w, by_addr };
    rc_init(&re);
    uint8_t b[23], other[6] = {9, 9, 9, 9, 9, 9};
    tx_says(RL_SUBSCRIBED, b, rl_enc_subscribed(b, 1, 517, 1, phone_a_addr, NULL));
    tx_says(RL_SUBSCRIBED, b, rl_enc_subscribed(b, 2, 517, 1, other, NULL));
    assert(rl_enc_subscribed(b, 3, 517, 1, other, "72389525976d") == 23);
    tx_says(RL_SUBSCRIBED, b, 23);
    assert(rc_stats()->sub_addr_matched == 1 && rc_stats()->sub_addr_unknown == 1);
    assert(rc_stats()->sub_hello == 1);
    rc_tick(true);
    assert(tx_complete == 1 && tx_asm.s.conn == 1);
    printf("subscribed+addr        ok\n");
}

/* Найти box ID пакета нужного вида в последней построенной сессии. */
static int find_box_id(e2e_pkt_kind_t kind, char *out)
{
    static box_pkt_t p;
    for (int i = 0; i < built.n; i++) {
        if (built.pkt[i].kind != kind) continue;
        assert(box_parse((const uint8_t *)built.pkt[i].data, (size_t)built.pkt[i].len, &p) == 0);
        snprintf(out, 64, "%s", box_pkt_primary_id(&p));
        return 1;
    }
    return 0;
}

static void phone_cb(box_pkt_type_t t, const char *phone, const char *box_id)
{
    char cb[64], pkt[BOX_MAX_PACKET];
    snprintf(cb, sizeof(cb), "1234abcd-0000-4000-8000-%s", phone);
    const char *b[1] = { box_id };
    int n = box_build_callback(pkt, sizeof(pkt), t, cb, b, 1);
    assert(n > 0);
    e2e_on_rx((const uint8_t *)pkt, (size_t)n);
}

static void session_full(uint16_t conn)
{
    subscribe(conn, 517);
    rc_tick(true);
    done(tx_asm.s.token, conn, built.n, built.n, 0);
}

static void t_two_phones(void)
{
    char x1[64], x2[64];
    /* второй телефон впервые видит M1 в X2 и отвечает CBB — это не ошибка */
    setup();
    session_full(1);
    assert(find_box_id(E2E_PK_BOX_X1, x1));
    phone_cb(BOX_PKT_CBB, "aaaaaaaaaaaa", x1);           /* телефон A принял M1 */
    session_full(2);
    assert(find_box_id(E2E_PK_BOX_X2, x2));
    phone_cb(BOX_PKT_CBB, "bbbbbbbbbbbb", x2);           /* телефон B: M1 для него новое */
    assert(!e2e_dedup_failed());
    phone_cb(BOX_PKT_CDB, "aaaaaaaaaaaa", x2);           /* A узнал дубль */
    assert(!e2e_dedup_failed() && e2e_phone_count() == 2);
    /* тот же телефон A прислал CBB на X2 — ошибка дедупликации */
    phone_cb(BOX_PKT_CBB, "aaaaaaaaaaaa", x2);
    assert(e2e_dedup_failed());
    printf("two phones dedup       ok\n");
}

static void t_asm_fuzz(void)
{
    static uint8_t buf[RL_PKT_HDR + BOX_MAX_PACKET + 32];
    rt_asm_t a;
    rt_init(&a);
    int complete = 0;
    for (int it = 0; it < 200000; it++) {
        uint16_t len = (uint16_t)(rand() % sizeof(buf));
        for (uint16_t i = 0; i < len; i++) buf[i] = (uint8_t)rand();
        if (rand() % 3 == 0 && len >= RL_PKT_HDR) {
            buf[4] = (uint8_t)(rand() % 3); buf[5] = (uint8_t)(1 + rand() % 3);
            buf[0] = buf[1] = 0; buf[2] = 1; buf[3] = 0;
        }
        if (rt_on_pkt(&a, buf, len) == RT_COMPLETE) {
            complete++;
            assert(a.s.count >= 1 && a.s.count <= E2E_MAX_SESSION_PKTS);
            for (int i = 0; i < a.s.count; i++) assert(a.s.pkt[i].len <= BOX_MAX_PACKET);
        }
    }
    /* неупорядоченная доставка отклоняется */
    uint8_t p[RL_PKT_HDR + 4];
    rt_init(&a);
    rl_enc_sess_pkt(p, sizeof(p), 9, 1, 1, 3, 0, "abcd", 4);
    assert(rt_on_pkt(&a, p, sizeof(p)) == RT_ERROR);
    rl_enc_sess_pkt(p, sizeof(p), 9, 1, 0, 3, 0, "abcd", 4);
    assert(rt_on_pkt(&a, p, sizeof(p)) == RT_NEED_MORE);
    rl_enc_sess_pkt(p, sizeof(p), 9, 1, 2, 3, 0, "abcd", 4);
    assert(rt_on_pkt(&a, p, sizeof(p)) == RT_ERROR);
    printf("assembler fuzz         ok (random completes=%d)\n", complete);
}

int main(int argc, char **argv)
{
    (void)argv;
    verbose = argc > 1;
    t_normal();
    t_partial_and_stale();
    t_restart();
    t_timeout();
    t_send_fail();
    t_queue();
    t_link_down_no_send();
    t_sub_addr();
    t_two_phones();
    t_asm_fuzz();
    printf("ALL PASSED\n");
    return 0;
}
