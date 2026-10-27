/*
 * Хост-тест store.c (ASan/UBSan): модель NOR-flash (запись только в стёртое, стирание
 * секторами), обрывы питания на произвольном байте записи или стирания, переполнение,
 * истечение 7 суток, квоты, выбор сообщений для телефона.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "store.h"

/* ---------- модель flash ---------- */

static uint8_t *fl;
static uint32_t fl_size;
static long     budget = -1;       /* байт до обрыва питания; -1 — без обрыва */
static int      dead;
static long     writes_total, erases_total;

static uint32_t r32(void) { return (uint32_t)rand() ^ ((uint32_t)rand() << 16); }

static int f_read(void *c, uint32_t off, void *b, size_t n)
{
    (void)c;
    if (dead) return -1;
    assert(off + n <= fl_size);
    memcpy(b, fl + off, n);
    return 0;
}

static int f_write(void *c, uint32_t off, const void *b, size_t n)
{
    (void)c;
    if (dead) return -1;
    assert(off + n <= fl_size);
    for (size_t i = 0; i < n; i++) assert(fl[off + i] == 0xff);   /* пишем только в стёртое */
    size_t k = n;
    if (budget >= 0) {
        if ((long)n > budget) { k = (size_t)budget; dead = 1; }
        budget -= (long)k;
    }
    for (size_t i = 0; i < k; i++) fl[off + i] &= ((const uint8_t *)b)[i];
    if (dead && k < n && (r32() & 1) && k + 1 <= n)                 /* «полубайт» на обрыве */
        fl[off + k] &= (uint8_t)(((const uint8_t *)b)[k] | (uint8_t)r32());
    writes_total++;
    return dead ? -1 : 0;
}

static int f_erase(void *c, uint32_t s)
{
    (void)c;
    if (dead) return -1;
    assert((s + 1) * ST_SECTOR <= fl_size);
    if (budget >= 0) {
        if (budget < 64) {                                           /* обрыв посреди стирания */
            for (uint32_t i = 0; i < ST_SECTOR; i++) if (r32() % 3 == 0) fl[s * ST_SECTOR + i] = 0xff;
            dead = 1;
            return -1;
        }
        budget -= 64;
    }
    memset(fl + s * ST_SECTOR, 0xff, ST_SECTOR);
    erases_total++;
    return 0;
}

static uint32_t clk;
static uint32_t now_s(void) { return clk; }
static int verbose;
static void lg(const char *s) { if (verbose) printf("    %s\n", s); }

static uint8_t *mem;
static size_t   mem_size;

static int mount(void)
{
    dead = 0;
    st_env_t e = { f_read, f_write, f_erase, NULL, fl_size, now_s, lg, mem, mem_size };
    return st_init(&e);
}

static void flash_new(uint32_t sectors, size_t index_entries)
{
    free(fl);
    free(mem);
    fl_size = sectors * ST_SECTOR;
    fl = malloc(fl_size);
    memset(fl, 0xff, fl_size);
    mem_size = ST_DELETED_RING * 8 + sectors * 12 + index_entries * sizeof(st_ent_t) + 64;
    mem = malloc(mem_size);
    budget = -1;
    assert(mount() == 0);
}

/* ---------- сообщения ---------- */

static void mk_id(char *out, uint32_t n)
{
    snprintf(out, 37, "%08x-0000-4000-8000-%012x", (unsigned)n, (unsigned)(n * 2654435761u));
}

typedef struct { char id[37], ttl[20], recv[64], type[8], bytes[200]; int uc; } tmsg_t;

static void mk_msg(tmsg_t *t, uint32_t n, box_msg_t *m)
{
    mk_id(t->id, n);
    snprintf(t->ttl, sizeof(t->ttl), "2026-11-%02u 12:00:00", (unsigned)(n % 28 + 1));
    snprintf(t->recv, sizeof(t->recv), "RCV%u+/=%u", (unsigned)n, (unsigned)(n * 7));
    snprintf(t->type, sizeof(t->type), n % 5 ? "text" : "file");
    int len = 10 + (int)(n * 37 % 150);
    for (int i = 0; i < len; i++) t->bytes[i] = (char)('A' + (n + (uint32_t)i) % 26);
    t->bytes[len] = '\0';
    t->uc = (int)(n % 9);
    *m = (box_msg_t){ t->id, t->ttl, t->uc, t->recv, t->type, t->bytes };
}

