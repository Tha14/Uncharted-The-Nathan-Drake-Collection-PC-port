#!/usr/bin/env python3
"""Facility wall fix (2026-10-31, inline session code 004 + 005, see recipe/transcript-mining/inline-writes/).
In overlay-r3 pak23/ps4/facility-multi.pak, GEOMETRY_1 Prototype360 is a coarse low-poly shell that drew a wall across
the walk-through doorway:
  1. every triangle of a '.Prototype360.' submesh whose extent exceeds 6 m is collapsed (index triple -> first index);
  2. the remaining box in Prototype360.Submesh1 (x -32.2..-28.4, y 0.2..4.9, z -14.2..-10.25) is collapsed too.
Idempotent: already collapsed triangles are skipped.  Backup: work/backup-wall/facility-multi.pak (written once).
usage: facility_collapse.py [--apply]      (dry run without --apply)
"""
import os
import shutil
import struct
import sys

sys.path.insert(0, '{{RECOVERED}}')      # mp1.py (Pak wrapper over ndpak) lives next to the fix jobs
from mp1 import Pak                       # noqa: E402

W = '{{PROJECT}}/work/'
F = W + 'overlay-r3/u3data/build/main/pak23/ps4/facility-multi.pak'
BK = W + 'backup-wall/'


def main():
    apply_ = '--apply' in sys.argv
    if not os.path.exists(F):
        print('facility-multi.pak not in overlay-r3: nothing to do (needs the converted level pak)'); return 3
    d = bytearray(open(F, 'rb').read())
    P = Pak(bytes(d))
    it = [i for i in P.items if i.type == 'GEOMETRY_1'][0]
    b = P.pbase(it.page) + it.data_off
    ns = struct.unpack_from('<I', d, b + 8)[0]
    tp, to = P.resolve(it.page, it.data_off + 0x10)
    pb = P.pbase(tp)
    big = box = 0
    for i in range(ns):
        o_ = to + i * 0x88
        nm = P.pstr(*P.resolve(tp, o_ + 8)) or ''
        if '.Prototype360.' not in nm:
            continue
        w = struct.unpack_from('<34I', d, pb + o_)
        st = P.resolve(tp, o_ + 0x30)
        sq = P.pbase(st[0]) + st[1]
        stride = struct.unpack_from('<H', d, sq + 2)[0]
        da = P.resolve(st[0], st[1] + 16)
        vb = P.pbase(da[0]) + da[1]
        ia = P.resolve(tp, o_ + 0x40)
        ib = P.pbase(ia[0]) + ia[1]
        V = [struct.unpack_from('<3f', d, vb + k * stride) for k in range(w[8])]
        sub1 = nm.endswith('Prototype360.Submesh1')
        for k in range(0, w[9] - 2, 3):
            x = struct.unpack_from('<3H', d, ib + k * 2)
            if x[0] == x[1] == x[2]:
                continue
            T = [V[j] for j in x]
            if max(max(t[j] for t in T) - min(t[j] for t in T) for j in range(3)) > 6:
                struct.pack_into('<3H', d, ib + k * 2, x[0], x[0], x[0]); big += 1
            elif sub1 and all(-32.2 <= v[0] <= -28.4 and -14.2 <= v[2] <= -10.25 and 0.2 <= v[1] <= 4.9 for v in T):
                struct.pack_into('<3H', d, ib + k * 2, x[0], x[0], x[0]); box += 1
    print('triangles to hide: %d large, %d box' % (big, box))
    if not apply_:
        print('dry run'); return 0
    if big or box:
        os.makedirs(BK, exist_ok=True)
        if not os.path.exists(BK + 'facility-multi.pak'):
            shutil.copy2(F, BK + 'facility-multi.pak')
        open(F + '.tmp', 'wb').write(d); os.replace(F + '.tmp', F)
        print('written', F)
    else:
        print('already applied')
    return 0


if __name__ == '__main__':
    sys.exit(main())
