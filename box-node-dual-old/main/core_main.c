/*
 * Роль CORE: автомат теста e2e, центральная роль BLE (приём от Android)
 * и менеджер сессий, которые отправляет телефону плата TX.
 */
#if defined(NODE_ROLE_CORE)

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "node.h"
#include "core_glue.h"
#include "e2e_test.h"
#include "link_glue.h"
#include "relay.h"

#define RELAY_STATUS_MS 30000

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

static void e2e_log(const char *line) { ESP_LOGI("E2E", "%s", line); }
static void relay_log(const char *line) { ESP_LOGI(TAG, "%s", line + (strncmp(line, "RELAY: ", 7) ? 0 : 7)); }

static int relay_send(uint8_t type, const uint8_t *d, uint16_t len)
{
    return link_send(type, d, len, 3000);
}

static int build_locked(e2e_session_t *s, uint16_t max_payload)
{
    E2E_LOCK();
    int n = e2e_build_session(s, max_payload);
    E2E_UNLOCK();
    return n;
}

static void commit_locked(const e2e_session_t *s, int delivered)
{
    E2E_LOCK();
    e2e_commit_session(s, delivered);
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
    uint32_t last_status = node_now_ms();
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
            char line[200];
            rc_status_line(line, sizeof(line));
            ESP_LOGI(TAG, "%s", line);
        }
    }
}

void core_init(void)
{
    g_e2e_lock = xSemaphoreCreateMutex();
    relay_q = xQueueCreate(16, sizeof(rmsg_t));
    configASSERT(g_e2e_lock && relay_q);

    e2e_env_t env = {
        .rand32 = esp_random,
        .now_ms = node_now_ms,
        .log = e2e_log,
        .node_suffix = NODE_SUFFIX,
        .ttl = E2E_TTL_DEFAULT,
        .timeout_ms = 15u * 60u * 1000u,
    };
    e2e_init(&env);

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
    xTaskCreate(relay_task, "relay", 6144, NULL, 5, NULL);
    central_task_start();
}

void core_on_sync(void)
{
    central_on_sync_();
}

#endif /* NODE_ROLE_CORE */
