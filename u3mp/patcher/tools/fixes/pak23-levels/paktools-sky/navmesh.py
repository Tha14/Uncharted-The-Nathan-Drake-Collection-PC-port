#!/usr/bin/env python3
"""NAV_MESH_1: PS3 (32-bit BE, 'NavM' v3) -> PS4 (64-bit LE).  See FINDINGS.md section 15.

PS3 header (u32 words; hdr size = w[2], 0xd0, or 0xe0 with an extra 16-byte record):
  0 'NavM'  1 ver  2 hdrSize  3 name*  4 id  5-7 0  8 polys*  9 verts*  10 u16 nPolys,u16 x
  11 nVerts  12-15 origin  16-19 extent  20-27 zero + quaternion  28-30 -extent  31 bytes*
  32-34 vec3  35 nBytes  36-38 u32 x3  39 u16 pair  40-43  44 data*  45-51  [52-55 extra]
Poly (0x50): {u16 pair, f32, u32, u32, 4 x {f32 x,y,z, u8[4] links}}
PS4 header 0xf0 (+0x10 extra), then verts, data, name (16-aligned, aa fill), polys (0x60), bytes.
PS4 poly (0x60): {32 zero bytes, 4 x vec4}: v0/v1 keep their link bytes, v2.w = poly f32,
v3.w = poly u16 pair.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps4writer import Blob

FILL = 0x12341234


def _pair(w):
    return ((w & 0xffff) << 16) | (w >> 16)


def _bswap(w):
    return struct.unpack('<I', struct.pack('>I', w))[0]


def read(p, it):
    pg, o = it.page, it.data_off
    hs = p.pu32(pg, o + 8)
    assert (hs - 0xd0) % 16 == 0 and 0xd0 <= hs <= 0x400, 'nav header size %#x' % hs
    W = [p.pu32(pg, o + 4 * k) for k in range(max(0x38, hs // 4))]
    assert W[0] == 0x4e61764d, 'not a NavM'
    end = o + it.size

    def tgt(k):
        r = p.resolve(pg, o + 4 * k)
        return None if r is None else r[1]
    T = {k: tgt(k) for k in (3, 8, 9, 31, 44)}
    for k in range(len(W)):
        if k not in T and (pg, o + 4 * k) in p.ptrmap and k < hs // 4:
            raise NotImplementedError('nav header word %d is a pointer' % k)
    nverts = W[11]
    npolys = W[10] >> 16
    B = lambda a, n: p.d[p.pbase(pg) + a:p.pbase(pg) + a + n]
    m = dict(W=W, hs=hs)
    m['verts'] = B(T[9], 16 * nverts) if T[9] else b''
    m['polys'] = B(T[8], 0x50 * npolys) if T[8] else b''
    m['bytes'] = B(T[31], W[35]) if T[31] else None
    m['name'] = p.pstr(pg, T[3]) if T[3] else None
    m['data'] = B(T[44], end - T[44]) if T[44] else None
    if T[44] and T[3] and not (T[3] < T[44]):
        raise NotImplementedError('unexpected nav data order')
    return m


def build(m):
    W, hs = m['W'], m['hs']
    nrec = (hs - 0xd0) // 16          # extra 16-byte records after the 0xd0 header (0, 1, 4, 10 seen)
    b = Blob('navmesh')
    H = [0] * (0x3c + 4 * nrec)
    H[2] = _pair(W[39]); H[3] = W[35]
    H[4:7] = W[28:31]; H[7] = FILL
    H[8:11] = W[32:35]; H[11:14] = W[36:39]; H[14] = H[15] = FILL
    H[16:24] = W[20:28]; H[24:28] = W[12:16]; H[28:32] = W[16:20]; H[32:36] = W[40:44]
    H[46] = W[0]; H[47] = W[1]; H[48] = W[2]; H[49] = W[11]; H[50] = W[4]
    H[51:55] = W[5:8] + [0]
    H[55] = _pair(W[10]); H[56:60] = W[45:49]
    for k in (49, 50, 51):
        if W[k]:
            raise NotImplementedError('nav header word %d = %#x' % (k, W[k]))
    for r in range(nrec):
        H[60 + 4 * r:63 + 4 * r] = W[52 + 4 * r:55 + 4 * r]; H[63 + 4 * r] = _bswap(W[55 + 4 * r])
    for v in H:
        b.u32(v)
    rel = []
    vo = b.pos
    b.raw(_sw(m['verts'], 4))
    if m['data'] is not None:
        do = b.pos
        b.raw(m['data'])
        b.pad(16, 0xaa)
        rel.append((44 * 4, do))
    if m['name'] is not None:
        no = b.pos
        b.cstr(m['name'])
        b.pad(16, 0xaa)
        rel.append((36 * 4, no))
    po = b.pos
    P = m['polys']
    for i in range(len(P) // 0x50):
        q = P[0x50 * i:0x50 * (i + 1)]
        hdr = struct.unpack('>4I', q[:16])
        if hdr[2] or hdr[3]:
            raise NotImplementedError('nav poly header words 2/3 non-zero')
        b.raw(b'\0' * 32)
        for k in range(4):
            v = q[16 + 16 * k:32 + 16 * k]
            b.raw(_sw(v[:12], 4))
            if k == 0:                      # byte 2 is a flag bitfield: bit order reversed on PS4
                w = bytearray(v[12:16])
                w[2] = int('{:08b}'.format(w[2])[::-1], 2)
                b.raw(bytes(w))
            elif k == 1:
                b.raw(v[12:16])
            elif k == 2:
                b.u32(hdr[1])
            else:
                b.u32(_pair(hdr[0]))
    if len(P):
        rel.append((38 * 4, po))
    if m['verts']:
        rel.append((40 * 4, vo))
    if m['bytes'] is not None:
        bo = b.pos
        b.raw(m['bytes'])
        rel.append((0, bo))
    for pos, t in rel:
        b.relocs.append((pos, b, t))
    return b


def _sw(x, n):
    return b''.join(x[i:i + n][::-1] for i in range(0, len(x), n))


def nav_blob(p, it):
    return build(read(p, it))
