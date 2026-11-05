#!/usr/bin/env python3
"""Borneo moving floor: dig-grass-main (dominant ground material) was converted with a GLOBAL_LEAF (foliage sway) template in
dig-coop-approach / -ship / -stage. Dropdown's copy has no GLOBAL_LEAF; PS3 carries no leaf/wind constants.
Repoints that material's feature list and effect to the same pak's dig-tree-rot-log:default (same features minus GLOBAL_LEAF and
SPECULAR_LIGHT_APPLY_METALNESS, same texture bind names). Own textures and specular values stay.
Dry run by default. --apply writes <pak>.bak-leaf once, then patches 20 bytes in place. Undo: copy .bak-leaf back.
usage: python -I fix_grass_leaf.py [--apply]"""
import struct, sys, os
SCRATCH = r"{{SCRATCH}}"
sys.path[:0] = [SCRATCH + "/gh2/u3mp/paktools"]
from ndpak import Pak
D = "{{PROJECT}}/work/overlay-r3/u3data/build/main/pak23/ps4/"
TARGET, DONOR = 'levels/dig/ground/dig-grass-main:default', 'levels/dig/tree/dig-tree-rot-log:default'
apply_ = '--apply' in sys.argv
bad = 0
def info(q, mp, o):
    nf, npar, nb, _ = struct.unpack_from('<4I', q.d, q.pbase(mp) + o + 0x30)
    fr = q.resolve(mp, o + 0x10)
    fe = [q.pstr(*q.resolve(fr[0], fr[1] + 8 * k)) for k in range(nf)]
    br = q.resolve(mp, o + 0x28); sl = []
    for k in range(nb):
        a = q.resolve(br[0], br[1] + 0x20 * k)
        sl.append(q.pstr(*a) if a else None)
    return nf, nb, fe, fr, q.resolve(mp, o + 0x18), sl
for pak in ('dig-coop-approach', 'dig-coop-ship', 'dig-coop-stage'):
    path = D + pak + '.pak'
    raw = open(path, 'rb').read(); q = Pak(raw)
    g = q.items_of('GEOMETRY_1')[0]; n = q.pu32(g.page, g.data_off + 0x48); mp, mo = q.resolve(g.page, g.data_off + 0x50)
    m = {}
    for i in range(n):
        o = mo + i * 0x48
        m.setdefault(q.pstr(*q.resolve(mp, o)).split(',')[0], o)
    if TARGET not in m:
        print(pak, 'no target'); continue
    t = info(q, mp, m[TARGET])
    if 'GLOBAL_LEAF' not in t[2]:
        print(pak, 'already without GLOBAL_LEAF'); continue
    if DONOR not in m:
        print(pak, 'NO DONOR, needs a reconvert'); bad += 1; continue
    d = info(q, mp, m[DONOR])
    diff = sorted(set(t[2]) ^ set(d[2]))
    ok = diff == ['GLOBAL_LEAF', 'SPECULAR_LIGHT_APPLY_METALNESS'] and t[1] == d[1] and t[5] == d[5] and t[3][0] == d[3][0] and t[4][0] == d[4][0]
    print(pak, 'target @%#x donor @%#x' % (q.pbase(mp) + m[TARGET], q.pbase(mp) + m[DONOR]), 'feature diff', diff, 'slots', t[5], 'same page', t[4][0] == d[4][0], 'OK' if ok else 'REFUSED')
    if not ok:
        bad += 1; continue
    a, b = q.pbase(mp) + m[TARGET], q.pbase(mp) + m[DONOR]
    print('   +0x10 %s -> %s  +0x18 %s -> %s  count %d -> %d' % (raw[a+0x10:a+0x18].hex(), raw[b+0x10:b+0x18].hex(), raw[a+0x18:a+0x20].hex(), raw[b+0x18:b+0x20].hex(), t[0], d[0]))
    if apply_:
        data = bytearray(raw)
        data[a+0x10:a+0x20] = raw[b+0x10:b+0x20]
        data[a+0x30:a+0x34] = raw[b+0x30:b+0x34]
        if not os.path.exists(path + '.bak-leaf'):
            open(path + '.bak-leaf', 'wb').write(raw)
        open(path, 'wb').write(data)
print('APPLIED' if apply_ else 'DRY RUN, nothing written', 'refused:', bad)
