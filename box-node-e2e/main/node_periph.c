/*
 * Периферия Box Node: advertising Box Messenger, GATT-сервис узла и
 * передающие сессии SID -> ACB -> CBB -> CDB -> BOX -> TRM -> TRM.
 */
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "node.h"
#include "e2e_test.h"

static const char *TAG = "BOX_TX";

/* Advertising UUID, который ищет скан-фильтр Android: 12345678-1234-5678-9012-123456789abc */
static const ble_uuid128_t adv_uuid = BLE_UUID128_INIT(
    0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, 0x12, 0x90,
    0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

/* Service: 12345678-1234-5678-9012-aabbccddeeff */
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x12, 0x90,
    0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

/* Characteristic: 12345678-1234-5678-aaaa-aabbccddeeff */
static const ble_uuid128_t chr_uuid = BLE_UUID128_INIT(
    0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0xaa, 0xaa,
    0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

static uint16_t chr_val_handle;

#define MAX_SUBS 4
static struct {
    uint16_t conn;
    bool     subscribed;
    bool     served;        /* сессия для этой подписки уже отправлена */
} subs[MAX_SUBS];

static QueueHandle_t tx_q;           /* conn_handle для запуска сессии */
static SemaphoreHandle_t tx_done;    /* сигнал NOTIFY_TX */
static volatile int tx_status;
static volatile uint16_t tx_conn = BLE_HS_CONN_HANDLE_NONE;

static int periph_gap_event(struct ble_gap_event *event, void *arg);

/* ---------- GATT ---------- */

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        static const char v[] = "BOX-NODE-E2E";
        return os_mbuf_append(ctxt->om, v, sizeof(v) - 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        ESP_LOGI(TAG, "write from conn=%u len=%u (ignored in E2E)",
                 conn_handle, (unsigned)OS_MBUF_PKTLEN(ctxt->om));
        return 0;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &chr_uuid.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &chr_val_handle,
            },
            { 0 }
        },
    },
    { 0 }
};

int periph_gatt_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(services);
    if (rc != 0) return rc;
    return ble_gatts_add_svcs(services);
}

/* ---------- advertising ---------- */

static void advertise(void)
{
    struct ble_hs_adv_fields f = {0}, rsp = {0};
    struct ble_gap_adv_params p = {0};

    if (ble_gap_adv_active()) return;

    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &adv_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) { ESP_LOGE(TAG, "adv fields rc=%d", rc); return; }

    rsp.name = (const uint8_t *)NODE_DEVICE_NAME;
    rsp.name_len = strlen(NODE_DEVICE_NAME);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) { ESP_LOGE(TAG, "scan rsp rc=%d", rc); return; }

    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(g_own_addr_type, NULL, BLE_HS_FOREVER, &p, periph_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising Box UUID as %s", NODE_DEVICE_NAME);
}

void periph_start(void)
{
    advertise();
}

/* ---------- подписчики ---------- */

static void sub_set(uint16_t conn, bool on)
{
    int free_i = -1;
    for (int i = 0; i < MAX_SUBS; i++) {
        if (subs[i].subscribed && subs[i].conn == conn) {
            subs[i].subscribed = on;
            if (on) subs[i].served = false;
            return;
        }
        if (!subs[i].subscribed && free_i < 0) free_i = i;
    }
    if (on && free_i >= 0) {
        subs[free_i].conn = conn;
        subs[free_i].subscribed = true;
        subs[free_i].served = false;
    }
}

static void sub_mark_served(uint16_t conn)
{
    for (int i = 0; i < MAX_SUBS; i++)
        if (subs[i].subscribed && subs[i].conn == conn) subs[i].served = true;
}

bool periph_conn_busy(uint16_t conn)
{
    if (tx_conn == conn) return true;
    for (int i = 0; i < MAX_SUBS; i++)
        if (subs[i].subscribed && subs[i].conn == conn && !subs[i].served) return true;
    return false;
}

static bool sub_is(uint16_t conn)
{
    for (int i = 0; i < MAX_SUBS; i++)
        if (subs[i].subscribed && subs[i].conn == conn) return true;
    return false;
}

