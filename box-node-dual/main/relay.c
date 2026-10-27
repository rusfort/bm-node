/* relay — см. relay.h */
#include "relay.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

size_t rl_enc_tx_info(uint8_t *out, uint8_t addr_type, const uint8_t addr[6])
{
    out[0] = addr_type;
    memcpy(out + 1, addr, 6);
    return 7;
}

size_t rl_enc_conn_mtu(uint8_t *out, uint16_t conn, uint16_t mtu)
{
    put16(out, conn);
    put16(out + 2, mtu);
    return 4;
}

size_t rl_enc_subscribed(uint8_t *out, uint16_t conn, uint16_t mtu, uint8_t addr_type, const uint8_t addr[6],
                         const char *hello_id12)
{
    put16(out, conn);
    put16(out + 2, mtu);
    out[4] = addr_type;
    memcpy(out + 5, addr, 6);
    if (hello_id12 == NULL || strlen(hello_id12) != 12) return 11;
    memcpy(out + 11, hello_id12, 12);
    return 23;
}

size_t rl_enc_conn(uint8_t *out, uint16_t conn)
{
    put16(out, conn);
    return 2;
}

size_t rl_enc_sess_pkt(uint8_t *out, size_t cap, uint16_t token, uint16_t conn, uint8_t idx,
                       uint8_t count, uint8_t kind, const void *data, uint16_t len)
{
    if (cap < (size_t)RL_PKT_HDR + len) return 0;
    put16(out, token);
    put16(out + 2, conn);
    out[4] = idx;
    out[5] = count;
    out[6] = kind;
    out[7] = 0;
    memcpy(out + RL_PKT_HDR, data, len);
    return RL_PKT_HDR + len;
}

size_t rl_enc_sess_done(uint8_t *out, uint16_t token, uint16_t conn, uint8_t delivered,
                        uint8_t count, int16_t rc)
{
    put16(out, token);
    put16(out + 2, conn);
    out[4] = delivered;
    out[5] = count;
    put16(out + 6, (uint16_t)rc);
    return 8;
}

/* ===================== CORE ===================== */

static struct {
    rc_env_t env;
    struct { uint16_t conn, mtu; char phone[13]; } pend[RL_MAX_PENDING];
    int      n_pend;

    bool     active;
    uint16_t token, conn;
    uint32_t started_at;
    e2e_session_t s;                 /* ~4 КБ: держим до SESS_DONE */

    bool     tx_addr_known;
    uint8_t  tx_addr[6];
    rc_stats_t st;
} C;

static void rlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void rlog(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (C.env.log) C.env.log(line);
}

void rc_init(const rc_env_t *env)
{
    memset(&C, 0, sizeof(C));
    C.env = *env;
    C.token = (uint16_t)(env->now_ms() & 0xffff);
}

static void pend_remove(uint16_t conn)
{
    for (int i = 0; i < C.n_pend; i++) {
        if (C.pend[i].conn == conn) {
            memmove(&C.pend[i], &C.pend[i + 1], (size_t)(C.n_pend - i - 1) * sizeof(C.pend[0]));
            C.n_pend--;
            return;
        }
    }
}

static void pend_add(uint16_t conn, uint16_t mtu, const char *phone)
{
    if (C.active && C.conn == conn) return;          /* уже обслуживаем */
    for (int i = 0; i < C.n_pend; i++) {
        if (C.pend[i].conn == conn) {
            C.pend[i].mtu = mtu;
            if (phone) { memcpy(C.pend[i].phone, phone, 12); C.pend[i].phone[12] = '\0'; }
            return;
        }
    }
    if (C.n_pend == RL_MAX_PENDING) {                 /* вытесняем самую старую */
        memmove(&C.pend[0], &C.pend[1], (RL_MAX_PENDING - 1) * sizeof(C.pend[0]));
        C.n_pend--;
    }
    C.pend[C.n_pend].conn = conn;
    C.pend[C.n_pend].mtu = mtu;
    if (phone) { memcpy(C.pend[C.n_pend].phone, phone, 12); C.pend[C.n_pend].phone[12] = '\0'; }
    else C.pend[C.n_pend].phone[0] = '\0';
    C.n_pend++;
}

