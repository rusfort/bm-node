#include "e2e_test.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MAX_PENDING_ACB  16
#define MAX_KNOWN_MSGS   64
#define MAX_SEEN_BOXES   64
#define MAX_KNOWN_SIDS   16
#define ID_CAP           64
#define MAX_PHONES       4

typedef struct {
    bool prepared, sent, acked;
    char cb_id[BOX_UUID_STR_LEN + 1];
    char box_id[ID_CAP];
} node_cb_t;

static struct {
    e2e_env_t env;
    char run[9];
    char m1[BOX_UUID_STR_LEN + 1];
    char x1[BOX_UUID_STR_LEN + 1];
    char x2[BOX_UUID_STR_LEN + 1];
    char m1_bytes[48];

    bool x1_box_sent;              /* BOX X1 хотя бы раз передан стеку */
    bool x1_cbb_rx;  char x1_cbb_id[ID_CAP];
    bool x1_ack_sent;
    bool x2_box_sent;
    bool x2_cdb_rx;  char x2_cdb_id[ID_CAP];
    bool x2_ack_sent;
    bool x2_wrong_cbb;             /* Android ответил CBB на дубликат — дедупликация не сработала */
    char x1_phone[13];             /* ID телефона, первым приславшего CBB(X1): он знает M1 */

    /* Телефоны по ID (последние 12 символов callback ID). Несколько телефонов
     * допустимы: сессии TX не адресованы, поэтому X1/X2 может получить любой. */
    struct { char id[13]; uint32_t cbb, cdb, x1_new, x2_new, x2_dup; } ph[MAX_PHONES];
    int  n_ph;

    node_cb_t ncbb, ncdb;

    char pend[MAX_PENDING_ACB][ID_CAP];
    int  n_pend;

    char known[MAX_KNOWN_MSGS][ID_CAP];
    int  n_known, known_next;

    char seen[MAX_SEEN_BOXES][ID_CAP];
    int  n_seen, seen_next;

    char rx_sid[ID_CAP];
    bool rx_has_sid;
    char ksid[MAX_KNOWN_SIDS][ID_CAP];
    int  n_ksid, ksid_next;

    uint32_t start_ms, last_status_ms;
    e2e_result_t result;

    /* статистика */
    uint32_t rx_count[8];
    uint32_t tx_sessions, tx_pkts_ok;
    uint32_t rx_box_msgs_new, rx_box_msgs_dup, rx_boxes_ignored;
} S;

static void logf_(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (S.env.log) S.env.log(line);
}

static void copy_id(char *dst, const char *src)
{
    snprintf(dst, ID_CAP, "%s", src ? src : "");
}

const char *e2e_pkt_kind_name(e2e_pkt_kind_t k)
{
    switch (k) {
    case E2E_PK_SID:    return "SID";
    case E2E_PK_ACB:    return "ACB";
    case E2E_PK_CBB:    return "CBB";
    case E2E_PK_CDB:    return "CDB";
    case E2E_PK_BOX_X1: return "BOX-X1(new)";
    case E2E_PK_BOX_X2: return "BOX-X2(dup)";
    case E2E_PK_TRM:    return "TRM";
    }
    return "?";
}

/* --- множества --- */

static bool ring_has(char (*arr)[ID_CAP], int n, const char *id)
{
    for (int i = 0; i < n; i++) if (strcmp(arr[i], id) == 0) return true;
    return false;
}

static void ring_add(char (*arr)[ID_CAP], int cap, int *n, int *next, const char *id)
{
    copy_id(arr[*next], id);
    *next = (*next + 1) % cap;
    if (*n < cap) (*n)++;
}

static bool pend_has(const char *id)
{
    for (int i = 0; i < S.n_pend; i++) if (strcmp(S.pend[i], id) == 0) return true;
    return false;
}

static void pend_add(const char *id)
{
    if (pend_has(id)) return;
    if (S.n_pend >= MAX_PENDING_ACB) {
        logf_("WARN ACB queue full, dropping callback %s", id);
        return;
    }
    copy_id(S.pend[S.n_pend++], id);
}

