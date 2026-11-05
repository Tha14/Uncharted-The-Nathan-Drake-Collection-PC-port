#!/usr/bin/env python3
"""visoff.py IN.pak OUT.pak : (agent P) switch the PVS off in place: every VIS_INFO_1 leaf decodes to nWords*4 bytes of 0xff
(every instance / submesh visible from every leaf), in both the AND (init 0xff) and OR (init 0) decode modes of vis-probe
(0xf223e0), and independent of the bit-word byte order.  The Huffman dict is replaced by one node {0xff, END}; each leaf stream
becomes 4*nWords '0' bits + one '1' bit; all leaves of a page point at one copy written over the old leaf-stream area (pointer
values are page-relative, the fixup table is unchanged).  G bit sets, regions, planes and the tree are untouched.
Diagnostic use: tells whether occlusion culling causes missing geometry, at the price of drawing everything."""
import sys, os, struct, json
sys.path[:0] = [os.path.dirname(os.path.abspath(__file__)), os.path.dirname(os.path.dirname(os.path.abspath(__file__)))]
from ndpak import Pak
from visinfo import huff_len


def patch(src, dst):
    q = Pak(src); d = bytearray(q.d)
    its = q.items_of('VIS_INFO_1')
    if not its:
        open(dst, 'wb').write(d); return dict(vis=False)
    it = its[0]; pg, o = it.page, it.data_off; h = (pg, o)
    U = lambda a, k=0: struct.unpack_from('<I', q.d, q.pbase(a[0]) + a[1] + k)[0]
    R = lambda a, k=0: q.resolve(a[0], a[1] + k)
    assert U(h) == 9
    nreg, nw, ndict = U(h, 4), U(h, 0xb0), U(h, 0xb4)
    dr = R(h, 0xb8); db = q.pbase(dr[0]) + dr[1]
    dic = [struct.unpack_from('<HH', q.d, db + 4 * i) for i in range(ndict)]
    nbits = 4 * nw * 8 + 1
    stream = bytes(int(('0' * (4 * nw) + '1').ljust(-(-nbits // 8) * 8 if False else ((4 * nw + 1 + 7) // 8) * 8, '0')[i:i + 8], 2)
                   for i in range(0, ((4 * nw + 1 + 7) // 8) * 8, 8))
    need = len(stream)
    regs = R(h, 8)
    leafptr = []        # (abs addr of the pointer slot, dest page, dest off)
    slots = {}          # dest (page, off) -> slot length
    for i in range(nreg):
        rg = (regs[0], regs[1] + 0x40 * i)
        nd = R(rg, 8)
        if not nd: continue
        seen, todo = set(), [0]
        while todo:
            n = todo.pop()
            if n in seen: continue
            seen.add(n)
            a, b = struct.unpack_from('<Ii', q.d, q.pbase(nd[0]) + nd[1] + 0x18 * n)
            sp = R(nd, 0x18 * n + 0x10)
            if sp:
                leafptr.append((q.pbase(nd[0]) + nd[1] + 0x18 * n + 0x10, sp))
                if sp not in slots:
                    ln, _ = huff_len(q.d[q.pbase(sp[0]) + sp[1]:q.pbase(sp[0]) + sp[1] + 0x10000], dic)
                    e = q.pbase(sp[0]) + sp[1] + ln
                    while e < len(q.d) and q.d[e] == 0xaa and e % 16: e += 1
                    slots[sp] = e - (q.pbase(sp[0]) + sp[1])
            else:
                left = struct.unpack('<i', struct.pack('<I', a))[0] >> 2
                todo += [c for c in (left, b) if c >= 0]
    # per destination page: the largest contiguous run of leaf slots
    home = {}
    for page in sorted({p for p, _ in slots}):
        offs = sorted(off for p, off in slots if p == page)
        best = (0, None); run_s = offs[0]; run_e = offs[0] + slots[(page, offs[0])]
        for off in offs[1:]:
            if off <= run_e + 16 and off >= run_e: run_e = max(run_e, off + slots[(page, off)])
            else:
                best = max(best, (run_e - run_s, run_s)); run_s, run_e = off, off + slots[(page, off)]
        best = max(best, (run_e - run_s, run_s))
        if best[0] < need:
            raise SystemExit('page %d: largest leaf area %d < %d' % (page, best[0], need))
        home[page] = best[1]
        a0 = q.pbase(page) + best[1]
        d[a0:a0 + need] = stream
    for slot, (p_, off) in leafptr:
        struct.pack_into('<Q', d, slot, home[p_])
    struct.pack_into('<HH', d, db, 0xff, 0x100)
    struct.pack_into('<I', d, q.pbase(pg) + o + 0xb4, 1)
    open(dst, 'wb').write(d)
    return dict(vis=True, leaves=len(slots), nwords=nw, stream=need, pages=len(home))


def verify(path):
    """every leaf decodes to exactly nWords*4 x 0xff"""
    q = Pak(path); it = q.items_of('VIS_INFO_1')[0]; pg, o = it.page, it.data_off
    U = lambda a, k=0: struct.unpack_from('<I', q.d, q.pbase(a[0]) + a[1] + k)[0]
    R = lambda a, k=0: q.resolve(a[0], a[1] + k); h = (pg, o)
    nreg, nw, ndict = U(h, 4), U(h, 0xb0), U(h, 0xb4); dr = R(h, 0xb8); db = q.pbase(dr[0]) + dr[1]
    dic = [struct.unpack_from('<HH', q.d, db + 4 * i) for i in range(ndict)]
    regs = R(h, 8); n_ok = 0
    for i in range(nreg):
        nd = R((regs[0], regs[1] + 0x40 * i), 8)
        if not nd: continue
        seen, todo = set(), [0]
        while todo:
            n = todo.pop()
            if n in seen: continue
            seen.add(n)
            a, b = struct.unpack_from('<Ii', q.d, q.pbase(nd[0]) + nd[1] + 0x18 * n)
            sp = R(nd, 0x18 * n + 0x10)
            if sp:
                _, out = huff_len(q.d[q.pbase(sp[0]) + sp[1]:q.pbase(sp[0]) + sp[1] + 0x10000], dic)
                assert out == b'\xff' * (4 * nw), (len(out), out[:8])
                n_ok += 1
            else:
                left = struct.unpack('<i', struct.pack('<I', a))[0] >> 2
                todo += [c for c in (left, b) if c >= 0]
    return n_ok


if __name__ == '__main__':
    r = patch(sys.argv[1], sys.argv[2]); r['verified_leaves'] = verify(sys.argv[2]) if r.get('vis') else 0
    print(json.dumps(r))
