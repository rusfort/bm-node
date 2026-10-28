/*
 * Роль CORE, центральная часть Box Node: ищет Android с Box Messenger, подключается к его
 * GATT server, находит сервис/характеристику по префиксам UUID, подписывается
 * на notifications и передаёт принятые пакеты логике узла (ретранслятор или e2e).
 */
#if defined(NODE_ROLE_CORE)

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"

#include "node.h"
#include "e2e_test.h"
#include "core_glue.h"
#include "link_glue.h"
#include "box_adv.h"

static const char *TAG = "BOX_RX";

#define SCAN_MS           8000
#define HOLD_MAX_MS       20000   /* максимум удержания соединения без TRM */
#define CLOSE_DELAY_MS    250     /* после TRM: дать дойти второму TRM */
/* Передача Node -> Android идёт через отдельную плату TX, поэтому CORE не нужно
 * освобождать радио для входящих подключений: пауза между сеансами короткая. */
#define COOLDOWN_MS       1500
/* Очерёдность телефонов: адрес, обслуженный недавно, берём только если за
 * FAIR_WAIT_MS скана не нашлось никого другого. */
#define FAIR_RECENT_MS    20000
#define FAIR_WAIT_MS      1500
#define STATS_MS          30000
/* Ответ на запись CCCD. Android (BleMessageExchangeService) в фазе сканирования/клиента
 * игнорирует onDescriptorWriteRequest без sendResponse — запрос висит до таймаута ATT.
 * Не ждём 20 с: через CCCD_WAIT_MS отключаемся и скоро пробуем снова (телефон
 * вернётся в фазу сервера). Обслуженным такой телефон не считаем — только полштрафа. */
#define CCCD_WAIT_MS      4000

/* Box UUID в рекламе и scan response разбирает box_adv.c. */

typedef enum { C_IDLE = 0, C_SCANNING, C_CONNECTING, C_CONNECTED } cstate_t;

static volatile cstate_t cstate = C_IDLE;
static volatile bool     synced;
static volatile uint16_t c_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint32_t c_conn_at, c_close_at, c_next_scan_at;
static volatile bool     c_subscribed;
static volatile bool     c_cccd_pending, c_got_data, c_no_answer;
static volatile uint32_t c_cccd_at;

static uint16_t svc_start, svc_end, chr_val, chr_end, cccd;
static char     peer_suffix[13];

typedef struct {
    uint16_t len;
    uint8_t  data[BOX_MAX_PACKET];
} rx_item_t;

static QueueHandle_t rx_q;

/* Соседи по рекламе и scan response (только из host task NimBLE). */
static bx_table_t nbt;
#define NOTE_MS 10000             /* не чаще: «телефон в фазе приёма — ждём» */

/* ---------- телефоны: адрес -> ID, очерёдность, статистика ---------- */

#define MAX_PEERS 6
static struct {
    uint8_t  addr[6];
    char     id[13];
    uint32_t last_served;      /* время отключения после сеанса */
    uint32_t connects, sessions, no_answer;
} peers[MAX_PEERS];
static int n_peers;
static portMUX_TYPE peers_mux = portMUX_INITIALIZER_UNLOCKED;

static ble_addr_t c_peer_addr;           /* адрес текущего соединения */
static ble_addr_t fair_cand;             /* отложенный кандидат (обслужен недавно) */
static bool       fair_has_cand;
static uint32_t   scan_started_at;
static bool       waited_tx_info_logged;

static int peer_find_locked(const uint8_t a[6])
{
    for (int i = 0; i < n_peers; i++)
        if (memcmp(peers[i].addr, a, 6) == 0) return i;
    return -1;
}

static int peer_get_locked(const uint8_t a[6])
{
    int i = peer_find_locked(a);
    if (i >= 0) return i;
    if (n_peers < MAX_PEERS) i = n_peers++;
    else {                                   /* вытесняем самый давний */
        i = 0;
        for (int k = 1; k < MAX_PEERS; k++)
            if (peers[k].last_served < peers[i].last_served) i = k;
    }
    memset(&peers[i], 0, sizeof(peers[i]));
    memcpy(peers[i].addr, a, 6);
    return i;
}

static bool peer_recent(const uint8_t a[6], uint32_t now)
{
    taskENTER_CRITICAL(&peers_mux);
    int i = peer_find_locked(a);
    bool r = i >= 0 && peers[i].last_served && now - peers[i].last_served < FAIR_RECENT_MS;
    taskEXIT_CRITICAL(&peers_mux);
    return r;
}

