#!/usr/bin/env python3
"""Change only the shared BOX test ID. Never flash or alter Android data."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

OLD = '10000000-0000-4000-8000-aabbccddeeff'
NEW = '10000000-0000-4000-8000-aabbccddee01'
MESSAGE = '20000000-0000-4000-8000-aabbccddeeff'
DEFINE = re.compile(rb'(?m)^#define BOX_TEST_BOX_ID "([0-9a-f-]{36})"[ \t]*\r?$')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, default=Path.cwd(), help='Path to tx (default: current directory)')
    parser.add_argument('--dry-run', action='store_true', help='Validate only; do not write files')
    args = parser.parse_args()
    root = args.project.resolve()
    target = root / 'main/box_reverse.h'
    if target.is_symlink():
        raise ValueError('box_reverse.h is a symlink; refusing to replace it')
    original = target.read_bytes()
    defines = list(DEFINE.finditer(original))
    if len(defines) != 1:
        raise ValueError('Expected exactly one BOX_TEST_BOX_ID definition')
    found = defines[0].group(1).decode('ascii')
    if found not in (OLD, NEW):
        raise ValueError(f'Unexpected current box_id: {found}')
    tx = (root / 'main/main.c').read_text()
    rx = (root / 'main/box_reverse.c').read_text()
    if not re.search(r'static\s+const\s+char\s+box_id\[\]\s*=\s*BOX_TEST_BOX_ID\s*;', tx):
        raise ValueError('TX does not use the shared BOX_TEST_BOX_ID')
    if not re.search(r'static\s+const\s+char\s+message_id\[\]\s*=\s*"' + re.escape(MESSAGE) + r'"\s*;', tx):
        raise ValueError('Original test message_id not found; refusing a different test')
    if not re.search(r'strcmp\s*\(\s*parsed\.box_ids\[i\]\s*,\s*BOX_TEST_BOX_ID\s*\)', rx):
        raise ValueError('RX CBB/CDB filter does not use the same BOX_TEST_BOX_ID')
    for name, source in [('TX', tx), ('RX', rx)]:
        if not re.search(r'#include\s+"box_reverse.h"', source):
            raise ValueError(f'{name} does not include box_reverse.h')
    if found == NEW:
        print('ALREADY APPLIED: duplicate test ID is installed; no files changed.')
        return
    start, end = defines[0].span(1)
    updated = original[:start] + NEW.encode() + original[end:]
    print(f'Project: {root}')
    print(f'box_id: {OLD} -> {NEW}')
    print(f'message_id unchanged: {MESSAGE}')
    if args.dry_run:
        print('DRY-RUN OK: only main/box_reverse.h would change; no files written.')
        return
    if target.read_bytes() != original:
        raise ValueError('Header changed during validation; retry after stopping edits')
    backup = Path(tempfile.mkdtemp(prefix='backup-before-duplicate-', dir=root))
    shutil.copy2(target, backup / 'box_reverse.h')
    (backup / 'manifest.json').write_text(json.dumps({
        'file': 'main/box_reverse.h',
        'sha256_before': hashlib.sha256(original).hexdigest(),
        'sha256_after': hashlib.sha256(updated).hexdigest(),
        'box_id_before': OLD, 'box_id_after': NEW,
    }, indent=2) + '\n')
    temp = None
    try:
        with tempfile.NamedTemporaryFile(prefix='.box_duplicate_', dir=target.parent, delete=False) as stream:
            temp = Path(stream.name)
            stream.write(updated)
            stream.flush()
            os.fsync(stream.fileno())
        shutil.copymode(target, temp)
        os.replace(temp, target)
        temp = None
    finally:
        if temp is not None:
            temp.unlink(missing_ok=True)
    print('APPLIED: main/box_reverse.h only. No build or flash performed.')
    print(f'BACKUP: {backup}')
    print(f'Restore from project root: cp "{backup}/box_reverse.h" main/box_reverse.h')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        print(f'NOT APPLIED / CHECK REQUIRED: {error}', file=sys.stderr)
        sys.exit(1)
