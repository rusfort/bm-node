/* Хост-тест box_adv.c: scan response, HELLO и выбор подключений CORE (ASan/UBSan). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "box_adv.h"

/* Реклама Android/TX: flags + полный список 128-битных UUID с SERVICE_UUID (21 байт). */
static size_t adv_box(uint8_t *o)
{
    o[0] = 2; o[1] = 0x01; o[2] = 0x06;
    o[3] = 17; o[4] = 0x07;
    memcpy(o + 5, bx_service_uuid_le, 16);
    return 21;
}

static void t_scan_rsp(void)
{
    uint8_t r[31];
    assert(bx_build_scan_rsp(r, BX_F_NODE | BX_F_SERVING, "AABBCCDDEEFF") == BX_SCAN_RSP_LEN);
    /* Ожидаемые байты: len=25, type=0x21, UUID LE, ver, flags, id */
    static const uint8_t exp_tail[8] = { 0x01, 0x03, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    assert(r[0] == 25 && r[1] == 0x21);
    assert(memcmp(r + 2, bx_service_uuid_le, 16) == 0);
    assert(memcmp(r + 18, exp_tail, 8) == 0);
    bx_info_t in;
    assert(bx_parse_ad(r, BX_SCAN_RSP_LEN, &in));
    assert(in.ver == 1 && in.flags == 3 && strcmp(in.id, "aabbccddeeff") == 0);
    assert(bx_build_scan_rsp(r, 0, "aabbccddeefg") == 0);       /* не hex */
    /* в составе других AD-структур и после обрезки */
    uint8_t big[31];
    big[0] = 3; big[1] = 0xff; big[2] = 0x12; big[3] = 0x34;   /* manufacturer data */
    memcpy(big + 4, r, 26);
    assert(bx_parse_ad(big, 30, &in) && in.flags == 3);
    assert(!bx_parse_ad(big, 29, &in));                         /* обрезано */
    r[18] = 2;                                                   /* ver 2 — не наш формат */
    assert(!bx_parse_ad(r, 26, &in));
    uint8_t a[31];
    size_t n = adv_box(a);
    assert(bx_ad_has_service_uuid(a, n) && !bx_parse_ad(a, n, &in));
    a[5] ^= 1;
    assert(!bx_ad_has_service_uuid(a, n));
    printf("scan response          ok\n");
}

static void t_hello(void)
{
    char h[BX_HELLO_MAX], id[13];
    uint8_t fl = 0xee;
    size_t n = bx_build_hello(h, sizeof(h), "AABBCCDDEEFF", BX_F_NODE | BX_F_SERVING);
    assert(n == 22 && strcmp(h, "V1\nHLO\naabbccddeeff\n03") == 0);
    assert(bx_parse_hello((const uint8_t *)h, n, id, &fl) && strcmp(id, "aabbccddeeff") == 0 && fl == 3);
    const char *noflags = "V1\nHLO\n72389525976d";
    assert(bx_parse_hello((const uint8_t *)noflags, strlen(noflags), id, &fl) && fl == 0 &&
           strcmp(id, "72389525976d") == 0);
    const char *withtime = "V1\nHLO\n72389525976d\n04\n2026-10-26 18:40:12";
    assert(bx_parse_hello((const uint8_t *)withtime, strlen(withtime), id, &fl) && fl == 4);
    const char *bad[] = { "V1\nHLO\n72389525976", "V1\nHLO\n72389525976dX\n00", "V1\nSID\n72389525976d",
                          "V1\nHLO\n7238952597zz\n00", "" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(!bx_parse_hello((const uint8_t *)bad[i], strlen(bad[i]), id, &fl));
    /* SID/TRM/BOX узла не похожи на HELLO */
    const char *sid = "V1\nSID\n00000000-0000-4000-8000-aabbccddeeff";
    assert(!bx_parse_hello((const uint8_t *)sid, strlen(sid), id, &fl));
    assert(bx_build_hello(h, 10, "aabbccddeeff", 0) == 0);
    printf("hello                  ok\n");
}

static const uint8_t A1[6] = { 1, 1, 1, 1, 1, 0x41 }, A2[6] = { 2, 2, 2, 2, 2, 0x42 },
                     TXA[6] = { 0xb6, 0xb2, 0x6b, 0x3b, 0x00, 0x10 }, ON[6] = { 3, 3, 3, 3, 3, 0x43 },
                     OLD[6] = { 4, 4, 4, 4, 4, 0x44 }, OLD2[6] = { 5, 5, 5, 5, 5, 0x45 };

static void t_decide(void)
{
    bx_table_t t;
    bx_table_init(&t, "AABBCCDDEEFF");
    uint8_t adv[31], rsp[31];
    size_t na = adv_box(adv);
    uint32_t now = 1000;

    /* Новый телефон в фазе приёма: не подключаемся. */
    bx_nb_t *n = bx_table_report(&t, A1, false, adv, na, now);
    assert(n && bx_decide(&t, n, now) == BX_WAIT);               /* scan response ещё нет */
    bx_build_scan_rsp(rsp, 0, "72389525976d");
    n = bx_table_report(&t, A1, true, rsp, 26, now + 5);
    assert(bx_decide(&t, n, now + 5) == BX_SKIP_NOT_SERVING);
    assert(bx_is_new_phone(n, now + 5));
    assert(bx_decide(&t, n, now + 2000) == BX_SKIP_NOT_SERVING);
    /* перешёл в раздачу */
    bx_build_scan_rsp(rsp, BX_F_SERVING | BX_F_HAS_OUT, "72389525976d");
    n = bx_table_report(&t, A1, true, rsp, 26, now + 2100);
    assert(bx_decide(&t, n, now + 2100) == BX_CONNECT);
    /* scan response пропали: сначала ждём, через 10 с — по-старому */
    assert(bx_decide(&t, n, now + 2100 + BX_INFO_FRESH_MS + 1) == BX_WAIT);
    assert(bx_decide(&t, n, now + 2100 + BX_STALE_FALLBACK_MS + 1) == BX_CONNECT);

    /* Своя TX объявляет NODE + наш ID. */
    bx_build_scan_rsp(rsp, BX_F_NODE | BX_F_SERVING, "aabbccddeeff");
    n = bx_table_report(&t, TXA, true, rsp, 26, now);
    assert(n && bx_decide(&t, n, now) == BX_SKIP_SELF);
    assert(bx_decide(&t, n, now + 60000) == BX_SKIP_SELF);       /* навсегда */
    assert(!bx_is_new_phone(n, now));

    /* Чужой узел: раздаёт всегда. */
    bx_build_scan_rsp(rsp, BX_F_NODE, "001122334455");
    n = bx_table_report(&t, ON, true, rsp, 26, now);
    assert(bx_decide(&t, n, now) == BX_CONNECT && !bx_is_new_phone(n, now));

    /* Старая версия: реклама + пустой scan response. */
    n = bx_table_report(&t, OLD, false, adv, na, now);
    assert(bx_decide(&t, n, now) == BX_WAIT);
    n = bx_table_report(&t, OLD, true, NULL, 0, now + 20);
    assert(bx_decide(&t, n, now + 20) == BX_CONNECT && !bx_is_new_phone(n, now + 20));

    /* Старая версия без scan response вообще: ждём BX_LEGACY_WAIT_MS. */
    n = bx_table_report(&t, OLD2, false, adv, na, now);
    assert(bx_decide(&t, n, now + BX_LEGACY_WAIT_MS - 1) == BX_WAIT);
    assert(bx_decide(&t, n, now + BX_LEGACY_WAIT_MS) == BX_CONNECT);

    /* Не Box: не храним. */
    uint8_t other[8] = { 2, 0x01, 0x06, 4, 0xff, 1, 2, 3 };
    assert(bx_table_report(&t, A2, false, other, 8, now) == NULL);
    assert(bx_table_find(&t, A2) == NULL && bx_decide(&t, NULL, now) == BX_SKIP_UNKNOWN);
    printf("decide                 ok\n");
}

static void t_table_full(void)
{
    bx_table_t t;
    bx_table_init(&t, "aabbccddeeff");
    uint8_t adv[31];
    size_t na = adv_box(adv);
    for (int i = 0; i < 100; i++) {
        uint8_t a[6] = { (uint8_t)i, 0, 0, 0, 0, 0x50 };
        assert(bx_table_report(&t, a, false, adv, na, (uint32_t)(i * 10)) != NULL);
    }
    int used = 0;
    for (int i = 0; i < BX_NB_MAX; i++) used += t.nb[i].used;
    assert(used == BX_NB_MAX);
    uint8_t last[6] = { 99, 0, 0, 0, 0, 0x50 };
    assert(bx_table_find(&t, last));
    printf("table eviction         ok\n");
}

static void t_fuzz(void)
{
    srand(7);
    uint8_t b[64];
    bx_info_t in;
    char id[13];
    uint8_t fl;
    int hits = 0;
    for (int i = 0; i < 300000; i++) {
        size_t n = (size_t)(rand() % 64);
        for (size_t k = 0; k < n; k++) b[k] = (uint8_t)rand();
        if (i % 3 == 0 && n >= 26) { bx_build_scan_rsp(b, (uint8_t)rand(), "0123456789ab"); }
        hits += bx_parse_ad(b, n, &in);
        bx_ad_has_service_uuid(b, n);
        bx_parse_hello(b, n, id, &fl);
    }
    assert(hits > 0);
    printf("fuzz 300000            ok (%d parsed)\n", hits);
}

int main(void)
{
    t_scan_rsp();
    t_hello();
    t_decide();
    t_table_full();
    t_fuzz();
    printf("ALL ADV TESTS PASSED\n");
    return 0;
}
