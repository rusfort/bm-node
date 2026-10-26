/*
 * Box Node — прошивка совместного теста (E2E) с Box Messenger Android.
 * Одна плата ESP32-S3 одновременно:
 *   - GATT server (peripheral): Android подписывается и получает SID/ACB/CBB/CDB/BOX/TRM;
 *   - GATT client (central): Node подключается к GATT server Android и принимает его сессии.
 * ESP-IDF v5.5.x, NimBLE.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "node.h"
#include "e2e_test.h"

static const char *TAG = "BOX_NODE";

SemaphoreHandle_t g_e2e_lock;
uint8_t g_own_addr_type;

void ble_store_config_init(void);

uint32_t node_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t node_rand32(void)
{
    return esp_random();
}

static void node_log(const char *line)
{
    ESP_LOGI("E2E", "%s", line);
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset; reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer address type failed rc=%d", rc);
        return;
    }
    uint8_t a[6] = {0};
    ble_hs_id_copy_addr(g_own_addr_type, a, NULL);
    ESP_LOGI(TAG, "Own BLE address %02x:%02x:%02x:%02x:%02x:%02x type=%u",
             a[5], a[4], a[3], a[2], a[1], a[0], g_own_addr_type);

    periph_start();
    central_on_sync();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    g_e2e_lock = xSemaphoreCreateMutex();
    configASSERT(g_e2e_lock);

    e2e_env_t env = {
        .rand32 = node_rand32,
        .now_ms = node_now_ms,
        .log = node_log,
        .node_suffix = NODE_SUFFIX,
        .ttl = E2E_TTL_DEFAULT,
        .timeout_ms = 15u * 60u * 1000u,
    };
    e2e_init(&env);

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    int rc = periph_gatt_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT init failed rc=%d", rc);
        return;
    }
    rc = ble_svc_gap_device_name_set(NODE_DEVICE_NAME);
    if (rc != 0) ESP_LOGW(TAG, "device name rc=%d", rc);

    ble_store_config_init();

    periph_task_start();
    central_task_start();

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "Box Node E2E firmware started");
}
