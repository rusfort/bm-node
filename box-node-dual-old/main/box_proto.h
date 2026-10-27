/*
 * Box Messenger V1 — разбор и сборка пакетов.
 * Чистый C без зависимостей от ESP-IDF: модуль собирается и тестируется на хосте.
 *
 * Форматы (совместимы с Android FileSyncWorker / Queue / Box):
 *   V1\nSID\n<session_uuid>
 *   V1\nTRM\n<session_uuid>
 *   V1\nACB\n<callback_id>[\n<callback_id>...]
 *   V1\nCBB\n<callback_id>\n<box_id>[\n<box_id>...]
 *   V1\nCDB\n<callback_id>\n<box_id>[\n<box_id>...]
 *   V1\nBOX\n<box_id>\n{<message_id>\n<ttl>\n<unit_count>\n<receiver>\n<type>\n<bytes>\n}...
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOX_VERSION          "V1"
#define BOX_MAX_PACKET       512   /* максимальная длина значения ATT */
#define BOX_UUID_STR_LEN     36
#define BOX_ID_SUFFIX_LEN    12
#define BOX_MAX_LINES        128
#define BOX_FIELDS_PER_MSG   6
#define BOX_HEADER_LEN       44    /* "V1\nBOX\n" + 36 + "\n" — Android режет substring(44) */

typedef enum {
    BOX_PKT_UNKNOWN = 0,
    BOX_PKT_SID,
    BOX_PKT_TRM,
    BOX_PKT_ACB,
    BOX_PKT_CBB,
    BOX_PKT_CDB,
    BOX_PKT_BOX,
} box_pkt_type_t;

typedef struct {
    box_pkt_type_t type;
    int   n_lines;
    char *lines[BOX_MAX_LINES];       /* указатели внутрь buf */
    char  buf[BOX_MAX_PACKET + 1];
} box_pkt_t;

typedef struct {
    const char *message_id;
    const char *ttl;
    int         unit_count;
    const char *receiver;
    const char *type;
    const char *bytes;
} box_msg_t;

/* Возвращает 0 при успехе, <0 — пакет некорректен (тип выставляется в UNKNOWN). */
int  box_parse(const uint8_t *data, size_t len, box_pkt_t *out);

const char *box_pkt_type_name(box_pkt_type_t t);

/* SID/TRM: идентификатор сессии; CBB/CDB: callback ID; BOX: box ID. */
const char *box_pkt_primary_id(const box_pkt_t *p);

/* CBB/CDB: список box ID; ACB: список callback ID. */
int  box_pkt_id_count(const box_pkt_t *p);
const char *box_pkt_id_at(const box_pkt_t *p, int i);

/* BOX: число сообщений и поля i-го сообщения. */
int  box_pkt_msg_count(const box_pkt_t *p);
int  box_pkt_msg_at(const box_pkt_t *p, int i, box_msg_t *out);

/* Сборка. Возвращают длину пакета или -1, если не помещается в cap. */
int  box_build_session(char *out, size_t cap, box_pkt_type_t sid_or_trm, const char *session_id);
int  box_build_acb(char *out, size_t cap, const char *const *callback_ids, int n);
int  box_build_callback(char *out, size_t cap, box_pkt_type_t cbb_or_cdb,
                        const char *callback_id, const char *const *box_ids, int n);
int  box_build_box(char *out, size_t cap, const char *box_id, const box_msg_t *msgs, int n);

/* UUID v4 в нижнем регистре; rand32 — источник случайности. */
void box_uuid_v4(char out[BOX_UUID_STR_LEN + 1], uint32_t (*rand32)(void));
/* UUID v4, у которого последние 12 символов заменены на суффикс узла (как UuidUtils.merge). */
void box_uuid_with_suffix(char out[BOX_UUID_STR_LEN + 1], const char *suffix12, uint32_t (*rand32)(void));
/* Последние 12 символов UUID (UuidUtils.getFinalPart). NULL, если строка короче. */
const char *box_uuid_suffix(const char *uuid);

bool box_is_uuid(const char *s);

#ifdef __cplusplus
}
#endif
