#!/usr/bin/env python3
"""vispatch.py IN.pak OUT.pak : in-place PVS bit-layout fix for an already built PS4 BG/stage pak (agent O).
Converted paks carry the PS3 VIS_INFO_1 leaf streams verbatim (big-endian u32 bit words); the PS4 runtime
reads little-endian words (see visinfo.swap_bits).  This decodes every leaf stream, pads to whole words with
0xff, byte-swaps each u32, re-encodes with the pak's own Huffman dict and writes it over the old stream
(same item layout; the new stream must fit the old slot = bytes up to the next 16-aligned object), then
byte-swaps the G bit sets.  Exit 2 when a stream cannot be re-encoded in place (rebuild with ps4stage)."""
import sys, os, struct, json
sys.path[:0] = [os.path.dirname(os.path.abspath(__file__)), os.path.dirname(os.path.dirname(os.path.abspath(__file__)))]
from ndpak import Pak
from visinfo import huff_len, _sw4, _codes, _encode


def patch(src, dst):
    q = Pak(src); d = bytearray(q.d)
    its = q.items_of('VIS_INFO_1')
    if not its:
        open(dst, 'wb').write(d); return dict(vis=False)
    it = its[0]; pg, o = it.page, it.data_off
    U = lambda a, k=0: struct.unpack_from('<I', q.d, q.pbase(a[0]) + a[1] + k)[0]
    R = lambda a, k=0: q.resolve(a[0], a[1] + k)
    h = (pg, o)
    if U(h) != 9:
        raise ValueError('VIS version')
    nreg, nw, ndict, ng = U(h, 4), U(h, 0xb0), U(h, 0xb4), U(h, 0xc0)
    dr = R(h, 0xb8); db = q.pbase(dr[0]) + dr[1]
    dic = [struct.unpack_from('<HH', q.d, db + 4 * i) for i in range(ndict)]
    codes = _codes(dic)
    regs = R(h, 8)
    leaves = {}
    for i in range(nreg):
        rg = (regs[0], regs[1] + 0x40 * i)
        nd = R(rg, 8)
        if not nd: continue
        seen, todo = set(), [0]
        while todo:
            n = todo.pop()
            if n in seen: continue
            seen.add(n)
            a, b = struct.unpack_from('<ii', q.d, q.pbase(nd[0]) + nd[1] + 0x18 * n)
            sp = R(nd, 0x18 * n + 0x10)
            if sp:
                leaves[q.pbase(sp[0]) + sp[1]] = sp
            else:
                todo += [c for c in (a >> 2, b) if c >= 0]
    starts = sorted(leaves)
    # highest PVS bit in use: instance id + one bit per submesh (ids are PS3 x8 & 0xffff, submesh j = id + 1 + j)
    g0 = q.items_of('GEOMETRY_1')[0]; gpg, gb = g0.page, g0.data_off
    ni = q.u64(q.pbase(gpg) + gb + 0x28); il = q.resolve(gpg, gb + 0x30); used = 0
    for i in range(ni):
        r = q.resolve(il[0], il[1] + 8 * i); base = q.pbase(r[0]) + r[1]
        iid, = struct.unpack_from('<I', q.d, base + 4); nsub, = struct.unpack_from('<I', q.d, base + 0x20)
        used = max(used, iid + nsub + 1)

    st = dict(vis=True, leaves=len(starts), nwords=nw, used_bits=used, changed=0, padded=0, same_len=0, shorter=0)
    for k, a in enumerate(starts):
        raw = bytes(q.d[a:a + 0x10000])
        n, out = huff_len(raw, dic)
        pad = -len(out) % 4
        st['padded'] += pad > 0
        new = bytearray(_sw4(out + b'\xff' * pad)[:4 * nw])
        # bits past the last used id are don't-care: set them, then drop trailing 0xff bytes (the runtime buffer
        # starts all-ones and only decoded bytes are ANDed in), so a padded last word does not grow the stream
        for bit in range(used, 8 * len(new)):
            new[bit >> 3] |= 1 << (bit & 7)
        new = bytes(new).rstrip(b'\xff')
        miss = set(new) - set(codes)
        if miss:
            raise SystemExit(2)
        enc = _encode(new, codes)
        slot = n
        while a + slot < len(q.d) and q.d[a + slot] == 0xaa and (a + slot) % 16: slot += 1
        if len(enc) > slot:
            print('stream at 0x%x: %d > slot %d' % (a, len(enc), slot)); raise SystemExit(2)
        assert huff_len(enc + b'\0' * 8, dic)[1] == new
        # equivalence on the used bits: PS4 LE bit k of the new set == PS3 BE bit k of the old one
        old = out + b'\xff' * (4 * nw - len(out)); nn = new + b'\xff' * (4 * nw - len(new))
        for bit in range(0, used):
            w, b_ = bit >> 5, bit & 31
            assert ((old[4 * w + 3 - (b_ >> 3)] >> (b_ & 7)) & 1) == ((nn[bit >> 3] >> (bit & 7)) & 1)
        d[a:a + n] = b'\xaa' * n
        d[a:a + len(enc)] = enc
        st['changed'] += enc != raw[:n]; st['same_len'] += len(enc) == n; st['shorter'] += len(enc) < n
    g = R(h, 0xc8)
    for j in range(ng):
        r = R(g, 16 * j + 8)
        if r:
            b = q.pbase(r[0]) + r[1]
            d[b:b + 4 * nw] = _sw4(bytes(q.d[b:b + 4 * nw]))
    st['G'] = ng
    open(dst, 'wb').write(d)
    return st


if __name__ == '__main__':
    print(json.dumps(patch(sys.argv[1], sys.argv[2])))
