"""verify_syms.py <before.bin> <after.bin>: after = before with symbols inserted (pure relayout check)"""
import sys, struct
sys.path.insert(0, '.')
from common import DC
def check(bp, ap):
    A = DC(bp); N = DC(ap)
    na, nn = len(A.syms), len(N.syms); delta = 16 * (nn - na)
    start = 0x18 + 16 * na
    mv = lambda o: o + delta if o >= start else o
    # symbols: all old present, values moved
    old = [(s, t, mv(v)) for s, t, v in A.syms]
    newl = list(N.syms); it = iter(newl)
    assert all(any(x == y for y in it) for x in old), 'old symbols not preserved in order'
    assert [x[0] for x in N.syms] == sorted(x[0] for x in N.syms)
    # data bytes: non-pointer words identical, pointer words moved
    for o in range(start, A.reloc, 8):
        if o in A.ptrs:
            assert (o + delta) in N.ptrs and N.ptr(o + delta) == mv(A.ptr(o)), hex(o)
        else:
            assert A.d[o:o + 8] == N.d[o + delta:o + delta + 8], hex(o)
            assert (o + delta) not in N.ptrs, hex(o)
    for p in N.ptrs:
        assert p < N.reloc and 0 < N.ptr(p) < N.reloc
    nb = struct.unpack_from('<I', N.d, N.reloc)[0]
    assert N.reloc + 4 + nb == len(N.d) and nb == (N.reloc + 63) // 64
    return delta
if __name__ == '__main__':
    print('relayout OK, delta', check(sys.argv[1], sys.argv[2]))
