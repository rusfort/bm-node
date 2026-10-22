/*
 * Box Messenger BLE GATT server for ESP-IDF v5.5.5 / NimBLE.
 */

#include <stdint.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "bleprph.h"

static const char *TAG = "BOX_GATT";

/* 12345678-1234-5678-9012-aabbccddeeff */
static const ble_uuid128_t box_service_uuid =
    BLE_UUID128_INIT(
        0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x12, 0x90,
        0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12
    );

/* 12345678-1234-5678-aaaa-aabbccddeeff */
static const ble_uuid128_t box_message_uuid =
    BLE_UUID128_INIT(
        0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0xaa, 0xaa,
        0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12
    );

static uint16_t box_message_handle;

static int
box_message_access(uint16_t conn_handle,
                   uint16_t attr_handle,
                   struct ble_gatt_access_ctxt *ctxt,
                   void *arg)
{
    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        static const char value[] = "BOX-TX";
        int rc = os_mbuf_append(ctxt->om, value, sizeof(value) - 1);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint8_t value[512];
        uint16_t len = 0;

        if (OS_MBUF_PKTLEN(ctxt->om) > sizeof(value)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        int rc = ble_hs_mbuf_to_flat(ctxt->om, value, sizeof(value), &len);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        ESP_LOGI(TAG,
                 "Write received: conn=%u handle=%u len=%u",
                 (unsigned)conn_handle,
                 (unsigned)attr_handle,
                 (unsigned)len);
        return 0;
    }

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def box_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &box_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &box_message_uuid.u,
                .access_cb = box_message_access,
                .flags = BLE_GATT_CHR_F_READ |
                         BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &box_message_handle,
            },
            {0}
        },
    },
    {0}
};

uint16_t
box_gatt_message_handle(void)
{
    return box_message_handle;
}

void
gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    (void)arg;
    char uuid[BLE_UUID_STR_LEN];

    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGI(TAG,
                 "Service: %s handle=%u",
                 ble_uuid_to_str(ctxt->svc.svc_def->uuid, uuid),
                 (unsigned)ctxt->svc.handle);
        break;

    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGI(TAG,
                 "Characteristic: %s value_handle=%u",
                 ble_uuid_to_str(ctxt->chr.chr_def->uuid, uuid),
                 (unsigned)ctxt->chr.val_handle);
        break;

    case BLE_GATT_REGISTER_OP_DSC:
        ESP_LOGI(TAG,
                 "Descriptor: %s handle=%u",
                 ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, uuid),
                 (unsigned)ctxt->dsc.handle);
        break;

    default:
        break;
    }
}

int
gatt_svr_init(void)
{
#if CONFIG_BT_NIMBLE_GAP_SERVICE
    ble_svc_gap_init();
#endif
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(box_services);
    if (rc != 0) {
        return rc;
    }
    return ble_gatts_add_svcs(box_services);
}