static int periph_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "incoming connect status=%d conn=%u",
                 event->connect.status, event->connect.conn_handle);
        if (event->connect.status != 0) advertise();
        else advertise();          /* продолжаем объявляться для следующих клиентов */
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "incoming disconnect conn=%u reason=%d",
                 event->disconnect.conn.conn_handle, event->disconnect.reason);
        sub_set(event->disconnect.conn.conn_handle, false);
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu conn=%u mtu=%u", event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
    case BLE_GAP_EVENT_NOTIFY_TX:
        return periph_conn_event(event);

    default:
        return 0;
    }
}

int periph_conn_event(struct ble_gap_event *event)
{
    switch (event->type) {
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == chr_val_handle) {
            uint16_t c = event->subscribe.conn_handle;
            bool on = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "subscription conn=%u notify=%d", c, on);
            sub_set(c, on);
            if (on) xQueueSend(tx_q, &c, 0);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.conn_handle == tx_conn &&
            event->notify_tx.attr_handle == chr_val_handle) {
            tx_status = event->notify_tx.status;
            xSemaphoreGive(tx_done);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        sub_set(event->disconnect.conn.conn_handle, false);
        return 0;

    default:
        return 0;
    }
}

/* ---------- передающая сессия ---------- */

static int send_one(uint16_t conn, const char *data, uint16_t len)
{
    for (int attempt = 0; attempt < 20; attempt++) {
        if (!sub_is(conn)) return BLE_HS_ENOTCONN;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        if (om == NULL) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }

        xSemaphoreTake(tx_done, 0);
        tx_status = -1;
        int rc = ble_gatts_notify_custom(conn, chr_val_handle, om);  /* om освобождается стеком */
        if (rc == BLE_HS_ENOMEM) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (rc != 0) return rc;

        if (xSemaphoreTake(tx_done, pdMS_TO_TICKS(500)) != pdTRUE) return BLE_HS_ETIMEOUT;
        return tx_status;
    }
    return BLE_HS_ENOMEM;
}

static void tx_task(void *arg)
{
    (void)arg;
    static e2e_session_t s;   /* ~4 КБ — статически */
    uint16_t conn;

    for (;;) {
        if (xQueueReceive(tx_q, &conn, pdMS_TO_TICKS(1000)) != pdTRUE) continue;

        vTaskDelay(pdMS_TO_TICKS(150));       /* дать Android закончить запись CCCD */
        if (!sub_is(conn)) continue;

        uint16_t mtu = ble_att_mtu(conn);
        if (mtu < 23) { ESP_LOGW(TAG, "conn=%u no MTU", conn); continue; }
        uint16_t payload = mtu - 3;

        E2E_LOCK();
        int n = e2e_build_session(&s, payload);
        E2E_UNLOCK();

        ESP_LOGI(TAG, "session start conn=%u mtu=%u sid=%s packets=%d", conn, mtu, s.sid, n);
        tx_conn = conn;
        int delivered = 0;
        for (int i = 0; i < n; i++) {
            int rc = send_one(conn, s.pkt[i].data, s.pkt[i].len);
            ESP_LOGI(TAG, "  notify %-12s len=%3u -> %s (rc=%d)", e2e_pkt_kind_name(s.pkt[i].kind),
                     s.pkt[i].len, rc == 0 ? "ok" : "FAILED", rc);
            if (rc != 0) break;
            delivered++;
            vTaskDelay(pdMS_TO_TICKS(10));    /* GATT_SEND_INTERVAL_MS, как в Android */
        }
        tx_conn = BLE_HS_CONN_HANDLE_NONE;

        sub_mark_served(conn);

        E2E_LOCK();
        e2e_commit_session(&s, delivered);
        E2E_UNLOCK();
    }
}

void periph_task_start(void)
{
    tx_q = xQueueCreate(8, sizeof(uint16_t));
    tx_done = xSemaphoreCreateBinary();
    configASSERT(tx_q && tx_done);
    xTaskCreate(tx_task, "box_tx", 6144, NULL, 5, NULL);
}