static void check_msg(uint32_t n)
{
    tmsg_t t;
    box_msg_t m;
    mk_msg(&t, n, &m);
    int i = st_find_msg(t.id);
    assert(i >= 0);
    static st_msg_t r;
    assert(st_read(i, &r) == 0);
    assert(strcmp(r.msg.message_id, t.id) == 0 && strcmp(r.msg.ttl, t.ttl) == 0);
    assert(strcmp(r.msg.receiver, t.recv) == 0 && strcmp(r.msg.type, t.type) == 0);
    assert(strcmp(r.msg.bytes, t.bytes) == 0 && r.msg.unit_count == t.uc);
    char b[37];
    st_box_id_for(t.id, b);
    assert(strcmp(b, r.box_id) == 0 && box_is_uuid(b) && st_find_box(b) == i);
}

static void check_invariants(void)
{
    const st_stats_t *s = st_stats();
    uint32_t n = 0, live = 0;
    for (uint32_t i = 0; i < s->cap; i++) {
        const st_ent_t *e = st_entry((int)i);
        if (!e) continue;
        n++;
        live += e->rec_len;
        static st_msg_t r;
        assert(st_read((int)i, &r) == 0);
    }
    assert(n == s->entries && live == s->bytes_live);
}

/* ---------- тесты ---------- */

static void t_basic(void)
{
    clk = 1000;
    flash_new(128, 2000);
    tmsg_t t;
    box_msg_t m;
    for (uint32_t n = 1; n <= 150; n++) { mk_msg(&t, n, &m); assert(st_put(&m, "72389525976d") == ST_NEW); }
    for (uint32_t n = 1; n <= 150; n++) { mk_msg(&t, n, &m); assert(st_put(&m, "70ce4cf097ea") == ST_DUP); }
    for (uint32_t n = 1; n <= 150; n++) check_msg(n);
    mk_msg(&t, 1, &m);
    int i = st_find_msg(t.id);
    assert(st_phone_has(i, "72389525976d") && st_phone_has(i, "70ce4cf097ea") && !st_phone_has(i, "aaaaaaaaaaaa"));
    assert(mount() == 0);                        /* перезагрузка */
    assert(st_stats()->entries == 150 && st_stats()->load_bad == 0);
    for (uint32_t n = 1; n <= 150; n++) check_msg(n);
    check_invariants();
    /* источник помнится, «у кого есть» после перезагрузки — только источник */
    i = st_find_msg(t.id);
    assert(st_phone_has(i, "72389525976d") && !st_phone_has(i, "70ce4cf097ea"));
    printf("basic + remount        ok\n");
}

static void t_validation(void)
{
    clk = 1000;
    flash_new(16, 200);
    tmsg_t t;
    box_msg_t m;
    mk_msg(&t, 5, &m);
    box_msg_t bad = m;
    bad.message_id = "not-a-uuid";
    assert(st_put(&bad, NULL) == ST_REJECT_SIZE);
    bad = m; bad.bytes = "";
    assert(st_put(&bad, NULL) == ST_REJECT_SIZE);
    bad = m; bad.receiver = "a\nb";
    assert(st_put(&bad, NULL) == ST_REJECT_SIZE);
    static char big[ST_MAX_FIELD + 2];
    memset(big, 'x', sizeof(big) - 1);
    bad = m; bad.bytes = big;
    assert(st_put(&bad, NULL) == ST_REJECT_SIZE);
    big[ST_MAX_FIELD] = '\0';
    bad = m; bad.bytes = big;
    assert(st_put(&bad, NULL) == ST_NEW);      /* ровно предел — можно */
    bad = m; bad.ttl = NULL; bad.message_id = "00000000-0000-4000-8000-000000000001";
    assert(st_put(&bad, NULL) == ST_REJECT_SIZE);
    bad.ttl = ""; 
    assert(st_put(&bad, NULL) == ST_REJECT_SIZE);
    printf("validation             ok\n");
}

static void t_expiry(void)
{
    clk = 5000;
    flash_new(16, 500);
    tmsg_t t;
    box_msg_t m;
    for (uint32_t n = 1; n <= 40; n++) { mk_msg(&t, n, &m); assert(st_put(&m, NULL) == ST_NEW); }
    clk += ST_MAX_AGE_S - 10;
    for (uint32_t n = 41; n <= 50; n++) { mk_msg(&t, n, &m); assert(st_put(&m, NULL) == ST_NEW); }
    st_maintain();
    assert(st_stats()->entries == 50);
    clk += 20;                                     /* первые 40 старше 7 суток */
    st_maintain();
    assert(st_stats()->entries == 10);
    mk_msg(&t, 3, &m);
    assert(st_put(&m, NULL) == ST_DUP);            /* недавно истёкшее не принимаем снова */
    assert(mount() == 0);
    assert(st_stats()->entries == 10 && st_stats()->expired == 40);
    for (uint32_t n = 41; n <= 50; n++) check_msg(n);
    printf("expiry 7 days          ok\n");
}

