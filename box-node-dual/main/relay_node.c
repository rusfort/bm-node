#include "relay_node.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "store.h"

/* ---------- состояние ---------- */

typedef struct {
    bool     used, locked;          /* locked: уже отправлялся — состав не меняем */
    uint8_t  kind;                  /* BOX_PKT_CBB / BOX_PKT_CDB */
    uint8_t  sends;
    uint8_t  n;
    char     id[BOX_UUID_STR_LEN + 1];
    char     target[BOX_ID_SUFFIX_LEN + 1];      /* "" — телефон неизвестен */
    char     boxes[RN_UUIDS_LIMIT][BOX_UUID_STR_LEN + 1];
    uint32_t created_ms;
} rn_cb_t;

typedef struct {
    bool     used;
    char     id[BOX_UUID_STR_LEN + 1];
    char     target[BOX_ID_SUFFIX_LEN + 1];
    uint32_t created_ms;
} rn_acb_t;

/* Что фиксируется после доставки пакета сессии. */
typedef enum { M_NONE = 0, M_ACB, M_CB, M_BOX } meta_kind_t;
typedef struct {
    uint8_t kind;
    uint8_t n;                       /* M_ACB: число id */
    int16_t ref[RN_UUIDS_LIMIT];     /* M_ACB: слоты acb; M_CB: слот cb; M_BOX: индекс store */
    uint64_t check;                  /* M_BOX: idh записи (не перепутать после вытеснения) */
} rn_meta_t;

static struct {
    rn_env_t   env;
    rn_cb_t    cb[RN_MAX_CALLBACKS];
    rn_acb_t   acb[RN_MAX_ACBS];
    uint64_t   recent[RN_RECENT_SESSIONS];
    int        recent_pos;

    char       rx_phone[BOX_ID_SUFFIX_LEN + 1];
    uint64_t   rx_sid;

    /* последняя собранная сессия */
    char       tx_sid[BOX_UUID_STR_LEN + 1];
    char       tx_phone[BOX_ID_SUFFIX_LEN + 1];
    int        tx_n;
    rn_meta_t  meta[E2E_MAX_SESSION_PKTS];

    uint32_t   last_maint_ms;
    rn_stats_t st;
    box_pkt_t  pkt;
    st_msg_t   msg;
} R;

static void nlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void nlog(const char *fmt, ...)
{
    if (!R.env.log) return;
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    R.env.log(b);
}

static bool phone_ok(const char *p) { return p && strlen(p) == BOX_ID_SUFFIX_LEN; }
static void copy_phone(char dst[BOX_ID_SUFFIX_LEN + 1], const char *p)
{
    if (phone_ok(p)) { memcpy(dst, p, BOX_ID_SUFFIX_LEN); dst[BOX_ID_SUFFIX_LEN] = '\0'; }
    else dst[0] = '\0';
}

void rn_init(const rn_env_t *env)
{
    memset(&R, 0, sizeof(R));
    R.env = *env;
    R.last_maint_ms = env->now_ms();
}

/* ---------- свои CBB/CDB ---------- */

static void cb_add(uint8_t kind, const char *target, const char *box_id)
{
    for (int i = 0; i < RN_MAX_CALLBACKS; i++) {                /* уже ждёт подтверждения */
        rn_cb_t *c = &R.cb[i];
        if (!c->used || c->kind != kind || strcmp(c->target, target)) continue;
        for (int k = 0; k < c->n; k++) if (!strcmp(c->boxes[k], box_id)) return;
    }
    rn_cb_t *slot = NULL;
    for (int i = 0; i < RN_MAX_CALLBACKS && !slot; i++) {
        rn_cb_t *c = &R.cb[i];
        if (c->used && !c->locked && c->kind == kind && !strcmp(c->target, target) && c->n < RN_UUIDS_LIMIT)
            slot = c;
    }
    if (!slot) {
        for (int i = 0; i < RN_MAX_CALLBACKS && !slot; i++) if (!R.cb[i].used) slot = &R.cb[i];
        if (!slot) {                                             /* вытесняем самый старый */
            int old = 0;
            for (int i = 1; i < RN_MAX_CALLBACKS; i++)
                if ((int32_t)(R.cb[i].created_ms - R.cb[old].created_ms) < 0) old = i;
            slot = &R.cb[old];
            R.st.cb_dropped_full++;
        }
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->kind = kind;
        copy_phone(slot->target, target);
        box_uuid_with_suffix(slot->id, R.env.node_suffix, R.env.rand32);
        slot->created_ms = R.env.now_ms();
    }
    memcpy(slot->boxes[slot->n++], box_id, BOX_UUID_STR_LEN + 1);
}

