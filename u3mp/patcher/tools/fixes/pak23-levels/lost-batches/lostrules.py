"""lostrules.py: the lost in-place batches as content rules (not md5-locked), for replaying them on a converter output.
  rule99(data)  batch99-lmuv: clear +0x20 bit 0x100 where slot-5 lightmap uvs are integer rows outside [0,1]
  rule100(data, classes='AC')  batch100 A/C (+B unless dropped; B is what revertB undoes)
  levelinfo(data, value)  n015/n015b: LEVEL_INFO_1 := value
Each returns a list of (offset, old_u32, new_u32); apply(data, writes) writes them into a bytearray."""
import os, sys, struct
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'lost', 'batch100-blockers', 'tools'))
from ndpak import Pak
from ps4bg import streams


def _instsubs(q):
    its = q.items_of('GEOMETRY_1')
    if not its: return
    g = its[0]; pg, b = g.page, g.data_off; il = q.resolve(pg, b + 0x30); n = q.u64(q.pbase(pg) + b + 0x28)
    if not il: return
    nsub = q.u64(q.pbase(pg) + b + 8); sref = q.resolve(pg, b + 0x10); mat = {}
    for k in range(nsub):
        o = sref[1] + 0x88 * k; nm = q.pstr(*q.resolve(sref[0], o + 8)); mr = q.resolve(sref[0], o + 0x48)
        mat[nm] = (q.pstr(*mr) if mr else '').split(',')[0]
    for i in range(n):
        r = q.resolve(il[0], il[1] + 8 * i); ns = q.pu32(r[0], r[1] + 0x20); sr = q.resolve(r[0], r[1] + 0x30)
        if not sr: continue
        for k in range(ns):
            o = sr[1] + 0x50 * k
            yield i, k, sr[0], o, q.pbase(sr[0]) + o, mat.get(q.pstr(*q.resolve(sr[0], o)), '?')


def _slot5(q, pg, o):
    nst = q.pu32(pg, o + 0x10); ref = q.resolve(pg, o + 0x18)
    for s in (streams(q, ref, nst) if ref else []):
        if s['slot'] == 5 and s['type'] == 0x22 and s['data'] is not None:
            return s['data']


def rule99(data):
    q = Pak(bytes(data)); W = []
    for i, k, pg, o, ab, m in _instsubs(q) or []:
        f = struct.unpack_from('<I', q.d, ab + 0x20)[0]
        if not f & 0x100: continue
        d = _slot5(q, pg, o)
        if d is None: continue
        if (d.max() > 1.001 or d.min() < -0.001) and np.all(np.abs(d - np.round(d)) < 1e-4):
            W.append((ab + 0x20, f, f & ~0x100))
    return W


def rule100(data, classes='ABC'):
    q = Pak(bytes(data)); W = {}
    for i, k, pg, o, ab, m in _instsubs(q) or []:
        f, f2 = struct.unpack_from('<II', q.d, ab + 0x20)
        blk = 'shadowblocker' in m or 'ik-only' in m or 'canopy-shadow' in m
        n20 = f
        if f & 0x100 and 'C' in classes:
            d = _slot5(q, pg, o)
            if d is not None and len(d) and (d == np.round(d)).all(): n20 &= ~0x100
        if blk and not (f & 0x10000 and f2 == 0x101):
            cls = 'A' if f & 0x10000 else 'B'
            if cls in classes:
                if cls == 'B': n20 |= (0x1010000 if 'ik-only' in m else 0x10000)
                W[ab + 0x24] = (f2, f2 | 0x101)
        if n20 != f: W[ab + 0x20] = (f, n20)
    return [(a, b, c) for a, (b, c) in sorted(W.items())]


def levelinfo(data, value):
    q = Pak(bytes(data)); it = q.items_of('LEVEL_INFO_1')
    off = q.pbase(it[0].page) + it[0].data_off
    old = struct.unpack_from('<I', q.d, off)[0]
    return [] if old == value else [(off, old, value)]


def apply(data, W):
    for off, old, new in W:
        assert struct.unpack_from('<I', data, off)[0] == old, hex(off)
        struct.pack_into('<I', data, off, new)
    return data
