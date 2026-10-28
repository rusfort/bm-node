#include "store.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------- формат ---------- */

#define SEC_MAGIC   0x31535842u   /* "BXS1" */
#define REC_MAGIC   0xB7A1u
#define SEC_HDR     16u
#define REC_HDR     8u
#define REC_FIXED   10u           /* got_at, src, unit_count */
#define REC_MAX     (REC_HDR + REC_FIXED + 5u * (2u + ST_MAX_FIELD) + 3u)

enum { S_FREE_CLEAN = 0, S_FREE_DIRTY, S_USED };

typedef struct {
    uint32_t seq;
    uint16_t used;        /* байт занято от начала сектора (включая заголовок) */
    uint16_t live;        /* байт живых записей */
    uint8_t  state;
    uint8_t  sealed;      /* дописывать нельзя (битая запись или полон) */
    uint8_t  pad[2];
} sec_t;

static struct {
    st_env_t   env;
    bool       ok;
    st_ent_t  *ent;
    uint32_t   cap, n, hw;   /* hw: все занятые записи индекса лежат ниже */
    uint64_t  *del;          /* кольцо удалённых */
    uint32_t   del_pos;
    sec_t     *sec;
    uint32_t   nsec, tail, max_seq;
    bool       has_tail;
    uint32_t   bytes_live;
    st_stats_t st;
    uint8_t    rec[REC_MAX];
} S;

static void slog(const char *fmt, ...)
{
    if (!S.env.log) return;
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    S.env.log(b);
}

/* ---------- хеши ---------- */

static uint64_t mix64(uint64_t z)
{
    z += 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint64_t fnv64(const char *s, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 0x100000001b3ull; }
    return h;
}

uint64_t st_hash64(const char *s)
{
    uint64_t h = mix64(fnv64(s, strlen(s)));
    return h ? h : 1;
}

uint32_t st_hash32(const char *s)
{
    uint64_t h = st_hash64(s);
    uint32_t r = (uint32_t)(h ^ (h >> 32));
    return r ? r : 1;
}

void st_box_id_for(const char *message_id, char out[BOX_UUID_STR_LEN + 1])
{
    uint64_t a = mix64(st_hash64(message_id) ^ 0x426f784e6f646531ull);   /* "BoxNode1" */
    uint64_t b = mix64(a ^ 0x5452414e53495431ull);
    uint8_t u[16];
    for (int i = 0; i < 8; i++) { u[i] = (uint8_t)(a >> (8 * i)); u[8 + i] = (uint8_t)(b >> (8 * i)); }
    u[6] = (uint8_t)((u[6] & 0x0f) | 0x40);
    u[8] = (uint8_t)((u[8] & 0x3f) | 0x80);
    static const char hx[] = "0123456789abcdef";
    int p = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[p++] = '-';
        out[p++] = hx[u[i] >> 4];
        out[p++] = hx[u[i] & 15];
    }
    out[p] = '\0';
}

static uint64_t box_hash_for_msgid(const char *message_id)
{
    char b[BOX_UUID_STR_LEN + 1];
    st_box_id_for(message_id, b);
    return st_hash64(b);
}

static uint32_t crc32(const uint8_t *d, size_t n)
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---------- flash ---------- */

static int f_read(uint32_t off, void *b, size_t n)
{
    int rc = S.env.read(S.env.ctx, off, b, n);
    if (rc) S.st.io_errors++;
    return rc;
}

static int f_write(uint32_t off, const void *b, size_t n)
{
    int rc = S.env.write(S.env.ctx, off, b, n);
    if (rc) S.st.io_errors++;
    return rc;
}

static int f_erase(uint32_t s)
{
    int rc = S.env.erase(S.env.ctx, s);
    if (rc) S.st.io_errors++;
    return rc;
}

static uint32_t now_s(void) { return S.env.now_s(); }

static bool expired_at(uint32_t got_at, uint32_t now)
{
    return now > got_at && now - got_at > ST_MAX_AGE_S;
}

static uint32_t capacity_bytes(void)
{
    uint32_t usable = S.nsec > ST_RESERVE_SECTORS ? S.nsec - ST_RESERVE_SECTORS : 1;
    return usable * (ST_SECTOR - SEC_HDR);
}

/* ---------- индекс ---------- */

