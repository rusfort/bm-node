/* Общие объявления прошивки Box Node E2E. */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Логический ID узла: 00000000-0000-4000-8000-aabbccddeeff (как в прошлых этапах). */
#define NODE_SUFFIX       "aabbccddeeff"
#define NODE_DEVICE_NAME  "BOX-NODE-01"

/* Префиксы реальных GATT UUID Box Messenger (UuidUtils.merge: первые 24 символа). */
#define BOX_SVC_PREFIX    "12345678-1234-5678-9012-"
#define BOX_CHR_PREFIX    "12345678-1234-5678-aaaa-"

extern SemaphoreHandle_t g_e2e_lock;
#define E2E_LOCK()   xSemaphoreTake(g_e2e_lock, portMAX_DELAY)
#define E2E_UNLOCK() xSemaphoreGive(g_e2e_lock)

extern uint8_t g_own_addr_type;

uint32_t node_now_ms(void);

struct ble_gap_event;

/* Периферия: GATT server Box + передающие сессии. */
/* События GATT server, пришедшие по соединению, которое инициировал Node (central):
 * Android может открыть GATT client поверх той же BLE-связи. */
int  periph_conn_event(struct ble_gap_event *event);
/* Есть подписчик на conn, которому ещё не отдана сессия, или сессия идёт сейчас. */
bool periph_conn_busy(uint16_t conn);
int  periph_gatt_init(void);
void periph_start(void);          /* вызывать из sync_cb */
void periph_task_start(void);

/* Центральная роль: подключение к GATT server Android и приём. */
void central_task_start(void);
void central_on_sync(void);