const char *core_phone_by_addr(const uint8_t a[6])
{
    static char out[13];     /* вызывается только из relay_task */
    bool found = false;
    taskENTER_CRITICAL(&peers_mux);
    int i = peer_find_locked(a);
    if (i >= 0 && peers[i].id[0]) { memcpy(out, peers[i].id, sizeof(out)); found = true; }
    taskEXIT_CRITICAL(&peers_mux);
    return found ? out : NULL;
}

static void print_peers(void)
{
    for (int i = 0; i < n_peers; i++) {
        const uint8_t *a = peers[i].addr;
        ESP_LOGI(TAG, "phone %s addr=%02x:%02x:%02x:%02x:%02x:%02x connects=%u sessions=%u cccd_no_answer=%u",
                 peers[i].id[0] ? peers[i].id : "?", a[5], a[4], a[3], a[2], a[1], a[0],
                 (unsigned)peers[i].connects, (unsigned)peers[i].sessions, (unsigned)peers[i].no_answer);
    }
}

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
    c_cccd_pending = false;
    if (err->status == BLE_HS_ENOTCONN && c_no_answer) return 0;   /* мы сами отключились */
    if (err->status != 0) {
        /* Android шлёт notifications всем подключённым клиентам и без CCCD,
         * поэтому соединение не рвём — продолжаем слушать. */
        ESP_LOGW(TAG, "CCCD write failed rc=%d, keep listening", err->status);
        c_subscribed = true;
        return 0;
    }
    c_subscribed = true;
    ESP_LOGI(TAG, "subscribed to Android %s (conn=%u val=%u cccd=%u), waiting for its session",
             peer_suffix, conn, chr_val, cccd);
    return 0;
}

static void subscribe_now(uint16_t conn)
{
    if (cccd == 0) {
        /* Android всё равно шлёт notifications всем подключённым клиентам. */
        ESP_LOGW(TAG, "no CCCD found, relying on unsolicited notifications");
        c_subscribed = true;
        return;
    }
    static const uint8_t on[2] = { 0x01, 0x00 };
    c_cccd_at = node_now_ms();
    c_cccd_pending = true;
    int rc = ble_gattc_write_flat(conn, cccd, on, sizeof(on), on_subscribed, NULL);
    if (rc != 0) { c_cccd_pending = false; drop("CCCD write start failed", rc); }
}

static int on_hello(uint16_t conn, const struct ble_gatt_error *err,
                    struct ble_gatt_attr *attr, void *arg)
{
    (void)attr; (void)arg;
    if (err->status == 0) ESP_LOGI(TAG, "HELLO sent to Android %s", peer_suffix);
    else ESP_LOGW(TAG, "HELLO write failed rc=%d, subscribing anyway", err->status);
    subscribe_now(conn);
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

    /* Телефон новой версии: сначала HELLO (ID узла), затем CCCD — сервер узнаёт наш ID
     * по адресу этого соединения до начала раздачи. Старым версиям HELLO не шлём. */
    bx_nb_t *n = bx_table_find(&nbt, c_peer_addr.val);
    if (bx_is_new_phone(n, node_now_ms())) {
        static char hello[BX_HELLO_MAX];
        size_t hl = bx_build_hello(hello, sizeof(hello), NODE_SUFFIX, BX_F_NODE);
        int rc = ble_gattc_write_flat(conn, chr_val, hello, (uint16_t)hl, on_hello, NULL);
        if (rc == 0) return 0;
        ESP_LOGW(TAG, "HELLO write start failed rc=%d, subscribing anyway", rc);
    }
    subscribe_now(conn);
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
    if (strcmp(peer_suffix, NODE_SUFFIX) == 0) {
        core_learn_tx_addr(c_peer_addr.val);     /* больше к этому адресу не подключаемся */
        drop("self loop: peer has our own ID (own TX), address remembered", 0);
        return 0;
    }
    taskENTER_CRITICAL(&peers_mux);
    {
        int pi = peer_get_locked(c_peer_addr.val);
        memcpy(peers[pi].id, peer_suffix, sizeof(peers[pi].id));
    }
    taskEXIT_CRITICAL(&peers_mux);
    if (chr_end == 0) chr_end = svc_end;