static int find_idh(uint64_t idh)
{
    for (uint32_t i = 0; i < S.hw; i++)
        if (S.ent[i].idh == idh) return (int)i;
    return -1;
}

static bool del_has(uint64_t idh)
{
    for (uint32_t i = 0; i < ST_DELETED_RING; i++)
        if (S.del[i] == idh) return true;
    return false;
}

static void del_add(uint64_t idh)
{
    S.del[S.del_pos] = idh;
    S.del_pos = (S.del_pos + 1) % ST_DELETED_RING;
}

static void ent_remove(int i, bool remember)
{
    st_ent_t *e = &S.ent[i];
    if (!e->idh) return;
    uint32_t s = e->off / ST_SECTOR;
    if (s < S.nsec && S.sec[s].live >= e->rec_len) S.sec[s].live -= e->rec_len;
    if (S.bytes_live >= e->rec_len) S.bytes_live -= e->rec_len;
    if (remember) del_add(e->idh);
    memset(e, 0, sizeof(*e));
    S.n--;
    while (S.hw > 0 && !S.ent[S.hw - 1].idh) S.hw--;
}

static int ent_alloc(void)
{
    for (uint32_t i = 0; i < S.cap; i++)
        if (!S.ent[i].idh) {
            if (i + 1 > S.hw) S.hw = i + 1;
            return (int)i;
        }
    return -1;
}

/* Индекс полон: вытеснить самую старую запись. */
static void evict_oldest(void)
{
    int best = -1;
    for (uint32_t i = 0; i < S.hw; i++)
        if (S.ent[i].idh && (best < 0 || S.ent[i].got_at < S.ent[best].got_at)) best = (int)i;
    if (best >= 0) { ent_remove(best, true); S.st.gc_evicted++; }
}

static uint32_t bloom_bits(const char *phone)
{
    return st_hash32(phone);
}

static bool bloom_has(uint64_t f, uint32_t h)
{
    return (f >> (h & 63) & 1) && (f >> ((h >> 6) & 63) & 1) && (f >> ((h >> 12) & 63) & 1);
}

static void bloom_add(st_ent_t *e, uint32_t h)
{
    if (bloom_has(e->have, h)) return;
    e->have |= 1ull << (h & 63) | 1ull << ((h >> 6) & 63) | 1ull << ((h >> 12) & 63);
    if (e->nhave < 255) e->nhave++;
}

/* ---------- секторы ---------- */

static uint32_t free_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < S.nsec; i++) n += S.sec[i].state != S_USED;
    return n;
}

static int alloc_sector(void)
{
    uint32_t start = S.has_tail ? (S.tail + 1) % S.nsec : 0;
    for (uint32_t k = 0; k < S.nsec; k++) {
        uint32_t s = (start + k) % S.nsec;
        if (S.sec[s].state == S_USED) continue;
        if (f_erase(s)) { S.sec[s].state = S_FREE_DIRTY; return -1; }
        uint8_t h[SEC_HDR];
        memset(h, 0xff, sizeof(h));
        put32(h, SEC_MAGIC);
        put32(h + 4, S.max_seq + 1);
        put32(h + 8, crc32(h, 8));
        if (f_write(s * ST_SECTOR, h, SEC_HDR)) { S.sec[s].state = S_FREE_DIRTY; return -1; }
        S.max_seq++;
        S.sec[s] = (sec_t){ .seq = S.max_seq, .used = SEC_HDR, .live = 0, .state = S_USED };
        S.tail = s;
        S.has_tail = true;
        return (int)s;
    }
    return -1;
}

/* Дописать готовую запись (len кратно 4). Возвращает адрес или UINT32_MAX. */
static uint32_t append_raw(const uint8_t *rec, uint32_t len, bool may_alloc_last)
{
    if (!S.has_tail || S.sec[S.tail].sealed || S.sec[S.tail].used + len > ST_SECTOR) {
        if (S.has_tail && S.sec[S.tail].state == S_USED) S.sec[S.tail].sealed = 1;
        if (!may_alloc_last && free_count() <= 1) return UINT32_MAX;
        if (alloc_sector() < 0) return UINT32_MAX;
    }
    sec_t *t = &S.sec[S.tail];
    uint32_t off = S.tail * ST_SECTOR + t->used;
    if (f_write(off, rec, len)) { t->sealed = 1; return UINT32_MAX; }
    t->used = (uint16_t)(t->used + len);
    return off;
}

