#!/usr/bin/env python3
"""cb3fix.py IN OUT [MIN]: raise every GEOMETRY_1 prototype's per-instance (scope-3) cbuffer capacity (+0x30) to
max(MIN (8), distinct material feature sets of the prototype + 2), in place (same size, no relocation).
The game allocates exactly that many slots per instance (0xe86b60) and 0xe86f10 dedupes the scope-3 cbuffers of
every material's VS/PS program by id, appending WITHOUT a bounds check; ps4stage's old retail-style 2/3 overflowed
with converted materials (Highrise kMaxAttrDefs assert in 0xe9a490, Museum 0xaaaaaaaa index read at 0x1281ac6).
Retail = distinct programs + 1 (+1 alpha blend).  ps4stage now writes the same rule; crashcheck G6 checks it."""
import os, sys, struct
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.dirname(HERE)]
from ndpak import Pak


def prototypes(q):
    """[(file offset of +0x30, capacity, distinct feature sets, name, water dummy)] of the pak's GEOMETRY_1 prototypes;
    water dummy = only 1-vertex submeshes (ps4stage's per-water-material dummy, retail capacity 1, never drawn)."""
    g = q.items_of('GEOMETRY_1')
    if not g:
        return []
    g = g[0]; pg, b = g.page, g.data_off
    S = lambda a, o: q.pstr(*q.resolve(a, o)) if q.resolve(a, o) else None
    nmat = q.u64(q.pbase(pg) + b + 0x48); mr = q.resolve(pg, b + 0x50); feats = {}
    for i in range(nmat if mr else 0):
        mp, mo = mr[0], mr[1] + 0x48 * i
        nf = struct.unpack_from('<I', q.d, q.pbase(mp) + mo + 0x30)[0]
        fr = q.resolve(mp, mo + 0x10)
        feats[S(mp, mo)] = frozenset(q.pstr(*q.resolve(fr[0], fr[1] + 8 * k)) for k in range(nf if fr else 0))
    nsub = q.u64(q.pbase(pg) + b + 8); sr = q.resolve(pg, b + 0x10)
    submat = {S(sr[0], sr[1] + 0x88 * k + 8): S(sr[0], sr[1] + 0x88 * k + 0x48) for k in range(nsub)}
    subnv = {S(sr[0], sr[1] + 0x88 * k + 8): q.pu32(sr[0], sr[1] + 0x88 * k + 0x20) for k in range(nsub)}
    out = []
    pl = q.resolve(pg, b + 0x20)
    for i in range(q.u64(q.pbase(pg) + b + 0x18)):
        r = q.resolve(pl[0], pl[1] + 8 * i)
        n = q.pu32(r[0], r[1] + 4); snr = q.resolve(r[0], r[1] + 0x18)
        sn = [S(snr[0], snr[1] + 0x10 * k) for k in range(n)]
        fs = {feats.get(submat.get(x)) for x in sn}
        dummy = bool(sn) and all(subnv.get(x) == 1 for x in sn)
        out.append((q.pbase(r[0]) + r[1] + 0x30, q.pu32(r[0], r[1] + 0x30), len(fs), S(r[0], r[1] + 0x10) or 'proto%d' % i, dummy))
    return out


def need(nfs, cap_min=8):
    return max(cap_min, nfs + 2)


def fix(src, dst, cap_min=8):
    q = Pak(src)
    d = bytearray(q.d)
    ps = prototypes(q); ch = 0
    for o, cap, nfs, _, dummy in ps:
        if not dummy and cap < need(nfs, cap_min):
            struct.pack_into('<I', d, o, need(nfs, cap_min)); ch += 1
    open(dst, 'wb').write(bytes(d))
    return len(ps), ch


if __name__ == '__main__':
    n, ch = fix(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 8)
    print('%s: %d prototypes, %d raised' % (os.path.basename(sys.argv[2]), n, ch))