static void acb_add(const char *cb_id, const char *target)
{
    for (int i = 0; i < RN_MAX_ACBS; i++)
        if (R.acb[i].used && !strcmp(R.acb[i].id, cb_id)) { R.acb[i].created_ms = R.env.now_ms(); return; }
    int slot = -1;
    for (int i = 0; i < RN_MAX_ACBS && slot < 0; i++) if (!R.acb[i].used) slot = i;
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < RN_MAX_ACBS; i++)
            if ((int32_t)(R.acb[i].created_ms - R.acb[slot].created_ms) < 0) slot = i;
        R.st.acb_dropped_full++;
    }
    rn_acb_t *a = &R.acb[slot];
    memset(a, 0, sizeof(*a));
    a->used = true;
    strncpy(a->id, cb_id, BOX_UUID_STR_LEN);
    copy_phone(a->target, target);
    a->created_ms = R.env.now_ms();
}

/* ---------- приём ---------- */

static bool recent_has(uint64_t h)
{
    for (int i = 0; i < RN_RECENT_SESSIONS; i++) if (R.recent[i] == h) return true;
    return false;
}

static void recent_add(uint64_t h)
{
    if (recent_has(h)) return;
    R.recent[R.recent_pos] = h;
    R.recent_pos = (R.recent_pos + 1) % RN_RECENT_SESSIONS;
}

void rn_rx_begin(const char *phone)
{
    copy_phone(R.rx_phone, phone);
    R.rx_sid = 0;
}

static void on_box(const box_pkt_t *p)
{
    const char *box_id = box_pkt_primary_id(p);
    const char *ph = R.rx_phone[0] ? R.rx_phone : NULL;
    int n = box_pkt_msg_count(p);
    bool any_new = false, any_dup = false;
    R.st.rx_boxes++;
    for (int i = 0; i < n; i++) {
        box_msg_t m;
        if (box_pkt_msg_at(p, i, &m)) { R.st.rx_msgs_rej++; continue; }
        if (m.unit_count < 0) {
            R.st.rx_msgs_rej++;
            nlog("NODE: msg %s rejected: unit_count is not a number", m.message_id);
            continue;
        }
        m.unit_count += 1;                                       /* как Box.getBoxContent */
        st_put_t r = st_put(&m, ph);
        if (r == ST_NEW) { any_new = true; R.st.rx_msgs_new++; }
        else if (r == ST_DUP) { any_dup = true; R.st.rx_msgs_dup++; }
        else {
            R.st.rx_msgs_rej++;
            if (r == ST_REJECT_SIZE) nlog("NODE: msg %s rejected, bad fields: %s", m.message_id, st_reject_detail());
            else if (r == ST_REJECT_QUOTA) nlog("NODE: msg %s rejected: source quota (20%%) reached", m.message_id);
            else nlog("NODE: msg %s rejected: flash error", m.message_id);
        }
    }
    if (any_new) cb_add(BOX_PKT_CBB, R.rx_phone, box_id);
    if (any_dup) cb_add(BOX_PKT_CDB, R.rx_phone, box_id);
    nlog("NODE: rx BOX %.8s from %s msgs=%d new=%d dup=%d", box_id, ph ? ph : "?", n, any_new, any_dup);
}

