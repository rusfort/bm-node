/*
 * tx_stream — потоковая отправка сессии на TX (чистый C, без ESP-IDF).
 *
 * Раньше TX ждал по UART всю сессию (до 40 пакетов по ~500 байт, stop-and-wait на
 * 115200 бод — 2–3 с) и только потом отправлял SID. Android ждёт первое сообщение
 * 2 с (msgWaitingTimeout) и рвал соединение: «message waiting timeout».
 * Теперь каждый принятый RL_SESS_PKT сразу уходит телефону notification'ом.
 *
 *   ts_on_pkt()  — пакет пришёл от CORE: отправлять его или пропустить;
 *   ts_on_sent() — результат notification; может завершить сессию;
 *   ts_tick()    — CORE замолчал посреди сессии: завершить по тайм-ауту.
 * Завершение сообщается через ts_done_t — TX отправляет RL_SESS_DONE.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TS_STALL_MS 4000          /* пауза между пакетами сессии от CORE, после которой сдаёмся */
#define TS_RC_STALLED (-4)        /* rc в SESS_DONE: сессия от CORE пришла не целиком */

typedef struct {
    bool     emit;                /* нужно отправить RL_SESS_DONE */
    bool     served;              /* сессия доставлена целиком — пометить подписку */
    uint16_t token, conn;
    uint8_t  delivered, count;
    int      rc;
} ts_done_t;

typedef struct {
    uint16_t token, conn;
    uint8_t  count, next, delivered;
    bool     active;              /* сессия в работе */
    bool     failed;              /* отправка оборвалась: остаток сессии пропускаем */
    uint32_t last_ms;
} ts_t;

typedef enum { TS_SEND = 0, TS_SKIP } ts_action_t;

void        ts_init(ts_t *t);
/* subscribed — подписан ли ещё conn пакета. prev — завершение прерванной сессии. */
ts_action_t ts_on_pkt(ts_t *t, uint16_t token, uint16_t conn, uint8_t idx, uint8_t count,
                      bool subscribed, uint32_t now, ts_done_t *prev, ts_done_t *done);
/* Вызывать только после TS_SEND. rc — 0 или ошибка notification. */
void        ts_on_sent(ts_t *t, int rc, uint32_t now, ts_done_t *done);
void        ts_tick(ts_t *t, uint32_t now, ts_done_t *done);

#ifdef __cplusplus
}
#endif
