#include "box_adv.h"

#include <string.h>

const uint8_t bx_service_uuid_le[16] = {
    0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, 0x12, 0x90,
    0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12,
};

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool id_to_bytes(const char *id12, uint8_t out[6])
{
    if (!id12) return false;
    for (int i = 0; i < 6; i++) {
        int h = hexval(id12[2 * i]), l = hexval(id12[2 * i + 1]);
        if (h < 0 || l < 0) return false;
        out[i] = (uint8_t)(h << 4 | l);
    }
    return true;
}

static void bytes_to_id(const uint8_t b[6], char out[13])
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) { out[2 * i] = hx[b[i] >> 4]; out[2 * i + 1] = hx[b[i] & 15]; }
    out[12] = '\0';
}

size_t bx_build_scan_rsp(uint8_t *out, uint8_t flags, const char *id12)
{
    uint8_t id[6];
    if (!id_to_bytes(id12, id)) return 0;
    out[0] = BX_SCAN_RSP_LEN - 1;
    out[1] = 0x21;
    memcpy(out + 2, bx_service_uuid_le, 16);
    out[18] = BX_VER;
    out[19] = flags;
    memcpy(out + 20, id, 6);
    return BX_SCAN_RSP_LEN;
}

bool bx_parse_ad(const uint8_t *ad, size_t len, bx_info_t *out)
{
    size_t i = 0;
    while (ad && i < len) {
        uint8_t l = ad[i];
        if (l == 0) break;
        if (i + 1 + l > len) break;                 /* обрезанная структура */
        const uint8_t *s = ad + i + 1;              /* s[0] — тип */
        if (s[0] == 0x21 && l >= 1 + 16 + 8 && memcmp(s + 1, bx_service_uuid_le, 16) == 0 &&
            s[17] == BX_VER) {
            out->ver = s[17];
            out->flags = s[18];
            bytes_to_id(s + 19, out->id);
            return true;
        }
        i += 1 + (size_t)l;
    }
    return false;
}

bool bx_ad_has_service_uuid(const uint8_t *ad, size_t len)
{
    size_t i = 0;
    while (ad && i < len) {
        uint8_t l = ad[i];
        if (l == 0) break;
        if (i + 1 + l > len) break;
        const uint8_t *s = ad + i + 1;
        if (s[0] == 0x06 || s[0] == 0x07)
            for (size_t k = 1; k + 16 <= l; k += 16)
                if (memcmp(s + k, bx_service_uuid_le, 16) == 0) return true;
        i += 1 + (size_t)l;
    }
    return false;
}

size_t bx_build_hello(char *out, size_t cap, const char *id12, uint8_t flags)
{
    uint8_t tmp[6];
    if (!id_to_bytes(id12, tmp) || cap < 25) return 0;
    char id[13];
    bytes_to_id(tmp, id);                          /* нижний регистр */
    static const char hx[] = "0123456789abcdef";
    memcpy(out, "V1\nHLO\n", 7);
    memcpy(out + 7, id, 12);
    out[19] = '\n';
    out[20] = hx[flags >> 4];
    out[21] = hx[flags & 15];
    out[22] = '\0';
    return 22;
}

bool bx_parse_hello(const uint8_t *d, size_t len, char id_out[13], uint8_t *flags_out)
{
    if (!d || len < 19 || memcmp(d, "V1\nHLO\n", 7) != 0) return false;
    uint8_t b[6];
    if (!id_to_bytes((const char *)d + 7, b)) return false;
    if (len > 19 && d[19] != '\n') return false;    /* ID ровно 12 символов */
    uint8_t fl = 0;
    if (len >= 22) {
        int h = hexval((char)d[20]), l = hexval((char)d[21]);
        if (h >= 0 && l >= 0) fl = (uint8_t)(h << 4 | l);
    }
    if (id_out) bytes_to_id(b, id_out);
    if (flags_out) *flags_out = fl;
    return true;
}

/* ---------- таблица соседей ---------- */

void bx_table_init(bx_table_t *t, const char *own_id12)
{
    memset(t, 0, sizeof(*t));
    uint8_t b[6];
    if (id_to_bytes(own_id12, b)) bytes_to_id(b, t->own_id);
}

bx_nb_t *bx_table_find(bx_table_t *t, const uint8_t addr[6])
{
    for (int i = 0; i < BX_NB_MAX; i++)
        if (t->nb[i].used && memcmp(t->nb[i].addr, addr, 6) == 0) return &t->nb[i];
    return NULL;
}

bx_nb_t *bx_table_report(bx_table_t *t, const uint8_t addr[6], bool is_scan_rsp,
                         const uint8_t *ad, size_t len, uint32_t now)
{
    bx_info_t info;
    bool has_info = bx_parse_ad(ad, len, &info);
    bool has_uuid = !is_scan_rsp && bx_ad_has_service_uuid(ad, len);

    bx_nb_t *n = bx_table_find(t, addr);
    if (!n) {
        if (!has_info && !has_uuid) return NULL;   /* чужие устройства не храним */
        int victim = -1;
        for (int i = 0; i < BX_NB_MAX; i++) {
            if (!t->nb[i].used || now - t->nb[i].last_seen > BX_FORGET_MS) { victim = i; break; }
            if (victim < 0 || t->nb[i].last_seen < t->nb[victim].last_seen) victim = i;
        }
        n = &t->nb[victim];
        memset(n, 0, sizeof(*n));
        n->used = true;
        memcpy(n->addr, addr, 6);
    }
    n->last_seen = now;
    if (has_uuid && !n->box_adv) { n->box_adv = true; n->first_box_adv = now; }
    if (is_scan_rsp) n->rsp_seen = true;
    if (has_info) { n->info = info; n->info_valid = true; n->info_at = now; }
    return n;
}

bool bx_is_new_phone(const bx_nb_t *n, uint32_t now)
{
    return n && n->info_valid && now - n->info_at <= BX_INFO_FRESH_MS && !(n->info.flags & BX_F_NODE);
}

bx_decision_t bx_decide(const bx_table_t *t, const bx_nb_t *n, uint32_t now)
{
    if (!n) return BX_SKIP_UNKNOWN;
    if (n->info_valid) {
        if ((n->info.flags & BX_F_NODE) && strcmp(n->info.id, t->own_id) == 0) return BX_SKIP_SELF;
        if (now - n->info_at <= BX_INFO_FRESH_MS) {
            if (n->info.flags & BX_F_NODE) return BX_CONNECT;      /* чужой узел раздаёт всегда */
            return (n->info.flags & BX_F_SERVING) ? BX_CONNECT : BX_SKIP_NOT_SERVING;
        }
        /* Данные устарели (scan response теряются): подождать, а через
         * BX_STALE_FALLBACK_MS подключаться по-старому, чтобы не застрять. */
        return now - n->info_at > BX_STALE_FALLBACK_MS ? BX_CONNECT : BX_WAIT;
    }
    if (!n->box_adv) return BX_WAIT;          /* пока видели только scan response без данных */
    if (n->rsp_seen) return BX_CONNECT;       /* scan response есть, данных нет — старая версия */
    if (now - n->first_box_adv >= BX_LEGACY_WAIT_MS) return BX_CONNECT;
    return BX_WAIT;
}

const char *bx_decision_name(bx_decision_t d)
{
    switch (d) {
    case BX_WAIT: return "wait";
    case BX_CONNECT: return "connect";
    case BX_SKIP_NOT_SERVING: return "not serving";
    case BX_SKIP_SELF: return "own TX";
    default: return "unknown";
    }
}