static void on_callback(const box_pkt_t *p)
{
    const char *cb_id = box_pkt_primary_id(p);
    const char *sender = box_uuid_suffix(cb_id);                /* UuidUtils.getFinalPart */
    if (!phone_ok(sender)) sender = R.rx_phone[0] ? R.rx_phone : NULL;
    if (p->type == BOX_PKT_CBB) R.st.rx_cbb++; else R.st.rx_cdb++;
    int matched = 0, n = box_pkt_id_count(p);
    for (int i = 0; i < n; i++) {
        int idx = st_find_box(box_pkt_id_at(p, i));
        if (idx < 0) continue;
        matched++;
        if (sender) st_mark_have_idx(idx, sender);
    }
    /* Как Android: ACB только если колбэк про наши коробки (иначе отберём его у адресата). */
    if (matched) { acb_add(cb_id, sender ? sender : ""); R.st.rx_cb_matched++; }
    nlog("NODE: rx %s %.8s from %s boxes=%d ours=%d", box_pkt_type_name(p->type), cb_id,
         sender ? sender : "?", n, matched);
}

static void on_acb(const box_pkt_t *p)
{
    R.st.rx_acb++;
    int n = box_pkt_id_count(p), hit = 0;
    for (int i = 0; i < n; i++) {
        const char *id = box_pkt_id_at(p, i);
        for (int k = 0; k < RN_MAX_CALLBACKS; k++)
            if (R.cb[k].used && !strcmp(R.cb[k].id, id)) { R.cb[k].used = false; hit++; R.st.cb_acked++; }
    }
    if (hit) nlog("NODE: rx ACB, %d of our callbacks confirmed", hit);
}

e2e_rx_action_t rn_on_rx(const uint8_t *data, size_t len)
{
    box_pkt_t *p = &R.pkt;
    if (box_parse(data, len, p) != 0) return E2E_RX_CONTINUE;
    switch (p->type) {
    case BOX_PKT_SID: {
        const char *sid = box_pkt_primary_id(p);
        if (!sid[0]) {
            R.st.rx_sessions++;
            /* сессия без id (Android с пустым currentSessionID): повтор не распознать,
             * принимаем целиком — дубли отсеет хранилище */
            R.rx_sid = 0;
            R.st.rx_sessions_noid++;
            nlog("NODE: rx SID without id (Android currentSessionID is empty) -> accept");
            break;
        }
        uint64_t h = st_hash64(sid);
        if (recent_has(h)) { R.st.rx_known_sessions++; return E2E_RX_CLOSE; }
        R.rx_sid = h;
        R.st.rx_sessions++;
        break;
    }
    case BOX_PKT_TRM: {
        const char *sid = box_pkt_primary_id(p);
        if (sid[0]) recent_add(st_hash64(sid));
        return E2E_RX_CLOSE;
    }
    case BOX_PKT_BOX: on_box(p); break;
    case BOX_PKT_CBB:
    case BOX_PKT_CDB: on_callback(p); break;
    case BOX_PKT_ACB: on_acb(p); break;
    default: break;
    }
    return E2E_RX_CONTINUE;
}

/* ---------- раздача ---------- */

static bool add_pkt(e2e_session_t *s, e2e_pkt_kind_t kind, const char *d, int len, uint16_t max_payload)
{
    if (len <= 0 || len > max_payload || s->n >= E2E_MAX_SESSION_PKTS) return false;
    e2e_pkt_t *k = &s->pkt[s->n];
    k->kind = kind;
    k->len = (uint16_t)len;
    memcpy(k->data, d, (size_t)len);
    k->data[len] = '\0';
    s->n++;
    return true;
}

static bool for_phone(const char *target, const char *phone)
{
    return !phone || !target[0] || !strcmp(target, phone);
}

