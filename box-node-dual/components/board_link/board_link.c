#include "board_link.h"

#include <string.h>

#define RAW_MAX   (BL_HDR_LEN + BL_MAX_DATA + 2)
#define ENC_MAX   (RAW_MAX + RAW_MAX / 254 + 2)

typedef struct {
    uint8_t  type;
    uint16_t len;
    uint8_t  data[BL_MAX_DATA];
} bl_qitem_t;

struct bl_link {
    bl_config_t cfg;

    uint8_t  rx[ENC_MAX];
    size_t   rx_len;
    bool     rx_overflow;

    bl_qitem_t q[BL_TXQ_LEN];
    int      q_head, q_count;
    uint16_t next_seq;
    bool     inflight;
    uint16_t inflight_seq;
    uint32_t sent_at;
    int      retries;

    bool     peer_known;
    uint16_t peer_boot;
    uint8_t  peer_role;
    bool     have_last;
    uint16_t last_rx_seq;
    uint32_t last_rx_ms, last_ping_ms;
    bool     up;

    uint8_t  raw[RAW_MAX];          /* приём: декодированный кадр */
    uint8_t  txraw[RAW_MAX];        /* передача: отдельный буфер, чтобы ACK не затирал принятые данные */
    uint8_t  enc[ENC_MAX + 2];

    bl_stats_t st;
};

size_t bl_link_size(void) { return sizeof(struct bl_link); }

const char *bl_role_name(uint8_t role)
{
    switch (role) {
    case BL_ROLE_CORE: return "CORE";
    case BL_ROLE_TX:   return "TX";
    default:           return "UNKNOWN";
    }
}

/* ---------- CRC16-CCITT (poly 0x1021, init 0xFFFF) ---------- */

uint16_t bl_crc16(const uint8_t *d, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)d[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* ---------- COBS ---------- */

size_t bl_cobs_encode(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 1, code_pos = 0;
    uint8_t code = 1;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == 0) {
            out[code_pos] = code;
            code_pos = o++;
            code = 1;
        } else {
            out[o++] = in[i];
            if (++code == 0xFF) {
                out[code_pos] = code;
                code_pos = o++;
                code = 1;
            }
        }
    }
    out[code_pos] = code;
    return o;
}

int bl_cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t code = in[i++];
        if (code == 0) return -1;
        for (uint8_t k = 1; k < code; k++) {
            if (i >= n || o >= cap || in[i] == 0) return -1;
            out[o++] = in[i++];
        }
        if (code != 0xFF && i < n) {
            if (o >= cap) return -1;
            out[o++] = 0;
        }
    }
    return (int)o;
}

/* ---------- отправка кадра ---------- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void send_raw(bl_link_t *l, uint8_t type, uint8_t flags, uint16_t seq,
                     const uint8_t *data, uint16_t len)
{
    uint8_t *r = l->txraw;
    r[0] = BL_VERSION;
    r[1] = type;
    r[2] = flags;
    r[3] = l->cfg.role;
    put16(r + 4, l->cfg.boot_id);
    put16(r + 6, seq);
    put16(r + 8, len);
    if (len) memcpy(r + BL_HDR_LEN, data, len);
    uint16_t crc = bl_crc16(r, BL_HDR_LEN + len);
    put16(r + BL_HDR_LEN + len, crc);

    l->enc[0] = 0;                                  /* разделитель: сбросить мусор у приёмника */
    size_t n = bl_cobs_encode(r, BL_HDR_LEN + len + 2, l->enc + 1);
    l->enc[1 + n] = 0;
    l->cfg.write(l->enc, n + 2, l->cfg.ctx);
    l->st.frames_tx++;
    l->st.bytes_tx += (uint32_t)(n + 2);
}

static void event(bl_link_t *l, const char *what)
{
    if (l->cfg.on_event) l->cfg.on_event(what, l->cfg.ctx);
}

static void start_next(bl_link_t *l)
{
    if (l->inflight || l->q_count == 0) return;
    bl_qitem_t *it = &l->q[l->q_head];
    l->inflight_seq = l->next_seq++;
    if (l->next_seq == 0) l->next_seq = 1;
    l->inflight = true;
    l->retries = 0;
    l->sent_at = l->cfg.now_ms();
    send_raw(l, it->type, BL_F_RELIABLE, l->inflight_seq, it->data, it->len);
}

static void pop_head(bl_link_t *l)
{
    l->q_head = (l->q_head + 1) % BL_TXQ_LEN;
    l->q_count--;
    l->inflight = false;
}

/* ---------- API ---------- */

void bl_init(bl_link_t *l, const bl_config_t *cfg)
{
    memset(l, 0, sizeof(*l));
    l->cfg = *cfg;
    l->next_seq = 1;
    l->last_ping_ms = cfg->now_ms();
    send_raw(l, BL_T_HELLO, 0, 0, NULL, 0);
}

