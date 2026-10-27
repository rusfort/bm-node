/*
 * Box Node Dual — Box Node на двух платах ESP32-S3, связанных UART.
 *   CORE: BLE central (подключается к GATT server Android и принимает),
 *         автомат совместного теста e2e, менеджер передающих сессий;
 *   TX:   BLE peripheral (GATT server Box), отправляет сессии телефону.
 * Роль выбирается при сборке: -D NODE_ROLE=CORE | TX. ESP-IDF v5.5.x, NimBLE.
 */
#include <stdio.h>
#include <string.h>

#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "board_link.h"
#include "link_glue.h"
#include "node.h"

static const char *TAG = "BOX_NODE";

uint8_t g_own_addr_type;

void ble_store_config_init(void);

uint32_t node_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset; reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) { ESP_LOGE(TAG, "ensure_addr failed rc=%d", rc); return; }
    rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer address type failed rc=%d", rc); return; }
    uint8_t a[6] = {0};
    ble_hs_id_copy_addr(g_own_addr_type, a, NULL);
    ESP_LOGI(TAG, "Own BLE address %02x:%02x:%02x:%02x:%02x:%02x type=%u",
             a[5], a[4], a[3], a[2], a[1], a[0], g_own_addr_type);
#if defined(NODE_ROLE_CORE)
    core_on_sync();
#else
    tx_on_sync();
#endif
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

    /* До запуска радио esp_random() без внешней энтропии может повторяться после
     * сброса. Включаем источник энтропии загрузчика на время генерации boot_id и
     * идентификаторов теста (e2e_init в core_init). */
    uint16_t boot_id;
    bootloader_random_enable();
    do { esp_fill_random(&boot_id, sizeof(boot_id)); } while (boot_id == 0);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    ESP_LOGI(TAG, "Box Node Dual role=%s fw=%s boot=%04x bt_mac=%02x:%02x:%02x:%02x:%02x:%02x",
             bl_role_name(MY_ROLE), FW_VERSION, boot_id,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

#if defined(NODE_ROLE_CORE)
    core_init();
#else
    tx_init();
#endif
    bootloader_random_disable();

    link_start(MY_ROLE, boot_id);

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
        return;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

#if defined(NODE_ROLE_TX)
    int rc = tx_gatt_init();
    if (rc != 0) { ESP_LOGE(TAG, "GATT init failed rc=%d", rc); return; }
    rc = ble_svc_gap_device_name_set(NODE_DEVICE_NAME);
#else
    /* CORE не публикует сервис Box: телефон должен получать сессии только от TX. */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(NODE_DEVICE_NAME "-CORE");
#endif
    if (rc != 0) ESP_LOGW(TAG, "device name rc=%d", rc);

    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "Box Node Dual %s started", bl_role_name(MY_ROLE));
}
