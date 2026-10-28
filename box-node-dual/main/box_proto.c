#include "box_proto.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

static box_pkt_type_t type_from_str(const char *s)
{
    if (strcmp(s, "SID") == 0) return BOX_PKT_SID;
    if (strcmp(s, "TRM") == 0) return BOX_PKT_TRM;
    if (strcmp(s, "ACB") == 0) return BOX_PKT_ACB;
    if (strcmp(s, "CBB") == 0) return BOX_PKT_CBB;
    if (strcmp(s, "CDB") == 0) return BOX_PKT_CDB;
    if (strcmp(s, "BOX") == 0) return BOX_PKT_BOX;
    return BOX_PKT_UNKNOWN;
}

const char *box_pkt_type_name(box_pkt_type_t t)
{
    switch (t) {
    case BOX_PKT_SID: return "SID";
    case BOX_PKT_TRM: return "TRM";
    case BOX_PKT_ACB: return "ACB";
    case BOX_PKT_CBB: return "CBB";
    case BOX_PKT_CDB: return "CDB";
    case BOX_PKT_BOX: return "BOX";
    default:          return "UNKNOWN";
    }
}

bool box_is_uuid(const char *s)
{
    if (s == NULL || strlen(s) != BOX_UUID_STR_LEN) return false;
    for (int i = 0; i < BOX_UUID_STR_LEN; i++) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!isxdigit((unsigned char)c)) {
            return false;
        }
    }
    return true;
}

int box_parse(const uint8_t *data, size_t len, box_pkt_t *out)
{
    memset(out, 0, sizeof(*out));
    out->type = BOX_PKT_UNKNOWN;

    if (data == NULL || len == 0 || len > BOX_MAX_PACKET) return -1;
    memcpy(out->buf, data, len);
    out->buf[len] = '\0';
    if (memchr(out->buf, '\0', len) != NULL) return -2;   /* встроенный NUL — мусор */

    /* Разбиваем по '\n', как Java String.split("\n"): хвостовые пустые строки отбрасываются. */
    char *p = out->buf;
    while (out->n_lines < BOX_MAX_LINES) {
        out->lines[out->n_lines++] = p;
        char *nl = strchr(p, '\n');
        if (nl == NULL) break;
        *nl = '\0';
        p = nl + 1;
        if (*p == '\0') break;
    }
    if (strchr(p, '\n') != NULL) return -3;               /* больше строк, чем BOX_MAX_LINES */
    while (out->n_lines > 0 && out->lines[out->n_lines - 1][0] == '\0') out->n_lines--;
    /* Android шлёт CRLF не будет, но на всякий случай убираем '\r'. */
    for (int i = 0; i < out->n_lines; i++) {
        size_t l = strlen(out->lines[i]);
        if (l > 0 && out->lines[i][l - 1] == '\r') out->lines[i][l - 1] = '\0';
    }

    if (out->n_lines < 2) return -4;
    if (strcmp(out->lines[0], BOX_VERSION) != 0) return -5;

    box_pkt_type_t t = type_from_str(out->lines[1]);
    /* SID/TRM без id: Android шлёт "V1\nSID\n" и "V1\nTRM\n" (7 байт), пока
     * LocalCache.currentSessionID == "" (так он инициализирован до первого
     * needToRefreshBoxQueue(…, true)). Принимаем как сессию без id. */
    static char empty_id[1];
    if (out->n_lines == 2 && (t == BOX_PKT_SID || t == BOX_PKT_TRM)) {
        empty_id[0] = '\0';
        out->lines[out->n_lines++] = empty_id;
    }
    if (out->n_lines < 3) return -4;
    switch (t) {
    case BOX_PKT_SID:
    case BOX_PKT_TRM:
        if (out->n_lines != 3) return -6;
        break;
    case BOX_PKT_ACB:
        for (int i = 2; i < out->n_lines; i++) if (out->lines[i][0] == '\0') return -7;
        break;
    case BOX_PKT_CBB:
    case BOX_PKT_CDB:
        if (out->n_lines < 4) return -8;
        for (int i = 2; i < out->n_lines; i++) if (out->lines[i][0] == '\0') return -9;
        break;
    case BOX_PKT_BOX:
        if (!box_is_uuid(out->lines[2])) return -10;
        if (out->n_lines < 3 + BOX_FIELDS_PER_MSG) return -11;
        if ((out->n_lines - 3) % BOX_FIELDS_PER_MSG != 0) return -12;
        break;
    default:
        return -13;
    }
    out->type = t;
    return 0;
}

const char *box_pkt_primary_id(const box_pkt_t *p)
{
    if (p->type == BOX_PKT_UNKNOWN || p->type == BOX_PKT_ACB) return NULL;
    return p->lines[2];
}

int box_pkt_id_count(const box_pkt_t *p)
{
    if (p->type == BOX_PKT_ACB) return p->n_lines - 2;
    if (p->type == BOX_PKT_CBB || p->type == BOX_PKT_CDB) return p->n_lines - 3;
    return 0;
}

const char *box_pkt_id_at(const box_pkt_t *p, int i)
{
    if (i < 0 || i >= box_pkt_id_count(p)) return NULL;
    return p->type == BOX_PKT_ACB ? p->lines[2 + i] : p->lines[3 + i];
}

int box_pkt_msg_count(const box_pkt_t *p)
{
    if (p->type != BOX_PKT_BOX) return 0;
    return (p->n_lines - 3) / BOX_FIELDS_PER_MSG;
}

