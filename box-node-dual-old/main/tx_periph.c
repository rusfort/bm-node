/*
 * Роль TX: advertising Box Messenger, GATT server узла и отправка телефону
 * сессий, которые строит CORE и присылает по UART.
 *
 *   телефон подписался  -> RL_SUBSCRIBED(conn, mtu)      -> CORE
 *   CORE                -> RL_SESS_PKT x N (одна сессия) -> TX
 *   TX отправил notifications -> RL_SESS_DONE(delivered)  -> CORE
 */
#if defined(NODE_ROLE_TX)

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
#include "link_glue.h"
#include "relay.h"

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
static volatile bool synced;

#define MAX_SUBS 4
static struct {
    uint16_t conn;
    bool     subscribed;
    bool     served;        /* сессия для этой подписки уже отправлена */
} subs[MAX_SUBS];
static portMUX_TYPE subs_mux = portMUX_INITIALIZER_UNLOCKED;

/* Управляющие события для tx_task. */
typedef struct {
    uint16_t conn;
    uint8_t  what;          /* CTL_* */
} ctl_t;
enum { CTL_SUB_ON = 0, CTL_SUB_OFF, CTL_RESYNC };

static QueueHandle_t ctl_q;
static QueueHandle_t sess_q;          /* rt_session_t, длина 1 */
static SemaphoreHandle_t tx_done;
static volatile int tx_status;
static volatile uint16_t tx_conn = BLE_HS_CONN_HANDLE_NONE;
static rt_asm_t asm_;                 /* только из задачи доставки канала */

static int gap_event(struct ble_gap_event *event, void *arg);

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

int tx_gatt_init(void)
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

    if (!synced || ble_gap_adv_active()) return;

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
    rc = ble_gap_adv_start(g_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising Box UUID as %s", NODE_DEVICE_NAME);
}

/* ---------- подписчики ---------- */

static void sub_set(uint16_t conn, bool on)
{
    taskENTER_CRITICAL(&subs_mux);
    int free_i = -1;
    for (int i = 0; i < MAX_SUBS; i++) {
        if (subs[i].subscribed && subs[i].conn == conn) {
            subs[i].subscribed = on;
            if (on) subs[i].served = false;
            taskEXIT_CRITICAL(&subs_mux);
            return;
        }
        if (!subs[i].subscribed && free_i < 0) free_i = i;
    }
    if (on && free_i >= 0) {
        subs[free_i].conn = conn;
        subs[free_i].subscribed = true;
        subs[free_i].served = false;
    }
    taskEXIT_CRITICAL(&subs_mux);
}

static void sub_mark_served(uint16_t conn)
{
    taskENTER_CRITICAL(&subs_mux);
    for (int i = 0; i < MAX_SUBS; i++)
        if (subs[i].subscribed && subs[i].conn == conn) subs[i].served = true;
    taskEXIT_CRITICAL(&subs_mux);
}

static bool sub_is(uint16_t conn)
{
    bool r = false;
    taskENTER_CRITICAL(&subs_mux);
    for (int i = 0; i < MAX_SUBS; i++)
        if (subs[i].subscribed && subs[i].conn == conn) r = true;
    taskEXIT_CRITICAL(&subs_mux);
    return r;
}

static void ctl_post(uint16_t conn, uint8_t what)
{
    ctl_t c = { conn, what };
    if (xQueueSend(ctl_q, &c, 0) != pdTRUE) ESP_LOGW(TAG, "ctl queue full (what=%u)", what);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        struct ble_gap_conn_desc desc;
        if (event->connect.status == 0 && ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
            const uint8_t *a = desc.peer_ota_addr.val;
            ESP_LOGI(TAG, "incoming connect conn=%u peer=%02x:%02x:%02x:%02x:%02x:%02x/%u",
                     event->connect.conn_handle, a[5], a[4], a[3], a[2], a[1], a[0],
                     desc.peer_ota_addr.type);
        } else {
            ESP_LOGI(TAG, "incoming connect status=%d conn=%u",
                     event->connect.status, event->connect.conn_handle);
        }
        advertise();           /* продолжаем объявляться для следующих клиентов */
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t c = event->disconnect.conn.conn_handle;
        ESP_LOGI(TAG, "incoming disconnect conn=%u reason=%d", c, event->disconnect.reason);
        bool was = sub_is(c);
        sub_set(c, false);
        if (was) ctl_post(c, CTL_SUB_OFF);
        advertise();
        return 0;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu conn=%u mtu=%u", event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == chr_val_handle) {
            uint16_t c = event->subscribe.conn_handle;
            bool on = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "subscription conn=%u notify=%d", c, on);
            sub_set(c, on);
            ctl_post(c, on ? CTL_SUB_ON : CTL_SUB_OFF);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.conn_handle == tx_conn &&
            event->notify_tx.attr_handle == chr_val_handle) {
            tx_status = event->notify_tx.status;
            xSemaphoreGive(tx_done);
        }
        return 0;

    default:
        return 0;
    }
}

/* ---------- канал с CORE ---------- */

static void send_done(uint16_t token, uint16_t conn, int delivered, int count, int rc)
{
    uint8_t b[8];
    size_t n = rl_enc_sess_done(b, token, conn, (uint8_t)delivered, (uint8_t)count, (int16_t)rc);
    if (link_send(RL_SESS_DONE, b, (uint16_t)n, 3000) != 0)
        ESP_LOGW(TAG, "SESS_DONE token=%04x not queued (link down)", token);
}