int rn_build_session(e2e_session_t *s, uint16_t max_payload, const char *phone)
{
    if (max_payload < 60) return -1;
    if (max_payload > BOX_MAX_PACKET) max_payload = BOX_MAX_PACKET;
    memset(s, 0, sizeof(*s));
    memset(R.meta, 0, sizeof(R.meta));
    copy_phone(R.tx_phone, phone);
    const char *ph = R.tx_phone[0] ? R.tx_phone : NULL;
    static char buf[BOX_MAX_PACKET + 1];
    const int reserve_trm = 2;

    box_uuid_v4(s->sid, R.env.rand32);
    int len = box_build_session(buf, sizeof(buf), BOX_PKT_SID, s->sid);
    if (!add_pkt(s, E2E_PK_SID, buf, len, max_payload)) return -1;

    /* ACB (по RN_UUIDS_LIMIT) */
    const char *ids[RN_UUIDS_LIMIT];
    int16_t refs[RN_UUIDS_LIMIT];
    int k = 0;
    for (int i = 0; i <= RN_MAX_ACBS; i++) {
        bool flush = (i == RN_MAX_ACBS) || k == RN_UUIDS_LIMIT;
        if (flush && k) {
            int n = k;
            while (n > 0 && ((len = box_build_acb(buf, sizeof(buf), ids, n)) < 0 || len > max_payload)) n--;
            if (n > 0 && s->n < E2E_MAX_SESSION_PKTS - reserve_trm && add_pkt(s, E2E_PK_ACB, buf, len, max_payload)) {
                rn_meta_t *m = &R.meta[s->n - 1];
                m->kind = M_ACB;
                m->n = (uint8_t)n;
                memcpy(m->ref, refs, sizeof(refs[0]) * (size_t)n);
            }
            k = 0;
        }
        if (i == RN_MAX_ACBS) break;
        if (!R.acb[i].used || !for_phone(R.acb[i].target, ph)) continue;
        ids[k] = R.acb[i].id;
        refs[k] = (int16_t)i;
        k++;
    }

    /* CBB, затем CDB — как Queue */
    for (int pass = 0; pass < 2; pass++) {
        uint8_t kind = pass == 0 ? BOX_PKT_CBB : BOX_PKT_CDB;
        for (int i = 0; i < RN_MAX_CALLBACKS; i++) {
            rn_cb_t *c = &R.cb[i];
            if (!c->used || c->kind != kind || !for_phone(c->target, ph)) continue;
            if (s->n >= E2E_MAX_SESSION_PKTS - reserve_trm) break;
            const char *bx[RN_UUIDS_LIMIT];
            for (int b = 0; b < c->n; b++) bx[b] = c->boxes[b];
            len = box_build_callback(buf, sizeof(buf), (box_pkt_type_t)kind, c->id, bx, c->n);
            if (add_pkt(s, kind == BOX_PKT_CBB ? E2E_PK_CBB : E2E_PK_CDB, buf, len, max_payload)) {
                R.meta[s->n - 1].kind = M_CB;
                R.meta[s->n - 1].ref[0] = (int16_t)i;
            }
        }
    }

    /* BOX: одно сообщение на коробку, box_id стабилен для сообщения */
    int room = E2E_MAX_SESSION_PKTS - reserve_trm - s->n;
    int sel[E2E_MAX_SESSION_PKTS];
    int ns = room > 0 ? st_select(ph, sel, room) : 0;
    for (int i = 0; i < ns; i++) {
        if (st_read(sel[i], &R.msg) != 0) continue;
        len = box_build_box(buf, sizeof(buf), R.msg.box_id, &R.msg.msg, 1);
        if (add_pkt(s, E2E_PK_BOX, buf, len, max_payload)) {
            R.meta[s->n - 1].kind = M_BOX;
            R.meta[s->n - 1].ref[0] = (int16_t)sel[i];
            R.meta[s->n - 1].check = st_entry(sel[i])->idh;
        }
    }

    len = box_build_session(buf, sizeof(buf), BOX_PKT_TRM, s->sid);
    add_pkt(s, E2E_PK_TRM, buf, len, max_payload);
    add_pkt(s, E2E_PK_TRM, buf, len, max_payload);
    memcpy(R.tx_sid, s->sid, sizeof(R.tx_sid));
    R.tx_n = s->n;
    R.st.tx_sessions++;
    return s->n;
}

