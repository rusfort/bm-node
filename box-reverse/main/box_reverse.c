/* BOX reverse-channel integration test. ESP-IDF 5.5 / NimBLE.
 * Host-only state machine; no sleeps or GATT waits in GAP callbacks.
 * Not a durable relay: ACK state is bounded and volatile. */
#include "box_reverse.h"
#include "box_protocol.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_npl.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

#if !CONFIG_BT_NIMBLE_ROLE_CENTRAL || !CONFIG_BT_NIMBLE_GATT_CLIENT
#error "Enable NimBLE Central and GATT Client"
#endif
#if CONFIG_EXAMPLE_EXTENDED_ADV
#error "This test requires legacy advertising (EXAMPLE_EXTENDED_ADV=n)"
#endif

#define ACK_CAPACITY BOX_REVERSE_ACK_CAPACITY
#define ACK_TTL_US (60LL * 1000 * 1000)
#define SCAN_MS 6500
#define CONNECT_MS 4500
#define PROCEDURE_MS 5000
#define FIRST_PACKET_MS 4000
#define NEXT_PACKET_MS 1500
#define SESSION_MS 20000
static const char *TAG = "BOX_RX";
/* Android advertises the fixed base UUID but exposes merged service UUIDs. */
static const ble_uuid128_t adv_uuid = BLE_UUID128_INIT(
    0xbc,0x9a,0x78,0x56,0x34,0x12,0x12,0x90,
    0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12);
static const char service_prefix[] = "12345678-1234-5678-9012-";
static const char message_prefix[] = "12345678-1234-5678-aaaa-";

typedef enum { RX_IDLE, RX_SCAN, RX_CONNECT, RX_MTU, RX_SERVICES,
               RX_CHARS, RX_DESCRIPTORS, RX_CCCD, RX_RECEIVE, RX_CLOSING } rx_state_t;
static rx_state_t state;
static struct ble_npl_callout timer;
static struct ble_npl_event resume_event;
static void (*resume_adv)(void);
static uint8_t own_type;
static bool synced;
static uint16_t peripheral_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t rx_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t svc_start, svc_end, chr_handle, chr_end, cccd_handle;
static char current_sid[37];
static int64_t session_started;
/* Reuse static parser buffers only in host context to protect host stack. */
static char rx_bytes[BOX_PROTOCOL_MAX_PACKET + 1];
static box_packet_t parsed;
static uint32_t connection_generation;
static portMUX_TYPE ack_lock = portMUX_INITIALIZER_UNLOCKED;
static struct { char id[37]; int64_t expires; } acks[ACK_CAPACITY];

static int gap_event(struct ble_gap_event *event, void *arg);
static int svc_cb(uint16_t, const struct ble_gatt_error *,
                  const struct ble_gatt_svc *, void *);
static int chr_cb(uint16_t, const struct ble_gatt_error *,
                  const struct ble_gatt_chr *, void *);
static int dsc_cb(uint16_t, const struct ble_gatt_error *, uint16_t,
                  const struct ble_gatt_dsc *, void *);
static int write_cb(uint16_t, const struct ble_gatt_error *,
                    struct ble_gatt_attr *, void *);

static void arm(unsigned ms)
{
    ble_npl_callout_reset(&timer, ble_npl_time_ms_to_ticks32(ms));
}

static void *generation_arg(void)
{
    return (void *)(uintptr_t)connection_generation;
}

static bool live(uint16_t conn, void *arg)
{
    return synced && conn == rx_conn && state != RX_CLOSING &&
           (uint32_t)(uintptr_t)arg == connection_generation;
}

static void resume_cb(struct ble_npl_event *event)
{
    (void)event;
    if (synced && state == RX_IDLE &&
        peripheral_conn == BLE_HS_CONN_HANDLE_NONE && resume_adv &&
        !ble_gap_adv_active()) resume_adv();
}

static void return_to_tx(void)
{
    state = RX_IDLE;
    if (!synced || peripheral_conn != BLE_HS_CONN_HANDLE_NONE) return;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &resume_event);
    /* Jitter prevents phase-locking with Android's advertising/scan cycle. */
    arm(6500 + esp_random() % 4000);
}

