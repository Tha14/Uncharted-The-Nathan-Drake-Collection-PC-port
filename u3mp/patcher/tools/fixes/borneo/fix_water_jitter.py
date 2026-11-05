#!/usr/bin/env python3
"""Restore PS3 refraction jitter on converted water materials (PS4 stage paks in the overlays).

ps4stage.py only splits PS3 packed constants when U3_STAGE_WATERPACK=1 (default 0), so
g_fRefractionJitter / g_fRefractionJitterHeight keep the donor template's values instead of PS3
'g_fRefractionJitter_fRefractionJitterHeight'. Only those two floats are changed, in place; file size
and pointers do not change.

Dry run by default. --apply writes <pak>.bak-jitter (never overwritten) then patches the pak.
usage: python -I fix_water_jitter.py [--apply] [--only REGEX]
Run with -I from another directory."""
import glob, os, re, struct, sys

SCRATCH = r"{{SCRATCH}}"
sys.path[:0] = [SCRATCH + "/gh2/u3mp/paktools", SCRATCH + "/gh2/u3mp/paktools/stage",
                "{{TRANSFER_TOOLS}}"]
from ndpak import Pak
from psarc_lib import Psarc
import ps3params

WORK = "{{PROJECT}}/work/"
OVERLAYS = ('overlay-r3', 'overlay-r2', 'overlay-port', 'overlay-mp')   # first wins, as in the boot log
PS3 = "{{PS3_DISC}}"
DATA2 = "{{PS3_DATA2}}/build/main/pak23"
PACKED, J, H = 'g_fRefractionJitter_fRefractionJitterHeight', 'g_fRefractionJitter', 'g_fRefractionJitterHeight'

args = sys.argv[1:]
apply_ = '--apply' in args
only = re.compile(args[args.index('--only') + 1]) if '--only' in args else None

arcs = [Psarc(PS3 + '/goty-patch.psarc'), Psarc(PS3 + '/goty-dlc.psarc'), Psarc(PS3 + '/build/main/pak23.psarc')]
arcs += [Psarc(f) for f in glob.glob(glob.escape(DATA2) + '/*.psarc')]


def ps3_pak(name):
    for a in arcs:
        for n in a.names:
            if os.path.basename(n) == name:
                return a.read(a.names.index(n) + 1)
    return None


effective = {}
for o in OVERLAYS:
    for f in glob.glob(glob.escape(WORK + o) + '/u3data/build/main/pak23/ps4/*.pak'):
        effective.setdefault(os.path.basename(f), f)

total = 0
for name, path in sorted(effective.items()):
    if 'ingame12' in name or (only and not only.search(name)):
        continue
    raw = open(path, 'rb').read()
    if b'water.fx' not in raw:
        continue
    q = Pak(raw)
    geo = q.items_of('GEOMETRY_1')
    if not geo:
        continue
    src = ps3_pak(name)
    if src is None:
        print('NO PS3 SOURCE', name)
        continue
    p3 = ps3params.all_material_params(Pak(src))
    g = geo[0]
    nmat = q.pu32(g.page, g.data_off + 0x48)
    mp, mo = q.resolve(g.page, g.data_off + 0x50)
    uses = {}
    edits = []
    for i in range(nmat):
        o = mo + i * 0x48
        full = q.pstr(*q.resolve(mp, o))
        sh = q.resolve(mp, o + 8)
        npar = struct.unpack_from('<I', q.d, q.pbase(mp) + o + 0x34)[0]
        pr = q.resolve(mp, o + 0x20)
        rec = {}
        for k in range(npar if pr else 0):
            po = pr[1] + 0x18 * k
            vr = q.resolve(pr[0], po + 8)
            pn = q.pstr(*q.resolve(pr[0], po))
            if vr:
                off = q.pbase(vr[0]) + vr[1]
                uses[off] = uses.get(off, 0) + 1
                rec[pn] = off
        if not sh or 'water' not in q.pstr(*sh) or J not in rec:
            continue
        key = full.split(',')[0].replace(':', '/')
        want = p3.get(key)
        if not want:
            print('  no PS3 material', name, key)
            continue
        vals = want.get(PACKED) or ([want[J][0], want[H][0]] if J in want and H in want else None)
        if not vals:
            print('  no PS3 jitter', name, key)
            continue
        for pn, v in ((J, vals[0]), (H, vals[1])):
            if pn in rec:
                old = struct.unpack_from('<f', raw, rec[pn])[0]
                if abs(old - v) > 1e-4:
                    edits.append((key, pn, rec[pn], old, v))
    if not edits:
        continue
    print(name, path.split('work/')[-1].split('/')[0])
    data = bytearray(raw)
    for key, pn, off, old, v in edits:
        shared = uses.get(off, 0) > 1
        print('  %-62s %-27s @%#x %.4f -> %.4f%s' % (key[-62:], pn, off, old, v, '  SHARED, skipped' if shared else ''))
        if not shared:
            struct.pack_into('<f', data, off, v)
            total += 1
    if apply_:
        bak = path + '.bak-jitter'
        if not os.path.exists(bak):
            open(bak, 'wb').write(raw)
        open(path, 'wb').write(data)
print('%s: %d floats' % ('APPLIED' if apply_ else 'DRY RUN, would change', total))