static void pend_remove(const char *id)
{
    for (int i = 0; i < S.n_pend; i++) {
        if (strcmp(S.pend[i], id) == 0) {
            memmove(&S.pend[i], &S.pend[i + 1], (size_t)(S.n_pend - i - 1) * ID_CAP);
            S.n_pend--;
            return;
        }
    }
}

/* --- итог --- */

static bool node_side_complete(void)
{
    return S.x1_cbb_rx && S.x1_ack_sent && S.x2_cdb_rx && S.x2_ack_sent &&
           S.ncbb.sent && S.ncdb.sent && !S.x2_wrong_cbb;
}

static void step_line(const char *name, bool done, const char *detail)
{
    logf_("  [%s] %-34s %s", done ? "OK" : "..", name, detail ? detail : "");
}

void e2e_print_status(void)
{
    uint32_t el = (S.env.now_ms() - S.start_ms) / 1000;
    logf_("==== E2E run %s  t=%us  result=%s ====", S.run, (unsigned)el,
          S.result == E2E_PASS ? "PASS" : S.result == E2E_FAIL ? "FAIL" : "RUNNING");
    step_line("1 NEW: BOX X1 -> Android", S.x1_box_sent, S.x1);
    step_line("2 Android CBB(X1) received", S.x1_cbb_rx, S.x1_cbb_rx ? S.x1_cbb_id : "");
    step_line("2 ACB(CBB X1) delivered  [A+1]", S.x1_ack_sent, "");
    step_line("3 DUP: BOX X2 (same M1) -> Android", S.x2_box_sent, S.x2);
    step_line("4 Android CDB(X2) received", S.x2_cdb_rx, S.x2_cdb_rx ? S.x2_cdb_id : "");
    step_line("4 ACB(CDB X2) delivered  [A+1]", S.x2_ack_sent, "");
    step_line("5 Node CBB(new box) delivered [C+1]", S.ncbb.sent, S.ncbb.prepared ? S.ncbb.box_id : "waiting Android BOX with new msg (send any chat msg on phone)");
    step_line("6 Node CDB(dup box) delivered [D+1]", S.ncdb.sent, S.ncdb.prepared ? S.ncdb.box_id : "waiting Android BOX with known msg");
    step_line("i Android ACB for Node CBB", S.ncbb.acked, "(info)");
    step_line("i Android ACB for Node CDB", S.ncdb.acked, "(info)");
    if (S.x2_wrong_cbb) logf_("  !! Android %s answered CBB to duplicate X2: duplicate NOT detected", S.x1_phone);
    for (int i = 0; i < S.n_ph; i++)
        logf_("  phone %s%s: CBB=%u CDB=%u | X1 new=%u X2 new=%u X2 dup=%u", S.ph[i].id,
              strcmp(S.ph[i].id, S.x1_phone) == 0 ? " (knows M1 first)" : "",
              (unsigned)S.ph[i].cbb, (unsigned)S.ph[i].cdb, (unsigned)S.ph[i].x1_new,
              (unsigned)S.ph[i].x2_new, (unsigned)S.ph[i].x2_dup);
    logf_("  rx: SID=%u TRM=%u BOX=%u CBB=%u CDB=%u ACB=%u | msgs new=%u dup=%u ignored_boxes=%u",
          (unsigned)S.rx_count[BOX_PKT_SID], (unsigned)S.rx_count[BOX_PKT_TRM],
          (unsigned)S.rx_count[BOX_PKT_BOX], (unsigned)S.rx_count[BOX_PKT_CBB],
          (unsigned)S.rx_count[BOX_PKT_CDB], (unsigned)S.rx_count[BOX_PKT_ACB],
          (unsigned)S.rx_box_msgs_new, (unsigned)S.rx_box_msgs_dup, (unsigned)S.rx_boxes_ignored);
    logf_("  tx: sessions=%u pkts_ok=%u pending_acb=%d",
          (unsigned)S.tx_sessions, (unsigned)S.tx_pkts_ok, S.n_pend);
    if (S.n_ph <= 1) logf_("  Expected Android delta: B+1 C+1 D+1 A+2");
    else logf_("  %d phones: per-phone counters are not deterministic (sessions from TX are not addressed)", S.n_ph);
}

