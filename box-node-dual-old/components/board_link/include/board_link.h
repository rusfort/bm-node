/*
 * board_link — надёжный канал между двумя платами Box Node по UART.
 * Чистый C без ESP-IDF: платформа передаёт байты UART и время.
 *
 * Кадр на линии:  COBS( header | data | crc16 ) 0x00
 *   header (10 байт, little-endian):
 *     u8  version   = BL_VERSION
 *     u8  type      — тип прикладного сообщения (BL_T_*)
 *     u8  flags     — BL_F_RELIABLE | BL_F_ACK
 *     u8  role      — роль отправителя (BL_ROLE_*)
 *     u16 boot_id   — случайный идентификатор загрузки отправителя
 *     u16 seq       — номер надёжного кадра (0 — ненадёжный)
 *     u16 len       — длина data
 *   crc16-CCITT (0xFFFF) по header+data
 *
 * Надёжность: stop-and-wait. На каждый надёжный кадр — ACK с тем же seq;
 * повтор по тайм-ауту; дубликаты по (boot_id, seq) не доставляются, но
 * подтверждаются повторно. Смена boot_id у соседа = сосед перезагрузился.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BL_VERSION        1
#define BL_MAX_DATA       600     /* V1-пакет 512 байт + служебные поля команд */
#define BL_HDR_LEN        10
#define BL_TXQ_LEN        8
#define BL_RETRY_MS       120
#define BL_MAX_RETRIES    25
#define BL_PING_MS        1000
#define BL_PEER_TIMEOUT_MS 3000

enum { BL_F_RELIABLE = 0x01, BL_F_ACK = 0x02 };
enum { BL_ROLE_UNKNOWN = 0, BL_ROLE_CORE = 1, BL_ROLE_TX = 2 };

/* Служебные типы (обрабатываются внутри модуля). */
enum {
    BL_T_ACK   = 0x00,
    BL_T_PING  = 0x01,
    BL_T_PONG  = 0x02,
    BL_T_HELLO = 0x03,
    BL_T_APP   = 0x10,            /* прикладные типы начинаются отсюда */
};

typedef struct {
    uint32_t frames_rx_ok, frames_tx, bytes_rx, bytes_tx;
    uint32_t crc_errors, cobs_errors, len_errors, overflow;
    uint32_t retransmits, delivered, dup_rx, send_failed, queue_full;
    uint32_t peer_restarts, link_ups, link_downs;
    uint32_t loopback;   /* свои же кадры вернулись на RX (эхо по линии) — отброшены */
} bl_stats_t;

typedef struct {
    uint8_t   role;
    uint16_t  boot_id;
    uint32_t  (*now_ms)(void);
    void      (*write)(const uint8_t *data, size_t len, void *ctx);
    /* Доставка прикладного кадра (BL_T_APP и выше), по порядку, без дубликатов. */
    void      (*on_frame)(uint8_t type, const uint8_t *data, uint16_t len, void *ctx);
    /* Изменение состояния канала / перезагрузка соседа. */
    void      (*on_event)(const char *what, void *ctx);
    void      *ctx;
} bl_config_t;

typedef struct bl_link bl_link_t;

size_t     bl_link_size(void);
void       bl_init(bl_link_t *l, const bl_config_t *cfg);
void       bl_rx_bytes(bl_link_t *l, const uint8_t *data, size_t len);
/* reliable=true: доставка с подтверждением и повторами. 0 — поставлено в очередь. */
int        bl_send(bl_link_t *l, uint8_t type, const uint8_t *data, uint16_t len, bool reliable);
void       bl_poll(bl_link_t *l);                      /* вызывать каждые ~10 мс */
bool       bl_link_up(const bl_link_t *l);
uint8_t    bl_peer_role(const bl_link_t *l);
uint16_t   bl_peer_boot_id(const bl_link_t *l);
int        bl_tx_queue_depth(const bl_link_t *l);
const bl_stats_t *bl_stats(const bl_link_t *l);
const char *bl_role_name(uint8_t role);

/* Вспомогательные функции (экспортируются для тестов). */
uint16_t   bl_crc16(const uint8_t *d, size_t n);
size_t     bl_cobs_encode(const uint8_t *in, size_t n, uint8_t *out);
int        bl_cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