static void close_rx(const char *why)
{
    if (state == RX_CLOSING) return;
    ESP_LOGI(TAG, "RX closing: %s", why);
    if (rx_conn == BLE_HS_CONN_HANDLE_NONE) { return_to_tx(); return; }
    state = RX_CLOSING;
    int rc = ble_gap_terminate(rx_conn, BLE_ERR_REM_USER_CONN_TERM);
    if (rc == BLE_HS_ENOTCONN) {
        rx_conn = BLE_HS_CONN_HANDLE_NONE;
        return_to_tx();
    } else {
        if (rc) ESP_LOGW(TAG, "terminate rc=%d; retry on watchdog", rc);
        arm(1000);
    }
}

static void timer_cb(struct ble_npl_event *event)
{
    (void)event;
    if (!synced || peripheral_conn != BLE_HS_CONN_HANDLE_NONE) return;
    if (state == RX_IDLE) {
        /* No outgoing RX connection may overlap a Node TX connection. */
        int rc = ble_gap_adv_stop();
        if (rc != 0 && rc != BLE_HS_EALREADY) { return_to_tx(); return; }
        struct ble_gap_disc_params p = {0};
        p.itvl = 0x50; p.window = 0x30;
        p.passive = 1; p.filter_duplicates = 1;
        state = RX_SCAN;
        rc = ble_gap_disc(own_type, SCAN_MS, &p, gap_event, NULL);
        if (rc) {
            ESP_LOGW(TAG, "scan start rc=%d", rc);
            return_to_tx();
        } else {
            ESP_LOGI(TAG, "RX scan started (%d ms)", SCAN_MS);
            arm(SCAN_MS + 1500);
        }
    } else if (state == RX_SCAN) {
        ble_gap_disc_cancel();
        return_to_tx();
    } else if (state == RX_CONNECT) {
        int rc = ble_gap_conn_cancel();
        ESP_LOGW(TAG, "connect watchdog; cancel rc=%d", rc);
        if (rc == BLE_HS_EALREADY) return_to_tx();
        else arm(1000); /* CONNECT failure callback returns to TX. */
    } else if (state == RX_CLOSING) {
        state = RX_RECEIVE;
        close_rx("disconnect watchdog");
    } else {
        close_rx("GATT / receive timeout");
    }
}

void box_reverse_init(void (*resume_advertising)(void))
{
    resume_adv = resume_advertising;
    ble_npl_event_init(&resume_event, resume_cb, NULL);
    ble_npl_callout_init(&timer, nimble_port_get_dflt_eventq(), timer_cb, NULL);
}

void box_reverse_on_sync(uint8_t own_addr_type)
{
    own_type = own_addr_type;
    synced = true;
    state = RX_IDLE;
    arm(8000 + esp_random() % 3000);
    ESP_LOGI(TAG, "Reverse channel enabled: TX windows alternate with RX scans");
}

void box_reverse_on_reset(void)
{
    synced = false;
    ble_npl_callout_stop(&timer);
    rx_conn = peripheral_conn = BLE_HS_CONN_HANDLE_NONE;
    ++connection_generation;
    state = RX_IDLE;
}

void box_reverse_peripheral_connected(uint16_t conn_handle)
{
    peripheral_conn = conn_handle;
    ble_npl_callout_stop(&timer);
    if (state == RX_SCAN) ble_gap_disc_cancel();
    state = RX_IDLE;
}

void box_reverse_peripheral_disconnected(uint16_t conn_handle)
{
    if (peripheral_conn != conn_handle) return;
    peripheral_conn = BLE_HS_CONN_HANDLE_NONE;
    return_to_tx();
}

static bool add_ack(const char *id)
{
    int slot = -1;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&ack_lock);
    for (int i = 0; i < ACK_CAPACITY; ++i) {
        if (!strcmp(acks[i].id, id)) { slot = i; break; }
        if (slot < 0 && (!acks[i].id[0] || acks[i].expires <= now)) slot = i;
    }
    if (slot >= 0) {
        memcpy(acks[slot].id, id, 37);
        acks[slot].expires = now + ACK_TTL_US;
    }
    portEXIT_CRITICAL(&ack_lock);
    if (slot < 0) ESP_LOGE(TAG, "ACB queue full; callback NOT acknowledged: %s", id);
    return slot >= 0;
}

size_t box_reverse_build_acb(unsigned slot, char *out, size_t capacity)
{
    if (!out || capacity < 44 || slot >= ACK_CAPACITY) return 0;
    int64_t now = esp_timer_get_time();
    size_t used = 0;
    portENTER_CRITICAL(&ack_lock);
    if (acks[slot].id[0] && acks[slot].expires > now) {
        memcpy(out, "V1\nACB\n", 7);
        memcpy(out + 7, acks[slot].id, 37);
        used = 43;
    }
    portEXIT_CRITICAL(&ack_lock);
    return used;
}