static void check_done(void)
{
    if (S.result != E2E_RUNNING) return;
    if (node_side_complete()) {
        S.result = E2E_PASS;
        logf_("################ E2E NODE-SIDE PASS ################");
        if (S.n_ph <= 1) logf_("Check Android debug counters now: expected delta B+1 C+1 D+1 A+2");
        else logf_("%d phones took part: check per-phone lines in the status below", S.n_ph);
        e2e_print_status();
    }
}

/* --- инициализация --- */

void e2e_init(const e2e_env_t *env)
{
    memset(&S, 0, sizeof(S));
    S.env = *env;
    if (S.env.ttl == NULL) S.env.ttl = E2E_TTL_DEFAULT;
    if (S.env.timeout_ms == 0) S.env.timeout_ms = 15u * 60u * 1000u;

    box_uuid_v4(S.m1, S.env.rand32);
    box_uuid_v4(S.x1, S.env.rand32);
    box_uuid_v4(S.x2, S.env.rand32);
    memcpy(S.run, S.m1, 8);
    S.run[8] = '\0';
    snprintf(S.m1_bytes, sizeof(S.m1_bytes), "ESP32-E2E-%s", S.run);

    /* Узел знает M1 с момента создания — значит, возврат M1 от Android будет дубликатом. */
    ring_add(S.known, MAX_KNOWN_MSGS, &S.n_known, &S.known_next, S.m1);

    S.start_ms = S.last_status_ms = S.env.now_ms();
    S.result = E2E_RUNNING;

    logf_("E2E init: run=%s node_suffix=%s ttl=\"%s\"", S.run, S.env.node_suffix, S.env.ttl);
    logf_("E2E ids: M1=%s X1=%s X2=%s", S.m1, S.x1, S.x2);
}

e2e_result_t e2e_result(void) { return S.result; }

void e2e_tick(void)
{
    uint32_t now = S.env.now_ms();
    if (S.result == E2E_RUNNING && now - S.start_ms > S.env.timeout_ms) {
        S.result = E2E_FAIL;
        logf_("################ E2E FAIL: timeout ################");
        e2e_print_status();
        return;
    }
    if (S.result == E2E_RUNNING && now - S.last_status_ms >= 20000) {
        S.last_status_ms = now;
        e2e_print_status();
    }
}

/* --- приём --- */

void e2e_rx_connection_begin(void)
{
    S.rx_has_sid = false;
    S.rx_sid[0] = '\0';
}

static int box_list_contains(const box_pkt_t *p, const char *box_id)
{
    for (int i = 0; i < box_pkt_id_count(p); i++)
        if (strcmp(box_pkt_id_at(p, i), box_id) == 0) return 1;
    return 0;
}

static int phone_idx(const char *sender)
{
    if (!sender) return -1;
    for (int i = 0; i < S.n_ph; i++)
        if (strcmp(S.ph[i].id, sender) == 0) return i;
    if (S.n_ph == MAX_PHONES) return -1;
    snprintf(S.ph[S.n_ph].id, sizeof(S.ph[0].id), "%s", sender);
    logf_("INFO new phone in test: %s", sender);
    return S.n_ph++;
}

bool e2e_dedup_failed(void) { return S.x2_wrong_cbb; }
int  e2e_phone_count(void) { return S.n_ph; }