/* ---------- сборка мусора ---------- */

static int gc_one(bool force_drop)
{
    /* 1) сектор без живых записей — просто стереть */
    int victim = -1;
    for (uint32_t s = 0; s < S.nsec; s++) {
        if (S.sec[s].state != S_USED || (S.has_tail && s == S.tail)) continue;
        if (S.sec[s].live == 0) { victim = (int)s; break; }
    }
    bool drop = force_drop;
    if (victim < 0) {
        /* 2) самый старый */
        for (uint32_t s = 0; s < S.nsec; s++) {
            if (S.sec[s].state != S_USED || (S.has_tail && s == S.tail)) continue;
            if (victim < 0 || S.sec[s].seq < S.sec[victim].seq) victim = (int)s;
        }
        if (victim < 0) return -1;
        if (S.bytes_live > capacity_bytes() / 100u * ST_COPY_LIMIT_PCT) drop = true;
    }
    S.st.gc_runs++;
    uint32_t lo = (uint32_t)victim * ST_SECTOR, hi = lo + ST_SECTOR, now = now_s();
    for (uint32_t i = 0; i < S.hw; i++) {
        st_ent_t *e = &S.ent[i];
        if (!e->idh || e->off < lo || e->off >= hi) continue;
        if (expired_at(e->got_at, now)) { ent_remove((int)i, true); S.st.expired++; continue; }
        if (drop) { ent_remove((int)i, true); S.st.gc_evicted++; continue; }
        uint32_t len = e->rec_len;
        if (len > REC_MAX || f_read(e->off, S.rec, len)) { ent_remove((int)i, false); continue; }
        uint32_t off = append_raw(S.rec, len, false);
        if (off == UINT32_MAX) { ent_remove((int)i, true); S.st.gc_evicted++; continue; }
        S.sec[victim].live = (uint16_t)(S.sec[victim].live >= len ? S.sec[victim].live - len : 0);
        e->off = off;
        S.sec[off / ST_SECTOR].live = (uint16_t)(S.sec[off / ST_SECTOR].live + len);
        S.st.gc_copied++;
    }
    if (f_erase((uint32_t)victim)) { S.sec[victim].state = S_FREE_DIRTY; S.sec[victim].live = 0; return -1; }
    S.sec[victim] = (sec_t){ .state = S_FREE_CLEAN };
    return 0;
}

/* ---------- запись ---------- */

static size_t field_len(const char *s) { return s ? strlen(s) : 0; }

static uint32_t build_record(const box_msg_t *m, uint32_t got_at, uint32_t src)
{
    const char *f[5] = { m->message_id, m->ttl, m->receiver, m->type, m->bytes };
    uint8_t *p = S.rec + REC_HDR;
    put32(p, got_at);
    put32(p + 4, src);
    put16(p + 8, (uint16_t)(m->unit_count < 0 ? 0 : m->unit_count > 65535 ? 65535 : m->unit_count));
    p += REC_FIXED;
    for (int i = 0; i < 5; i++) {
        size_t n = field_len(f[i]);
        put16(p, (uint16_t)n);
        memcpy(p + 2, f[i] ? f[i] : "", n);
        p += 2 + n;
    }
    uint32_t plen = (uint32_t)(p - (S.rec + REC_HDR));
    uint32_t total = (REC_HDR + plen + 3u) & ~3u;
    memset(p, 0xff, total - REC_HDR - plen);
    put16(S.rec, REC_MAGIC);
    put16(S.rec + 2, (uint16_t)plen);
    put32(S.rec + 4, crc32(S.rec + REC_HDR, plen));
    return total;
}

/* Разбор payload; строки — указатели внутрь буфера dst (копия с '\0'). */
static int parse_payload(const uint8_t *p, uint32_t plen, uint32_t *got_at, uint32_t *src,
                         int *unit_count, char *dst, size_t dst_cap, const char *out[5])
{
    if (plen < REC_FIXED) return -1;
    *got_at = get32(p);
    *src = get32(p + 4);
    *unit_count = get16(p + 8);
    uint32_t i = REC_FIXED;
    size_t d = 0;
    for (int k = 0; k < 5; k++) {
        if (i + 2 > plen) return -1;
        uint16_t n = get16(p + i);
        i += 2;
        if (n > ST_MAX_FIELD || i + n > plen || d + n + 1 > dst_cap) return -1;
        memcpy(dst + d, p + i, n);
        dst[d + n] = '\0';
        out[k] = dst + d;
        d += n + 1;
        i += n;
    }
    return i == plen ? 0 : -1;
}

