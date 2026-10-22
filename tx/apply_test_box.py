#!/usr/bin/env python3
from pathlib import Path
import shutil

path = Path("main/main.c")
if not path.is_file():
    raise SystemExit("Run this script from the tx project root; main/main.c was not found")

old = '''        char sid[37], sid_packet[48], trm_packet[48];
        box_generate_session_uuid(sid);
        snprintf(sid_packet, sizeof(sid_packet), "V1\\nSID\\n%s", sid);
        snprintf(trm_packet, sizeof(trm_packet), "V1\\nTRM\\n%s", sid);
        ESP_LOGI(tag, "Test session started: conn=%u sid=%s",
                 (unsigned)r.conn_handle, sid);

        if (box_notify_packet(r.conn_handle, sid_packet, "SID") != 0) continue;
        vTaskDelay(pdMS_TO_TICKS(BOX_TX_PACKET_DELAY_MS));
        if (box_notify_packet(r.conn_handle, trm_packet, "TRM-1") != 0) continue;
        vTaskDelay(pdMS_TO_TICKS(BOX_TX_PACKET_DELAY_MS));
        if (box_notify_packet(r.conn_handle, trm_packet, "TRM-2") != 0) continue;
        ESP_LOGI(tag, "Test session queued completely: conn=%u sid=%s",
                 (unsigned)r.conn_handle, sid);'''

new = '''        static const char box_id[] =
            "10000000-0000-4000-8000-aabbccddeeff";
        static const char message_id[] =
            "20000000-0000-4000-8000-aabbccddeeff";

        char sid[37];
        char sid_packet[48];
        char trm_packet[48];
        char box_packet[256];

        box_generate_session_uuid(sid);
        snprintf(sid_packet, sizeof(sid_packet), "V1\\nSID\\n%s", sid);
        snprintf(box_packet, sizeof(box_packet),
                 "V1\\nBOX\\n%s\\n"
                 "%s\\n"
                 "2099-12-31 23:59:59\\n"
                 "0\\n"
                 "BOX-TEST-RECEIVER\\n"
                 "text\\n"
                 "ESP32-BOX-TEST",
                 box_id,
                 message_id);
        snprintf(trm_packet, sizeof(trm_packet), "V1\\nTRM\\n%s", sid);

        ESP_LOGI(tag,
                 "Test session started: conn=%u sid=%s box=%s message=%s",
                 (unsigned)r.conn_handle,
                 sid,
                 box_id,
                 message_id);

        if (box_notify_packet(r.conn_handle, sid_packet, "SID") != 0) continue;
        vTaskDelay(pdMS_TO_TICKS(BOX_TX_PACKET_DELAY_MS));
        if (box_notify_packet(r.conn_handle, box_packet, "BOX") != 0) continue;
        vTaskDelay(pdMS_TO_TICKS(BOX_TX_PACKET_DELAY_MS));
        if (box_notify_packet(r.conn_handle, trm_packet, "TRM-1") != 0) continue;
        vTaskDelay(pdMS_TO_TICKS(BOX_TX_PACKET_DELAY_MS));
        if (box_notify_packet(r.conn_handle, trm_packet, "TRM-2") != 0) continue;
        ESP_LOGI(tag, "Test session queued completely: conn=%u sid=%s",
                 (unsigned)r.conn_handle, sid);'''

text = path.read_text()
if new in text:
    print("Test BOX is already present; nothing changed")
    raise SystemExit(0)
if old not in text:
    raise SystemExit("Expected SID/TRM block was not found; main.c was not changed")

backup = path.with_suffix(".c.before_test_box")
shutil.copy2(path, backup)
path.write_text(text.replace(old, new, 1))
print(f"Updated {path}")
print(f"Backup: {backup}")