    ESP_LOGI(TAG, "Box characteristic found: peer_id=%s val_handle=%u", peer_suffix, chr_val);
    node_rx_begin(peer_suffix);

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

static void connect_to(const ble_addr_t *addr, int rssi, const char *note)
{
    const uint8_t *a = addr->val;
    ESP_LOGI(TAG, "Box device %02x:%02x:%02x:%02x:%02x:%02x rssi=%d -> connect%s",
             a[5], a[4], a[3], a[2], a[1], a[0], rssi, note);
    c_peer_addr = *addr;
    fair_has_cand = false;
    bx_nb_t *n = bx_table_find(&nbt, addr->val);
    if (n && n->info_valid && !(n->info.flags & BX_F_NODE)) {
        taskENTER_CRITICAL(&peers_mux);
        int pi = peer_get_locked(addr->val);
        memcpy(peers[pi].id, n->info.id, sizeof(peers[pi].id));
        taskEXIT_CRITICAL(&peers_mux);
    }
    cstate = C_CONNECTING;
    int rc = ble_gap_connect(g_own_addr_type, addr, 5000, NULL, central_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "connect rc=%d", rc);
        cstate = C_IDLE;
        c_next_scan_at = node_now_ms() + COOLDOWN_MS;
    }
}

static void start_scan(void)
{
    struct ble_gap_disc_params p = {0};
    /* Без фильтра повторов: флаг SERVING в scan response меняется на ходу, а с фильтром
     * контроллер сообщил бы о телефоне только один раз за скан. */
    p.filter_duplicates = 0;
    p.passive = 0;               /* активное: нужен scan response с ID и флагами */
    int rc = ble_gap_disc(g_own_addr_type, SCAN_MS, &p, central_gap_event, NULL);
    if (rc == 0) {
        cstate = C_SCANNING;
        scan_started_at = node_now_ms();
        fair_has_cand = false;
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
        if (cstate != C_SCANNING) return 0;
        const struct ble_gap_disc_desc *d = &event->disc;
        uint32_t now = node_now_ms();
        bool is_rsp = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
        bx_nb_t *n = bx_table_report(&nbt, d->addr.val, is_rsp, d->data, d->length_data, now);

        /* Отложенный кандидат (обслужен недавно): никого другого за FAIR_WAIT_MS не нашлось. */
        if (fair_has_cand && now - scan_started_at >= FAIR_WAIT_MS) {
            bx_nb_t *c = bx_table_find(&nbt, fair_cand.val);
            if (bx_decide(&nbt, c, now) == BX_CONNECT) {
                ble_addr_t a = fair_cand;
                ble_gap_disc_cancel();
                connect_to(&a, 0, " (recent, no other phone found)");
                return 0;
            }
            fair_has_cand = false;            /* кандидат перестал раздавать */
        }
        if (!n) return 0;

        uint8_t own[6], txa[6];
        ble_hs_id_copy_addr(g_own_addr_type, own, NULL);
        if (memcmp(own, d->addr.val, 6) == 0) return 0;
        bx_decision_t dec = bx_decide(&nbt, n, now);
        bool tx_known = core_tx_addr(txa);
        if (dec == BX_SKIP_SELF) {
            if (!tx_known || memcmp(txa, d->addr.val, 6) != 0) {
                ESP_LOGI(TAG, "own TX found by scan response (node id %s)", n->info.id);
                core_learn_tx_addr(d->addr.val);
            }
            return 0;
        }
        if (tx_known && memcmp(txa, d->addr.val, 6) == 0) return 0;   /* своя плата TX */
        if (!tx_known && link_ok()) {
            /* TX на связи, но её адрес ещё не пришёл: не рискуем подключиться к ней. */
            if (!waited_tx_info_logged) {
                ESP_LOGI(TAG, "waiting TX address from TX board before connecting");
                waited_tx_info_logged = true;
            }
            return 0;
        }
        if (dec == BX_SKIP_NOT_SERVING) {
            if (n->note_at == 0 || now - n->note_at >= NOTE_MS) {
                n->note_at = now;
                ESP_LOGI(TAG, "phone %s is in scan phase (not SERVING) -> wait", n->info.id);
            }
            return 0;
        }
        if (dec != BX_CONNECT) return 0;

        if (peer_recent(d->addr.val, now)) {
            if (!fair_has_cand) { fair_cand = d->addr; fair_has_cand = true; }
            return 0;                       /* сначала поищем другой телефон */
        }
        const char *note = !n->info_valid ? " (old app, no scan data)"
                         : (n->info.flags & BX_F_NODE) ? " (other Box Node)" : " (SERVING)";
        if (n->info_valid) ESP_LOGI(TAG, "peer id %s flags=%02x", n->info.id, n->info.flags);
        ble_gap_disc_cancel();
        connect_to(&d->addr, d->rssi, note);
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (cstate == C_SCANNING) {
            if (fair_has_cand &&
                bx_decide(&nbt, bx_table_find(&nbt, fair_cand.val), node_now_ms()) == BX_CONNECT) {
                connect_to(&fair_cand, 0, " (recent, no other phone found)");
                return 0;
            }
            fair_has_cand = false;
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
        c_cccd_pending = c_got_data = c_no_answer = false;
        svc_start = svc_end = chr_val = chr_end = cccd = 0;
        peer_suffix[0] = '\0';
        cstate = C_CONNECTED;
        taskENTER_CRITICAL(&peers_mux);
        peers[peer_get_locked(c_peer_addr.val)].connects++;
        taskEXIT_CRITICAL(&peers_mux);
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
            taskENTER_CRITICAL(&peers_mux);
            {
                int pi = peer_get_locked(c_peer_addr.val);
                uint32_t t = node_now_ms();
                /* не ответил на CCCD — сеанса не было: полштрафа, чтобы скоро повторить */
                peers[pi].last_served = c_no_answer ? (t - FAIR_RECENT_MS / 2) | 1u : t;
                if (c_no_answer) peers[pi].no_answer++;
            }
            taskEXIT_CRITICAL(&peers_mux);
            c_cccd_pending = false;
            c_no_answer = false;
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
        c_got_data = true;
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
        e2e_rx_action_t act = node_on_rx(it.data, it.len);
        if (act == E2E_RX_CLOSE && c_close_at == 0) {
            c_close_at = node_now_ms() + CLOSE_DELAY_MS;
            taskENTER_CRITICAL(&peers_mux);
            peers[peer_get_locked(c_peer_addr.val)].sessions++;
            taskEXIT_CRITICAL(&peers_mux);
        }
    }
}

static void central_mgr(void *arg)
{
    (void)arg;
    uint32_t last_stats = node_now_ms();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (node_now_ms() - last_stats >= STATS_MS) {
            last_stats = node_now_ms();
            print_peers();
        }

        node_tick();

        if (!synced) continue;
        uint32_t now = node_now_ms();

        switch (cstate) {
        case C_IDLE:
            if ((int32_t)(now - c_next_scan_at) >= 0) start_scan();
            break;
        case C_SCANNING:
            /* Отложенного кандидата подключает обработчик отчётов сканера (host task):
             * без фильтра повторов отчёты приходят непрерывно. */
            break;
        case C_CONNECTED: {
            if (c_close_at && (int32_t)(now - c_close_at) >= 0) {
                ESP_LOGI(TAG, "Android session done -> disconnect");
                c_close_at = 0;
                ble_gap_terminate(c_conn, BLE_ERR_REM_USER_CONN_TERM);
            } else if (c_cccd_pending && !c_got_data && !c_no_answer &&
                       now - c_cccd_at > CCCD_WAIT_MS) {
                ESP_LOGW(TAG, "phone %s did not answer CCCD in %d ms (its GATT server is in scan/client "
                         "phase) -> disconnect, retry soon", peer_suffix, CCCD_WAIT_MS);
                c_no_answer = true;
                ble_gap_terminate(c_conn, BLE_ERR_REM_USER_CONN_TERM);
            } else if (now - c_conn_at > HOLD_MAX_MS) {
                ESP_LOGI(TAG, "hold timeout without TRM -> disconnect");
                c_conn_at = now;
                ble_gap_terminate(c_conn, BLE_ERR_REM_USER_CONN_TERM);
            }
            break;
        }
        default:
            break;
        }
    }
}

void central_on_sync_(void)
{
    synced = true;
    c_next_scan_at = node_now_ms() + 500;
}

void central_task_start(void)
{
    bx_table_init(&nbt, NODE_SUFFIX);
    rx_q = xQueueCreate(24, sizeof(rx_item_t));
    configASSERT(rx_q);
    xTaskCreate(rx_worker, "box_rx", 6144, NULL, 5, NULL);
    xTaskCreate(central_mgr, "box_central", 4096, NULL, 4, NULL);
}

#endif /* NODE_ROLE_CORE */
