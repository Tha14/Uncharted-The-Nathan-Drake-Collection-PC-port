#!/usr/bin/env python3
"""Hide excess companion-level triangles by collapsing their index entries (a,b,c -> a,a,a). File size does not change.
NOT run by its author. Dry run unless --apply.
  apply_excess.py                 default tier from plan.json + the Airstrip wall (air-wall.json)
  apply_excess.py --only-wall     only the Airstrip wall
  apply_excess.py --add ID ...    also apply named review clusters (IDs from hits.tsv)
  apply_excess.py --restore       copy the backups back (needs --apply to write)
Each pak: md5 must match the plan, every index triple must match, backup to work/backup-excess/<pak> (never overwritten)."""
import hashlib, json, os, shutil, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(HERE + '/../../work') + '/'
PK = ROOT + 'overlay-r3/u3data/build/main/pak23/ps4/'
BK = ROOT + 'backup-excess/'
a = sys.argv[1:]; apply_ = '--apply' in a
add = a[a.index('--add') + 1:] if '--add' in a else []
add = [x for x in add if not x.startswith('--')]
if '--restore' in a:
    for f in sorted(os.listdir(BK)) if os.path.isdir(BK) else []:
        print('restore', f)
        if apply_:
            shutil.copy2(BK + f, PK + f)
    sys.exit(0 if apply_ else print('dry run: nothing written (use --apply)'))
wall = json.load(open(HERE + '/air-wall.json'))
plan = {}
if '--only-wall' not in a and os.path.exists(HERE + '/plan.json'):
    plan = json.load(open(HERE + '/plan.json'))
work = {}
for pak, e in plan.items():
    for cid, c in e['clusters'].items():
        if c['tier'] == 'default' or cid in add:
            work.setdefault(pak, dict(md5=e['md5'], bytes=e['bytes'], tris={}))['tris'].update({t[0]: t for t in c['tris']})
w = work.setdefault(wall['pak'], dict(md5=wall['md5'], bytes=wall['bytes'], tris={}))
if w['md5'] != wall['md5']:
    sys.exit('plan and wall list disagree on ' + wall['pak'])
w['tris'].update({t[0]: t for t in wall['tris']})
todo = []
for pak, e in sorted(work.items()):
    d = bytearray(open(PK + pak, 'rb').read())
    if hashlib.md5(d).hexdigest() != e['md5'] or len(d) != e['bytes']:
        sys.exit('pak changed since the plan was built: ' + pak)
    for off, x, y, z in e['tris'].values():
        if struct.unpack_from('<3H', d, off) != (x, y, z):
            sys.exit('index mismatch in %s at %#x' % (pak, off))
        struct.pack_into('<3H', d, off, x, x, x)
    print('collapse %d triangles in %s' % (len(e['tris']), pak)); todo.append((pak, d))
if not apply_:
    print('dry run: nothing written (use --apply)'); sys.exit(0)
os.makedirs(BK, exist_ok=True)
for pak, d in todo:
    if not os.path.exists(BK + pak):
        shutil.copy2(PK + pak, BK + pak)
    open(PK + pak + '.tmp', 'wb').write(d)
    os.replace(PK + pak + '.tmp', PK + pak)
    print('written', pak, 'backup', BK + pak)