static void on_rx_callback(const box_pkt_t *p)
{
    const char *cb = box_pkt_primary_id(p);
    const char *sender = box_uuid_suffix(cb);
    bool is_cbb = p->type == BOX_PKT_CBB;

    if (sender && strcmp(sender, S.env.node_suffix) == 0) {
        logf_("WARN RX %s with own suffix (self loop) ignored: %s", is_cbb ? "CBB" : "CDB", cb);
        return;
    }

    bool has_x1 = box_list_contains(p, S.x1);
    bool has_x2 = box_list_contains(p, S.x2);
    logf_("RX %s id=%s boxes=%d x1=%d x2=%d", is_cbb ? "CBB" : "CDB", cb,
          box_pkt_id_count(p), has_x1, has_x2);

    if (!has_x1 && !has_x2) {
        logf_("  foreign callback (other run/device) -> no ACB, keeps Android A unchanged");
        return;
    }

    int pi = phone_idx(sender);
    if (pi >= 0) {
        if (is_cbb) S.ph[pi].cbb++; else S.ph[pi].cdb++;
        if (is_cbb && has_x1) S.ph[pi].x1_new++;
        if (is_cbb && has_x2) S.ph[pi].x2_new++;
        if (!is_cbb && has_x2) S.ph[pi].x2_dup++;
    }

    if (is_cbb && has_x1 && !S.x1_cbb_rx) {
        S.x1_cbb_rx = true;
        copy_id(S.x1_cbb_id, cb);
        snprintf(S.x1_phone, sizeof(S.x1_phone), "%s", sender ? sender : "");
        logf_("STEP 2: Android %s stored M1 as new and sent CBB(X1) -> ACB scheduled", S.x1_phone);
    } else if (is_cbb && has_x1 && sender && strcmp(sender, S.x1_phone) != 0) {
        logf_("INFO second phone %s also stored M1 as new (CBB X1)", sender);
    }
    if (!is_cbb && has_x2 && !S.x2_cdb_rx) {
        S.x2_cdb_rx = true;
        copy_id(S.x2_cdb_id, cb);
        logf_("STEP 4: Android detected duplicate M1 and sent CDB(X2) -> ACB scheduled");
    }
    if (is_cbb && has_x2) {
        /* Ошибка дедупликации — только если CBB(X2) прислал телефон, который уже
         * подтвердил M1 через CBB(X1). Другой телефон мог впервые увидеть M1 в X2. */
        if (S.x1_cbb_rx && sender && strcmp(sender, S.x1_phone) == 0) {
            S.x2_wrong_cbb = true;
            logf_("WARN Android %s sent CBB for duplicate box X2: M1 was saved again?", sender);
        } else {
            logf_("INFO phone %s did not know M1 and stored it from X2 (not a dedup error)",
                  sender ? sender : "?");
        }
    }
    if (!is_cbb && has_x1) {
        logf_("WARN Android sent CDB for X1: M1 was already known before the test");
    }
    pend_add(cb);
}

static void on_rx_acb(const box_pkt_t *p)
{
    for (int i = 0; i < box_pkt_id_count(p); i++) {
        const char *id = box_pkt_id_at(p, i);
        if (S.ncbb.prepared && strcmp(id, S.ncbb.cb_id) == 0) {
            if (!S.ncbb.acked) logf_("INFO Android confirmed Node CBB with ACB: %s", id);
            S.ncbb.acked = true;
        } else if (S.ncdb.prepared && strcmp(id, S.ncdb.cb_id) == 0) {
            if (!S.ncdb.acked) logf_("INFO Android confirmed Node CDB with ACB: %s", id);
            S.ncdb.acked = true;
        } else {
            logf_("RX ACB for unknown callback %s (ignored)", id);
        }
    }
}

static void prepare_node_cb(node_cb_t *cb, const char *box_id, const char *what)
{
    cb->prepared = true;
    box_uuid_with_suffix(cb->cb_id, S.env.node_suffix, S.env.rand32);
    copy_id(cb->box_id, box_id);
    logf_("%s prepared: callback=%s box=%s", what, cb->cb_id, box_id);
}

