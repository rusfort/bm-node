/*
 * relay_node — логика узла-ретранслятора Box Node (замена автомата e2e_test).
 *
 * Принимает от телефонов BOX/CBB/CDB/ACB/SID/TRM, складывает сообщения в store,
 * отвечает подтверждениями так же, как это делает Android (FileSyncWorker / Queue), и
 * собирает сессии раздачи: SID, ACB, CBB, CDB, BOX (по одному сообщению), TRM ×2.
 * Чистый C: всё платформенное — через rn_env_t; хранилище (st_init) поднимает вызывающий.
 * Вызывающая сторона сериализует вызовы.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "box_proto.h"
#include "e2e_test.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RN_MAX_CALLBACKS     32           /* свои CBB/CDB, ждущие ACB */
#define RN_MAX_ACBS          32           /* свои ACB на чужие CBB/CDB */
#define RN_UUIDS_LIMIT       10           /* как Queue.UUIDS_LIMIT */
#define RN_CB_GIVEUP_MS      (15u * 60u * 1000u)
#define RN_CB_MAX_SENDS      30
#define RN_ACB_TTL_MS        (2u * 60u * 1000u)
#define RN_RECENT_SESSIONS   16

typedef struct {
    uint32_t (*rand32)(void);
    uint32_t (*now_ms)(void);
    void     (*log)(const char *line);    /* может быть NULL */
    const char *node_suffix;              /* 12 hex */
} rn_env_t;

typedef struct {
    uint32_t rx_sessions, rx_sessions_noid, rx_known_sessions, rx_boxes, rx_msgs_new, rx_msgs_dup, rx_msgs_rej;
    uint32_t rx_cbb, rx_cdb, rx_cb_matched, rx_acb, cb_acked;
    uint32_t tx_sessions, tx_msgs, tx_pkts, cb_given_up, cb_dropped_full, acb_dropped_full;
} rn_stats_t;

void            rn_init(const rn_env_t *env);

/* Приём: телефон — GATT server, узел читает notifications. phone — суффикс ID из UUID
 * характеристики (12 hex) или NULL. */
void            rn_rx_begin(const char *phone);
e2e_rx_action_t rn_on_rx(const uint8_t *data, size_t len);

/* Раздача через TX. phone — ID подписавшегося телефона или NULL (неизвестен).
 * Возвращает число пакетов (>0) или <0. */
int             rn_build_session(e2e_session_t *s, uint16_t max_payload, const char *phone);
/* delivered — сколько пакетов подряд с начала доставлено. */
void            rn_commit_session(const e2e_session_t *s, int delivered);

void            rn_tick(void);                     /* раз в 100 мс … 1 с */
const rn_stats_t *rn_stats(void);
void            rn_status_line(char *out, size_t cap);
int             rn_pending_callbacks(void);
int             rn_pending_acbs(void);

#ifdef __cplusplus
}
#endif