void rn_commit_session(const e2e_session_t *s, int delivered)
{
    if (strcmp(s->sid, R.tx_sid) != 0) return;               /* не та сессия */
    if (delivered > R.tx_n) delivered = R.tx_n;
    const char *ph = R.tx_phone[0] ? R.tx_phone : NULL;
    int msgs = 0;
    for (int i = 0; i < delivered; i++) {
        rn_meta_t *m = &R.meta[i];
        switch (m->kind) {
        case M_ACB:
            /* Удаляем только если доставили тому, кому адресовано; иначе ждём срока. */
            for (int k = 0; k < m->n; k++) {
                rn_acb_t *a = &R.acb[m->ref[k]];
                if (a->used && ph && (!a->target[0] || !strcmp(a->target, ph))) a->used = false;
            }
            break;
        case M_CB: {
            rn_cb_t *c = &R.cb[m->ref[0]];
            if (c->used) { c->locked = true; if (c->sends < 255) c->sends++; }
            break;
        }
        case M_BOX: {
            const st_ent_t *e = st_entry(m->ref[0]);
            if (!e || e->idh != m->check) break;
            st_mark_sent(m->ref[0]);
            if (ph) st_mark_have_idx(m->ref[0], ph);
            msgs++;
            break;
        }
        default: break;
        }
    }
    R.st.tx_pkts += (uint32_t)delivered;
    R.st.tx_msgs += (uint32_t)msgs;
    R.tx_sid[0] = '\0';
    nlog("NODE: session to %s delivered %d/%d packets, %d messages", ph ? ph : "?", delivered, R.tx_n, msgs);
}

/* ---------- обслуживание ---------- */

void rn_tick(void)
{
    uint32_t now = R.env.now_ms();
    if (now - R.last_maint_ms < 1000) return;
    R.last_maint_ms = now;
    st_maintain();
    for (int i = 0; i < RN_MAX_CALLBACKS; i++) {
        rn_cb_t *c = &R.cb[i];
        if (c->used && (now - c->created_ms > RN_CB_GIVEUP_MS || c->sends >= RN_CB_MAX_SENDS)) {
            c->used = false;
            R.st.cb_given_up++;
        }
    }
    for (int i = 0; i < RN_MAX_ACBS; i++)
        if (R.acb[i].used && now - R.acb[i].created_ms > RN_ACB_TTL_MS) R.acb[i].used = false;
}

const rn_stats_t *rn_stats(void) { return &R.st; }

int rn_pending_callbacks(void)
{
    int n = 0;
    for (int i = 0; i < RN_MAX_CALLBACKS; i++) n += R.cb[i].used;
    return n;
}

int rn_pending_acbs(void)
{
    int n = 0;
    for (int i = 0; i < RN_MAX_ACBS; i++) n += R.acb[i].used;
    return n;
}

void rn_status_line(char *out, size_t cap)
{
    const rn_stats_t *t = &R.st;
    snprintf(out, cap,
             "node: rx sess=%u (no id %u) known=%u boxes=%u msgs new=%u dup=%u rej=%u cbb=%u cdb=%u acb=%u | tx sess=%u msgs=%u pkts=%u | wait cb=%d acb=%d acked=%u gaveup=%u",
             (unsigned)t->rx_sessions, (unsigned)t->rx_sessions_noid, (unsigned)t->rx_known_sessions,
             (unsigned)t->rx_boxes,
             (unsigned)t->rx_msgs_new, (unsigned)t->rx_msgs_dup, (unsigned)t->rx_msgs_rej,
             (unsigned)t->rx_cbb, (unsigned)t->rx_cdb, (unsigned)t->rx_acb, (unsigned)t->tx_sessions,
             (unsigned)t->tx_msgs, (unsigned)t->tx_pkts, rn_pending_callbacks(), rn_pending_acbs(),
             (unsigned)t->cb_acked, (unsigned)t->cb_given_up);
}
