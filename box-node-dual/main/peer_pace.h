/*
 * peer_pace — как часто CORE подключается к одному телефону (чистый C).
 *
 * Android раздаёт свою очередь (часто одни и те же десятки BOX) каждому, кто
 * подключится. Раньше CORE подключался к единственному телефону рядом каждые
 * 4–5 с и получал одни дубликаты: занят Bluetooth телефона, тратится батарея.
 *
 * После сеанса без новых данных телефон уходит на паузу 30 с, 60 с, 120 с,
 * дальше по 120 с. Сеанс с новыми сообщениями или подтверждениями — пауза снята.
 * Пауза ограничена PP_MAX_MS: новое сообщение телефона ждёт не дольше двух минут.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PP_FIRST_MS  30000u
#define PP_MAX_MS    120000u

typedef struct {
    uint8_t  idle;        /* сеансов подряд без новых данных */
    uint32_t until;       /* пауза до этого времени (0 — нет паузы) */
} pp_t;

uint32_t pp_quiet_ms(uint8_t idle);
/* Сеанс закончен. useful — пришло новое (сообщения, подтверждения наших колбэков). */
void     pp_on_session(pp_t *p, bool useful, uint32_t now);
bool     pp_quiet(const pp_t *p, uint32_t now);
/* Сколько ещё ждать, мс (0 — можно подключаться). */
uint32_t pp_left_ms(const pp_t *p, uint32_t now);

#ifdef __cplusplus
}
#endif
