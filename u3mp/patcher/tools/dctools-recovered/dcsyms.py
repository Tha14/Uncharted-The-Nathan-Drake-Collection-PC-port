"""Insert new symbols into a PS4 DC module ('00CD').

The symbol table sits right after the 0x18-byte header ({u32 sid, u32 type, u64 value ptr} per
entry, sorted by sid; the runtime binary-searches it). Adding entries moves every byte after the
table by 16*k: every pointer slot and every pointer target at or after the old data start is shifted
by that delta, the relocation bitmap is rebuilt, the header reloc offset and symbol count updated.
Use: build the new objects with DCEdit first (appended data), then call add_symbols() on the bytes."""
import struct

def parse(d):
    assert d[:4] == b'00CD'
    reloc = struct.unpack_from('<Q', d, 8)[0]
    n = struct.unpack_from('<I', d, 0x14)[0]
    nb = struct.unpack_from('<I', d, reloc)[0]
    bm = d[reloc + 4:reloc + 4 + nb]
    ptrs = set()
    for bi, b in enumerate(bm):
        if b:
            for k in range(8):
                if b >> k & 1: ptrs.add((bi * 8 + k) * 8)
    syms = [struct.unpack_from('<IIQ', d, 0x18 + 16 * i) for i in range(n)]
    return reloc, syms, ptrs

def add_symbols(d, new):
    """d: complete DC bytes; new: list of (sid, type, value_offset_in_d). Returns new bytes, delta, map()"""
    reloc, syms, ptrs = parse(d)
    n = len(syms); start = 0x18 + 16 * n
    k = len(new); delta = 16 * k
    have = {s for s, t, v in syms}
    for s, t, v in new: assert s not in have, hex(s)
    def mv(o): return o + delta if o >= start else o
    body = bytearray(d[start:reloc])
    newptrs = set()
    for p in ptrs:
        if p < start: continue                     # symbol table slots: rewritten below
        tgt = struct.unpack_from('<Q', d, p)[0]
        struct.pack_into('<Q', body, p - start, mv(tgt))
        newptrs.add(p + delta)
    allsyms = sorted([(s, t, mv(v)) for s, t, v in syms] + [(s, t, mv(v)) for s, t, v in new])
    head = bytearray(d[:0x18])
    struct.pack_into('<I', head, 0x14, n + k)
    tab = bytearray()
    for i, (s, t, v) in enumerate(allsyms):
        tab += struct.pack('<IIQ', s, t, v)
        newptrs.add(0x18 + 16 * i + 8)
    out = head + tab + body
    while len(out) % 16: out.append(0)
    newreloc = len(out)
    nb = (newreloc + 63) // 64
    bm = bytearray(nb)
    for p in newptrs:
        assert p % 8 == 0 and p < newreloc
        w = p // 8; bm[w // 8] |= 1 << (w % 8)
    struct.pack_into('<Q', out, 8, newreloc)
    out += struct.pack('<I', nb) + bm
    return bytes(out), delta, mv
