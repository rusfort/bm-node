#!/usr/bin/env python3
"""Apply to the tested tx/main.c; no implicit SDK or sdkconfig changes."""
import argparse
import datetime
import json
import pathlib
import shutil
import sys

MARKER = '/* BOX_REVERSE_CHANNEL_V1 */'
FILES = ('box_reverse.c', 'box_reverse.h', 'box_protocol.c', 'box_protocol.h')

def replace_once(text, old, new, label):
    if text.count(old) != 1:
        raise ValueError(f'{label}: expected exactly one match, found {text.count(old)}')
    return text.replace(old, new, 1)

def patch_main(text):
    if MARKER in text:
        raise ValueError('Reverse channel already installed; refusing to overwrite')
    if '"V1\\nBOX\\n%s\\n"' not in text:
        raise ValueError('Expected the tested BOX-enabled main.c, not an older template')
    text = replace_once(text, '#include "bleprph.h"',
                        '#include "bleprph.h"\n' + MARKER + '\n#include "box_reverse.h"', 'include')
    text = replace_once(text, '    struct ble_hs_adv_fields fields = {0};\n',
                        '    if (ble_gap_adv_active()) return;\n    struct ble_hs_adv_fields fields = {0};\n', 'advertising guard')
    text = replace_once(text, '    /* Printing ADDR */',
                        '    box_reverse_on_sync(own_addr_type);\n\n    /* Printing ADDR */', 'sync hook')
    text = replace_once(text, '    MODLOG_DFLT(ERROR, "Resetting state; reason=%d\\n", reason);',
                        '    box_reverse_on_reset();\n    box_subscribed_conn = BLE_HS_CONN_HANDLE_NONE;\n'
                        '    MODLOG_DFLT(ERROR, "Resetting state; reason=%d\\n", reason);', 'reset hook')
    text = replace_once(text, '            bleprph_print_conn_desc(&desc);',
                        '            bleprph_print_conn_desc(&desc);\n'
                        '            box_reverse_peripheral_connected(event->connect.conn_handle);', 'peripheral connect')
    text = replace_once(text, '        MODLOG_DFLT(INFO, "disconnect; reason=%d ", event->disconnect.reason);',
                        '        box_reverse_peripheral_disconnected(event->disconnect.conn.conn_handle);\n'
                        '        MODLOG_DFLT(INFO, "disconnect; reason=%d ", event->disconnect.reason);', 'peripheral disconnect')
    text = replace_once(text, '    box_tx_queue = xQueueCreate(',
                        '    box_reverse_init(bleprph_advertise);\n\n    box_tx_queue = xQueueCreate(', 'module init')
    text = replace_once(text, '            "10000000-0000-4000-8000-aabbccddeeff";',
                        '            BOX_TEST_BOX_ID;', 'known test box')
    text = replace_once(text, '        char box_packet[256];',
                        '        char box_packet[256];\n        char acb_packet[44];', 'ACB buffer')
    text = replace_once(text,
                        '        if (box_notify_packet(r.conn_handle, box_packet, "BOX") != 0) continue;',
                        '        bool acb_failed = false;\n'
                        '        for (unsigned slot = 0; slot < BOX_REVERSE_ACK_CAPACITY; ++slot) {\n'
                        '            if (!box_reverse_build_acb(slot, acb_packet, sizeof(acb_packet))) continue;\n'
                        '            if (box_notify_packet(r.conn_handle, acb_packet, "ACB") != 0) {\n'
                        '                acb_failed = true; break;\n'
                        '            }\n'
                        '            ESP_LOGI(tag, "ACB queued: callback_id=%s (not Android confirmation)", acb_packet + 7);\n'
                        '            vTaskDelay(pdMS_TO_TICKS(BOX_TX_PACKET_DELAY_MS));\n'
                        '        }\n'
                        '        if (acb_failed) continue;\n'
                        '        if (box_notify_packet(r.conn_handle, box_packet, "BOX") != 0) continue;', 'ACB TX')
    return text

def prepare(root, source):
    paths = [root / 'main/main.c', root / 'main/CMakeLists.txt', root / 'sdkconfig']
    for p in paths:
        if not p.is_file(): raise ValueError(f'Missing project file: {p}')
    cfg = paths[2].read_text()
    for setting in ('CONFIG_BT_NIMBLE_ROLE_CENTRAL=y', 'CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y',
                    'CONFIG_BT_NIMBLE_ROLE_OBSERVER=y', 'CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y',
                    'CONFIG_BT_NIMBLE_GATT_CLIENT=y', 'CONFIG_BT_NIMBLE_GATT_SERVER=y'):
        if setting not in cfg.splitlines(): raise ValueError(f'Enable {setting} using idf.py menuconfig')
    if 'CONFIG_EXAMPLE_EXTENDED_ADV=y' in cfg.splitlines():
        raise ValueError('Extended advertising not supported in this test')
    mtus = [l for l in cfg.splitlines() if l.startswith('CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=')]
    if len(mtus) != 1 or int(mtus[0].split('=')[1]) < 483:
        raise ValueError('Set CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517')
    result = {paths[0]: patch_main(paths[0].read_text())}
    cmake = paths[1].read_text()
    result[paths[1]] = replace_once(cmake, '"gatt_svr.c")',
        '"gatt_svr.c"\n         "box_reverse.c"\n         "box_protocol.c")', 'CMake sources')
    for name in FILES:
        target = root / 'main' / name
        if target.exists(): raise ValueError(f'Target already exists: {target}; resolve manually')
        result[target] = (source / 'main' / name).read_text()
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    root = args.project.expanduser().resolve()
    try:
        result = prepare(root, pathlib.Path(__file__).resolve().parent)
        for p in result: print('Will update:', p.relative_to(root))
        if args.dry_run:
            print('DRY RUN OK. No files changed.'); return 0
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
        backup = root / ('backup-before-reverse-' + stamp)
        backup.mkdir()
        manifest = {}
        for p in result:
            rel = str(p.relative_to(root))
            manifest[rel] = p.exists()
            if p.exists():
                dest = backup / rel; dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(p, dest)
        (backup / 'manifest.json').write_text(json.dumps(manifest, indent=2))
        try:
            for p, text in result.items(): p.write_text(text)
        except Exception:
            for rel, existed in manifest.items():
                target = root / rel
                if existed: shutil.copy2(backup / rel, target)
                elif target.exists(): target.unlink()
            raise
        print('Applied. Backup:', backup)
        print('gatt_svr.c, bleprph.h and sdkconfig left unchanged. Run: idf.py build')
        return 0
    except (ValueError, OSError) as exc:
        print('NOT APPLIED:', exc, file=sys.stderr); return 1

if __name__ == '__main__':
    sys.exit(main())
