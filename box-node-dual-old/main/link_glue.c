#include "link_glue.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "node.h"

#define LINK_UART        UART_NUM_1
#define LINK_TX_GPIO     17
#define LINK_RX_GPIO     18
#define LINK_BAUD        115200
#define STATS_PERIOD_MS  30000
#define DELIVER_Q_LEN    16

static const char *TAG = "LINK";

typedef struct {
    uint8_t  is_event;
    uint8_t  type;          /* тип кадра или link_ev_t */
    uint16_t len;
    uint8_t  data[BL_MAX_DATA];
} deliver_t;

static bl_link_t *link_;
static SemaphoreHandle_t lock;
static QueueHandle_t deliver_q;
static link_frame_cb_t frame_cb;
static link_event_cb_t event_cb;
static uint32_t deliver_drops;

static void uart_write(const uint8_t *d, size_t n, void *ctx)
{
    (void)ctx;
    uart_write_bytes(LINK_UART, d, n);
}

/* Вызываются под lock из задач канала: только копируем в очередь. */
static void on_frame(uint8_t type, const uint8_t *d, uint16_t len, void *ctx)
{
    (void)ctx;
    static deliver_t it;
    it.is_event = 0;
    it.type = type;
    it.len = len;
    memcpy(it.data, d, len);
    if (xQueueSend(deliver_q, &it, 0) != pdTRUE) deliver_drops++;
}

static void on_event(const char *what, void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "event: %s (peer role=%s boot=%04x)", what,
             bl_role_name(bl_peer_role(link_)), bl_peer_boot_id(link_));
    static deliver_t it;
    it.is_event = 1;
    it.len = 0;
    if (strcmp(what, "link up") == 0) it.type = LINK_EV_UP;
    else if (strcmp(what, "link down") == 0) it.type = LINK_EV_DOWN;
    else if (strcmp(what, "peer restarted") == 0) it.type = LINK_EV_PEER_RESTARTED;
    else it.type = LINK_EV_SEND_FAILED;
    if (xQueueSend(deliver_q, &it, 0) != pdTRUE) deliver_drops++;
}

static void print_stats(void)
{
    const bl_stats_t *s = bl_stats(link_);
    ESP_LOGI(TAG, "link=%s peer=%s boot=%04x | rx=%" PRIu32 " tx=%" PRIu32 " retx=%" PRIu32
             " dup=%" PRIu32 " failed=%" PRIu32 " | crc=%" PRIu32 " cobs=%" PRIu32 " len=%" PRIu32
             " | restarts=%" PRIu32 " ups=%" PRIu32 " downs=%" PRIu32 " loop=%" PRIu32 " drops=%" PRIu32,
             bl_link_up(link_) ? "UP" : "DOWN", bl_role_name(bl_peer_role(link_)), bl_peer_boot_id(link_),
             s->frames_rx_ok, s->frames_tx, s->retransmits, s->dup_rx, s->send_failed,
             s->crc_errors, s->cobs_errors, s->len_errors,
             s->peer_restarts, s->link_ups, s->link_downs, s->loopback, deliver_drops);
}

static void rx_task(void *arg)
{
    (void)arg;
    static uint8_t buf[256];
    for (;;) {
        int n = uart_read_bytes(LINK_UART, buf, sizeof(buf), pdMS_TO_TICKS(10));
        if (n > 0) {
            xSemaphoreTake(lock, portMAX_DELAY);
            bl_rx_bytes(link_, buf, (size_t)n);
            xSemaphoreGive(lock);
        }
    }
}

static void poll_task(void *arg)
{
    (void)arg;
    uint32_t last = node_now_ms();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreTake(lock, portMAX_DELAY);
        bl_poll(link_);
        if (node_now_ms() - last >= STATS_PERIOD_MS) {
            last = node_now_ms();
            print_stats();
        }
        xSemaphoreGive(lock);
    }
}

static void deliver_task(void *arg)
{
    (void)arg;
    static deliver_t it;
    for (;;) {
        if (xQueueReceive(deliver_q, &it, portMAX_DELAY) != pdTRUE) continue;
        if (it.is_event) { if (event_cb) event_cb((link_ev_t)it.type); }
        else if (frame_cb) frame_cb(it.type, it.data, it.len);
    }
}

void link_set_handlers(link_frame_cb_t fcb, link_event_cb_t ecb)
{
    frame_cb = fcb;
    event_cb = ecb;
}

void link_start(uint8_t role, uint16_t boot_id)
{
    ESP_LOGI(TAG, "UART%d TX=GPIO%d RX=GPIO%d %d 8N1 role=%s boot=%04x",
             LINK_UART, LINK_TX_GPIO, LINK_RX_GPIO, LINK_BAUD, bl_role_name(role), boot_id);

    const uart_config_t uc = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 8192, 8192, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, LINK_TX_GPIO, LINK_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_pullup_en(LINK_RX_GPIO);

    link_ = malloc(bl_link_size());
    lock = xSemaphoreCreateMutex();
    deliver_q = xQueueCreate(DELIVER_Q_LEN, sizeof(deliver_t));
    configASSERT(link_ && lock && deliver_q);

    bl_config_t cfg = {
        .role = role,
        .boot_id = boot_id,
        .now_ms = node_now_ms,
        .write = uart_write,
        .on_frame = on_frame,
        .on_event = on_event,
        .ctx = NULL,
    };
    xSemaphoreTake(lock, portMAX_DELAY);
    bl_init(link_, &cfg);
    xSemaphoreGive(lock);

    xTaskCreate(rx_task, "link_rx", 4096, NULL, 7, NULL);
    xTaskCreate(poll_task, "link_poll", 4096, NULL, 6, NULL);
    xTaskCreate(deliver_task, "link_dlv", 6144, NULL, 5, NULL);
}

int link_send(uint8_t type, const void *data, uint16_t len, uint32_t wait_ms)
{
    uint32_t start = node_now_ms();
    for (;;) {
        xSemaphoreTake(lock, portMAX_DELAY);
        bool up = bl_link_up(link_);
        int rc = up ? bl_send(link_, type, (const uint8_t *)data, len, true) : -1;
        xSemaphoreGive(lock);
        if (rc == 0) return 0;
        if (node_now_ms() - start >= wait_ms) return -1;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool link_ok(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = bl_link_up(link_) && bl_peer_role(link_) == PEER_ROLE;
    xSemaphoreGive(lock);
    return ok;
}