/* Последний отказ ST_REJECT_SIZE — для лога (какое поле не прошло). */
static char last_reject[160];

static bool fields_ok(const box_msg_t *m)
{
    if (!m) { snprintf(last_reject, sizeof(last_reject), "no message"); return false; }
    const char *f[5] = { m->message_id, m->ttl, m->receiver, m->type, m->bytes };
    static const char *const nm[5] = { "id", "ttl", "receiver", "type", "bytes" };
    size_t l[5];
    for (int i = 0; i < 5; i++) l[i] = field_len(f[i]);
    const char *why = NULL;
    /* message_id — ключ дедупликации (у Android это UUID, но формат не навязываем). */
    if (l[0] == 0 || l[0] > ST_MAX_ID_LEN) why = "id length";
    /* пустые bytes: insertTransitMessage их отвергает, да и в коробке поле потеряется */
    else if (l[4] == 0) why = "bytes empty";
    for (int i = 0; i < 5 && !why; i++) {
        if (l[i] > ST_MAX_FIELD) why = nm[i];
        else if (f[i] && strchr(f[i], '\n')) why = nm[i];
    }
    if (!why) return true;
    snprintf(last_reject, sizeof(last_reject), "%s: id=%u%s ttl=%u receiver=%u type=%u bytes=%u (max %u)",
             why, (unsigned)l[0], box_is_uuid(m->message_id) ? "(uuid)" : "", (unsigned)l[1],
             (unsigned)l[2], (unsigned)l[3], (unsigned)l[4], (unsigned)ST_MAX_FIELD);
    return false;
}

const char *st_reject_detail(void) { return last_reject; }

st_put_t st_put(const box_msg_t *m, const char *src_phone)
{
    if (!S.ok) return ST_REJECT_IO;
    if (!fields_ok(m)) { S.st.put_reject++; return ST_REJECT_SIZE; }
    uint64_t idh = st_hash64(m->message_id);
    uint32_t src = src_phone ? st_hash32(src_phone) : 0;
    int i = find_idh(idh);
    if (i >= 0) {
        if (src) bloom_add(&S.ent[i], src);
        S.st.put_dup++;
        return ST_DUP;
    }
    if (del_has(idh)) { S.st.put_dup++; return ST_DUP; }

    uint32_t now = now_s();
    uint32_t len = build_record(m, now, src);
    if (src) {
        uint32_t mine = 0;
        for (uint32_t k = 0; k < S.hw; k++)
            if (S.ent[k].idh && S.ent[k].src == src) mine += S.ent[k].rec_len;
        if (mine + len > capacity_bytes() / 100u * ST_SOURCE_PCT) { S.st.put_reject++; return ST_REJECT_QUOTA; }
    }
    if (S.n >= S.cap) evict_oldest();
    /* место во flash: держим хотя бы один свободный сектор под запись */
    for (int guard = 0; free_count() < 2 && guard < 4; guard++)
        if (gc_one(true) != 0) break;
    len = build_record(m, now, src);                 /* S.rec мог быть занят сборкой мусора */
    uint32_t off = append_raw(S.rec, len, true);
    if (off == UINT32_MAX) { S.st.put_reject++; return ST_REJECT_IO; }

    int k = ent_alloc();
    if (k < 0) { S.st.put_reject++; return ST_REJECT_IO; }
    st_ent_t *e = &S.ent[k];
    memset(e, 0, sizeof(*e));
    e->idh = idh;
    e->grp = st_group_of(m->message_id);
    e->boxh = box_hash_for_msgid(m->message_id);
    e->off = off;
    e->got_at = now;
    e->src = src;
    e->rec_len = (uint16_t)len;
    if (src) bloom_add(e, src);
    S.sec[off / ST_SECTOR].live = (uint16_t)(S.sec[off / ST_SECTOR].live + len);
    S.bytes_live += len;
    S.n++;
    S.st.put_new++;
    return ST_NEW;
}