static bool uuid_has_prefix(const ble_uuid_t *uuid, const char *prefix)
{
    if (uuid->type != BLE_UUID_TYPE_128) return false;
    char text[BLE_UUID_STR_LEN];
    ble_uuid_to_str(uuid, text);
    return !strncmp(text, prefix, strlen(prefix));
}

static bool advert_matches(const uint8_t *data, size_t len)
{
    /* Parse AD data without allocating a large ble_hs_adv_fields structure. */
    for (size_t pos = 0; pos < len;) {
        size_t n = data[pos];
        if (n == 0) break;
        if (n < 1 || pos + n + 1 > len) return false;
        uint8_t type = data[pos + 1];
        if (type == BLE_HS_ADV_TYPE_COMP_UUIDS128 ||
            type == BLE_HS_ADV_TYPE_INCOMP_UUIDS128) {
            for (size_t off = pos + 2; off + 16 <= pos + n + 1; off += 16)
                if (!memcmp(data + off, adv_uuid.value, 16)) return true;
        }
        pos += n + 1;
    }
    return false;
}

static void discover_services(void)
{
    state = RX_SERVICES;
    arm(PROCEDURE_MS);
    int rc = ble_gattc_disc_all_svcs(rx_conn, svc_cb, generation_arg());
    if (rc) close_rx("cannot start service discovery");
}

static int mtu_cb(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t mtu, void *arg)
{
    if (!live(conn, arg) || state != RX_MTU) return 0;
    ESP_LOGI(TAG, "MTU exchange status=%d mtu=%u", error->status, mtu);
    if (error->status || mtu < 483) { close_rx("MTU too small for Android batches"); return 0; }
    discover_services();
    return 0;
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *svc, void *arg)
{
    if (!live(conn, arg) || state != RX_SERVICES) return 0;
    if (!error->status) {
        if (!svc_start && uuid_has_prefix(&svc->uuid.u, service_prefix)) {
            svc_start = svc->start_handle; svc_end = svc->end_handle;
            ESP_LOGI(TAG, "BOX service handles=%u..%u", svc_start, svc_end);
        }
    } else if (error->status == BLE_HS_EDONE && svc_start) {
        state = RX_CHARS; arm(PROCEDURE_MS);
        int rc = ble_gattc_disc_all_chrs(conn, svc_start, svc_end, chr_cb, arg);
        if (rc) close_rx("cannot start characteristic discovery");
    } else close_rx("service discovery failed / BOX service absent");
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg)
{
    if (!live(conn, arg) || state != RX_CHARS) return 0;
    if (!error->status) {
        if (chr_handle && chr->def_handle > chr_handle && chr_end == svc_end)
            chr_end = chr->def_handle - 1;
        if (!chr_handle && uuid_has_prefix(&chr->uuid.u, message_prefix) &&
            (chr->properties & BLE_GATT_CHR_PROP_NOTIFY)) {
            chr_handle = chr->val_handle; chr_end = svc_end;
        }
    } else if (error->status == BLE_HS_EDONE && chr_handle && chr_end > chr_handle) {
        state = RX_DESCRIPTORS; arm(PROCEDURE_MS);
        int rc = ble_gattc_disc_all_dscs(conn, chr_handle, chr_end, dsc_cb, arg);
        if (rc) close_rx("cannot start CCCD discovery");
    } else close_rx("message characteristic not found / discovery failed");
    return 0;
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t value_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)value_handle;
    if (!live(conn, arg) || state != RX_DESCRIPTORS) return 0;
    if (!error->status) {
        if (ble_uuid_u16(&dsc->uuid.u) == 0x2902) cccd_handle = dsc->handle;
    } else if (error->status == BLE_HS_EDONE && cccd_handle) {
        static const uint8_t enable[] = {1, 0};
        state = RX_CCCD; arm(PROCEDURE_MS);
        int rc = ble_gattc_write_flat(conn, cccd_handle, enable, sizeof(enable), write_cb, arg);
        if (rc) close_rx("cannot write CCCD");
    } else close_rx("CCCD absent / descriptor discovery failed");
    return 0;
}

static int write_cb(uint16_t conn, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    if (!live(conn, arg) || state != RX_CCCD) return 0;
    if (error->status) { close_rx("CCCD write failed"); return 0; }
    state = RX_RECEIVE; arm(FIRST_PACKET_MS);
    ESP_LOGI(TAG, "Subscribed: conn=%u characteristic=%u cccd=%u", conn, chr_handle, cccd_handle);
    return 0;
}

