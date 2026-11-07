"""Uncharted 2 LIGHT_TABLE_1: PS3 (0x40-byte header, big-endian) -> PS4 (0x68-byte header, little-endian).

Layout found on the PS3/PS4 pairs of the shared multiplayer levels (village-multi-wide, gompa-multi-wide,
dig-multi, stage-multi; check: u2conv/validate.py lighttable). Uncharted 3 uses a 0x60 / 0xa0 header instead
(u3mp ps4stage.convert_light_table), so the U3 converter cannot read U2 tables.

  PS3 +0x00 ptr flags (u8 bytes, up to the end of the item)    -> PS4 +0x00 ptr
  PS3 +0x04..+0x20 u32 (8 words; +0x10 record count,           -> PS4 +0x08..+0x24
                    +0x18 local light count)                      (PS4 +0x28 / +0x2c: 0)
  PS3 +0x24..+0x3c ptr (7 slots)                                -> PS4 +0x30..+0x60 (8-byte slots, same order)
        +0x28 records (0x3c each; unlike U3, word +0x30 is a swapped u32)
        +0x30 local lights (0xf0 each: floats)
        +0x3c u32 block (up to the flags)
  PS4 order: header, 0xaa pad to 16, flags, then the blocks in slot order, each 16-aligned (0xaa pad).
Blocks are swapped as u32 words; block sizes are the gaps between the PS3 sub-blocks.
"""
import struct

PS3_HDR = 0x40
PS4_HDR = 0x68


def _words(d, raw_at=None, stride=None):
    d = d[:len(d) // 4 * 4]
    n = len(d) // 4
    out = bytearray(struct.pack('<%dI' % n, *struct.unpack('>%dI' % n, d)))
    if raw_at is not None:
        for k in range(0, len(d) - raw_at - 3, stride):
            out[k + raw_at:k + raw_at + 4] = d[k + raw_at:k + raw_at + 4]
    return bytes(out)


def convert_light_table(p, P, keep_lights, report, gobo=None):
    """drop-in for ps4stage.convert_light_table on Uncharted 2 paks"""
    from ps4writer import Blob
    its = p.items_of('LIGHT_TABLE_1')
    if not its:
        return None
    it = its[0]
    pg, b = it.page, it.data_off
    base = p.pbase(pg) + b
    u = lambda o: p.pu32(pg, b + o)
    ptrs = {}
    for o in [0] + list(range(0x24, PS3_HDR, 4)):
        r = p.resolve(pg, b + o)
        if r is not None:
            if r[0] != pg:
                raise ValueError('LIGHT_TABLE_1 block on another page')
            ptrs[o] = r[1] - b
    ends = sorted(set(ptrs.values()) | {it.size})

    def block(o):
        t = ptrs[o]
        n = [x for x in ends if x > t][0] - t
        return p.d[base + t:base + t + n]

    parts = {}
    for o3, d in ((o, block(o)) for o in sorted(ptrs) if o):
        o4 = 0x30 + (o3 - 0x24) * 2
        if o3 == 0x28:
            parts[o4] = _words(d[:0x3c * u(0x10)])
        elif o3 == 0x30:
            parts[o4] = _words(d[:0xf0 * u(0x18)])
        else:
            parts[o4] = _words(d)
    flags = block(0) if 0 in ptrs else b''
    if u(0x18):
        report.setdefault('lights', []).append('%d local lights converted' % u(0x18))
    lb = Blob('lighttable')
    if 0 in ptrs:
        lb.ptr(lb, 0x70)
    else:
        lb.u64(0)
    for o in range(4, 0x24, 4):
        lb.u32(u(o))
    lb.u32(0).u32(0)
    off = 0x70 + ((len(flags) + 15) & ~15)
    pos = {}
    for o4 in sorted(parts):
        pos[o4] = off
        off = (off + len(parts[o4]) + 15) & ~15
    for o4 in range(0x30, PS4_HDR, 8):
        if o4 in pos:
            lb.ptr(lb, pos[o4])
        else:
            lb.u64(0)
    assert lb.pos == PS4_HDR, hex(lb.pos)
    while lb.pos < 0x70:
        lb.u8(0xaa)
    lb.raw(flags)
    while lb.pos % 16:
        lb.u8(0xaa)
    for o4 in sorted(parts):
        while lb.pos < pos[o4]:
            lb.u8(0xaa)
        lb.raw(parts[o4])
    return P.item('LIGHT_TABLE_1', lb) if P is not None else lb
