/*
 * Центральная роль Box Node: ищет Android с Box Messenger, подключается к его
 * GATT server, находит сервис/характеристику по префиксам UUID, подписывается
 * на notifications и передаёт принятые пакеты в автомат e2e_test.
 */
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"

#include "node.h"
#include "e2e_test.h"

static const char *TAG = "BOX_RX";

#define SCAN_MS           8000
#define HOLD_MAX_MS       40000   /* максимум удержания соединения без TRM */
#define CLOSE_DELAY_MS    250     /* после TRM: дать дойти второму TRM */
#define COOLDOWN_MS       1500

/* Advertising UUID Box Messenger: 12345678-1234-5678-9012-123456789abc */
static const ble_uuid128_t adv_uuid = BLE_UUID128_INIT(
    0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, 0x12, 0x90,
    0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

typedef enum { C_IDLE = 0, C_SCANNING, C_CONNECTING, C_CONNECTED } cstate_t;

static volatile cstate_t cstate = C_IDLE;
static volatile bool     synced;
static volatile uint16_t c_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint32_t c_conn_at, c_close_at, c_next_scan_at;
static volatile bool     c_subscribed;

static uint16_t svc_start, svc_end, chr_val, chr_end, cccd;
static char     peer_suffix[13];

typedef struct {
    uint16_t len;
    uint8_t  data[BOX_MAX_PACKET];
} rx_item_t;

static QueueHandle_t rx_q;

static int central_gap_event(struct ble_gap_event *event, void *arg);

/* ---------- утилиты ---------- */

static bool uuid_has_prefix(const ble_uuid_any_t *u, const char *prefix, char suffix_out[13])
{
    if (u->u.type != BLE_UUID_TYPE_128) return false;
    char s[BLE_UUID_STR_LEN];
    ble_uuid_to_str(&u->u, s);
    if (strncmp(s, prefix, strlen(prefix)) != 0) return false;
    if (suffix_out) {
        memcpy(suffix_out, s + 24, 12);
        suffix_out[12] = '\0';
    }
    return true;
}

static void drop(const char *why, int rc)
{
    ESP_LOGW(TAG, "%s rc=%d -> disconnect", why, rc);
    if (c_conn != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(c_conn, BLE_ERR_REM_USER_CONN_TERM);
}

/* ---------- discovery chain: MTU -> service -> chr -> CCCD -> subscribe ---------- */

static int on_subscribed(uint16_t conn, const struct ble_gatt_error *err,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)attr; (void)arg;
    if (err->status != 0) { drop("CCCD write failed", err->status); return 0; }
    c_subscribed = true;
    ESP_LOGI(TAG, "subscribed to Android %s (conn=%u val=%u cccd=%u), waiting for its session",
             peer_suffix, conn, chr_val, cccd);
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle; (void)arg;
    if (err->status == 0) {
        if (dsc->uuid.u.type == BLE_UUID_TYPE_16 &&
            ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
            cccd = dsc->handle;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) { drop("descriptor discovery failed", err->status); return 0; }

    if (cccd == 0) {
        /* Android всё равно шлёт notifications всем подключённым клиентам. */
        ESP_LOGW(TAG, "no CCCD found, relying on unsolicited notifications");
        c_subscribed = true;
        return 0;
    }
    static const uint8_t on[2] = { 0x01, 0x00 };
    int rc = ble_gattc_write_flat(conn, cccd, on, sizeof(on), on_subscribed, NULL);
    if (rc != 0) drop("CCCD write start failed", rc);
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (err->status == 0) {
        if (chr_val != 0 && chr_end == 0) chr_end = chr->def_handle - 1;
        char sfx[13];
        if (chr_val == 0 && uuid_has_prefix(&chr->uuid, BOX_CHR_PREFIX, sfx)) {
            chr_val = chr->val_handle;
            memcpy(peer_suffix, sfx, sizeof(sfx));
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) { drop("characteristic discovery failed", err->status); return 0; }
    if (chr_val == 0) { drop("Box characteristic not found", 0); return 0; }
    if (strcmp(peer_suffix, NODE_SUFFIX) == 0) { drop("self loop: peer has our own ID", 0); return 0; }
    if (chr_end == 0) chr_end = svc_end;

    ESP_LOGI(TAG, "Box characteristic found: peer_id=%s val_handle=%u", peer_suffix, chr_val);
    E2E_LOCK();
    e2e_rx_connection_begin();
    E2E_UNLOCK();

    int rc = ble_gattc_disc_all_dscs(conn, chr_val, chr_end, on_dsc, NULL);
    if (rc != 0) drop("descriptor discovery start failed", rc);
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (err->status == 0) {
        if (svc_start == 0 && uuid_has_prefix(&svc->uuid, BOX_SVC_PREFIX, NULL)) {
            svc_start = svc->start_handle;
            svc_end = svc->end_handle;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) { drop("service discovery failed", err->status); return 0; }
    if (svc_start == 0) { drop("Box service not found", 0); return 0; }
    int rc = ble_gattc_disc_all_chrs(conn, svc_start, svc_end, on_chr, NULL);
    if (rc != 0) drop("characteristic discovery start failed", rc);
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "MTU exchange status=%d mtu=%u", err->status, mtu);
    int rc = ble_gattc_disc_all_svcs(conn, on_svc, NULL);
    if (rc != 0) drop("service discovery start failed", rc);
    return 0;
}

/* ---------- GAP ---------- */

static bool adv_is_box(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) return false;
    for (int i = 0; i < f.num_uuids128; i++)
        if (ble_uuid_cmp(&f.uuids128[i].u, &adv_uuid.u) == 0) return true;
    return false;
}

static void start_scan(void)
{
    struct ble_gap_disc_params p = {0};
    p.filter_duplicates = 1;
    p.passive = 0;               /* активное: UUID может быть и в scan response */
    int rc = ble_gap_disc(g_own_addr_type, SCAN_MS, &p, central_gap_event, NULL);
    if (rc == 0) {
        cstate = C_SCANNING;
    } else {
        ESP_LOGW(TAG, "scan start rc=%d", rc);
        c_next_scan_at = node_now_ms() + COOLDOWN_MS;
    }
}

static int central_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (cstate != C_SCANNING || !adv_is_box(&event->disc)) return 0;
        uint8_t own[6];
        ble_hs_id_copy_addr(g_own_addr_type, own, NULL);
        if (memcmp(own, event->disc.addr.val, 6) == 0) return 0;   /* свой TX */

        ble_gap_disc_cancel();
        const uint8_t *a = event->disc.addr.val;
        ESP_LOGI(TAG, "Box device %02x:%02x:%02x:%02x:%02x:%02x rssi=%d -> connect",
                 a[5], a[4], a[3], a[2], a[1], a[0], event->disc.rssi);
        cstate = C_CONNECTING;
        int rc = ble_gap_connect(g_own_addr_type, &event->disc.addr, 5000, NULL,
                                 central_gap_event, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "connect rc=%d", rc);
            cstate = C_IDLE;
            c_next_scan_at = node_now_ms() + COOLDOWN_MS;
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (cstate == C_SCANNING) {
            cstate = C_IDLE;
            c_next_scan_at = node_now_ms() + 300;
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "outgoing connect failed status=%d", event->connect.status);
            cstate = C_IDLE;
            c_next_scan_at = node_now_ms() + COOLDOWN_MS;
            return 0;
        }
        c_conn = event->connect.conn_handle;
        c_conn_at = node_now_ms();
        c_close_at = 0;
        c_subscribed = false;
        svc_start = svc_end = chr_val = chr_end = cccd = 0;
        peer_suffix[0] = '\0';
        cstate = C_CONNECTED;
        ESP_LOGI(TAG, "connected to Android GATT server conn=%u", c_conn);
        {
            int rc = ble_gattc_exchange_mtu(c_conn, on_mtu, NULL);
            if (rc != 0) drop("MTU exchange start failed", rc);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle == c_conn) {
            ESP_LOGI(TAG, "disconnected from Android conn=%u reason=%d",
                     c_conn, event->disconnect.reason);
            c_conn = BLE_HS_CONN_HANDLE_NONE;
            cstate = C_IDLE;
            c_next_scan_at = node_now_ms() + COOLDOWN_MS;
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.conn_handle != c_conn) return 0;
        static rx_item_t it;      /* выполняется только в host task */
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len == 0 || len > BOX_MAX_PACKET) {
            ESP_LOGW(TAG, "notification len=%u dropped", len);
            return 0;
        }
        it.len = len;
        os_mbuf_copydata(event->notify_rx.om, 0, len, it.data);
        if (xQueueSend(rx_q, &it, 0) != pdTRUE) ESP_LOGW(TAG, "rx queue full, packet dropped");
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        return 0;

    default:
        return 0;
    }
}

