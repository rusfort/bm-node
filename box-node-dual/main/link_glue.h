/* Обвязка board_link для ESP-IDF: UART1, задачи приёма/опроса, очередь доставки. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_link.h"

typedef enum { LINK_EV_UP = 0, LINK_EV_DOWN, LINK_EV_PEER_RESTARTED, LINK_EV_SEND_FAILED } link_ev_t;

/* Колбэки вызываются из отдельной задачи доставки (не под блокировкой канала),
 * поэтому из них можно вызывать link_send. */
typedef void (*link_frame_cb_t)(uint8_t type, const uint8_t *data, uint16_t len);
typedef void (*link_event_cb_t)(link_ev_t ev);

/* Обработчики задаются до link_start. */
void     link_set_handlers(link_frame_cb_t on_frame, link_event_cb_t on_event);
void     link_start(uint8_t role, uint16_t boot_id);
/* Надёжная отправка. Ждёт места в очереди канала до wait_ms. 0 — поставлено. */
int      link_send(uint8_t type, const void *data, uint16_t len, uint32_t wait_ms);
/* Канал поднят и на той стороне плата с ожидаемой ролью. */
bool     link_ok(void);
