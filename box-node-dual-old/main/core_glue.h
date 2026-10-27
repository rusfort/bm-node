/* Внутренние объявления роли CORE. */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern SemaphoreHandle_t g_e2e_lock;
#define E2E_LOCK()   xSemaphoreTake(g_e2e_lock, portMAX_DELAY)
#define E2E_UNLOCK() xSemaphoreGive(g_e2e_lock)

void central_task_start(void);
void central_on_sync_(void);

/* Адрес своей TX, найденный по признаку self loop (если TX_INFO ещё не пришёл). */
void core_learn_tx_addr(const uint8_t addr[6]);
/* ID телефона по BLE-адресу из соединений CORE; NULL — не встречался. */
const char *core_phone_by_addr(const uint8_t addr[6]);