static void finish(int delivered, const char *why)
{
    if (!C.active) return;
    C.env.commit(&C.s, delivered);
    rlog("RELAY: session token=%04x conn=%u %s delivered=%d/%d",
         C.token, C.conn, why, delivered, C.s.n);
    C.active = false;
}

void rc_on_msg(uint8_t type, const uint8_t *d, uint16_t len)
{
    switch (type) {
    case RL_TX_INFO:
        if (len < 7) break;
        memcpy(C.tx_addr, d + 1, 6);
        C.tx_addr_known = true;
        rlog("RELAY: TX BLE address %02x:%02x:%02x:%02x:%02x:%02x",
             d[6], d[5], d[4], d[3], d[2], d[1]);
        break;

    case RL_SUBSCRIBED: {
        if (len < 4) break;
        C.st.subscribed++;
        char hid[13];
        const char *phone = NULL;
        if (len >= 23) {
            const uint8_t *a = d + 5;
            memcpy(hid, d + 11, 12);
            hid[12] = '\0';
            phone = hid;
            C.st.sub_hello++;
            rlog("RELAY: Android subscribed on TX conn=%u mtu=%u addr=%02x:%02x:%02x:%02x:%02x:%02x/%u phone=%s (HELLO)",
                 get16(d), get16(d + 2), a[5], a[4], a[3], a[2], a[1], a[0], d[4], hid);
        } else if (len >= 11) {
            const uint8_t *a = d + 5;
            const char *ph = C.env.phone_by_addr ? C.env.phone_by_addr(a) : NULL;
            if (ph) C.st.sub_addr_matched++; else C.st.sub_addr_unknown++;
            if (ph && strlen(ph) == 12) { memcpy(hid, ph, 13); phone = hid; }
            rlog("RELAY: Android subscribed on TX conn=%u mtu=%u addr=%02x:%02x:%02x:%02x:%02x:%02x/%u phone=%s",
                 get16(d), get16(d + 2), a[5], a[4], a[3], a[2], a[1], a[0], d[4],
                 ph ? ph : "? (address not seen by CORE)");
        } else {
            rlog("RELAY: Android subscribed on TX conn=%u mtu=%u", get16(d), get16(d + 2));
        }
        pend_add(get16(d), get16(d + 2), phone);
        break;
    }

    case RL_UNSUBSCRIBED:
        if (len < 2) break;
        pend_remove(get16(d));
        break;

    case RL_SESS_DONE: {
        if (len < 8) break;
        uint16_t token = get16(d), conn = get16(d + 2);
        int delivered = d[4];
        int16_t rc = (int16_t)get16(d + 6);
        if (!C.active || token != C.token || conn != C.conn) {
            C.st.stale_done++;
            rlog("RELAY: stale SESS_DONE token=%04x conn=%u ignored", token, conn);
            break;
        }
        C.st.sessions_done++;
        C.st.pkts_delivered += (uint32_t)delivered;
        char why[32];
        snprintf(why, sizeof(why), "done rc=%d", rc);
        finish(delivered, why);
        break;
    }

    default:
        rlog("RELAY: unexpected msg type=0x%02x len=%u", type, len);
        break;
    }
}

void rc_on_peer_restart(void)
{
    if (C.active) {
        C.st.aborted_restart++;
        finish(0, "aborted (TX restarted)");
    }
    C.n_pend = 0;              /* старые conn у перезагруженного TX недействительны */
    /* Адрес TX (публичный MAC) после перезагрузки не меняется — не забываем его,
     * иначе CORE может успеть подключиться к своей же TX до прихода TX_INFO. */
}

void rc_on_link_up(void)
{
    C.env.send(RL_SYNC_REQ, NULL, 0);
}

