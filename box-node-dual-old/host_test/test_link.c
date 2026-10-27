/*
 * Хост-тест board_link: два узла соединены эмулятором UART с потерями,
 * искажениями байтов и обрывами. Проверяется, что каждое надёжное сообщение
 * доставлено ровно один раз и по порядку, а перезагрузка соседа обнаруживается.
 *
 *   cc -std=c11 -Wall -Wextra -I ../components/board_link/include \
 *      test_link.c ../components/board_link/board_link.c -o test_link && ./test_link
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_link.h"

static uint32_t now;
static uint32_t t_now(void) { return now; }

static uint32_t rng = 1;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

/* Канал: кольцевой буфер байтов в одну сторону с задержкой доставки по времени. */
#define CH_CAP (1 << 20)
typedef struct { uint8_t b[CH_CAP]; uint32_t t[CH_CAP]; size_t head, tail; } chan_t;
static chan_t ab, ba;

static double p_drop, p_flip, p_cut;
static uint32_t busy_until[2];

static void ch_write(chan_t *c, const uint8_t *d, size_t n)
{
    /* 115200 бод ≈ 11.5 байт/мс */
    int idx = c == &ab ? 0 : 1;
    if (busy_until[idx] < now) busy_until[idx] = now;
    for (size_t i = 0; i < n; i++) {
        if ((rnd() % 1000000) < p_cut * 1e6) return;        /* обрыв остатка кадра */
        if ((rnd() % 1000000) < p_drop * 1e6) continue;     /* потеря байта */
        uint8_t v = d[i];
        if ((rnd() % 1000000) < p_flip * 1e6) v ^= (uint8_t)(1u << (rnd() % 8));
        busy_until[idx] += (i % 12 == 0) ? 1 : 0;
        c->b[c->head % CH_CAP] = v;
        c->t[c->head % CH_CAP] = busy_until[idx] + 1;
        c->head++;
    }
}

static void wr_a(const uint8_t *d, size_t n, void *ctx) { (void)ctx; ch_write(&ab, d, n); }
static void wr_b(const uint8_t *d, size_t n, void *ctx) { (void)ctx; ch_write(&ba, d, n); }

static void ch_pump(chan_t *c, bl_link_t *dst)
{
    uint8_t tmp[256];
    size_t k = 0;
    while (c->tail < c->head && c->t[c->tail % CH_CAP] <= now && k < sizeof(tmp))
        tmp[k++] = c->b[c->tail++ % CH_CAP];
    if (k) bl_rx_bytes(dst, tmp, k);
}

/* Приёмник B проверяет порядок: каждое сообщение несёт u32 номер + шаблон. */
static uint32_t b_expect, b_got, b_dups_seen;
static uint32_t restarts_seen;

static void on_frame_b(uint8_t type, const uint8_t *d, uint16_t len, void *ctx)
{
    (void)ctx;
    assert(type == 0x20);
    assert(len >= 4);
    uint32_t id; memcpy(&id, d, 4);
    for (uint16_t i = 4; i < len; i++) assert(d[i] == (uint8_t)(id * 31 + i));
    if (id != b_expect) {
        fprintf(stderr, "order violation: got %u expected %u\n", id, b_expect);
        abort();
    }
    b_expect++;
    b_got++;
}

static void on_event(const char *what, void *ctx)
{
    if (strcmp(what, "peer restarted") == 0) restarts_seen++;
    (void)ctx;
}

static void build_msg(uint8_t *buf, uint32_t id, uint16_t len)
{
    memcpy(buf, &id, 4);
    for (uint16_t i = 4; i < len; i++) buf[i] = (uint8_t)(id * 31 + i);
}

static int scenario(const char *name, double drop, double flip, double cut, uint32_t msgs)
{
    static _Alignas(8) uint8_t ma[16384], mb[16384];
    assert(bl_link_size() <= sizeof(ma));
    bl_link_t *a = (bl_link_t *)ma, *b = (bl_link_t *)mb;

    memset(&ab, 0, sizeof(ab)); memset(&ba, 0, sizeof(ba)); busy_until[0] = busy_until[1] = 0;
    p_drop = drop; p_flip = flip; p_cut = cut;
    now = 0; b_expect = 0; b_got = 0; b_dups_seen = 0; restarts_seen = 0;

    bl_config_t ca = { BL_ROLE_CORE, 0x1111, t_now, wr_a, NULL, on_event, NULL };
    bl_config_t cb = { BL_ROLE_TX,   0x2222, t_now, wr_b, on_frame_b, on_event, NULL };
    bl_init(a, &ca);
    bl_init(b, &cb);

    uint32_t sent = 0, failed_before = 0;
    uint8_t buf[BL_MAX_DATA];
    while (b_got < msgs && now < 3600000) {
        now++;
        if (sent < msgs && bl_tx_queue_depth(a) < BL_TXQ_LEN) {
            uint16_t len = (uint16_t)(4 + rnd() % (BL_MAX_DATA - 3));
            build_msg(buf, sent, len);
            if (bl_send(a, 0x20, buf, len, true) == 0) sent++;
        }
        ch_pump(&ab, b);
        ch_pump(&ba, a);
        if (now % 10 == 0) { bl_poll(a); bl_poll(b); }
        if (bl_stats(a)->send_failed != failed_before) {
            fprintf(stderr, "%s: send_failed at msg %u\n", name, b_expect);
            return 1;
        }
    }
    const bl_stats_t *sa = bl_stats(a), *sb = bl_stats(b);
    printf("%-22s msgs=%u t=%us retx=%u crc=%u cobs=%u len=%u dup=%u ok=%s\n", name, b_got,
           now / 1000, sa->retransmits, sb->crc_errors + sa->crc_errors,
           sb->cobs_errors + sa->cobs_errors, sb->len_errors + sa->len_errors, sb->dup_rx,
           b_got == msgs ? "yes" : "NO");
    return b_got == msgs ? 0 : 1;
}