static void on_rx_box(const box_pkt_t *p)
{
    const char *box_id = box_pkt_primary_id(p);
    if (ring_has(S.seen, S.n_seen, box_id)) {
        logf_("RX BOX %s already processed -> skipped", box_id);
        return;
    }
    ring_add(S.seen, MAX_SEEN_BOXES, &S.n_seen, &S.seen_next, box_id);

    int n = box_pkt_msg_count(p), n_new = 0, n_dup = 0;
    for (int i = 0; i < n; i++) {
        box_msg_t m;
        box_pkt_msg_at(p, i, &m);
        bool known = ring_has(S.known, S.n_known, m.message_id);
        logf_("  msg %s ttl=\"%s\" units=%d type=%s len=%u -> %s", m.message_id, m.ttl,
              m.unit_count, m.type, (unsigned)strlen(m.bytes), known ? "DUP" : "NEW");
        if (known) {
            n_dup++;
        } else {
            n_new++;
            ring_add(S.known, MAX_KNOWN_MSGS, &S.n_known, &S.known_next, m.message_id);
        }
    }
    S.rx_box_msgs_new += (uint32_t)n_new;
    S.rx_box_msgs_dup += (uint32_t)n_dup;
    logf_("RX BOX id=%s msgs=%d new=%d dup=%d", box_id, n, n_new, n_dup);

    bool answered = false;
    if (n_new > 0 && !S.ncbb.prepared) {
        prepare_node_cb(&S.ncbb, box_id, "STEP 5: Node CBB");
        answered = true;
    }
    if (n_dup > 0 && !S.ncdb.prepared) {
        prepare_node_cb(&S.ncdb, box_id, "STEP 6: Node CDB");
        answered = true;
    }
    if (!answered) {
        S.rx_boxes_ignored++;
        logf_("  no callback for this box (test sends exactly one CBB and one CDB)");
    }
}

e2e_rx_action_t e2e_on_rx(const uint8_t *data, size_t len)
{
    static box_pkt_t p;              /* 1+ КБ — не на стеке задачи */
    int rc = box_parse(data, len, &p);
    if (rc != 0) {
        logf_("RX invalid packet len=%u rc=%d", (unsigned)len, rc);
        return E2E_RX_CONTINUE;
    }
    if (p.type < 8) S.rx_count[p.type]++;

    e2e_rx_action_t act = E2E_RX_CONTINUE;
    switch (p.type) {
    case BOX_PKT_SID: {
        const char *sid = box_pkt_primary_id(&p);
        copy_id(S.rx_sid, sid);
        S.rx_has_sid = true;
        if (ring_has(S.ksid, S.n_ksid, sid)) {
            logf_("RX SID %s already fully received -> close", sid);
            act = E2E_RX_CLOSE;
        } else {
            logf_("RX SID %s", sid);
        }
        break;
    }
    case BOX_PKT_TRM: {
        const char *sid = box_pkt_primary_id(&p);
        if (S.rx_has_sid && strcmp(sid, S.rx_sid) == 0) {
            if (!ring_has(S.ksid, S.n_ksid, sid))
                ring_add(S.ksid, MAX_KNOWN_SIDS, &S.n_ksid, &S.ksid_next, sid);
            logf_("RX TRM %s (session complete)", sid);
        } else {
            logf_("RX TRM %s without matching SID", sid);
        }
        act = E2E_RX_CLOSE;
        break;
    }
    case BOX_PKT_ACB: on_rx_acb(&p); break;
    case BOX_PKT_CBB:
    case BOX_PKT_CDB: on_rx_callback(&p); break;
    case BOX_PKT_BOX: on_rx_box(&p); break;
    default: break;
    }
    check_done();
    return act;
}

/* --- передача --- */

static bool add_pkt(e2e_session_t *s, e2e_pkt_kind_t kind, int len, uint16_t max_payload)
{
    if (len < 0) {
        logf_("WARN %s build failed", e2e_pkt_kind_name(kind));
        return false;
    }
    if (len > max_payload) {
        logf_("WARN %s len=%d > payload %u, skipped", e2e_pkt_kind_name(kind), len, max_payload);
        return false;
    }
    s->pkt[s->n].kind = kind;
    s->pkt[s->n].len = (uint16_t)len;
    s->n++;
    return true;
}

