/*
 * Совместный тест Node <-> Android: новое сообщение, дубликат и подтверждения
 * в обоих направлениях. Чистый C, без ESP-IDF; вызывающая сторона обязана
 * сериализовать вызовы (в прошивке — мьютекс в e2e_glue).
 *
 * Этапы (целевой прирост счётчиков Android: B+1 C+1 D+1 A+2):
 *   1. NEW      Node -> BOX X1 {M1}           Android: B+1, ставит CBB(X1)
 *   2. ACK-NEW  Android -> CBB(X1); Node -> ACB(id CBB)          Android: A+1
 *   3. DUP      Node -> BOX X2 {M1}           Android: B+0, ставит CDB(X2)
 *   4. ACK-DUP  Android -> CDB(X2); Node -> ACB(id CDB)          Android: A+1
 *   5. RX-NEW   Android -> BOX Y с неизвестным Node сообщением; Node -> CBB(Y)   Android: C+1
 *   6. RX-DUP   Android -> BOX Z с M1 (Node его знает);   Node -> CDB(Z)          Android: D+1
 *   (инфо)      Android -> ACB на CBB/CDB узла
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "box_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define E2E_MAX_SESSION_PKTS  8
#define E2E_MAX_ACB_IDS       8
#define E2E_TTL_DEFAULT       "2026-12-31 23:59:59"

typedef struct {
    uint32_t (*rand32)(void);
    uint32_t (*now_ms)(void);
    void     (*log)(const char *line);      /* одна строка без '\n' */
    const char *node_suffix;                /* 12 hex — хвост ID узла */
    const char *ttl;                        /* TTL тестового сообщения */
    uint32_t    timeout_ms;                 /* общий тайм-аут теста */
} e2e_env_t;

typedef enum {
    E2E_PK_SID = 0, E2E_PK_ACB, E2E_PK_CBB, E2E_PK_CDB, E2E_PK_BOX_X1, E2E_PK_BOX_X2, E2E_PK_TRM,
} e2e_pkt_kind_t;

typedef struct {
    e2e_pkt_kind_t kind;
    uint16_t       len;
    char           data[BOX_MAX_PACKET + 1];
} e2e_pkt_t;

typedef struct {
    char      sid[BOX_UUID_STR_LEN + 1];
    int       n;
    e2e_pkt_t pkt[E2E_MAX_SESSION_PKTS];
    /* что фиксируется при успешной доставке */
    int       n_acb;
    char      acb_ids[E2E_MAX_ACB_IDS][64];
    bool      has_cbb, has_cdb;
} e2e_session_t;

typedef enum {
    E2E_RX_CONTINUE = 0,
    E2E_RX_CLOSE,             /* сессия завершена или уже известна — можно отключаться */
} e2e_rx_action_t;

typedef enum { E2E_RUNNING = 0, E2E_PASS, E2E_FAIL } e2e_result_t;

void            e2e_init(const e2e_env_t *env);

/* Приём (Node — GATT client, Android — GATT server). */
void            e2e_rx_connection_begin(void);
e2e_rx_action_t e2e_on_rx(const uint8_t *data, size_t len);

/* Передача (Node — GATT server). max_payload = ATT_MTU - 3. */
int             e2e_build_session(e2e_session_t *s, uint16_t max_payload);
/* delivered_pkts — сколько пакетов подряд с начала подтверждены стеком (status=0). */
void            e2e_commit_session(const e2e_session_t *s, int delivered_pkts);

/* Периодический вызов: тайм-ауты и сводка. */
void            e2e_tick(void);
e2e_result_t    e2e_result(void);
/* CBB на X2 от телефона, который уже знает M1 (ошибка дедупликации Android). */
bool            e2e_dedup_failed(void);
int             e2e_phone_count(void);
void            e2e_print_status(void);

const char     *e2e_pkt_kind_name(e2e_pkt_kind_t k);

#ifdef __cplusplus
}
#endif
