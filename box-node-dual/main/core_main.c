/*
 * Роль CORE: логика узла (по умолчанию — ретранслятор с хранилищем транзитных сообщений,
 * при сборке с -D NODE_MODE=E2E — прежний автомат теста e2e), центральная роль BLE
 * (приём от Android) и менеджер сессий, которые отправляет телефону плата TX.
 */
#if defined(NODE_ROLE_CORE)

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "node.h"
#include "core_glue.h"
#include "e2e_test.h"
#include "link_glue.h"
#include "relay.h"
#include "relay_node.h"
#include "store.h"

#define RELAY_STATUS_MS 30000
#define CLOCK_SAVE_MS   (10u * 60u * 1000u)
#define TRANSIT_SUBTYPE 0x40
#define INDEX_PSRAM_ENTRIES 16000u     /* ~770 КБ PSRAM */
#define INDEX_INTERNAL_ENTRIES 400u    /* без PSRAM */

static const char *TAG = "RELAY";

SemaphoreHandle_t g_e2e_lock;

typedef struct {
    uint8_t  is_event;
    uint8_t  type;
    uint16_t len;
    uint8_t  data[16];      /* сообщения TX -> CORE короткие (<= 8 байт) */
} rmsg_t;

static QueueHandle_t relay_q;
static portMUX_TYPE addr_mux = portMUX_INITIALIZER_UNLOCKED;
static bool    tx_addr_known;
static uint8_t tx_addr[6];

bool core_tx_addr(uint8_t addr[6])
{
    taskENTER_CRITICAL(&addr_mux);
    bool k = tx_addr_known;
    if (k) memcpy(addr, tx_addr, 6);
    taskEXIT_CRITICAL(&addr_mux);
    return k;
}

void core_learn_tx_addr(const uint8_t addr[6])
{
    taskENTER_CRITICAL(&addr_mux);
    tx_addr_known = true;
    memcpy(tx_addr, addr, 6);
    taskEXIT_CRITICAL(&addr_mux);
}

#if defined(NODE_MODE_E2E)
static void e2e_log(const char *line) { ESP_LOGI("E2E", "%s", line); }
#endif
static void relay_log(const char *line) { ESP_LOGI(TAG, "%s", line + (strncmp(line, "RELAY: ", 7) ? 0 : 7)); }

static int relay_send(uint8_t type, const uint8_t *d, uint16_t len)
{
    return link_send(type, d, len, 3000);
}

/* ---------- часы узла (секунды, не убывают между перезагрузками) ---------- */

static uint32_t clk_base;

static uint32_t node_clock_s(void) { return clk_base + (uint32_t)(esp_timer_get_time() / 1000000); }

static void clock_load(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open("bxnode", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "clk", &v);
        nvs_close(h);
    }
    clk_base = v;
}

