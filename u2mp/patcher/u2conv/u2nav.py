"""Uncharted 2 NAV_MESH_1: PS3 (32-bit BE) -> PS4 (64-bit LE).  Used by u2ingame (replaces navmesh.nav_blob).

U2 nav meshes have no 'NavM' header (U3, paktools/navmesh.py).  Offsets below are item relative.

PS3 header 0x80 (u32 words)                      PS4 header 0xa0
  w0-3   origin vec4                               +00 w0-3
  w4     verts*      w5 nVerts   w6 u32            +10 verts*   +18 w5  +1c w6
  w7     name*                                     +20 name*
  w13    polys*                                    +28 polys*
  w8-11  extent vec3 + 0                           +30 w8-11
  w12    {u16 nPolys, u8 nPortals, u8}             +40 same (u16 swapped, bytes raw)
  w15    info*       w16 portals* (or 0)           +50 info*    +58 portals*
  w18    sid                                       +94 w18
  w24-27 quaternion                                +70 w24-27
  every other word is 0 (checked; anything else raises NavMeshUnsupported)
Poly 0x50 -> 0x60:
  PS3 {u16 a, u16 b, f32, 0, 0, 4 x {f32 x,y,z, u8[4]}}
  PS4 {u16 a, u16 b, f32, u8[4] v0.w, u8[4] v1.w, 16 zero bytes, 4 x {f32 x,y,z, 0}}  (v2.w/v3.w must be 0)
  v0.w/v1.w are link bytes, copied raw except v0.w byte 2 (flag bits): bit order reversed, as U3.
Portal 0x10 -> 0x18: PS3 {u32 sid, 0, 0, u8[4]} -> PS4 {u32 sid, 0, u64 0, u8[4], 0}
Info 0x50 -> 0x54: 12 f32 (min, max, scale vec4s), u16 x6, u32 nBytes, bytes*, 3 zero words.
Bytes: copied raw (nBytes of them).  Vertices: vec4 f32, word swapped.
PS4 order: header, verts, name (aa pad 16), polys, info (aa pad 16), bytes, [aa pad 16, portals].
PS3 order: header, polys, portals, info, bytes, verts, name.
"""
import struct

from ps4writer import Blob

AA = 0xaa


class NavMeshUnsupported(Exception):
    pass


def _sw(x, n=4):
    return b''.join(x[i:i + n][::-1] for i in range(0, len(x), n))


def _u16s(x):
    return _sw(x, 2)


KNOWN_PTRS = (4, 7, 13, 15, 16)
KNOWN_WORDS = (0, 1, 2, 3, 5, 6, 8, 9, 10, 11, 12, 18, 24, 25, 26, 27)


