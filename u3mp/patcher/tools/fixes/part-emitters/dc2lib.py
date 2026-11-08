"""PS4 dc2 particle modules (dc2/part-module/*.bin, dc2/particle-*.bin), layout verified on retail files:
 0x00 '00CD', u32 1; 0x08 u64 reloc; 0x10 u32 1, u32 nsyms; 0x18 u64 ptr -> 0x20 (relocated);
 0x20 + 16*i: {u32 sid, u32 type, u64 offset (relocated)}; data from 0x20 + 16*n; reloc: u32 nbytes + bitmap (1 bit per 8 bytes).
(partconv/dc2io.py assumed {u64 off, u32 sid, u32 type} at 0x18, which pairs every sid with the previous entry's offset.)
part-emitters copy: load() takes bytes directly (no temp file); otherwise unchanged from the batch45/batch48 tool."""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dc import DC


def load(path_or_bytes):
    c = DC(path_or_bytes)
    assert c.d[:4] == b'00CD' and c.ptr(0x18) == 0x20 and 0x18 in c.ptrs, 'not dc2'
    n = c.u32(0x14)
    c.syms = [(c.u32(0x20 + 16 * i), c.u32(0x24 + 16 * i), c.ptr(0x28 + 16 * i)) for i in range(n)]
    for i in range(n): assert (0x28 + 16 * i) in c.ptrs
    c.bysid = {s: (t, o) for s, t, o in c.syms}
    c.data0 = 0x20 + 16 * n
    return c


def build(d, ptrs, syms, old_n):
    """d: bytes of an existing dc2 image up to its reloc (table for old_n symbols at 0x20), possibly with appended data;
    ptrs: relocated slot offsets in d (table slots ignored); syms: [(sid, type, offset in d)]. Returns dc2 bytes."""
    old0 = 0x20 + 16 * old_n
    syms = sorted(syms)
    new0 = 0x20 + 16 * len(syms); delta = new0 - old0
    out = bytearray(d[:0x20]) + bytearray(new0 - 0x20) + bytearray(d[old0:])
    struct.pack_into('<I', out, 0x14, len(syms)); struct.pack_into('<Q', out, 0x18, 0x20)
    P = {0x18}
    for p in ptrs:
        if p < old0: continue
        q = p + delta; v = struct.unpack_from('<Q', out, q)[0]
        assert v >= old0, ('pointer into header/table', hex(p), hex(v))
        struct.pack_into('<Q', out, q, v + delta); P.add(q)
    for i, (s, t, o) in enumerate(syms):
        assert o >= old0
        struct.pack_into('<IIQ', out, 0x20 + 16 * i, s, t, o + delta); P.add(0x28 + 16 * i)
    while len(out) % 4: out.append(0)
    reloc = len(out); nb = (reloc + 63) // 64; bm = bytearray(nb)
    for p in P:
        assert p % 8 == 0 and p < reloc
        w = p // 8; bm[w // 8] |= 1 << (w % 8)
    struct.pack_into('<Q', out, 8, reloc)
    res = bytes(out + struct.pack('<I', nb) + bm)
    load(res)
    return res
