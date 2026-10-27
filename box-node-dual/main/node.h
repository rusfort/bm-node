/* Общие объявления прошивки Box Node Dual (две платы: CORE и TX). */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if defined(NODE_ROLE_CORE)
#define MY_ROLE BL_ROLE_CORE
#define PEER_ROLE BL_ROLE_TX
#elif defined(NODE_ROLE_TX)
#define MY_ROLE BL_ROLE_TX
#define PEER_ROLE BL_ROLE_CORE
#else
#error "NODE_ROLE_CORE or NODE_ROLE_TX must be defined"
#endif

#define FW_VERSION        "dual-0.4"

/* Логический ID узла: 00000000-0000-4000-8000-aabbccddeeff (как в прошлых этапах). */
#define NODE_SUFFIX       "aabbccddeeff"
#define NODE_DEVICE_NAME  "BOX-NODE-01"

/* Префиксы реальных GATT UUID Box Messenger (UuidUtils.merge: первые 24 символа). */
#define BOX_SVC_PREFIX    "12345678-1234-5678-9012-"
#define BOX_CHR_PREFIX    "12345678-1234-5678-aaaa-"

extern uint8_t g_own_addr_type;
uint32_t node_now_ms(void);

/* ---- роль CORE ---- */
void core_init(void);            /* до nimble_port_freertos_init */
void core_on_sync(void);
/* Адрес BLE своей платы TX (чтобы не подключаться к ней при сканировании). */
bool core_tx_addr(uint8_t addr[6]);

/* ---- роль TX ---- */
int  tx_gatt_init(void);
void tx_init(void);
void tx_on_sync(void);
