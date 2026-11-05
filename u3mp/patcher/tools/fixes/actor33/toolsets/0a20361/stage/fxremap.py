"""PS4 prime-shader EFFECT: string-table index fields + remapping.

An EFFECT item references names (techniques, vertex inputs, samplers, textures, cbuffer
variables) as indices into the pak-wide string table that hangs off EFFECT_TABLE+0x18
({u32 n, u32 0, char* names[n]}).  Tables differ per pak, so an effect copied from another
pak must have every index field rewritten.  Layout (offsets in the EFFECT data):

  header   +0x00 ptr passes, +0x08 u64 npasses
  pass     0xd0 bytes: +0x00 u16 technique, u16 quality, u16 tags  (string idx), u16 ?
           +0x10 ptr inputs, +0x18 u32 ninputs
           +0x68 u32 nVSCbuf, +0x80 ptr VS cbuffers, +0x88 ptr VS binary, +0x90 u32 size
           +0x98 u32 nSamplers, +0x9c u32 nTextures, +0xa0 u32 nPSCbuf
           +0xa8 ptr samplers, +0xb0 ptr textures, +0xb8 ptr PS cbuffers, +0xc0 ptr PS binary
  input    16 bytes  {u32 name idx, u32 slot, u32 index, u32 format}
  sampler  0x3c bytes {u32 name idx, ...}
  texture  8 bytes   {u16 texture name idx, u16 sampler name idx, u32}
  cbuffer  0x30 bytes {ptr vars, u32 nvars, u32 size, hashes...}
  cbvar    0x20 bytes {u16 name idx, u16 idx, u16 idx, ...}
"""
import struct


class FxLayoutError(Exception):
    pass


def index_fields(data, ptrs):
    """data: EFFECT item bytes, ptrs: {field offset: target offset in the same item (or None
    for pointers to other items)}.  Returns sorted list of (offset, size) of string-index fields."""
    def P(o):
        if o not in ptrs:
            return None
        return ptrs[o]
    out = []
    u32 = lambda o: struct.unpack_from('<I', data, o)[0]
    passes = P(0)
    npass = u32(8)
    if passes is None or npass == 0 or npass > 64:
        raise FxLayoutError('header')
    seen_cb = set()

    def cbufs(lst, n):
        if n == 0:
            return
        if lst is None:
            raise FxLayoutError('cbuffer list')
        for k in range(n):
            r = lst + 0x30 * k
            if r in seen_cb:
                continue
            seen_cb.add(r)
            vars_ = P(r)
            nv = u32(r + 8)
            if nv and vars_ is None:
                raise FxLayoutError('cbvars')
            for j in range(nv):
                v = vars_ + 0x20 * j
                out.extend([(v, 2), (v + 2, 2), (v + 4, 2)])
    seen = set()
    for i in range(npass):
        p = passes + 0xd0 * i
        out.extend([(p, 2), (p + 2, 2), (p + 4, 2)])
        inp, nin = P(p + 0x10), u32(p + 0x18)
        if nin and inp is None:
            raise FxLayoutError('inputs')
        if nin and inp not in seen:
            seen.add(inp)
            for k in range(nin):
                out.append((inp + 16 * k, 4))
        cbufs(P(p + 0x80), u32(p + 0x68))
        ns, nt, npc = u32(p + 0x98), u32(p + 0x9c), u32(p + 0xa0)
        sm, tx = P(p + 0xa8), P(p + 0xb0)
        if (ns and sm is None) or (nt and tx is None):
            raise FxLayoutError('samplers/textures')
        if ns and sm not in seen:
            seen.add(sm)
            for k in range(ns):
                out.append((sm + 0x3c * k, 4))
        if nt and tx not in seen:
            seen.add(tx)
            for k in range(nt):
                out.extend([(tx + 8 * k, 2), (tx + 8 * k + 2, 2)])
        cbufs(P(p + 0xb8), npc)
    return sorted(set(out))


def remap(data, ptrs, mapping):
    """return a copy of data with every index field v replaced by mapping[v]"""
    b = bytearray(data)
    for o, sz in index_fields(data, ptrs):
        fmt = '<H' if sz == 2 else '<I'
        v = struct.unpack_from(fmt, b, o)[0]
        if v not in mapping:
            raise FxLayoutError('index %d at %#x outside the source string table' % (v, o))
        struct.pack_into(fmt, b, o, mapping[v])
    return bytes(b)
