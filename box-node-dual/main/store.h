/*
 * store — хранилище транзитных сообщений Box Node во flash.
 *
 * Журнал «только дописываем» по секторам 4 КБ с CRC у каждой записи, индекс в RAM
 * (PSRAM). Срок хранения — ST_MAX_AGE_S от момента приёма узлом, TTL сообщения не
 * используется. При нехватке места вытесняются самые старые записи.
 * Чистый C: flash, время и память приходят через st_env_t, модуль тестируется на хосте.
 * Не потокобезопасен: вызывающая сторона сериализует вызовы.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "box_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST_SECTOR           4096u
#define ST_MAX_AGE_S        (7u * 24u * 3600u)  /* 7 суток */
#define ST_RESERVE_SECTORS  4                   /* держать свободными */
#define ST_COPY_LIMIT_PCT   80   /* сборка мусора переписывает живое, пока занято ≤ 80% */
#define ST_SOURCE_PCT       20   /* доля одного источника (телефона) */
#define ST_MAX_FIELD        480  /* предел строки в сообщении */
#define ST_DELETED_RING     4096 /* недавно вытесненные/истёкшие ID */

typedef struct {
    int  (*read)(void *ctx, uint32_t off, void *buf, size_t len);
    int  (*write)(void *ctx, uint32_t off, const void *buf, size_t len);
    int  (*erase)(void *ctx, uint32_t sector);        /* номер сектора */
    void *ctx;
    uint32_t size;                                    /* байт, кратно ST_SECTOR */
    uint32_t (*now_s)(void);                          /* часы узла, секунды, не убывают */
    void (*log)(const char *line);                    /* может быть NULL */
    void    *mem;                                     /* память под индекс */
    size_t   mem_size;
} st_env_t;

typedef enum {
    ST_NEW = 0,
    ST_DUP,            /* уже есть или недавно удалено */
    ST_REJECT_SIZE,    /* поля слишком длинные / неверные */
    ST_REJECT_QUOTA,   /* источник занял больше ST_SOURCE_PCT */
    ST_REJECT_IO,      /* ошибка flash */
} st_put_t;

/* Запись индекса (48 байт). */
typedef struct {
    uint64_t idh;          /* хеш message_id; 0 — свободно */
    uint64_t boxh;         /* хеш box_id, который узел даёт этому сообщению */
    uint64_t have;         /* фильтр Блума: у каких телефонов уже есть */
    uint32_t off;          /* адрес записи в разделе */
    uint32_t got_at;       /* когда принято (часы узла) */
    uint32_t src;          /* хеш ID телефона, от которого пришло */
    uint32_t last_sent;
    uint16_t rec_len;
    uint8_t  sends;        /* сколько раз отдали (насыщается на 255) */
    uint8_t  nhave;        /* сколько разных телефонов отметили (приблизительно) */
} st_ent_t;

typedef struct {
    uint32_t entries, cap, bytes_live, sectors_used, sectors_free, sectors_total;
    uint32_t put_new, put_dup, put_reject, gc_runs, gc_copied, gc_evicted, expired;
    uint32_t load_records, load_bad, io_errors;
} st_stats_t;

/* Сообщение, прочитанное из хранилища: строки лежат в buf. */
typedef struct {
    box_msg_t msg;
    char      box_id[BOX_UUID_STR_LEN + 1];
    char      buf[5 * (ST_MAX_FIELD + 1)];
} st_msg_t;

/* Монтирование: читает журнал, строит индекс. Возвращает 0 или <0. */
int   st_init(const st_env_t *env);
/* Сколько записей индекса помещается в mem_size. */
uint32_t st_capacity_for(size_t mem_size);

/* Приём сообщения от телефона src_phone (12 hex или NULL). unit_count сохраняется как есть. */
st_put_t st_put(const box_msg_t *m, const char *src_phone);
bool     st_known(const char *message_id);

/* Отметить, что у телефона есть сообщение. */
void  st_mark_have_idx(int idx, const char *phone);
/* По box_id из CBB/CDB: индекс записи или -1. */
int   st_find_box(const char *box_id);
int   st_find_msg(const char *message_id);
bool  st_phone_has(int idx, const char *phone);
void  st_mark_sent(int idx);

/* Выбрать до max записей для телефона (NULL — неизвестен): сначала те, что отдавали реже,
 * при равенстве — принятые позже. У известного телефона пропускаются те, что у него есть. */
int   st_select(const char *phone, int *out_idx, int max);
/* Прочитать сообщение. 0 или <0. */
int   st_read(int idx, st_msg_t *out);
const st_ent_t *st_entry(int idx);

/* Обслуживание (раз в секунду): истечение срока, сборка мусора (не больше одного сектора). */
void  st_maintain(void);
const st_stats_t *st_stats(void);
/* Самое позднее время приёма среди хранимых (для восстановления часов узла), 0 — пусто. */
uint32_t st_newest(void);
void  st_status_line(char *out, size_t cap);

/* box_id, который узел присваивает сообщению (стабилен, зависит только от message_id). */
void  st_box_id_for(const char *message_id, char out[BOX_UUID_STR_LEN + 1]);
uint64_t st_hash64(const char *s);
uint32_t st_hash32(const char *s);

#ifdef __cplusplus
}
#endif