static void t_fill(void)
{
    clk = 100;
    flash_new(24, 5000);                           /* ~90 КБ полезного места */
    tmsg_t t;
    box_msg_t m;
    uint32_t newest = 0;
    for (uint32_t n = 1; n <= 4000; n++) {
        mk_msg(&t, n, &m);
        char src[13];
        snprintf(src, sizeof(src), "%012x", (unsigned)(n % 7));
        st_put_t r = st_put(&m, src);
        assert(r == ST_NEW);
        newest = n;
        if (n % 10 == 0) { clk += 30; st_maintain(); }
        const st_stats_t *s = st_stats();
        assert(s->sectors_free >= 1 && s->io_errors == 0);
    }
    for (uint32_t n = newest - 20; n <= newest; n++) check_msg(n);
    check_invariants();
    const st_stats_t *s = st_stats();
    assert(s->gc_evicted > 0 && s->bytes_live <= 24u * ST_SECTOR);
    uint32_t before = s->entries;
    assert(mount() == 0);
    assert(st_stats()->entries == before);
    check_invariants();
    printf("fill + gc              ok (kept %u, erases %ld)\n", (unsigned)before, erases_total);
}

static void t_ring(void)
{
    /* Журнал проходит много кругов; время идёт, старое истекает или вытесняется,
     * свежие сообщения всегда на месте, ошибок flash нет. */
    clk = 100;
    flash_new(20, 3000);
    tmsg_t t;
    box_msg_t m;
    uint32_t n = 1;
    for (; n <= 20000; n++) {
        mk_msg(&t, n, &m);
        assert(st_put(&m, NULL) == ST_NEW);
        clk += 97;
        if (n % 20 == 0) st_maintain();
        assert(st_stats()->io_errors == 0);
    }
    for (uint32_t k = n - 60; k < n; k++) check_msg(k);
    check_invariants();
    const st_stats_t *s = st_stats();
    assert(s->gc_evicted > 0 && s->gc_runs > 20000 / 30);
    /* медленный поток: за 7 суток меньше, чем помещается, — старое уходит по сроку */
    uint32_t ev = s->gc_evicted;
    for (uint32_t k = 0; k < 3000; k++, n++) {
        mk_msg(&t, n, &m);
        assert(st_put(&m, NULL) == ST_NEW);
        clk += 4000;
        st_maintain();
    }
    for (uint32_t k = n - 140; k < n; k++) check_msg(k);
    check_invariants();
    assert(s->expired > 2000 && s->gc_evicted - ev < 300 && st_stats()->entries <= ST_MAX_AGE_S / 4000 + 1);
    printf("ring laps              ok (gc %u copied %u evicted %u expired %u)\n",
           (unsigned)s->gc_runs, (unsigned)s->gc_copied, (unsigned)s->gc_evicted, (unsigned)s->expired);
}

static void t_quota(void)
{
    clk = 100;
    flash_new(20, 5000);
    tmsg_t t;
    box_msg_t m;
    int newc = 0;
    uint32_t n = 1;
    for (; n < 5000; n++) {
        mk_msg(&t, n, &m);
        st_put_t r = st_put(&m, "111111111111");
        if (r == ST_REJECT_QUOTA) break;
        assert(r == ST_NEW);
        newc++;
    }
    assert(n < 5000 && newc > 10);
    mk_msg(&t, 90000, &m);
    assert(st_put(&m, "222222222222") == ST_NEW);
    mk_msg(&t, 90001, &m);
    assert(st_put(&m, NULL) == ST_NEW);
    printf("quota per source       ok (%d accepted)\n", newc);
}

static void t_select(void)
{
    clk = 100;
    flash_new(16, 500);
    tmsg_t t;
    box_msg_t m;
    for (uint32_t n = 1; n <= 10; n++) { mk_msg(&t, n, &m); clk++; assert(st_put(&m, "aaaaaaaaaaaa") == ST_NEW); }
    int idx[20];
    assert(st_select("aaaaaaaaaaaa", idx, 20) == 0);          /* источнику не отдаём */
    int k = st_select("bbbbbbbbbbbb", idx, 20);
    assert(k == 10);
    const st_ent_t *e0 = st_entry(idx[0]), *e9 = st_entry(idx[9]);
    assert(e0->got_at > e9->got_at);                          /* новые первыми */
    st_mark_sent(idx[0]);
    st_mark_sent(idx[1]);
    int top[3];
    assert(st_select(NULL, top, 3) == 3);
    assert(st_entry(top[0])->sends == 0 && top[0] == idx[2]); /* реже отданные первыми */
    st_mark_have_idx(idx[5], "bbbbbbbbbbbb");
    assert(st_select("bbbbbbbbbbbb", idx, 20) == 9);
    printf("select + have          ok\n");
}

