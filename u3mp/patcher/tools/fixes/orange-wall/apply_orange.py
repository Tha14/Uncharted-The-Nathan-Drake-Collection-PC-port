#!/usr/bin/env python3
"""Hide the duplicate control tower that air-multi-strip.pak draws beside the real one on Airstrip.
Dry run unless --apply is given. Session step of 2026-11-01 18:27, restored as a recipe step 2026-11-07 (its plan rebuilt
from the recorded states work/backup-orange -> work/backup-excess by scratch/pak23-recovery/airstrip/mk_orange.py).
Collapses the triangles listed in orange-plan.json (index buffer entries a,b,c -> a,a,a); the file size does not change.
Refuses if the pak's md5 differs from the one the plan was built from. Backup: work/backup-orange/air-multi-strip.pak
(an existing backup is never overwritten). Undo: --restore --apply copies the backup back."""
import hashlib, json, os, shutil, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = '{{PROJECT}}/work/'
plan = json.load(open(HERE + '/orange-plan.json'))
TARGET = ROOT + 'overlay-r3/u3data/build/main/pak23/ps4/' + plan['pak']
BACKUP = ROOT + 'backup-orange/' + plan['pak']
apply_ = '--apply' in sys.argv
if '--restore' in sys.argv:
    print('restore', BACKUP, '->', TARGET)
    if not os.path.exists(BACKUP):
        sys.exit('no backup')
    if apply_:
        shutil.copy2(BACKUP, TARGET)
    else:
        print('dry run: nothing written (use --apply)')
    sys.exit(0)
data = bytearray(open(TARGET, 'rb').read())
if hashlib.md5(data).hexdigest() == plan.get('md5_after'):
    print('already applied: the duplicate tower is collapsed'); sys.exit(0)
if hashlib.md5(data).hexdigest() != plan['md5'] or len(data) != plan['bytes']:
    sys.exit('pak changed since the plan was built: ' + TARGET)
n = 0
for off, a, b, c in plan['tris']:
    if struct.unpack_from('<3H', data, off) != (a, b, c):
        sys.exit('index mismatch at %#x' % off)
    struct.pack_into('<3H', data, off, a, a, a)
    n += 1
print('collapse %d triangles in %s' % (n, plan['pak']))
if not apply_:
    print('dry run: nothing written (use --apply)')
    sys.exit(0)
os.makedirs(os.path.dirname(BACKUP), exist_ok=True)
if not os.path.exists(BACKUP):          # an earlier import or pass already saved this same input state: keep it
    shutil.copy2(TARGET, BACKUP)
tmp = TARGET + '.tmp'
open(tmp, 'wb').write(data)
os.replace(tmp, TARGET)
print('written; backup at', BACKUP)