bool st_known(const char *message_id)
{
    uint64_t h = st_hash64(message_id);
    return find_idh(h) >= 0 || del_has(h);
}

int st_find_msg(const char *message_id) { return find_idh(st_hash64(message_id)); }

int st_find_box(const char *box_id)
{
    uint64_t h = st_hash64(box_id);
    for (uint32_t i = 0; i < S.hw; i++)
        if (S.ent[i].idh && S.ent[i].boxh == h) return (int)i;
    return -1;
}

void st_mark_have_idx(int idx, const char *phone)
{
    if (idx < 0 || (uint32_t)idx >= S.cap || !S.ent[idx].idh || !phone) return;
    bloom_add(&S.ent[idx], bloom_bits(phone));
}

bool st_phone_has(int idx, const char *phone)
{
    if (idx < 0 || (uint32_t)idx >= S.cap || !S.ent[idx].idh || !phone) return false;
    return bloom_has(S.ent[idx].have, bloom_bits(phone));
}

void st_mark_sent(int idx)
{
    if (idx < 0 || (uint32_t)idx >= S.cap || !S.ent[idx].idh) return;
    if (S.ent[idx].sends < 255) S.ent[idx].sends++;
    S.ent[idx].last_sent = now_s();
}

const st_ent_t *st_entry(int idx)
{
    if (idx < 0 || (uint32_t)idx >= S.cap || !S.ent[idx].idh) return NULL;
    return &S.ent[idx];
}

static bool better(const st_ent_t *a, const st_ent_t *b)
{
    if (a->sends != b->sends) return a->sends < b->sends;
    return a->got_at > b->got_at;
}

uint32_t st_group_of(const char *message_id)
{
    if (!message_id || strlen(message_id) <= BOX_UUID_STR_LEN) return 0;
    char base[BOX_UUID_STR_LEN + 1];
    memcpy(base, message_id, BOX_UUID_STR_LEN);
    base[BOX_UUID_STR_LEN] = '\0';
    return st_hash32(base) | 1u;
}

static bool selectable(const st_ent_t *e, uint32_t h, bool known, uint32_t now)
{
    if (!e->idh || expired_at(e->got_at, now)) return false;
    if (known && bloom_has(e->have, h)) return false;
    return true;
}

int st_select(const char *phone, int *out, int max)
{
    if (max <= 0) return 0;
    uint32_t h = phone ? bloom_bits(phone) : 0, now = now_s();
    bool known = phone != NULL;
    /* 1) лучшие max записей по приоритету */
    static int best[64];
    int lim = max < 64 ? max : 64;
    int n = 0;
    for (uint32_t i = 0; i < S.hw; i++) {
        const st_ent_t *e = &S.ent[i];
        if (!selectable(e, h, known, now)) continue;
        int pos = n;
        while (pos > 0 && better(e, &S.ent[best[pos - 1]])) pos--;
        if (pos >= lim) continue;
        int last = n < lim ? n : lim - 1;
        for (int k = last; k > pos; k--) best[k] = best[k - 1];
        best[pos] = (int)i;
        if (n < lim) n++;
    }
    /* 2) части одного сообщения держим вместе: за выбранной частью сразу идут остальные
     *    части той же группы, чтобы получатель мог собрать сообщение за одну сессию. */
    int m = 0;
    for (int a = 0; a < n && m < lim; a++) {
        int idx = best[a];
        bool dup = false;
        for (int k = 0; k < m; k++) if (out[k] == idx) { dup = true; break; }
        if (dup) continue;
        out[m++] = idx;
        uint32_t g = S.ent[idx].grp;
        if (!g) continue;
        for (uint32_t i = 0; i < S.hw && m < lim; i++) {
            if ((int)i == idx || S.ent[i].grp != g || !selectable(&S.ent[i], h, known, now)) continue;
            bool have = false;
            for (int k = 0; k < m; k++) if (out[k] == (int)i) { have = true; break; }
            if (!have) out[m++] = (int)i;
        }
    }
    return m;
}