void rc_tick(bool link_ok)
{
    uint32_t now = C.env.now_ms();

    if (C.active) {
        if (now - C.started_at > RL_SESS_TIMEOUT_MS) {
            C.st.aborted_timeout++;
            finish(0, "aborted (no SESS_DONE)");
        }
        return;
    }
    if (!link_ok || C.n_pend == 0) return;

    uint16_t conn = C.pend[0].conn, mtu = C.pend[0].mtu;
    char phone[13];
    memcpy(phone, C.pend[0].phone, sizeof(phone));
    pend_remove(conn);
    if (mtu < 23) { rlog("RELAY: conn=%u bad mtu=%u", conn, mtu); return; }

    int n = C.env.build(&C.s, (uint16_t)(mtu - 3), phone[0] ? phone : NULL);
    if (n <= 0 || n > E2E_MAX_SESSION_PKTS) { rlog("RELAY: build failed n=%d", n); return; }

    C.token++;
    if (C.token == 0) C.token = 1;
    C.conn = conn;
    C.active = true;
    C.started_at = now;
    C.st.sessions_sent++;
    rlog("RELAY: session -> TX token=%04x conn=%u mtu=%u phone=%s sid=%s packets=%d",
         C.token, conn, mtu, phone[0] ? phone : "?", C.s.sid, n);

    static uint8_t buf[RL_PKT_HDR + BOX_MAX_PACKET];
    for (int i = 0; i < n; i++) {
        size_t m = rl_enc_sess_pkt(buf, sizeof(buf), C.token, conn, (uint8_t)i, (uint8_t)n,
                                   (uint8_t)C.s.pkt[i].kind, C.s.pkt[i].data, C.s.pkt[i].len);
        if (m == 0 || C.env.send(RL_SESS_PKT, buf, (uint16_t)m) != 0) {
            C.st.aborted_send++;
            finish(0, "aborted (link send failed)");
            return;
        }
    }
    C.started_at = C.env.now_ms();   /* отсчёт ожидания SESS_DONE — от конца передачи */
}

bool rc_tx_addr(uint8_t addr[6])
{
    if (!C.tx_addr_known) return false;
    memcpy(addr, C.tx_addr, 6);
    return true;
}

const rc_stats_t *rc_stats(void) { return &C.st; }

void rc_status_line(char *out, size_t cap)
{
    snprintf(out, cap,
             "relay: subs=%lu (hello=%lu addr known=%lu unknown=%lu) sent=%lu done=%lu pkts=%lu | aborted timeout=%lu restart=%lu send=%lu stale=%lu | pending=%d active=%d",
             (unsigned long)C.st.subscribed, (unsigned long)C.st.sub_hello, (unsigned long)C.st.sub_addr_matched,
             (unsigned long)C.st.sub_addr_unknown, (unsigned long)C.st.sessions_sent,
             (unsigned long)C.st.sessions_done, (unsigned long)C.st.pkts_delivered,
             (unsigned long)C.st.aborted_timeout, (unsigned long)C.st.aborted_restart,
             (unsigned long)C.st.aborted_send, (unsigned long)C.st.stale_done,
             C.n_pend, C.active ? 1 : 0);
}

/* ===================== TX ===================== */

void rt_init(rt_asm_t *a)
{
    memset(a, 0, sizeof(*a));
}

rt_result_t rt_on_pkt(rt_asm_t *a, const uint8_t *d, uint16_t len)
{
    if (len < RL_PKT_HDR) return RT_ERROR;
    uint16_t token = get16(d), conn = get16(d + 2);
    uint8_t idx = d[4], count = d[5], kind = d[6];
    uint16_t dlen = (uint16_t)(len - RL_PKT_HDR);
    if (count == 0 || count > E2E_MAX_SESSION_PKTS || idx >= count || dlen > BOX_MAX_PACKET) {
        a->active = false;
        return RT_ERROR;
    }

    if (idx == 0) {                 /* начало новой сессии отменяет недособранную */
        a->active = true;
        a->next_idx = 0;
        a->s.token = token;
        a->s.conn = conn;
        a->s.count = count;
    }
    if (!a->active || token != a->s.token || conn != a->s.conn || count != a->s.count ||
        idx != a->next_idx) {
        a->active = false;
        return RT_ERROR;
    }

    a->s.pkt[idx].kind = kind;
    a->s.pkt[idx].len = dlen;
    memcpy(a->s.pkt[idx].data, d + RL_PKT_HDR, dlen);
    a->next_idx++;
    if (a->next_idx == count) {
        a->active = false;
        return RT_COMPLETE;
    }
    return RT_NEED_MORE;
}
