#!/usr/bin/env python3
"""WATER_DATA: PS3 (32-bit BE) -> PS4 (64-bit LE).  Layout from the retail rosetta pairs grave-water (mesh water),
grave-01-firstyard-water and grave-03-docks-water (grid water).

PS3 item (12 B): {u32 version 5, u32 nObjects, ptr objects}          PS4 item (16 B): {u32 5, u32 n, ptr objects}
PS3 object 0xf0 -> PS4 object 0x108 (retail single-object files show 0x110 = object + 8 bytes 0xaa alignment pad;
  objects are packed at 0x108 - a 0x110 stride shifts every object after the first: Museum fault 0x1281ac6):
  w0..w19          -> same words (u32 swap); w1 = material hash (PS3: material entry +0x14 hash;
                      PS4: sid of the PS4 material key 'dir/file:variant')
  +0x50 verts*     -> +0x50 ptr ; +0x54 aux* (16 zero bytes on every PS3 file) -> +0x58 null ; +0x58 idx* -> +0x60
  w23..w49         -> PS4 w26..w52 (+3 words: three widened pointers)
  +0xc8 techniques* (PS3 technique name strings) -> dropped (PS4 w53 = pad 0xaaaaaaaa)
  w51..w59         -> PS4 w56..w64 (retail carries exporter garbage in w59..w65; written as the PS3 values / 0)
  PS4 w65 0; then 0xaa words up to the 16-aligned data blocks
Data blocks (after the objects, 16-aligned, in object order: verts then idx):
  verts: w18 vertices; stride = block / w18:
     4  (grid water: height/flow texels) -> raw bytes
     40 (mesh water) -> pos f32x3, f32, u16x2, u32, u8x4, u8 u8 u16, u16x2, u16x2 (byte orders from grave-water)
  idx:  mesh water: w19 triangles of u16 indices (swap); grid water: 8 u32 (grid header, swap)."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps4writer import Blob
from dc import sid

PS3_OBJ, PS4_OBJ = 0xf0, 0x108       # engine walks objects at 0x108 (0x127d067 imul 0x108, watermgr.cpp)
PAD = 0xaaaaaaaa
# mesh vertex word permutations (PS3 byte index per PS4 byte)
VPERM = ['3210', '3210', '3210', '3210', '1032', '3210', '0123', '0132', '1032', '1032']


def _perm(w, p):
    return bytes(w[int(c)] for c in p)


def material_hashes(p):
    """PS3 material entry hash (+0x14) -> material name"""
    mt = p.items_of('MATERIAL_TABLE_1')
    if not mt:
        return {}
    mt = mt[0]; pg, b = mt.page, mt.data_off
    nmat = p.pu32(pg, b + 0xc); ma = p.resolve(pg, b + 0x20)
    out = {}
    for i in range(nmat):
        m = p.resolve(ma[0], ma[1] + 4 * i)
        name = p.pstr(*p.resolve(*m))
        out[p.pu32(m[0], m[1] + 0x14)] = name
    return out


def objects(p):
    """[(object ref (pg, off), material ps3 name or None)]"""
    its = p.items_of('WATER_DATA')
    if not its:
        return []
    it = its[0]
    ver, n = p.pu32(it.page, it.data_off), p.pu32(it.page, it.data_off + 4)
    assert ver == 5, 'WATER_DATA version %d' % ver
    arr = p.resolve(it.page, it.data_off + 8)
    mh = material_hashes(p)
    out = []
    for k in range(n):
        o = (arr[0], arr[1] + PS3_OBJ * k)
        out.append((o, mh.get(p.pu32(o[0], o[1] + 4))))
    return out


def ps4key(name):
    d, v = name.rsplit('/', 1)
    return d + ':' + v


def water_blob(p, matkey=None):
    """PS4 WATER_DATA blob; matkey(ps3 material name) -> PS4 material key (default dir/file:variant)"""
    obs = objects(p)
    if not obs:
        return None
    matkey = matkey or ps4key
    B = Blob('water')
    B.u32(5).u32(len(obs)).ptr(B, 0x10)
    body = []
    for o, mname in obs:
        pg, off = o
        base = p.pbase(pg) + off
        w = [p.d[base + 4 * i:base + 4 * i + 4] for i in range(PS3_OBJ // 4)]
        u = lambda i: struct.unpack('>I', w[i])[0]
        out = bytearray()
        for i in range(20):
            out += w[i][::-1]
        if mname:
            struct.pack_into('<I', out, 4, sid(matkey(mname)))
        rv = p.resolve(pg, off + 0x50); ri = p.resolve(pg, off + 0x58); rs = p.resolve(pg, off + 0xc8)
        ptr_pos = len(out); out += bytes(24)            # +0x50 verts, +0x58 null, +0x60 idx (filled below)
        for i in range(23, 50):
            out += w[i][::-1]
        out += struct.pack('<II', 0, PAD) if False else b''
        # PS4 w53: pad (techniques pointer dropped); w54..w55 0
        out += struct.pack('<I', PAD) + bytes(8)
        for i in range(51, 60):
            out += w[i][::-1]
        out += struct.pack('<I', 0)
        assert len(out) == PS4_OBJ, hex(len(out))
        # data blocks
        nv, nt = u(18), u(19)
        vend = p.resolve(pg, off + 0x54)
        vsize = (vend[1] - rv[1]) if (vend and vend[0] == rv[0]) else None
        stride = 40 if nv and vsize and vsize // nv >= 40 else 4
        vb = p.d[p.pbase(rv[0]) + rv[1]:p.pbase(rv[0]) + rv[1] + nv * stride]
        if stride == 4:
            verts = bytes(vb)
        else:
            verts = bytearray()
            for k in range(nv):
                v = vb[40 * k:40 * k + 40]
                for j in range(10):
                    verts += _perm(v[4 * j:4 * j + 4], VPERM[j])
            verts = bytes(verts)
        ib = p.pbase(ri[0]) + ri[1]
        if stride == 4:
            idx = b''.join(p.d[ib + 4 * k:ib + 4 * k + 4][::-1] for k in range(8))
        else:
            idx = b''.join(p.d[ib + 2 * k:ib + 2 * k + 2][::-1] for k in range(3 * nt))
        body.append((out, ptr_pos, verts, idx))
    objstart = B.pos
    for out, _, _, _ in body:
        B.raw(bytes(out))
    while B.pos % 16:
        B.u32(PAD)
    for k, (out, ptr_pos, verts, idx) in enumerate(body):
        B.pad(16)
        vo = B.pos; B.raw(verts)
        B.pad(2) if False else None
        io = B.pos; B.raw(idx)
        o = objstart + PS4_OBJ * k
        # patch the two pointers inside the already written object
        struct.pack_into('<Q', B.b, o + 0x50, 0); struct.pack_into('<Q', B.b, o + 0x60, 0)
        B.relocs.append((o + 0x50, B, vo)); B.relocs.append((o + 0x60, B, io))
    return B


def materials(p):
    """PS3 names of the materials water objects use (unique, in object order)"""
    out = []
    for o, m in objects(p):
        if m and m not in out:
            out.append(m)
    return out


if __name__ == '__main__':
    from ndpak import Pak
    p = Pak(sys.argv[1])
    for o, m in objects(p):
        print(o, m)
    b = water_blob(p)
    print(len(b.b) if b else None)
