/*
 * box_adv — данные арбитража Box Messenger (спецификация «кто раздаёт первым» v2):
 *   - Service Data 128-bit в scan response: ver, flags, id6;
 *   - HELLO-запись клиента: "V1\nHLO\n<id12>\n<flags hex2>";
 *   - таблица соседей CORE и правило «подключаться или нет».
 * Чистый C без ESP-IDF: собирается и тестируется на хосте.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BX_VER            0x01
#define BX_F_NODE         0x01
#define BX_F_SERVING      0x02
#define BX_F_HAS_OUT      0x04

#define BX_SCAN_RSP_LEN   26      /* 1 len + 1 type + 16 uuid + 8 data */
#define BX_HELLO_MAX      32

/* SERVICE_UUID Box Messenger 12345678-1234-5678-9012-123456789abc, little-endian (как в эфире). */
extern const uint8_t bx_service_uuid_le[16];

typedef struct {
    uint8_t ver;
    uint8_t flags;
    char    id[13];               /* 12 hex-символов, нижний регистр */
} bx_info_t;

/* Scan response: одна AD-структура Service Data 128-bit. id12 — 12 hex-символов.
 * Возвращает длину (26) или 0 при неверном id. */
size_t bx_build_scan_rsp(uint8_t *out, uint8_t flags, const char *id12);

/* Ищет в AD-данных (adv или scan response) Service Data с SERVICE_UUID и ver=1. */
bool   bx_parse_ad(const uint8_t *ad, size_t len, bx_info_t *out);
/* Есть ли в AD-данных SERVICE_UUID в списке 128-битных UUID (тип 0x06/0x07). */
bool   bx_ad_has_service_uuid(const uint8_t *ad, size_t len);

/* HELLO. Возвращает длину строки (без '\0') или 0. */
size_t bx_build_hello(char *out, size_t cap, const char *id12, uint8_t flags);
/* true — пакет является HELLO с корректным ID (flags необязательны, по умолчанию 0). */
bool   bx_parse_hello(const uint8_t *d, size_t len, char id_out[13], uint8_t *flags_out);

/* ---------- таблица соседей CORE ---------- */

#define BX_NB_MAX            12
#define BX_INFO_FRESH_MS     3000   /* флаги из scan response считаются текущими */
#define BX_LEGACY_WAIT_MS    800    /* нет scan response за это время — старая версия */
#define BX_STALE_FALLBACK_MS 10000  /* scan response пропали надолго — подключаться по-старому */
#define BX_FORGET_MS         30000

typedef enum {
    BX_WAIT = 0,           /* ещё не ясно: ждём scan response */
    BX_CONNECT,            /* новая версия в фазе раздачи, Box Node или старая версия */
    BX_SKIP_NOT_SERVING,   /* новая версия в фазе приёма: подключаться бесполезно */
    BX_SKIP_SELF,          /* своя плата TX (NODE + свой ID) */
    BX_SKIP_UNKNOWN,       /* не Box */
} bx_decision_t;

typedef struct {
    uint8_t   addr[6];
    bool      used;
    bool      box_adv;           /* видели рекламу с SERVICE_UUID */
    bool      rsp_seen;          /* видели scan response */
    bool      info_valid;
    bx_info_t info;
    uint32_t  first_box_adv, last_seen, info_at;
    uint32_t  note_at;           /* для вызывающего: когда последний раз писали о соседе в лог */
} bx_nb_t;

typedef struct {
    bx_nb_t nb[BX_NB_MAX];
    char    own_id[13];
} bx_table_t;

void           bx_table_init(bx_table_t *t, const char *own_id12);
/* Отчёт сканера. is_scan_rsp — тип отчёта SCAN_RSP. Возвращает запись или NULL. */
bx_nb_t       *bx_table_report(bx_table_t *t, const uint8_t addr[6], bool is_scan_rsp,
                               const uint8_t *ad, size_t len, uint32_t now);
bx_nb_t       *bx_table_find(bx_table_t *t, const uint8_t addr[6]);
bx_decision_t  bx_decide(const bx_table_t *t, const bx_nb_t *n, uint32_t now);
/* Сосед — новая версия приложения (не узел) с актуальными данными: ему шлём HELLO. */
bool           bx_is_new_phone(const bx_nb_t *n, uint32_t now);
const char    *bx_decision_name(bx_decision_t d);

#ifdef __cplusplus
}
#endif