int st_read(int idx, st_msg_t *out)
{
    const st_ent_t *e = st_entry(idx);
    if (!e || e->rec_len > REC_MAX || e->rec_len < REC_HDR) return -1;
    if (f_read(e->off, S.rec, e->rec_len)) return -2;
    uint16_t plen = get16(S.rec + 2);
    if (get16(S.rec) != REC_MAGIC || REC_HDR + plen > e->rec_len ||
        crc32(S.rec + REC_HDR, plen) != get32(S.rec + 4)) return -3;
    uint32_t got_at, src;
    int uc;
    const char *f[5];
    if (parse_payload(S.rec + REC_HDR, plen, &got_at, &src, &uc, out->buf, sizeof(out->buf), f)) return -4;
    out->msg = (box_msg_t){ f[0], f[1], uc, f[2], f[3], f[4] };
    st_box_id_for(f[0], out->box_id);
    return 0;
}

/* ---------- монтирование ---------- */

uint32_t st_capacity_for(size_t mem_size)
{
    size_t fixed = ST_DELETED_RING * sizeof(uint64_t) + 2048u * sizeof(sec_t) + 64u;
    if (mem_size <= fixed) return 0;
    return (uint32_t)((mem_size - fixed) / sizeof(st_ent_t));
}

static void load_sector(uint32_t s, uint32_t now)
{
    sec_t *sc = &S.sec[s];
    uint32_t pos = SEC_HDR;
    uint8_t h[REC_HDR];
    static char strs[5 * (ST_MAX_FIELD + 1)];   /* не на стеке: st_init зовут из main task */
    while (pos + REC_HDR <= ST_SECTOR) {
        uint32_t off = s * ST_SECTOR + pos;
        if (f_read(off, h, REC_HDR)) { sc->sealed = 1; break; }
        uint16_t mg = get16(h), plen = get16(h + 2);
        if (mg == 0xffff && plen == 0xffff && get32(h + 4) == 0xffffffffu) {      /* конец */
            /* хвост должен быть стёрт целиком, иначе дописывать сюда нельзя */
            for (uint32_t p = pos; p < ST_SECTOR && !sc->sealed; p += sizeof(S.rec) / 4 * 4) {
                uint32_t n = ST_SECTOR - p < sizeof(S.rec) / 4 * 4 ? ST_SECTOR - p : sizeof(S.rec) / 4 * 4;
                if (f_read(s * ST_SECTOR + p, S.rec, n)) { sc->sealed = 1; break; }
                for (uint32_t k = 0; k < n; k++) if (S.rec[k] != 0xff) { sc->sealed = 1; break; }
            }
            break;
        }
        uint32_t total = (REC_HDR + plen + 3u) & ~3u;
        if (mg != REC_MAGIC || total > REC_MAX || pos + total > ST_SECTOR) {
            S.st.load_bad++; sc->sealed = 1; break;
        }
        if (f_read(off, S.rec, total) || crc32(S.rec + REC_HDR, plen) != get32(S.rec + 4)) {
            S.st.load_bad++; sc->sealed = 1; break;
        }
        uint32_t got_at, src;
        int uc;
        const char *f[5];
        if (parse_payload(S.rec + REC_HDR, plen, &got_at, &src, &uc, strs, sizeof(strs), f) ||
            !f[0][0] || !f[4][0]) {
            S.st.load_bad++; sc->sealed = 1; break;
        }
        pos += total;
        S.st.load_records++;
        if (expired_at(got_at, now)) { S.st.expired++; continue; }
        uint64_t idh = st_hash64(f[0]);
        int i = find_idh(idh);
        if (i >= 0) {                          /* более поздняя копия (сборка мусора) */
            st_ent_t *e = &S.ent[i];
            uint32_t os = e->off / ST_SECTOR;
            if (S.sec[os].live >= e->rec_len) S.sec[os].live -= e->rec_len;
            S.bytes_live -= e->rec_len;
            e->off = off;
            e->rec_len = (uint16_t)total;
        } else {
            if (S.n >= S.cap) continue;
            i = ent_alloc();
            st_ent_t *e = &S.ent[i];
            memset(e, 0, sizeof(*e));
            e->idh = idh;
            e->grp = st_group_of(f[0]);
            e->boxh = box_hash_for_msgid(f[0]);
            e->off = off;
            e->got_at = got_at;
            e->src = src;
            e->rec_len = (uint16_t)total;
            if (src) bloom_add(e, src);
            S.n++;
        }
        sc->live = (uint16_t)(sc->live + total);
        S.bytes_live += total;
    }
    sc->used = (uint16_t)pos;
}