static void on_packet(struct os_mbuf *om)
{
    uint16_t n = OS_MBUF_PKTLEN(om), copied;
    if (n > BOX_PROTOCOL_MAX_PACKET ||
        ble_hs_mbuf_to_flat(om, rx_bytes, sizeof(rx_bytes) - 1, &copied) || copied != n) {
        ESP_LOGW(TAG, "RX oversize / invalid mbuf: bytes=%u", n); return;
    }
    if (!box_protocol_parse(rx_bytes, n, &parsed)) {
        ESP_LOGW(TAG, "Invalid V1 control packet: bytes=%u", n); return;
    }
    if (esp_timer_get_time() - session_started > SESSION_MS * 1000LL) {
        close_rx("session duration limit"); return;
    }
    arm(NEXT_PACKET_MS);
    if (parsed.type == BOX_PACKET_SID) {
        memcpy(current_sid, parsed.id, sizeof(current_sid));
        ESP_LOGI(TAG, "RX SID=%s", current_sid);
    } else if (parsed.type == BOX_PACKET_TRM) {
        if (!current_sid[0] || strcmp(current_sid, parsed.id)) {
            ESP_LOGW(TAG, "TRM session mismatch; ignored"); return;
        }
        ESP_LOGI(TAG, "RX TRM sid=%s", parsed.id);
        close_rx("matching termination received");
    } else if (parsed.type == BOX_PACKET_CBB || parsed.type == BOX_PACKET_CDB) {
        bool ours = false, all_ours = true;
        for (size_t i = 0; i < parsed.box_count; ++i) {
            if (!strcmp(parsed.box_ids[i], BOX_TEST_BOX_ID)) ours = true;
            else all_ours = false;
        }
        const char *kind = parsed.type == BOX_PACKET_CBB ? "CBB" : "CDB";
        ESP_LOGI(TAG, "RX %s id=%s boxes=%u match_test_box=%u", kind, parsed.id,
                 (unsigned)parsed.box_count, (unsigned)ours);
        /* Do not ACK callbacks concerning other senders: that would erase
         * confirmations before their intended sender has received them. */
        if (!ours || !all_ours) {
            ESP_LOGI(TAG, "Callback not ACKed: unknown / mixed box IDs"); return;
        }
        if (add_ack(parsed.id))
            ESP_LOGI(TAG, "ACB pending: callback_id=%s (RAM, retry 60s)", parsed.id);
    } else {
        ESP_LOGI(TAG, "RX other V1 packet (%u bytes); not stored or ACKed", n);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (state != RX_SCAN || peripheral_conn != BLE_HS_CONN_HANDLE_NONE) break;
        if (!advert_matches(event->disc.data, event->disc.length_data)) break;
        /* State changes before cancel so DISC_COMPLETE cannot restart TX. */
        state = RX_CONNECT;
        if (ble_gap_disc_cancel() != 0) { return_to_tx(); break; }
        ESP_LOGI(TAG, "Android candidate found; connecting");
        arm(CONNECT_MS + 1500);
        {
            int rc = ble_gap_connect(own_type, &event->disc.addr, CONNECT_MS,
                                     NULL, gap_event, NULL);
            if (rc) { ESP_LOGW(TAG, "connect start rc=%d", rc); return_to_tx(); }
        }
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (state == RX_SCAN) return_to_tx();
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) {
            ESP_LOGI(TAG, "RX connect failed status=%d", event->connect.status);
            return_to_tx(); break;
        }
        rx_conn = event->connect.conn_handle;
        ++connection_generation;
        svc_start = svc_end = chr_handle = chr_end = cccd_handle = 0;
        current_sid[0] = '\0'; session_started = esp_timer_get_time();
        state = RX_MTU; arm(PROCEDURE_MS);
        ESP_LOGI(TAG, "RX connected: conn=%u", rx_conn);
        {
            int rc = ble_gattc_exchange_mtu(rx_conn, mtu_cb, generation_arg());
            if (rc) close_rx("cannot start MTU exchange");
        }
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.conn_handle == rx_conn &&
            event->notify_rx.attr_handle == chr_handle &&
            (state == RX_RECEIVE || state == RX_CCCD))
            on_packet(event->notify_rx.om);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "RX disconnected: reason=%d", event->disconnect.reason);
        rx_conn = BLE_HS_CONN_HANDLE_NONE; ++connection_generation;
        return_to_tx();
        break;
    default: break;
    }
    return 0;
}
