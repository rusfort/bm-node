/*
 * relay — прикладной протокол между платами CORE и TX поверх board_link.
 * Чистый C без ESP-IDF и FreeRTOS: всё платформенное приходит через env.
 *
 * Роли:
 *   TX   — GATT server Box: телефон подключается к TX и подписывается.
 *          TX сообщает CORE о подписке, получает от CORE готовую сессию
 *          (пакеты V1) и отправляет её телефону notifications.
 *   CORE — central + автомат теста e2e: строит сессии и учитывает доставку.
 *
 * Сообщения (все надёжные, little-endian):
 *   TX -> CORE  RL_TX_INFO      u8 addr_type, u8 addr[6] (порядок NimBLE)
 *   TX -> CORE  RL_SUBSCRIBED   u16 conn, u16 mtu [, u8 addr_type, u8 addr[6] — адрес телефона]
 *   TX -> CORE  RL_UNSUBSCRIBED u16 conn
 *   CORE -> TX  RL_SYNC_REQ     —  (TX отвечает TX_INFO и неотданными подписками)
 *   CORE -> TX  RL_SESS_PKT     u16 token, u16 conn, u8 idx, u8 count, u8 kind, u8 0, data
 *   TX -> CORE  RL_SESS_DONE    u16 token, u16 conn, u8 delivered, u8 count, i16 rc
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "e2e_test.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RL_TX_INFO      = 0x30,
    RL_SUBSCRIBED   = 0x31,
    RL_UNSUBSCRIBED = 0x32,
    RL_SYNC_REQ     = 0x33,
    RL_SESS_PKT     = 0x40,
    RL_SESS_DONE    = 0x41,
};

#define RL_PKT_HDR        8
#define RL_MAX_PENDING    4
#define RL_SESS_TIMEOUT_MS 20000   /* CORE: ждать SESS_DONE не дольше */

/* rc в SESS_DONE (свои коды, отрицательные; >0 — код NimBLE) */
enum { RL_RC_OK = 0, RL_RC_NOT_SUBSCRIBED = -1, RL_RC_BUSY = -2, RL_RC_BAD_SESSION = -3 };

/* ---------- кодирование ---------- */

size_t rl_enc_tx_info(uint8_t *out, uint8_t addr_type, const uint8_t addr[6]);
size_t rl_enc_conn_mtu(uint8_t *out, uint16_t conn, uint16_t mtu);
/* SUBSCRIBED с адресом телефона на стороне TX (11 байт). */
size_t rl_enc_subscribed(uint8_t *out, uint16_t conn, uint16_t mtu, uint8_t addr_type, const uint8_t addr[6]);
size_t rl_enc_conn(uint8_t *out, uint16_t conn);
size_t rl_enc_sess_pkt(uint8_t *out, size_t cap, uint16_t token, uint16_t conn, uint8_t idx,
                       uint8_t count, uint8_t kind, const void *data, uint16_t len);
size_t rl_enc_sess_done(uint8_t *out, uint16_t token, uint16_t conn, uint8_t delivered,
                        uint8_t count, int16_t rc);

/* ---------- CORE: менеджер сессий ---------- */

typedef struct {
    /* Надёжная отправка соседу; 0 — принято каналом. Может блокироваться. */
    int      (*send)(uint8_t type, const uint8_t *data, uint16_t len);
    uint32_t (*now_ms)(void);
    void     (*log)(const char *line);
    /* Обёртки над e2e_build_session / e2e_commit_session (с блокировкой автомата). */
    int      (*build)(e2e_session_t *s, uint16_t max_payload);
    void     (*commit)(const e2e_session_t *s, int delivered);
    /* Необязательно: ID телефона по BLE-адресу (из соединений CORE), NULL — неизвестен. */
    const char *(*phone_by_addr)(const uint8_t addr[6]);
} rc_env_t;

typedef struct {
    uint32_t subscribed, sessions_sent, sessions_done, pkts_delivered;
    uint32_t aborted_timeout, aborted_restart, aborted_send, stale_done;
    uint32_t sub_addr_matched, sub_addr_unknown;
} rc_stats_t;

void  rc_init(const rc_env_t *env);
/* Сообщение от TX. Не вызывает send — безопасно из любого контекста. */
void  rc_on_msg(uint8_t type, const uint8_t *data, uint16_t len);
/* TX перезагрузился или канал пропал надолго: прервать активную сессию. */
void  rc_on_peer_restart(void);
/* Канал поднялся: запросить у TX текущее состояние. */
void  rc_on_link_up(void);
/* Вызывать периодически (~50 мс). Здесь строятся и отправляются сессии. */
void  rc_tick(bool link_ok);
/* Адрес TX (для фильтра своего TX при сканировании). false — ещё не известен. */
bool  rc_tx_addr(uint8_t addr[6]);
const rc_stats_t *rc_stats(void);
void  rc_status_line(char *out, size_t cap);

/* ---------- TX: сборщик сессий ---------- */

typedef struct {
    uint8_t  kind;
    uint16_t len;
    uint8_t  data[BOX_MAX_PACKET];
} rt_pkt_t;

typedef struct {
    uint16_t token, conn;
    uint8_t  count;
    rt_pkt_t pkt[E2E_MAX_SESSION_PKTS];
} rt_session_t;

typedef struct {
    rt_session_t s;
    uint8_t      next_idx;
    bool         active;
} rt_asm_t;

typedef enum { RT_NEED_MORE = 0, RT_COMPLETE, RT_ERROR } rt_result_t;

void        rt_init(rt_asm_t *a);
/* Кадр RL_SESS_PKT. При RT_COMPLETE сессия лежит в a->s (до следующего вызова). */
rt_result_t rt_on_pkt(rt_asm_t *a, const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif
