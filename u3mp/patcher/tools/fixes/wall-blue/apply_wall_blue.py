#!/usr/bin/env python3
"""apply_wall_blue.py - OPTIONAL look change for the Airstrip blue corrugated wall. NOT a bug fix: these tints match PS3 exactly.
Lowers the saturation of g_colorTint1 on the blue corrugated paint (keeps its brightness), in place; file sizes do not change.
  apply_wall_blue.py                 dry run, writes nothing
  apply_wall_blue.py --sat 0.5       saturation kept, 0..1 (default 0.5; 1 = PS3 value, 0 = grey)
  apply_wall_blue.py --out DIR       dry run that also writes the edited paks to DIR (for crashcheck)
  apply_wall_blue.py --apply         back up to work/backup-wall-blue (never overwritten), then write the overlay
  apply_wall_blue.py --restore       show what would be restored; add --apply to copy the backups back
Refuses if any pak differs from the md5 it was planned against, or if the game is running. Keeps the excess-wall fix (asserted)."""
import hashlib, json, os, shutil, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(HERE, '..', 'map-colors2'))
from lib import Pak, W, OV
BK = W + 'backup-wall-blue/'
PS3_TINT = (0.15142753720283508, 0.3543285131454468, 0.6324456930160522)
MATS = ('air-yem-metal-corrugated-main-blue:', 'air-yem-metal-corrugated-main-blue-striped:', 'air-yem-metal-corrugated-main-blue-emblem:')
TARGETS = {'air-multi.pak': (OV, 'af34e7ec4aea89b51fa0c4372f98ae7c'), 'air-multi-shade.pak': (OV, 'bb7da60e4b3160bc6ba49397b39b33f3'),
           'air-multi-strip.pak': (OV, None)}
PIN = os.path.join(HERE, 'pins.json')
a = sys.argv[1:]; apply_ = '--apply' in a
sat = float(a[a.index('--sat') + 1]) if '--sat' in a else 0.5
out = a[a.index('--out') + 1] if '--out' in a else None
if not 0 <= sat <= 1: sys.exit('--sat must be 0..1')
if apply_ and any(l.split()[:1] == ['uncharted.exe'] for l in subprocess.run(['tasklist'], capture_output=True, text=True).stdout.lower().splitlines()): sys.exit('game running: stop it first')
md5 = lambda b: hashlib.md5(b).hexdigest()
pins = json.load(open(PIN))
if '--restore' in a:
    for f, (d, _) in TARGETS.items():
        if not os.path.exists(BK + f): print('no backup', f); continue
        if md5(open(BK + f, 'rb').read()) != pins[f]: sys.exit('backup does not match the pinned md5: ' + f)
        print('restore', f)
        if apply_: shutil.copy2(BK + f, d + f)
    print('restored' if apply_ else 'dry run: nothing written (use --restore --apply)'); sys.exit(0)
L = 0.2126 * PS3_TINT[0] + 0.7152 * PS3_TINT[1] + 0.0722 * PS3_TINT[2]
new = tuple(L + sat * (t - L) for t in PS3_TINT)
print('g_colorTint1 %s -> %s (sat %.2f)' % (tuple(round(x, 3) for x in PS3_TINT), tuple(round(x, 3) for x in new), sat))
todo = []
for f, (d, _) in TARGETS.items():
    raw = open(d + f, 'rb').read()
    if md5(raw) != pins[f]: sys.exit('pak changed since this script was planned: ' + f)
    q = Pak(raw); buf = bytearray(raw); slots = {}; n = 0
    for g in q.items_of('GEOMETRY_1'):
        pg, b = g.page, g.data_off; nm_ = q.u64(q.pbase(pg) + b + 0x48); mp, mo = q.resolve(pg, b + 0x50)
        for i in range(nm_):
            o = mo + i * 0x48; name = q.pstr(*q.resolve(mp, o)).split(',')[0].split('/')[-1]; npar = struct.unpack_from('<I', q.d, q.pbase(mp) + o + 0x34)[0]; pr = q.resolve(mp, o + 0x20)
            for k in range(npar if pr else 0):
                po = pr[1] + 0x18 * k; vr = q.resolve(pr[0], po + 8)
                if not vr: continue
                at = q.pbase(vr[0]) + vr[1]; slots.setdefault(at, []).append((name, q.pstr(*q.resolve(pr[0], po))))
    for at, users in slots.items():
        hit = [u for u in users if u[1] == 'g_colorTint1' and u[0].startswith(MATS)]
        if not hit: continue
        if len(hit) != len(users): sys.exit('value slot shared with another material in %s: %s' % (f, users))
        cur = struct.unpack_from('<3f', buf, at)
        if any(abs(x - y) > 1e-4 for x, y in zip(cur, PS3_TINT)): sys.exit('unexpected tint in %s %s: %s' % (f, hit, cur))
        struct.pack_into('<3f', buf, at, *new); n += 1; print(' ', f, hit[0][0])
    if not n: sys.exit('no blue material found in ' + f)
    if f == 'air-multi-strip.pak':
        for off, x, *_ in json.load(open(os.path.join(HERE, '..', 'excess-wall', 'air-wall.json')))['tris']:
            if struct.unpack_from('<3H', buf, off) != (x, x, x): sys.exit('excess-wall fix not present at %#x' % off)
        print('  excess-wall collapse intact')
    if len(buf) != len(raw) or sum(p != r for p, r in zip(buf, raw)) > n * 12: sys.exit('edit larger than planned: ' + f)
    todo.append((f, d, buf))
if out:
    os.makedirs(out, exist_ok=True)
    for f, d, buf in todo: open(os.path.join(out, f), 'wb').write(buf)
if not apply_:
    print('dry run: overlay not written (use --apply)'); sys.exit(0)
os.makedirs(BK, exist_ok=True)
for f, d, buf in todo:
    if not os.path.exists(BK + f): shutil.copy2(d + f, BK + f)
    open(d + f + '.tmp', 'wb').write(buf); os.replace(d + f + '.tmp', d + f); print('written', f)
print('APPLIED. Undo: --restore --apply')
