/* Юнит-тесты и фаззинг box_proto.c (ASan/UBSan). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "box_proto.h"

static uint32_t r32(void) { return (uint32_t)rand() ^ ((uint32_t)rand() << 16); }
static box_pkt_t p;

static int parse(const char *s) { return box_parse((const uint8_t *)s, strlen(s), &p); }

int main(void)
{
    char out[600], id[37];
    srand(1);

    assert(parse("V1\nSID\n11111111-2222-4333-8444-555555555555") == 0 && p.type == BOX_PKT_SID);
    assert(parse("V1\nTRM\nabc\n") == 0 && p.type == BOX_PKT_TRM);
    assert(parse("V2\nSID\nx") != 0);
    assert(parse("V1\nXYZ\nx") != 0);
    assert(parse("V1\nSID") != 0);
    assert(parse("V1\nACB\na\nb\n") == 0 && box_pkt_id_count(&p) == 2 && !strcmp(box_pkt_id_at(&p, 1), "b"));
    assert(parse("V1\nACB\na\n\nb") != 0);
    assert(parse("V1\nCBB\ncb\nb1\nb2") == 0 && box_pkt_id_count(&p) == 2 && !strcmp(box_pkt_primary_id(&p), "cb"));
    assert(parse("V1\nCDB\ncb") != 0);

    box_uuid_v4(id, r32);
    assert(box_is_uuid(id) && id[14] == '4');
    box_msg_t m[2] = { { "m1", "2026-12-31 23:59:59", 0, "R", "text", "hello" },
                       { "m2", "2026-12-31 23:59:59", 7, "R2", "file", "x" } };
    int n = box_build_box(out, sizeof(out), id, m, 2);
    assert(n > 0);
    assert(strncmp(out + 7, id, 36) == 0 && out[BOX_HEADER_LEN - 1] == '\n');
    assert(box_parse((uint8_t *)out, (size_t)n, &p) == 0 && box_pkt_msg_count(&p) == 2);
    box_msg_t g; box_pkt_msg_at(&p, 1, &g);
    assert(!strcmp(g.message_id, "m2") && g.unit_count == 7 && !strcmp(g.bytes, "x"));
    assert(parse("V1\nBOX\n11111111-2222-4333-8444-555555555555\na\nb\nc\nd\ne") != 0); /* 5 полей */

    char sfx[37];
    box_uuid_with_suffix(sfx, "aabbccddeeff", r32);
    assert(box_is_uuid(sfx) && !strcmp(box_uuid_suffix(sfx), "aabbccddeeff"));

    const char *ids[1] = { sfx };
    n = box_build_callback(out, sizeof(out), BOX_PKT_CBB, sfx, ids, 1);
    assert(n == 3 + 4 + 37 + 36 && parse(out) == 0);
    assert(box_build_session(out, 10, BOX_PKT_SID, sfx) < 0);         /* не помещается */

    /* слишком большой пакет */
    static char big[700]; memset(big, 'a', sizeof(big) - 1);
    assert(box_parse((uint8_t *)big, 600, &p) != 0);

    /* фаззинг: случайные байты и мутации валидного BOX */
    uint8_t buf[512];
    for (int it = 0; it < 200000; it++) {
        size_t len = (size_t)(rand() % 512) + 1;
        if (it & 1) {
            n = box_build_box((char *)buf, sizeof(buf), id, m, 2);
            len = (size_t)n;
            for (int k = 0; k < 4; k++) buf[rand() % n] = (uint8_t)(rand() % 4 == 0 ? '\n' : rand());
        } else {
            for (size_t k = 0; k < len; k++) buf[k] = (uint8_t)(rand() % 3 ? '\n' + (rand() % 80) : '\n');
        }
        if (box_parse(buf, len, &p) == 0) {
            for (int k = 0; k < box_pkt_id_count(&p); k++) assert(box_pkt_id_at(&p, k));
            for (int k = 0; k < box_pkt_msg_count(&p); k++) { box_pkt_msg_at(&p, k, &g); assert(g.bytes); }
        }
    }
    puts("test_proto: all passed");
    return 0;
}
