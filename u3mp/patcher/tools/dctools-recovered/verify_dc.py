#!/usr/bin/env python3
"""verify_dc.py <base.bin> <new.bin> [allowed patch offsets as hex,len ...]
Generic append-only check for a PS4 DC module: parses with paktools/dc.py and dcconv/dclib.py,
container layout, pointer slots, original bytes/pointer bits unchanged except listed fields."""
import sys, struct
sys.path.insert(0, '/home/claude/u3/paktools'); sys.path.insert(0, '/home/claude/u3/dcconv')
import dc as dcA, dclib as dcB

def check(base, new, allowed):
    A = dcA.DC(base); N = dcA.DC(new); NB = dcB.DC(open(new, 'rb').read())
    d = N.d
    assert NB.nsym == len(N.syms) == len(A.syms)
    assert N.syms == A.syms, 'symbol table changed'
    nb = struct.unpack_from('<I', d, N.reloc)[0]
    assert N.reloc + 4 + nb == len(d) and nb == (N.reloc + 63) // 64, 'container size'
    for p in N.ptrs:
        assert p % 8 == 0 and p < N.reloc, hex(p)
        t = N.ptr(p); assert 0 < t < N.reloc, (hex(p), hex(t))
    ok = set()
    for o, l in allowed:
        ok.update(range(o, o + l))
    ok.update(range(8, 16))
    diff = [i for i in range(A.reloc) if A.d[i] != d[i] and i not in ok]
    assert not diff, 'unexpected original byte changes at %s' % [hex(x) for x in diff[:10]]
    lost = [p for p in A.ptrs if p not in N.ptrs]
    assert not lost, 'pointer bits lost %s' % lost[:5]
    newin = [p for p in N.ptrs if p < A.reloc and p not in A.ptrs]
    assert all(any(o <= p < o + l for o, l in allowed) for p in newin), 'new pointer bits in original region'
    return A, N

if __name__ == '__main__':
    allowed = [(int(x.split(',')[0], 16), int(x.split(',')[1])) for x in sys.argv[3:]]
    check(sys.argv[1], sys.argv[2], allowed); print('OK')