int bl_send(bl_link_t *l, uint8_t type, const uint8_t *data, uint16_t len, bool reliable)
{
    if (len > BL_MAX_DATA || type < BL_T_APP) return -1;
    if (!reliable) {
        send_raw(l, type, 0, 0, data, len);
        return 0;
    }
    if (l->q_count >= BL_TXQ_LEN) {
        l->st.queue_full++;
        return -2;
    }
    bl_qitem_t *it = &l->q[(l->q_head + l->q_count) % BL_TXQ_LEN];
    it->type = type;
    it->len = len;
    if (len) memcpy(it->data, data, len);
    l->q_count++;
    start_next(l);
    return 0;
}

static void process(bl_link_t *l)
{
    int n = bl_cobs_decode(l->rx, l->rx_len, l->raw, sizeof(l->raw));
    if (n < 0) { l->st.cobs_errors++; return; }
    if (n < BL_HDR_LEN + 2) { l->st.len_errors++; return; }
    const uint8_t *r = l->raw;
    uint16_t len = get16(r + 8);
    if (r[0] != BL_VERSION || len > BL_MAX_DATA || n != BL_HDR_LEN + len + 2) {
        l->st.len_errors++;
        return;
    }
    if (bl_crc16(r, BL_HDR_LEN + len) != get16(r + BL_HDR_LEN + len)) {
        l->st.crc_errors++;
        return;
    }

    uint8_t  type = r[1], flags = r[2], role = r[3];
    uint16_t boot = get16(r + 4), seq = get16(r + 6);
    uint32_t now = l->cfg.now_ms();

    /* Собственный кадр вернулся на RX (например, обесточенная соседняя плата
     * отражает сигнал через защитные диоды). Без этой проверки свой ACK на
     * свой же кадр ложно подтвердил бы доставку. */
    if (role == l->cfg.role && boot == l->cfg.boot_id) {
        l->st.loopback++;
        return;
    }

    l->st.frames_rx_ok++;
    if (!l->peer_known || boot != l->peer_boot) {
        if (l->peer_known) {
            l->st.peer_restarts++;
            event(l, "peer restarted");
        }
        l->peer_known = true;
        l->peer_boot = boot;
        l->have_last = false;
    }
    l->peer_role = role;
    l->last_rx_ms = now;
    if (!l->up) {
        l->up = true;
        l->st.link_ups++;
        event(l, "link up");
    }

    if (flags & BL_F_ACK) {
        if (l->inflight && seq == l->inflight_seq) {
            pop_head(l);
            start_next(l);
        }
        return;
    }

    if (flags & BL_F_RELIABLE) {
        send_raw(l, BL_T_ACK, BL_F_ACK, seq, NULL, 0);
        if (l->have_last && seq == l->last_rx_seq) {
            l->st.dup_rx++;
            return;
        }
        l->have_last = true;
        l->last_rx_seq = seq;
    }

    switch (type) {
    case BL_T_PING:  send_raw(l, BL_T_PONG, 0, 0, NULL, 0); break;
    case BL_T_PONG:
    case BL_T_HELLO:
    case BL_T_ACK:   break;
    default:
        if (type >= BL_T_APP) {
            l->st.delivered++;
            if (l->cfg.on_frame) l->cfg.on_frame(type, r + BL_HDR_LEN, len, l->cfg.ctx);
        }
        break;
    }
}

void bl_rx_bytes(bl_link_t *l, const uint8_t *data, size_t len)
{
    l->st.bytes_rx += (uint32_t)len;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (b == 0) {
            if (l->rx_len > 0 && !l->rx_overflow) process(l);
            else if (l->rx_overflow) l->st.overflow++;
            l->rx_len = 0;
            l->rx_overflow = false;
        } else if (l->rx_len < sizeof(l->rx)) {
            l->rx[l->rx_len++] = b;
        } else {
            l->rx_overflow = true;
        }
    }
}

void bl_poll(bl_link_t *l)
{
    uint32_t now = l->cfg.now_ms();

    if (l->inflight && now - l->sent_at >= BL_RETRY_MS) {
        if (l->retries >= BL_MAX_RETRIES) {
            l->st.send_failed++;
            pop_head(l);
            event(l, "reliable send failed (no ACK)");
        } else {
            bl_qitem_t *it = &l->q[l->q_head];
            l->retries++;
            l->st.retransmits++;
            l->sent_at = now;
            send_raw(l, it->type, BL_F_RELIABLE, l->inflight_seq, it->data, it->len);
        }
    }
    start_next(l);

    if (now - l->last_ping_ms >= BL_PING_MS) {
        l->last_ping_ms = now;
        send_raw(l, BL_T_PING, 0, 0, NULL, 0);
    }
    if (l->up && now - l->last_rx_ms > BL_PEER_TIMEOUT_MS) {
        l->up = false;
        l->st.link_downs++;
        event(l, "link down");
    }
}

bool bl_link_up(const bl_link_t *l) { return l->up; }
uint8_t bl_peer_role(const bl_link_t *l) { return l->peer_role; }
uint16_t bl_peer_boot_id(const bl_link_t *l) { return l->peer_boot; }
int bl_tx_queue_depth(const bl_link_t *l) { return l->q_count; }
const bl_stats_t *bl_stats(const bl_link_t *l) { return &l->st; }