static void t_power_cut(void)
{
    /* Тысячи обрывов: всё, что st_put подтвердил (ST_NEW), после перезагрузки на месте,
     * если с тех пор ничего не вытеснялось; ни одна запись не читается битой. */
    srand(12345);
    clk = 100;
    flash_new(12, 3000);
    static uint32_t committed[20000];
    int nc = 0;
    uint32_t next = 1;
    long cuts = 0, evict_runs = 0;
    for (int round = 0; round < 3000; round++) {
        budget = (long)(r32() % 20000);
        uint32_t ev0 = st_stats()->gc_evicted;
        int nc0 = nc;
        (void)ev0;
        while (!dead) {
            uint32_t op = r32() % 100;
            tmsg_t t;
            box_msg_t m;
            if (op < 80) {
                mk_msg(&t, next, &m);
                st_put_t r = st_put(&m, (op & 1) ? "aaaaaaaaaaaa" : "bbbbbbbbbbbb");
                if (r == ST_NEW && !dead) { if (nc < 20000) committed[nc++] = next; }
                next++;
            } else if (op < 90) {
                if (nc) { mk_msg(&t, committed[r32() % (uint32_t)nc], &m); st_put(&m, NULL); }
            } else {
                clk += 600;
                st_maintain();
            }
        }
        cuts++;
        bool evicted = st_stats()->gc_evicted != ev0 || st_stats()->expired;
        budget = -1;
        assert(mount() == 0);
        check_invariants();
        if (!evicted) {
            for (int i = nc0; i < nc; i++) {
                tmsg_t t;
                box_msg_t m;
                mk_msg(&t, committed[i], &m);
                const st_ent_t *e = NULL;
                int ix = st_find_msg(t.id);
                if (ix >= 0) e = st_entry(ix);
                if (!e) {
                    /* допустимо только истечение срока */
                    continue;
                }
                check_msg(committed[i]);
            }
        } else {
            evict_runs++;
        }
        if (nc > 15000) nc = 0;
    }
    printf("power cut x%ld          ok (rounds with eviction %ld, writes %ld)\n", cuts, evict_runs, writes_total);
}

static void t_power_cut_strict(void)
{
    /* Без вытеснения (журнал с запасом): всё подтверждённое (ST_NEW) переживает любой обрыв. */
    srand(777);
    clk = 100;
    flash_new(256, 6000);
    static uint32_t committed[6000];
    int nc = 0, rounds = 0;
    uint32_t next = 1, evicted = 0;
    for (; rounds < 400 && nc < 2500; rounds++) {
        budget = (long)(r32() % 6000);
        while (!dead && nc < 2500) {
            tmsg_t t;
            box_msg_t m;
            mk_msg(&t, next, &m);
            if (st_put(&m, NULL) == ST_NEW && !dead) committed[nc++] = next;
            next++;
            if ((r32() % 50) == 0) st_maintain();
        }
        evicted += st_stats()->gc_evicted;
        budget = -1;
        assert(mount() == 0);
        assert(evicted == 0);
        for (int i = 0; i < nc; i++) check_msg(committed[i]);
        check_invariants();
    }
    printf("power cut strict       ok (%d rounds, %d committed, all present)\n", rounds, nc);
}

static void t_garbage(void)
{
    /* Случайный мусор во flash: монтирование не падает, битое не читается. */
    srand(99);
    for (int it = 0; it < 300; it++) {
        clk = 100;
        flash_new(10, 500);
        tmsg_t t;
        box_msg_t m;
        for (uint32_t n = 1; n <= 60; n++) { mk_msg(&t, n, &m); st_put(&m, NULL); }
        for (int k = 0; k < 40; k++) fl[r32() % fl_size] = (uint8_t)r32();
        assert(mount() == 0);
        check_invariants();
        for (uint32_t n = 100; n < 160; n++) { mk_msg(&t, n, &m); st_put(&m, NULL); }
        check_invariants();
    }
    printf("garbage flash x300     ok\n");
}

int main(int argc, char **argv)
{
    (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    verbose = argc > 1;
    t_basic();
    t_validation();
    t_expiry();
    t_select();
    t_quota();
    t_fill();
    t_ring();
    t_garbage();
    t_power_cut_strict();
    t_power_cut();
    printf("ALL STORE TESTS PASSED\n");
    return 0;
}