int e2e_build_session(e2e_session_t *s, uint16_t max_payload)
{
    memset(s, 0, sizeof(*s));
    box_uuid_v4(s->sid, S.env.rand32);
    size_t cap = sizeof(s->pkt[0].data);

    add_pkt(s, E2E_PK_SID,
            box_build_session(s->pkt[s->n].data, cap, BOX_PKT_SID, s->sid), max_payload);

    if (S.n_pend > 0) {
        const char *ids[E2E_MAX_ACB_IDS];
        int n = S.n_pend < E2E_MAX_ACB_IDS ? S.n_pend : E2E_MAX_ACB_IDS;
        for (int i = 0; i < n; i++) ids[i] = S.pend[i];
        if (add_pkt(s, E2E_PK_ACB, box_build_acb(s->pkt[s->n].data, cap, ids, n), max_payload)) {
            s->n_acb = n;
            for (int i = 0; i < n; i++) copy_id(s->acb_ids[i], ids[i]);
        }
    }
    if (S.ncbb.prepared && !S.ncbb.sent) {
        const char *b[1] = { S.ncbb.box_id };
        s->has_cbb = add_pkt(s, E2E_PK_CBB,
            box_build_callback(s->pkt[s->n].data, cap, BOX_PKT_CBB, S.ncbb.cb_id, b, 1), max_payload);
    }
    if (S.ncdb.prepared && !S.ncdb.sent) {
        const char *b[1] = { S.ncdb.box_id };
        s->has_cdb = add_pkt(s, E2E_PK_CDB,
            box_build_callback(s->pkt[s->n].data, cap, BOX_PKT_CDB, S.ncdb.cb_id, b, 1), max_payload);
    }

    if (S.result == E2E_RUNNING) {
        box_msg_t m = { S.m1, S.env.ttl, 0, "BOX-TEST-RECEIVER", "text", S.m1_bytes };
        if (!S.x1_cbb_rx) {
            add_pkt(s, E2E_PK_BOX_X1, box_build_box(s->pkt[s->n].data, cap, S.x1, &m, 1), max_payload);
        } else if (!S.x2_cdb_rx) {
            add_pkt(s, E2E_PK_BOX_X2, box_build_box(s->pkt[s->n].data, cap, S.x2, &m, 1), max_payload);
        }
    }

    add_pkt(s, E2E_PK_TRM, box_build_session(s->pkt[s->n].data, cap, BOX_PKT_TRM, s->sid), max_payload);
    add_pkt(s, E2E_PK_TRM, box_build_session(s->pkt[s->n].data, cap, BOX_PKT_TRM, s->sid), max_payload);
    return s->n;
}

void e2e_commit_session(const e2e_session_t *s, int delivered)
{
    S.tx_sessions++;
    if (delivered > s->n) delivered = s->n;
    if (delivered < 0) delivered = 0;
    S.tx_pkts_ok += (uint32_t)delivered;

    for (int i = 0; i < delivered; i++) {
        switch (s->pkt[i].kind) {
        case E2E_PK_ACB:
            for (int k = 0; k < s->n_acb; k++) {
                pend_remove(s->acb_ids[k]);
                if (S.x1_cbb_rx && strcmp(s->acb_ids[k], S.x1_cbb_id) == 0 && !S.x1_ack_sent) {
                    S.x1_ack_sent = true;
                    logf_("STEP 2 done: ACB for CBB(X1) delivered -> Android A+1");
                }
                if (S.x2_cdb_rx && strcmp(s->acb_ids[k], S.x2_cdb_id) == 0 && !S.x2_ack_sent) {
                    S.x2_ack_sent = true;
                    logf_("STEP 4 done: ACB for CDB(X2) delivered -> Android A+1");
                }
            }
            break;
        case E2E_PK_CBB:
            if (!S.ncbb.sent) logf_("STEP 5 done: Node CBB delivered -> Android C+1");
            S.ncbb.sent = true;
            break;
        case E2E_PK_CDB:
            if (!S.ncdb.sent) logf_("STEP 6 done: Node CDB delivered -> Android D+1");
            S.ncdb.sent = true;
            break;
        case E2E_PK_BOX_X1:
            if (!S.x1_box_sent) logf_("STEP 1: BOX X1 with new M1 delivered -> Android B+1 expected");
            S.x1_box_sent = true;
            break;
        case E2E_PK_BOX_X2:
            if (!S.x2_box_sent) logf_("STEP 3: BOX X2 with duplicate M1 delivered -> Android B+0");
            S.x2_box_sent = true;
            break;
        default:
            break;
        }
    }
    logf_("TX session %s committed: %d/%d packets delivered", s->sid, delivered, s->n);
    check_done();
}
