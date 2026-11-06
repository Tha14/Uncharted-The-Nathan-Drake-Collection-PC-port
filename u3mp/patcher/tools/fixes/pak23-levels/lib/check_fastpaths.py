"""Compare lib/fastpaths.py with the original functions of a paktools tree on random inputs (bit-exact or exit 1).
usage: check_fastpaths.py <paktools dir> [rounds]"""
import os
import random
import sys

sys.path[:0] = [sys.argv[1], os.path.join(sys.argv[1], 'stage'), os.path.dirname(os.path.abspath(__file__))]
import numpy as np
import collision
import ps3geom
import fastpaths

N = int(sys.argv[2]) if len(sys.argv) > 2 else 200000
rnd = random.Random(1106)
F32 = np.float32
bad = 0


def same_box(a, b):
    return a[0] == b[0] and a[1].tobytes() == b[1].tobytes() and a[2].tobytes() == b[2].tobytes()


def rbox(scale):
    lo = np.array([rnd.uniform(-scale, scale) for _ in range(3)], dtype=F32)
    ext = np.array([abs(rnd.gauss(0, scale)) * rnd.choice((0, 1e-7, 1e-3, 1, 1, 1)) for _ in range(3)], dtype=F32)
    return lo, (lo + ext).astype(F32)


for i in range(N):
    scale = rnd.choice((1e-3, 0.5, 1.0, 2.0, 50.0, 1000.0, 3e4))
    pmin, pmax = rbox(scale)
    # a child box inside (mostly), on the parent's faces, or slightly outside
    t0 = np.array([rnd.choice((0.0, 1.0, rnd.random(), rnd.random(), -1e-6, 1 + 1e-6)) for _ in range(3)])
    t1 = np.array([rnd.choice((0.0, 1.0, rnd.random(), rnd.random(), -1e-6, 1 + 1e-6)) for _ in range(3)])
    a = pmin + (pmax - pmin) * np.minimum(t0, t1)
    b = pmin + (pmax - pmin) * np.maximum(t0, t1)
    kind = i % 3
    cmin, cmax = (a.astype(F32), b.astype(F32)) if kind else (list(map(float, a)), list(map(float, b)))
    if kind == 2:
        pmin, pmax = list(map(float, pmin)), list(map(float, pmax))
    if not same_box(collision._enc_box(cmin, cmax, pmin, pmax), fastpaths.enc_box(cmin, cmax, pmin, pmax)):
        bad += 1
        if bad < 5:
            print('enc_box differs', cmin, cmax, pmin, pmax)
for i in range(max(2000, N // 50)):
    widths = [rnd.choice((0, 1, 2, 5, 8, 10, 11, 12, 16, 21, 32)) for _ in range(rnd.randint(1, 4))]
    count = rnd.randint(0, 60)
    nbytes = (count * sum(widths) + 7) // 8 + rnd.choice((0, 0, 1, 3))
    buf = bytes(rnd.getrandbits(8) for _ in range(nbytes))
    o = f = None
    try:
        o = ps3geom._bits_be(buf, widths, count)
    except ValueError:
        o = 'ValueError'
    try:
        f = fastpaths.bits_be(buf, widths, count)
    except ValueError:
        f = 'ValueError'
    oe, fe = isinstance(o, str), isinstance(f, str)
    if oe != fe or (not oe and (o.dtype != f.dtype or o.shape != f.shape or not np.array_equal(o, f))):
        bad += 1
        if bad < 10:
            print('bits_be differs', widths, count, nbytes)
print('fastpaths == original on %d boxes and %d bit records' % (N, max(2000, N // 50)) if not bad else '%d differences' % bad)
sys.exit(1 if bad else 0)