static void clock_save(void)
{
    nvs_handle_t h;
    if (nvs_open("bxnode", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "clk", node_clock_s());
    nvs_commit(h);
    nvs_close(h);
}

/* ---------- хранилище: раздел transit ---------- */

#if !defined(NODE_MODE_E2E)
static const esp_partition_t *transit;

static int fl_read(void *c, uint32_t off, void *b, size_t n)
{
    (void)c;
    return esp_partition_read(transit, off, b, n) == ESP_OK ? 0 : -1;
}

static int fl_write(void *c, uint32_t off, const void *b, size_t n)
{
    (void)c;
    return esp_partition_write(transit, off, b, n) == ESP_OK ? 0 : -1;
}

static int fl_erase(void *c, uint32_t sector)
{
    (void)c;
    return esp_partition_erase_range(transit, sector * ST_SECTOR, ST_SECTOR) == ESP_OK ? 0 : -1;
}

static void store_log(const char *line) { ESP_LOGI("STORE", "%s", line + (strncmp(line, "STORE: ", 7) ? 0 : 7)); }
static void node_log(const char *line) { ESP_LOGI("NODE", "%s", line + (strncmp(line, "NODE: ", 6) ? 0 : 6)); }

static bool store_start(void)
{
    transit = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, TRANSIT_SUBTYPE, "transit");
    if (!transit) {
        ESP_LOGE("STORE", "partition 'transit' not found: flash partition table (idf.py erase-flash flash)");
        return false;
    }
    uint32_t size = transit->size / ST_SECTOR * ST_SECTOR;
    if (size / ST_SECTOR > 2048) size = 2048 * ST_SECTOR;
    uint32_t nsec = size / ST_SECTOR;
    size_t fixed = ST_DELETED_RING * sizeof(uint64_t) + nsec * 12u + 64u;
    size_t mem_size = fixed + INDEX_PSRAM_ENTRIES * sizeof(st_ent_t);
    void *mem = heap_caps_malloc(mem_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const char *where = "PSRAM";
    if (!mem) {
        mem_size = fixed + INDEX_INTERNAL_ENTRIES * sizeof(st_ent_t);
        mem = heap_caps_malloc(mem_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        where = "internal RAM (no PSRAM)";
    }
    if (!mem) { ESP_LOGE("STORE", "no memory for index"); return false; }
    ESP_LOGI("STORE", "partition transit at 0x%lx, %lu KB, %lu sectors; index %u KB in %s",
             (unsigned long)transit->address, (unsigned long)(size / 1024), (unsigned long)nsec,
             (unsigned)(mem_size / 1024), where);
    st_env_t e = {
        .read = fl_read, .write = fl_write, .erase = fl_erase, .ctx = NULL, .size = size,
        .now_s = node_clock_s, .log = store_log, .mem = mem, .mem_size = mem_size,
    };
    int64_t t0 = esp_timer_get_time();
    if (st_init(&e) != 0) { ESP_LOGE("STORE", "mount failed"); return false; }
    /* часы не должны отставать от уже принятых сообщений (если NVS стёрли отдельно) */
    uint32_t newest = st_newest();
    if (newest > node_clock_s()) clk_base += newest - node_clock_s();
    ESP_LOGI("STORE", "mounted in %lld ms, node clock %lu s", (long long)((esp_timer_get_time() - t0) / 1000),
             (unsigned long)node_clock_s());
    return true;
}
static bool store_ok;
#endif

/* ---------- логика узла: e2e или ретранслятор ---------- */

void node_rx_begin(const char *phone)
{
    E2E_LOCK();
#if defined(NODE_MODE_E2E)
    (void)phone;
    e2e_rx_connection_begin();
#else
    rn_rx_begin(phone);
#endif
    E2E_UNLOCK();
}

e2e_rx_action_t node_on_rx(const uint8_t *data, size_t len)
{
    E2E_LOCK();
#if defined(NODE_MODE_E2E)
    e2e_rx_action_t a = e2e_on_rx(data, len);
#else
    e2e_rx_action_t a = store_ok ? rn_on_rx(data, len) : E2E_RX_CONTINUE;
#endif
    E2E_UNLOCK();
    return a;
}

void node_tick(void)
{
    E2E_LOCK();
#if defined(NODE_MODE_E2E)
    e2e_tick();
#else
    if (store_ok) rn_tick();
#endif
    E2E_UNLOCK();
}

static int build_locked(e2e_session_t *s, uint16_t max_payload, const char *phone)
{
    E2E_LOCK();
#if defined(NODE_MODE_E2E)
    (void)phone;
    int n = e2e_build_session(s, max_payload);
#else
    int n = store_ok ? rn_build_session(s, max_payload, phone) : -1;
#endif
    E2E_UNLOCK();
    return n;
}

static void commit_locked(const e2e_session_t *s, int delivered)
{
    E2E_LOCK();
#if defined(NODE_MODE_E2E)
    e2e_commit_session(s, delivered);
#else
    if (store_ok) rn_commit_session(s, delivered);
#endif
    E2E_UNLOCK();
}

/* Из задачи доставки канала: только в очередь, вся логика — в relay_task. */
static void on_link_frame(uint8_t type, const uint8_t *d, uint16_t len)
{
    rmsg_t m = { 0, type, len, {0} };
    if (len > sizeof(m.data)) {
        ESP_LOGW(TAG, "link msg type=0x%02x len=%u too long, dropped", type, len);
        return;
    }
    memcpy(m.data, d, len);
    if (xQueueSend(relay_q, &m, pdMS_TO_TICKS(100)) != pdTRUE) ESP_LOGW(TAG, "relay queue full");
}

static void on_link_event(link_ev_t ev)
{
    rmsg_t m = { 1, (uint8_t)ev, 0, {0} };
    xQueueSend(relay_q, &m, pdMS_TO_TICKS(100));
}

static void relay_task(void *arg)
{
    (void)arg;
    rmsg_t m;
    uint32_t last_status = node_now_ms(), last_clock = node_now_ms();
    for (;;) {
        if (xQueueReceive(relay_q, &m, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (m.is_event) {
                if (m.type == LINK_EV_PEER_RESTARTED) rc_on_peer_restart();
                if (m.type == LINK_EV_UP || m.type == LINK_EV_PEER_RESTARTED) rc_on_link_up();
            } else {
                rc_on_msg(m.type, m.data, m.len);
            }
            uint8_t a[6];
            if (rc_tx_addr(a)) {
                taskENTER_CRITICAL(&addr_mux);
                tx_addr_known = true;
                memcpy(tx_addr, a, 6);
                taskEXIT_CRITICAL(&addr_mux);
            }
        }
        rc_tick(link_ok());

        if (node_now_ms() - last_status >= RELAY_STATUS_MS) {
            last_status = node_now_ms();
            char line[240];
            rc_status_line(line, sizeof(line));
            ESP_LOGI(TAG, "%s", line);
#if !defined(NODE_MODE_E2E)
            if (store_ok) {
                E2E_LOCK();
                rn_status_line(line, sizeof(line));
                ESP_LOGI("NODE", "%s", line);
                st_status_line(line, sizeof(line));
                E2E_UNLOCK();
                ESP_LOGI("STORE", "%s clock=%lus", line, (unsigned long)node_clock_s());
            }
#endif
        }
        if (node_now_ms() - last_clock >= CLOCK_SAVE_MS) {
            last_clock = node_now_ms();
            clock_save();
        }
    }
}

void core_init(void)
{
    g_e2e_lock = xSemaphoreCreateMutex();
    relay_q = xQueueCreate(16, sizeof(rmsg_t));
    configASSERT(g_e2e_lock && relay_q);

    clock_load();
#if defined(NODE_MODE_E2E)
    ESP_LOGI(TAG, "node mode: E2E test (build option NODE_MODE=E2E)");
    e2e_env_t env = {
        .rand32 = esp_random,
        .now_ms = node_now_ms,
        .log = e2e_log,
        .node_suffix = NODE_SUFFIX,
        .ttl = E2E_TTL_DEFAULT,
        .timeout_ms = 15u * 60u * 1000u,
    };
    e2e_init(&env);
#else
    ESP_LOGI(TAG, "node mode: RELAY (transit store, messages kept up to 7 days)");
    store_ok = store_start();
    if (store_ok) {
        rn_env_t ne = { .rand32 = esp_random, .now_ms = node_now_ms, .log = node_log, .node_suffix = NODE_SUFFIX };
        rn_init(&ne);
    }
    clock_save();
#endif

    rc_env_t re = {
        .send = relay_send,
        .now_ms = node_now_ms,
        .log = relay_log,
        .build = build_locked,
        .commit = commit_locked,
        .phone_by_addr = core_phone_by_addr,
    };
    rc_init(&re);

    link_set_handlers(on_link_frame, on_link_event);
    xTaskCreate(relay_task, "relay", 8192, NULL, 5, NULL);
    central_task_start();
}

void core_on_sync(void)
{
    central_on_sync_();
}

#endif /* NODE_ROLE_CORE */
