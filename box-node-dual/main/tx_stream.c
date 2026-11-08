#include "tx_stream.h"

#include <string.h>

#include "relay.h"

static void finish(ts_t *t, int rc, ts_done_t *d)
{
    d->emit = true;
    d->served = rc == 0 && t->delivered == t->count;
    d->token = t->token;
    d->conn = t->conn;
    d->delivered = t->delivered;
    d->count = t->count;
    d->rc = rc;
    t->active = false;
    t->failed = false;
}

void ts_init(ts_t *t)
{
    memset(t, 0, sizeof(*t));
}

ts_action_t ts_on_pkt(ts_t *t, uint16_t token, uint16_t conn, uint8_t idx, uint8_t count,
                      bool subscribed, uint32_t now, ts_done_t *prev, ts_done_t *done)
{
    memset(prev, 0, sizeof(*prev));
    memset(done, 0, sizeof(*done));

    if (idx == 0) {
        /* Новая сессия. Незавершённую прежнюю закрываем тем, что успели отдать. */
        if (t->active && !t->failed && !(t->token == token && t->conn == conn))
            finish(t, TS_RC_STALLED, prev);
        t->active = true;
        t->failed = false;
        t->token = token;
        t->conn = conn;
        t->count = count;
        t->next = 0;
        t->delivered = 0;
        t->last_ms = now;
        if (!subscribed) {
            t->failed = true;          /* остаток этой сессии молча пропускаем */
            done->emit = true;
            done->token = token;
            done->conn = conn;
            done->count = count;
            done->rc = RL_RC_NOT_SUBSCRIBED;
            if (count == 1) t->active = false;
            else t->next = 1;
            return TS_SKIP;
        }
        t->next = 1;
        return TS_SEND;
    }

    /* Продолжение: только по порядку и только для текущей сессии. */
    if (!t->active || token != t->token || conn != t->conn || count != t->count || idx != t->next)
        return TS_SKIP;
    t->last_ms = now;
    t->next++;
    if (t->failed) {
        if (t->next >= t->count) { t->active = false; t->failed = false; }
        return TS_SKIP;
    }
    return TS_SEND;
}

void ts_on_sent(ts_t *t, int rc, uint32_t now, ts_done_t *done)
{
    memset(done, 0, sizeof(*done));
    if (!t->active || t->failed) return;
    t->last_ms = now;
    if (rc != 0) {
        finish(t, rc, done);
        /* Остаток сессии от CORE ещё идёт — пропустим его по номерам. */
        if (t->next < t->count) { t->active = true; t->failed = true; }
        return;
    }
    t->delivered++;
    if (t->next >= t->count) finish(t, 0, done);
}

void ts_tick(ts_t *t, uint32_t now, ts_done_t *done)
{
    memset(done, 0, sizeof(*done));
    if (!t->active || now - t->last_ms < TS_STALL_MS) return;
    if (t->failed) { t->active = false; t->failed = false; return; }  /* SESS_DONE уже ушёл */
    finish(t, TS_RC_STALLED, done);
}