int st_init(const st_env_t *env)
{
    memset(&S, 0, sizeof(S));
    S.env = *env;
    if (!env->read || !env->write || !env->erase || !env->now_s || env->size < ST_SECTOR * 8 ||
        env->size % ST_SECTOR) return -1;
    S.nsec = env->size / ST_SECTOR;
    if (S.nsec > 2048) return -1;
    uint8_t *m = (uint8_t *)env->mem;
    size_t need_fixed = ST_DELETED_RING * sizeof(uint64_t) + S.nsec * sizeof(sec_t);
    if (!m || env->mem_size < need_fixed + 16 * sizeof(st_ent_t)) return -2;
    memset(m, 0, env->mem_size);
    S.del = (uint64_t *)m;
    S.sec = (sec_t *)(m + ST_DELETED_RING * sizeof(uint64_t));
    S.ent = (st_ent_t *)(m + need_fixed);
    S.cap = (uint32_t)((env->mem_size - need_fixed) / sizeof(st_ent_t));

    /* заголовки секторов */
    for (uint32_t s = 0; s < S.nsec; s++) {
        uint8_t h[SEC_HDR];
        if (f_read(s * ST_SECTOR, h, SEC_HDR)) { S.sec[s].state = S_FREE_DIRTY; continue; }
        if (get32(h) == SEC_MAGIC && get32(h + 8) == crc32(h, 8)) {
            S.sec[s].state = S_USED;
            S.sec[s].seq = get32(h + 4);
            if (S.sec[s].seq > S.max_seq) S.max_seq = S.sec[s].seq;
        } else {
            S.sec[s].state = S_FREE_DIRTY;   /* стирается перед первой записью */
        }
    }
    /* записи — по возрастанию seq */
    uint32_t now = now_s(), last = 0;
    for (;;) {
        int next = -1;
        for (uint32_t s = 0; s < S.nsec; s++)
            if (S.sec[s].state == S_USED && S.sec[s].seq > last &&
                (next < 0 || S.sec[s].seq < S.sec[next].seq)) next = (int)s;
        if (next < 0) break;
        last = S.sec[next].seq;
        load_sector((uint32_t)next, now);
        S.tail = (uint32_t)next;
        S.has_tail = true;
    }
    S.ok = true;
    slog("STORE: mounted %u sectors, %u messages (%u bytes live), records=%u bad=%u expired=%u cap=%u",
         (unsigned)S.nsec, (unsigned)S.n, (unsigned)S.bytes_live, (unsigned)S.st.load_records,
         (unsigned)S.st.load_bad, (unsigned)S.st.expired, (unsigned)S.cap);
    return 0;
}

uint32_t st_newest(void)
{
    uint32_t m = 0;
    for (uint32_t i = 0; i < S.hw; i++)
        if (S.ent[i].idh && S.ent[i].got_at > m) m = S.ent[i].got_at;
    return m;
}

void st_maintain(void)
{
    if (!S.ok) return;
    uint32_t now = now_s();
    for (uint32_t i = 0; i < S.hw; i++)
        if (S.ent[i].idh && expired_at(S.ent[i].got_at, now)) { ent_remove((int)i, true); S.st.expired++; }
    if (free_count() < ST_RESERVE_SECTORS) gc_one(false);
}

const st_stats_t *st_stats(void)
{
    S.st.entries = S.n;
    S.st.cap = S.cap;
    S.st.bytes_live = S.bytes_live;
    S.st.sectors_total = S.nsec;
    S.st.sectors_free = free_count();
    S.st.sectors_used = S.nsec - S.st.sectors_free;
    return &S.st;
}

void st_status_line(char *out, size_t cap)
{
    const st_stats_t *t = st_stats();
    snprintf(out, cap,
             "store: msgs=%u/%u live=%uKB sectors used=%u free=%u | put new=%u dup=%u rej=%u | gc=%u copied=%u evicted=%u expired=%u io=%u",
             (unsigned)t->entries, (unsigned)t->cap, (unsigned)(t->bytes_live / 1024),
             (unsigned)t->sectors_used, (unsigned)t->sectors_free, (unsigned)t->put_new,
             (unsigned)t->put_dup, (unsigned)t->put_reject, (unsigned)t->gc_runs,
             (unsigned)t->gc_copied, (unsigned)t->gc_evicted, (unsigned)t->expired,
             (unsigned)t->io_errors);
}