static void on_link_frame(uint8_t type, const uint8_t *d, uint16_t len)
{
    switch (type) {
    case RL_SYNC_REQ:
        ctl_post(0, CTL_RESYNC);
        break;
    case RL_SESS_PKT: {
        rt_result_t r = rt_on_pkt(&asm_, d, len);
        if (r == RT_ERROR) {
            ESP_LOGW(TAG, "bad session packet len=%u", len);
            if (len >= 4) send_done((uint16_t)(d[0] | d[1] << 8), (uint16_t)(d[2] | d[3] << 8), 0, 0,
                                    RL_RC_BAD_SESSION);
        } else if (r == RT_COMPLETE) {
            if (xQueueSend(sess_q, &asm_.s, 0) != pdTRUE) {
                ESP_LOGW(TAG, "session token=%04x rejected: busy", asm_.s.token);
                send_done(asm_.s.token, asm_.s.conn, 0, asm_.s.count, RL_RC_BUSY);
            }
        }
        break;
    }
    default:
        ESP_LOGW(TAG, "unexpected link msg type=0x%02x len=%u", type, len);
        break;
    }
}

static void on_link_event(link_ev_t ev)
{
    if (ev == LINK_EV_UP || ev == LINK_EV_PEER_RESTARTED) ctl_post(0, CTL_RESYNC);
}

static int send_subscribed(uint16_t conn, uint32_t wait_ms)
{
    uint8_t b[11], a[6] = {0}, at = 0xff;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) == 0) {
        memcpy(a, desc.peer_ota_addr.val, 6);
        at = desc.peer_ota_addr.type;
    }
    uint16_t mtu = ble_att_mtu(conn);
    ESP_LOGI(TAG, "subscribed conn=%u mtu=%u -> CORE", conn, mtu);
    return link_send(RL_SUBSCRIBED, b, (uint16_t)rl_enc_subscribed(b, conn, mtu, at, a), wait_ms);
}

static void resync(void)
{
    if (synced) {
        uint8_t a[6], b[7];
        ble_hs_id_copy_addr(g_own_addr_type, a, NULL);
        link_send(RL_TX_INFO, b, (uint16_t)rl_enc_tx_info(b, g_own_addr_type, a), 1000);
    }
    uint16_t conns[MAX_SUBS];
    int n = 0;
    taskENTER_CRITICAL(&subs_mux);
    for (int i = 0; i < MAX_SUBS; i++)
        if (subs[i].subscribed && !subs[i].served) conns[n++] = subs[i].conn;
    taskEXIT_CRITICAL(&subs_mux);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "resync: pending subscription conn=%u -> CORE", conns[i]);
        send_subscribed(conns[i], 1000);
    }
}

/* ---------- отправка сессии ---------- */

static int send_one(uint16_t conn, const uint8_t *data, uint16_t len)
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

static const char *kind_name(uint8_t k) { return e2e_pkt_kind_name((e2e_pkt_kind_t)k); }

static void run_session(const rt_session_t *s)
{
    if (!sub_is(s->conn)) {
        ESP_LOGW(TAG, "session token=%04x for conn=%u: not subscribed anymore", s->token, s->conn);
        send_done(s->token, s->conn, 0, s->count, RL_RC_NOT_SUBSCRIBED);
        return;
    }
    ESP_LOGI(TAG, "session start token=%04x conn=%u mtu=%u packets=%u",
             s->token, s->conn, ble_att_mtu(s->conn), s->count);
    tx_conn = s->conn;
    int delivered = 0, last_rc = 0;
    for (int i = 0; i < s->count; i++) {
        int rc = send_one(s->conn, s->pkt[i].data, s->pkt[i].len);
        ESP_LOGI(TAG, "  notify %-12s len=%3u -> %s (rc=%d)", kind_name(s->pkt[i].kind),
                 s->pkt[i].len, rc == 0 ? "ok" : "FAILED", rc);
        if (rc != 0) { last_rc = rc; break; }
        delivered++;
        vTaskDelay(pdMS_TO_TICKS(10));    /* GATT_SEND_INTERVAL_MS, как в Android */
    }
    tx_conn = BLE_HS_CONN_HANDLE_NONE;
    sub_mark_served(s->conn);
    send_done(s->token, s->conn, delivered, s->count, last_rc);
}

static void tx_task(void *arg)
{
    (void)arg;
    static rt_session_t s;   /* ~4 КБ — статически */
    ctl_t c;
    for (;;) {
        if (xQueueReceive(sess_q, &s, 0) == pdTRUE) run_session(&s);

        if (xQueueReceive(ctl_q, &c, pdMS_TO_TICKS(20)) != pdTRUE) continue;
        switch (c.what) {
        case CTL_SUB_ON: {
            vTaskDelay(pdMS_TO_TICKS(150));       /* дать Android закончить запись CCCD */
            if (!sub_is(c.conn)) break;
            if (send_subscribed(c.conn, 1000) != 0)
                ESP_LOGW(TAG, "CORE not reachable: session for conn=%u will wait for link", c.conn);
            break;
        }
        case CTL_SUB_OFF: {
            uint8_t b[2];
            link_send(RL_UNSUBSCRIBED, b, (uint16_t)rl_enc_conn(b, c.conn), 500);
            break;
        }
        case CTL_RESYNC:
            resync();
            break;
        }
    }
}

void tx_init(void)
{
    ctl_q = xQueueCreate(16, sizeof(ctl_t));
    sess_q = xQueueCreate(1, sizeof(rt_session_t));
    tx_done = xSemaphoreCreateBinary();
    configASSERT(ctl_q && sess_q && tx_done);
    rt_init(&asm_);
    link_set_handlers(on_link_frame, on_link_event);
    xTaskCreate(tx_task, "box_tx", 6144, NULL, 5, NULL);
}

void tx_on_sync(void)
{
    synced = true;
    advertise();
    ctl_post(0, CTL_RESYNC);       /* сообщить CORE свой BLE-адрес */
}

#endif /* NODE_ROLE_TX */
