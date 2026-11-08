/* Хост-тест tx_stream.c: SID уходит сразу, обрывы, тайм-ауты (ASan/UBSan). */
#include <assert.h>
#include <stdio.h>

#include "tx_stream.h"
#include "relay.h"

static int checks;
#define CHECK(c) do { assert(c); checks++; } while (0)

int main(void)
{
    ts_t t; ts_done_t p, d;

    /* 1. Первый пакет (SID) отправляется сразу, не дожидаясь остальных. */
    ts_init(&t);
    CHECK(ts_on_pkt(&t, 1, 7, 0, 3, true, 0, &p, &d) == TS_SEND && !p.emit && !d.emit);
    ts_on_sent(&t, 0, 10, &d); CHECK(!d.emit);
    CHECK(ts_on_pkt(&t, 1, 7, 1, 3, true, 60, &p, &d) == TS_SEND);
    ts_on_sent(&t, 0, 70, &d); CHECK(!d.emit);
    CHECK(ts_on_pkt(&t, 1, 7, 2, 3, true, 120, &p, &d) == TS_SEND);
    ts_on_sent(&t, 0, 130, &d);
    CHECK(d.emit && d.served && d.rc == 0 && d.delivered == 3 && d.count == 3 && d.token == 1 && d.conn == 7);
    CHECK(!t.active);

    /* 2. Обрыв notification на втором пакете: DONE(delivered=1), остаток пропускается, DONE один. */
    ts_init(&t);
    ts_on_pkt(&t, 2, 7, 0, 4, true, 0, &p, &d); ts_on_sent(&t, 0, 1, &d);
    CHECK(ts_on_pkt(&t, 2, 7, 1, 4, true, 2, &p, &d) == TS_SEND);
    ts_on_sent(&t, 7, 3, &d);
    CHECK(d.emit && !d.served && d.rc == 7 && d.delivered == 1);
    CHECK(ts_on_pkt(&t, 2, 7, 2, 4, true, 4, &p, &d) == TS_SKIP && !d.emit && !p.emit);
    CHECK(ts_on_pkt(&t, 2, 7, 3, 4, true, 5, &p, &d) == TS_SKIP && !d.emit);
    CHECK(!t.active);
    ts_tick(&t, 99999, &d); CHECK(!d.emit);

    /* 3. Подписки уже нет: DONE NOT_SUBSCRIBED сразу, остальные пакеты пропускаются. */
    ts_init(&t);
    CHECK(ts_on_pkt(&t, 3, 9, 0, 2, false, 0, &p, &d) == TS_SKIP);
    CHECK(d.emit && d.rc == RL_RC_NOT_SUBSCRIBED && d.delivered == 0 && d.count == 2);
    CHECK(ts_on_pkt(&t, 3, 9, 1, 2, false, 1, &p, &d) == TS_SKIP && !d.emit);
    CHECK(!t.active);

    /* 4. CORE замолчал посреди сессии: тайм-аут TS_STALL_MS. */
    ts_init(&t);
    ts_on_pkt(&t, 4, 7, 0, 5, true, 1000, &p, &d); ts_on_sent(&t, 0, 1010, &d);
    ts_tick(&t, 1010 + TS_STALL_MS - 1, &d); CHECK(!d.emit);
    ts_tick(&t, 1010 + TS_STALL_MS, &d);
    CHECK(d.emit && d.rc == TS_RC_STALLED && d.delivered == 1 && !d.served);
    ts_tick(&t, 1010 + 2 * TS_STALL_MS, &d); CHECK(!d.emit);

    /* 5. Новая сессия прерывает незаконченную: DONE для старой, новая идёт. */
    ts_init(&t);
    ts_on_pkt(&t, 5, 7, 0, 3, true, 0, &p, &d); ts_on_sent(&t, 0, 1, &d);
    CHECK(ts_on_pkt(&t, 6, 8, 0, 2, true, 2, &p, &d) == TS_SEND);
    CHECK(p.emit && p.token == 5 && p.delivered == 1 && p.rc == TS_RC_STALLED);
    ts_on_sent(&t, 0, 3, &d); CHECK(!d.emit);
    ts_on_pkt(&t, 6, 8, 1, 2, true, 4, &p, &d); ts_on_sent(&t, 0, 5, &d);
    CHECK(d.emit && d.served && d.token == 6 && d.delivered == 2);

    /* 6. Пакет не по порядку / чужой токен — пропуск без последствий. */
    ts_init(&t);
    ts_on_pkt(&t, 7, 7, 0, 3, true, 0, &p, &d);
    CHECK(ts_on_pkt(&t, 7, 7, 2, 3, true, 1, &p, &d) == TS_SKIP);
    CHECK(ts_on_pkt(&t, 8, 7, 1, 3, true, 1, &p, &d) == TS_SKIP);
    ts_on_sent(&t, 0, 2, &d); CHECK(!d.emit && t.active);

    /* 7. Сессия из одного пакета. */
    ts_init(&t);
    CHECK(ts_on_pkt(&t, 9, 1, 0, 1, true, 0, &p, &d) == TS_SEND);
    ts_on_sent(&t, 0, 1, &d); CHECK(d.emit && d.served && d.delivered == 1);

    /* 8. Обрыв на последнем пакете: DONE с ошибкой, без «хвоста». */
    ts_init(&t);
    ts_on_pkt(&t, 10, 1, 0, 2, true, 0, &p, &d); ts_on_sent(&t, 0, 1, &d);
    ts_on_pkt(&t, 10, 1, 1, 2, true, 2, &p, &d); ts_on_sent(&t, 3, 3, &d);
    CHECK(d.emit && d.rc == 3 && d.delivered == 1 && !t.active);

    printf("test_tx_stream: %d checks OK\n", checks);
    return 0;
}
