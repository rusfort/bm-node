#!/usr/bin/env python3
"""Restore files recorded by apply_reverse.py. Requires an explicit backup path."""
import argparse
import json
from pathlib import Path
import shutil
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--project', type=Path, default=Path.cwd())
p.add_argument('--backup', type=Path, required=True)
p.add_argument('--dry-run', action='store_true')
a = p.parse_args()
root = a.project.expanduser().resolve()
backup = a.backup.expanduser().resolve()
allowed = {'main/main.c', 'main/CMakeLists.txt', 'main/box_reverse.c',
           'main/box_reverse.h', 'main/box_protocol.c', 'main/box_protocol.h'}
manifest = json.loads((backup / 'manifest.json').read_text())
if set(manifest) != allowed or any(type(v) is not bool for v in manifest.values()):
    raise SystemExit('Invalid backup manifest')
for rel, existed in manifest.items():
    if existed and not (backup / rel).is_file():
        raise SystemExit(f'Incomplete backup: {rel}')
for rel, existed in manifest.items():
    target = root / rel
    print('Restore' if existed else 'Remove added file', rel)
    if not a.dry_run:
        if existed: shutil.copy2(backup / rel, target)
        elif target.exists(): target.unlink()
print('Dry run complete' if a.dry_run else 'Restored. Run idf.py build')