/* ---------- задачи ---------- */

static void rx_worker(void *arg)
{
    (void)arg;
    static rx_item_t it;
    for (;;) {
        if (xQueueReceive(rx_q, &it, portMAX_DELAY) != pdTRUE) continue;
        E2E_LOCK();
        e2e_rx_action_t act = e2e_on_rx(it.data, it.len);
        E2E_UNLOCK();
        if (act == E2E_RX_CLOSE && c_close_at == 0)
            c_close_at = node_now_ms() + CLOSE_DELAY_MS;
    }
}

static void central_mgr(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));

        E2E_LOCK();
        e2e_tick();
        E2E_UNLOCK();

        if (!synced) continue;
        uint32_t now = node_now_ms();

        switch (cstate) {
        case C_IDLE:
            if ((int32_t)(now - c_next_scan_at) >= 0) start_scan();
            break;
        case C_CONNECTED:
            if (c_close_at && (int32_t)(now - c_close_at) >= 0) {
                ESP_LOGI(TAG, "Android session done -> disconnect");
                c_close_at = 0;
                ble_gap_terminate(c_conn, BLE_ERR_REM_USER_CONN_TERM);
            } else if (now - c_conn_at > HOLD_MAX_MS) {
                ESP_LOGI(TAG, "hold timeout without TRM -> disconnect");
                c_conn_at = now;
                ble_gap_terminate(c_conn, BLE_ERR_REM_USER_CONN_TERM);
            }
            break;
        default:
            break;
        }
    }
}

void central_on_sync(void)
{
    synced = true;
    c_next_scan_at = node_now_ms() + 500;
}

void central_task_start(void)
{
    rx_q = xQueueCreate(24, sizeof(rx_item_t));
    configASSERT(rx_q);
    xTaskCreate(rx_worker, "box_rx", 6144, NULL, 5, NULL);
    xTaskCreate(central_mgr, "box_central", 4096, NULL, 4, NULL);
}