int box_pkt_msg_at(const box_pkt_t *p, int i, box_msg_t *m)
{
    if (i < 0 || i >= box_pkt_msg_count(p)) return -1;
    char *const *f = &p->lines[3 + i * BOX_FIELDS_PER_MSG];
    m->message_id = f[0];
    m->ttl        = f[1];
    m->unit_count = 0;
    for (const char *c = f[2]; *c; c++) {
        if (!isdigit((unsigned char)*c)) { m->unit_count = -1; break; }
        m->unit_count = m->unit_count * 10 + (*c - '0');
        if (m->unit_count > 1000000) { m->unit_count = -1; break; }
    }
    m->receiver = f[3];
    m->type     = f[4];
    m->bytes    = f[5];
    return 0;
}

/* --- сборка --- */

typedef struct { char *p; size_t cap; size_t len; bool overflow; } sb_t;

static void sb_add(sb_t *b, const char *s)
{
    size_t l = strlen(s);
    if (b->overflow || b->len + l + 1 > b->cap) { b->overflow = true; return; }
    memcpy(b->p + b->len, s, l);
    b->len += l;
    b->p[b->len] = '\0';
}

static int sb_done(sb_t *b)
{
    if (b->overflow || b->len > BOX_MAX_PACKET) return -1;
    return (int)b->len;
}

int box_build_session(char *out, size_t cap, box_pkt_type_t t, const char *sid)
{
    if (t != BOX_PKT_SID && t != BOX_PKT_TRM) return -1;
    sb_t b = { out, cap, 0, false };
    if (cap) out[0] = '\0';
    sb_add(&b, BOX_VERSION "\n");
    sb_add(&b, box_pkt_type_name(t));
    sb_add(&b, "\n");
    sb_add(&b, sid);
    return sb_done(&b);
}

int box_build_acb(char *out, size_t cap, const char *const *ids, int n)
{
    if (n <= 0) return -1;
    sb_t b = { out, cap, 0, false };
    if (cap) out[0] = '\0';
    sb_add(&b, BOX_VERSION "\nACB");
    for (int i = 0; i < n; i++) { sb_add(&b, "\n"); sb_add(&b, ids[i]); }
    return sb_done(&b);
}

int box_build_callback(char *out, size_t cap, box_pkt_type_t t,
                       const char *cb_id, const char *const *box_ids, int n)
{
    if ((t != BOX_PKT_CBB && t != BOX_PKT_CDB) || n <= 0) return -1;
    sb_t b = { out, cap, 0, false };
    if (cap) out[0] = '\0';
    sb_add(&b, BOX_VERSION "\n");
    sb_add(&b, box_pkt_type_name(t));
    sb_add(&b, "\n");
    sb_add(&b, cb_id);
    for (int i = 0; i < n; i++) { sb_add(&b, "\n"); sb_add(&b, box_ids[i]); }
    return sb_done(&b);
}

int box_build_box(char *out, size_t cap, const char *box_id, const box_msg_t *msgs, int n)
{
    if (!box_is_uuid(box_id) || n <= 0) return -1;
    sb_t b = { out, cap, 0, false };
    char num[16];
    if (cap) out[0] = '\0';
    sb_add(&b, BOX_VERSION "\nBOX\n");
    sb_add(&b, box_id);
    sb_add(&b, "\n");
    for (int i = 0; i < n; i++) {
        snprintf(num, sizeof(num), "%d", msgs[i].unit_count);
        const char *f[6] = { msgs[i].message_id, msgs[i].ttl, num,
                             msgs[i].receiver, msgs[i].type, msgs[i].bytes };
        for (int k = 0; k < 6; k++) {
            /* ttl, receiver, type могут быть пустыми (Java split сохраняет внутренние пустые
             * строки); id, unit_count и последнее поле bytes — нет. */
            bool may_be_empty = (k == 1 || k == 3 || k == 4);
            if (f[k] == NULL) { if (!may_be_empty) return -1; f[k] = ""; }
            if ((f[k][0] == '\0' && !may_be_empty) || strchr(f[k], '\n')) return -1;
            sb_add(&b, f[k]);
            sb_add(&b, "\n");          /* как StringBuilder в Android: '\n' после каждого поля */
        }
    }
    return sb_done(&b);
}

/* --- UUID --- */

void box_uuid_v4(char out[BOX_UUID_STR_LEN + 1], uint32_t (*rand32)(void))
{
    uint8_t r[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t v = rand32();
        r[i] = (uint8_t)v; r[i + 1] = (uint8_t)(v >> 8); r[i + 2] = (uint8_t)(v >> 16); r[i + 3] = (uint8_t)(v >> 24);
    }
    r[6] = (uint8_t)((r[6] & 0x0f) | 0x40);
    r[8] = (uint8_t)((r[8] & 0x3f) | 0x80);
    snprintf(out, BOX_UUID_STR_LEN + 1,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
             r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

void box_uuid_with_suffix(char out[BOX_UUID_STR_LEN + 1], const char *suffix12, uint32_t (*rand32)(void))
{
    box_uuid_v4(out, rand32);
    memcpy(out + BOX_UUID_STR_LEN - BOX_ID_SUFFIX_LEN, suffix12, BOX_ID_SUFFIX_LEN);
}

const char *box_uuid_suffix(const char *uuid)
{
    size_t l = uuid ? strlen(uuid) : 0;
    if (l < BOX_ID_SUFFIX_LEN) return NULL;
    return uuid + l - BOX_ID_SUFFIX_LEN;
}