def read(p, it):
    pg, o = it.page, it.data_off
    if it.size < 0x80:
        raise NavMeshUnsupported('NAV_MESH_1 of %#x bytes' % it.size)
    W = [p.pu32(pg, o + 4 * k) for k in range(32)]
    if W[0] == 0x4e61764d:
        raise NavMeshUnsupported('NAV_MESH_1 with a NavM header (U3 layout) in a U2 pak')
    T = {}
    for k in range(32):
        r = p.resolve(pg, o + 4 * k)
        if r is not None:
            if k not in KNOWN_PTRS or r[0] != pg:
                raise NavMeshUnsupported('nav header word %d is a pointer' % k)
            T[k] = r[1]
        elif W[k] and k not in KNOWN_WORDS:
            raise NavMeshUnsupported('nav header word %d = %#x' % (k, W[k]))
    for k in KNOWN_PTRS:
        if k not in T and W[k]:
            raise NavMeshUnsupported('nav header word %d = %#x (not a pointer)' % (k, W[k]))
    base = p.pbase(pg)
    end = o + it.size

    def B(a, n):
        if a < o or a + n > end:
            raise NavMeshUnsupported('nav data outside the item')
        return bytes(p.d[base + a:base + a + n])

    npolys, nportals = W[12] >> 16, (W[12] >> 8) & 0xff
    m = dict(W=W)
    m['verts'] = B(T[4], 16 * W[5]) if 4 in T else None
    m['name'] = p.pstr(pg, T[7]) if 7 in T else None
    m['polys'] = B(T[13], 0x50 * npolys) if 13 in T else None
    if (16 in T) != bool(nportals):
        raise NavMeshUnsupported('nav portal count %d / pointer mismatch' % nportals)
    m['portals'] = B(T[16], 0x10 * nportals) if 16 in T else None
    m['info'] = m['bytes'] = None
    if 15 in T:
        m['info'] = info = B(T[15], 0x50)
        r = p.resolve(pg, T[15] + 0x40)
        nb = struct.unpack('>I', info[0x3c:0x40])[0]
        if any(info[0x44:0x50]):
            raise NavMeshUnsupported('nav info tail words non-zero')
        for k in range(0, 0x40, 4):
            if p.resolve(pg, T[15] + k) is not None:
                raise NavMeshUnsupported('nav info word %d is a pointer' % (k // 4))
        if r is not None:
            if r[0] != pg:
                raise NavMeshUnsupported('nav bytes on another page')
            m['bytes'] = B(r[1], nb)
        elif info[0x40:0x44] != b'\0' * 4:
            raise NavMeshUnsupported('nav info word 16 not a pointer')
    return m


def build(m):
    W = m['W']
    b = Blob('navmesh')
    H = bytearray(0xa0)

    def put(off, k, n=1):
        struct.pack_into('<%dI' % n, H, off, *W[k:k + n])
    put(0x00, 0, 4)
    put(0x18, 5, 2)
    put(0x30, 8, 4)
    H[0x40:0x44] = _u16s(struct.pack('>H', W[12] >> 16)) + struct.pack('>H', W[12] & 0xffff)
    put(0x70, 24, 4)
    put(0x94, 18)
    b.raw(bytes(H))
    rel = []
    if m['verts'] is not None:
        rel.append((0x10, b.pos))
        b.raw(_sw(m['verts']))
    if m['name'] is not None:
        rel.append((0x20, b.pos))
        b.cstr(m['name'])
        b.pad(16, AA)
    if m['polys'] is not None:
        rel.append((0x28, b.pos))
        P = m['polys']
        for i in range(len(P) // 0x50):
            q = P[0x50 * i:0x50 * (i + 1)]
            if any(q[8:16]) or any(q[0x3c:0x40]) or any(q[0x4c:0x50]):
                raise NavMeshUnsupported('nav poly %d: unknown non-zero words' % i)
            w0 = bytearray(q[0x1c:0x20])
            w0[2] = int('{:08b}'.format(w0[2])[::-1], 2)       # flag bits, order reversed on PS4 (as U3)
            b.raw(_u16s(q[0:4]) + _sw(q[4:8]) + bytes(w0) + q[0x2c:0x30] + b'\0' * 16)
            for k in range(4):
                b.raw(_sw(q[0x10 + 0x10 * k:0x1c + 0x10 * k]) + b'\0' * 4)
    if m['info'] is not None:
        rel.append((0x50, b.pos))
        x = m['info']
        io = b.pos
        b.raw(_sw(x[:0x30]) + _u16s(x[0x30:0x3c]) + _sw(x[0x3c:0x40]))
        b.u64(0)
        b.raw(_sw(x[0x44:0x50]))
        b.pad(16, AA)
        if m['bytes'] is not None:
            b.relocs.append((io + 0x40, b, b.pos))
            b.raw(m['bytes'])
    if m['portals'] is not None:
        b.pad(16, AA)
        rel.append((0x58, b.pos))
        P = m['portals']
        for i in range(len(P) // 0x10):
            q = P[0x10 * i:0x10 * (i + 1)]
            if any(q[4:12]):
                raise NavMeshUnsupported('nav portal %d: unknown non-zero words' % i)
            b.raw(_sw(q[0:4]) + b'\0' * 12 + q[12:16] + b'\0' * 4)
    for pos, t in rel:
        b.relocs.append((pos, b, t))
    return b


def nav_blob(p, it):
    return build(read(p, it))
