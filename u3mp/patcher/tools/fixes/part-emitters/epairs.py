"""PS3/PS4 emitter (type 0x4fec6221) pairs of the 33 MP part modules, and the PS3 emitter inputs of emitconv.
part-emitters copy: merges what build48 used from epairs.py (M3), rules.py (sub3/sub4/iterpairs), harness.py (spans3) and
validate48.py (ps3_inputs), unchanged, plus the s11 sample collection that made s11map.pkl (from the batch48 session log),
so the samples are learned from the player's own files. Modules are passed as bytes, not paths."""
import os, sys, bisect, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dc import DC
from graft44 import Mod

E_TYPE = 0x4fec6221
S3 = [0x134 + 4 * k for k in range(14)]; S4 = [0xa8 + 8 * k for k in range(11)]
NT = (0x4fec6221, 0x77589471, 0x509ad90c, 0x58a8d2b9, 0x110f9a0f, 0xab7f2b3e, 0xf6b95830, 0xb3c01ba2, 0x91f789d)


class M3:
    def __init__(s, p):
        s.c = c = DC(p); s.T = sorted(set(c.ptr(q) for q in c.ptrs if c.ptr(q) < c.reloc) | {o for _, _, o in c.syms})
    def end(s, t):
        k = bisect.bisect_right(s.T, t); return s.T[k] if k < len(s.T) else s.c.reloc


def sub3(a, o, k): return a.c.ptr(o + S3[k]) if (o + S3[k]) in a.c.ptrs else None
def sub4(b, o4, j): return b.c.ptr(o4 + S4[j]) if (o4 + S4[j]) in b.c.ptrs else None


def iterpairs(pairs):
    """pairs: [(module name, M3, Mod)] in module-name order"""
    for m, a, b in pairs:
        for s, t, o in a.c.syms:
            if t != E_TYPE or s not in b.c.bysid: continue
            o4 = b.c.bysid[s][1]
            yield m, s, a, o, b, o4


def spans3(a, o):
    """PS3 slot k -> bytes, using the next distinct slot address (s13: its own span minus a trailing type word)"""
    addrs = [sub3(a, o, k) for k in range(14)]
    out = []
    for k, x in enumerate(addrs):
        if x is None: out.append(None); continue
        nxt = sorted(set(y for y in addrs if y is not None and y > x))
        e = nxt[0] if nxt else a.end(x)
        if nxt and any(addrs[kk] == nxt[0] for kk in (4, 11, 12)): e -= 4
        if not nxt:
            e = a.end(x)
            while e - 4 >= x and a.c.u32(e - 4) in (0x4fec6221, 0x77589471, 0) and (e - 4) not in a.c.ptrs: e -= 4
        out.append(a.c.d[x:e])
    return out


def ps3_inputs(a, o):
    s3 = spans3(a, o); r3 = a.c.d[o:o + 0x16c]
    x11 = sub3(a, o, 11); x12 = sub3(a, o, 12)
    nested = []
    x13 = sub3(a, o, 13)
    for i in range(r3[0x130]):
        t = a.c.ptr(x13 + 4 * i); ty = a.c.u32(t - 4); e = a.end(t)
        if e < a.c.reloc and a.c.u32(e - 4) in NT: e -= 4
        w = a.c.d[t:e]; nested.append((ty, b''.join((w[q:q + 4] if q == 4 else w[q:q + 4][::-1]) for q in range(0, len(w), 4))))
    return r3, s3, a.c.u32(x11 - 4), a.c.u32(x12 - 4), nested


def s11_samples(pairs):
    """PS3 s11 typed struct -> PS4 s8 word lists of every emitter pair, per struct type (what s11map.pkl held)"""
    samples = collections.defaultdict(list)
    for m, s, a, o, b, o4 in iterpairs(pairs):
        x = sub3(a, o, 11); y = sub4(b, o4, 8)
        if x is None: continue
        ty = a.c.u32(x - 4)
        n3 = (a.end(x) - x) // 4 - 1     # minus trailing header of s12
        e4 = b.end(y); n4 = (e4 - y) // 4 - 2
        w3 = [a.c.d[x + 4 * i:x + 4 * i + 4] for i in range(n3)]; w4 = [b.c.d[y + 4 * j:y + 4 * j + 4] for j in range(n4)]
        samples[ty].append((w3, w4))
    return samples