static int restart_scenario(void)
{
    static _Alignas(8) uint8_t ma[16384], mb[16384];
    bl_link_t *a = (bl_link_t *)ma, *b = (bl_link_t *)mb;
    memset(&ab, 0, sizeof(ab)); memset(&ba, 0, sizeof(ba)); busy_until[0] = busy_until[1] = 0;
    p_drop = p_flip = p_cut = 0;
    now = 0; b_expect = 0; b_got = 0; restarts_seen = 0;

    bl_config_t ca = { BL_ROLE_CORE, 0x1111, t_now, wr_a, NULL, on_event, NULL };
    bl_config_t cb = { BL_ROLE_TX,   0x2222, t_now, wr_b, on_frame_b, on_event, NULL };
    bl_init(a, &ca);
    bl_init(b, &cb);
    uint8_t buf[64];
    for (uint32_t i = 0; i < 5; i++) { build_msg(buf, i, 20); bl_send(a, 0x20, buf, 20, true);
        for (int k = 0; k < 200; k++) { now++; ch_pump(&ab, b); ch_pump(&ba, a); if (now % 10 == 0) { bl_poll(a); bl_poll(b); } } }
    assert(b_got == 5);

    /* Перезагрузка A: новый boot_id, seq снова с 1 — B должен принять, а не счесть дубликатом. */
    ca.boot_id = 0x3333;
    bl_init(a, &ca);
    for (uint32_t i = 5; i < 10; i++) { build_msg(buf, i, 20); bl_send(a, 0x20, buf, 20, true);
        for (int k = 0; k < 200; k++) { now++; ch_pump(&ab, b); ch_pump(&ba, a); if (now % 10 == 0) { bl_poll(a); bl_poll(b); } } }
    assert(b_got == 10);
    assert(restarts_seen >= 1);

    /* Обрыв линии: link down у обоих, затем восстановление. */
    p_cut = 1.0;
    for (int k = 0; k < 5000; k++) { now++; ch_pump(&ab, b); ch_pump(&ba, a); if (now % 10 == 0) { bl_poll(a); bl_poll(b); } }
    assert(!bl_link_up(a) && !bl_link_up(b));
    p_cut = 0;
    for (int k = 0; k < 3000; k++) { now++; ch_pump(&ab, b); ch_pump(&ba, a); if (now % 10 == 0) { bl_poll(a); bl_poll(b); } }
    assert(bl_link_up(a) && bl_link_up(b));
    assert(bl_peer_role(a) == BL_ROLE_TX && bl_peer_role(b) == BL_ROLE_CORE);
    printf("%-22s restarts_seen=%u link_downs=%u/%u ok=yes\n", "restart+cut", restarts_seen,
           bl_stats(a)->link_downs, bl_stats(b)->link_downs);
    return 0;
}

/* Петля: свои кадры возвращаются на свой же RX (обесточенный сосед отражает сигнал).
 * Кадры должны отбрасываться: ни link up, ни ложного подтверждения доставки. */
static void wr_loop(const uint8_t *d, size_t n, void *ctx) { (void)ctx; ch_write(&ab, d, n); }
static int loopback_scenario(void)
{
    static _Alignas(8) uint8_t ma[16384];
    bl_link_t *a = (bl_link_t *)ma;
    memset(&ab, 0, sizeof(ab)); busy_until[0] = busy_until[1] = 0;
    p_drop = p_flip = p_cut = 0;
    now = 0;
    bl_config_t ca = { BL_ROLE_CORE, 0x4444, t_now, wr_loop, NULL, on_event, NULL };
    bl_init(a, &ca);
    uint8_t buf[64];
    build_msg(buf, 1, 40);
    assert(bl_send(a, 0x20, buf, 40, true) == 0);
    for (int k = 0; k < 6000; k++) { now++; ch_pump(&ab, a); if (now % 10 == 0) bl_poll(a); }
    const bl_stats_t *s = bl_stats(a);
    int ok = !bl_link_up(a) && s->loopback > 0 && s->send_failed == 1 && s->frames_rx_ok == 0;
    printf("%-22s loop=%u failed=%u link_up=%d ok=%s\n", "loopback", s->loopback, s->send_failed,
           bl_link_up(a), ok ? "yes" : "NO");
    return ok ? 0 : 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* COBS: круговое преобразование на случайных данных, включая нули и 0xFF-блоки */
    static uint8_t in[2000], enc[2100], dec[2000];
    for (int it = 0; it < 20000; it++) {
        size_t n = rnd() % 1000;
        for (size_t i = 0; i < n; i++) in[i] = (rnd() % 4 == 0) ? 0 : (uint8_t)rnd();
        if (it % 7 == 0) memset(in, 0x55, n);
        size_t e = bl_cobs_encode(in, n, enc);
        for (size_t i = 0; i < e; i++) assert(enc[i] != 0);
        int d = bl_cobs_decode(enc, e, dec, sizeof(dec));
        assert(d == (int)n && memcmp(in, dec, n) == 0);
    }
    assert(bl_crc16((const uint8_t *)"123456789", 9) == 0x29B1);   /* CRC-16/CCITT-FALSE */
    puts("cobs/crc ok");

    int f = 0;
    f |= scenario("clean", 0, 0, 0, 2000);
    f |= scenario("noisy 1e-4", 1e-4, 1e-4, 0, 2000);
    f |= scenario("noisy 1e-3", 1e-3, 1e-3, 1e-4, 1000);
    f |= restart_scenario();
    f |= loopback_scenario();
    puts(f ? "FAILED" : "ALL PASSED");
    return f;
}
