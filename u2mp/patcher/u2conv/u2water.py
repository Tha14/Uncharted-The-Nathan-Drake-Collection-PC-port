"""Uncharted 2 WATER_DATA (version 3): PS3 -> PS4.  Found on the retail pairs village, treasure, gompa, warzone and
shambhala-multi (check: validate.py water).  Installed in place of the U3 paktools/stage/waterdata.py functions
ps4stage calls (objects, materials, water_blob, ps4key), see u2paktools.setup.

PS3 item (12 B) {u32 3, u32 nObjects, ptr objects}      PS4 item {u32 3, u32 n, ptr objects} + 0x10 pad -> objects
PS3 object 0x70 -> PS4 object 0x88 (packed):
  w0..w18           -> same words (u32 swap); w1 = material hash (PS3: material table entry +0x14 hash; PS4: sid of
                       the PS4 material key 'dir/file:variant'); w17 = vertex count, w18 = triangle count
  PS4 +0x4c         0xaa pad
  +0x4c verts*      -> +0x50        +0x50 idx* -> +0x58
  +0x54 u32         -> +0x60 (+0x64 pad)
  +0x58 ptr         -> +0x68 null (PS3: a zero-length block, the next object's vertices)
  +0x5c u32         -> +0x70 ; +0x60..+0x6c -> +0x74..+0x80 ; PS4 +0x84 0.  Retail PS4 has exporter garbage in
                       +0x78..+0x84 (0.3125 / NaN words, no PS3 counterpart); written as the PS3 values (zero).
Data blocks after the objects (16-aligned, per object: vertices then indices): vertices 40 B: f32 x3, then 7 words
with their own byte orders (VPERM), indices u16 triangle lists (swap).
"""
import struct

import u2paktools  # noqa: F401  (paktools on sys.path)
import waterdata

PS3_OBJ, PS4_OBJ = 0x70, 0x88
PAD = 0xaaaaaaaa
VERT = 40
# vertex word byte orders (PS3 byte index per PS4 byte); U3 differs in words 3, 5, 6
VPERM = ['3210', '3210', '3210', '1032', '1032', '1032', '1023', '0132', '1032', '1032']
ps4key = waterdata.ps4key


def objects(p):
    """[(object ref (pg, off), material PS3 name or None)]"""
    its = p.items_of('WATER_DATA')
    if not its:
        return []
    it = its[0]
    ver, n = p.pu32(it.page, it.data_off), p.pu32(it.page, it.data_off + 4)
    if ver != 3:
        raise ValueError('U2 WATER_DATA version %d (expected 3)' % ver)
    arr = p.resolve(it.page, it.data_off + 8)
    mh = waterdata.material_hashes(p)
    return [((arr[0], arr[1] + PS3_OBJ * k), mh.get(p.pu32(arr[0], arr[1] + PS3_OBJ * k + 4))) for k in range(n)]


def materials(p):
    out = []
    for _o, m in objects(p):
        if m and m not in out:
            out.append(m)
    return out


def water_blob(p, matkey=None):
    from ps4writer import Blob
    from dc import sid
    obs = objects(p)
    if not obs:
        return None
    matkey = matkey or ps4key
    B = Blob('u2water')
    B.u32(3).u32(len(obs)).ptr(B, 0x10)
    B.pad(16, 0xaa)
    body = []
    for (pg, off), mname in obs:
        base = p.pbase(pg) + off
        u = lambda o: struct.unpack_from('>I', p.d, base + o)[0]  # noqa: E731
        out = bytearray(struct.pack('<19I', *struct.unpack_from('>19I', p.d, base)))
        if mname:
            struct.pack_into('<I', out, 4, sid(matkey(mname)))
        out += struct.pack('<I', PAD) + bytes(16)
        out += struct.pack('<II', u(0x54), PAD) + bytes(8)
        out += struct.pack('<5I', u(0x5c), u(0x60), u(0x64), u(0x68), u(0x6c)) + bytes(4)
        assert len(out) == PS4_OBJ, hex(len(out))
        nv, nt = u(0x44), u(0x48)
        rv, ri = p.resolve(pg, off + 0x4c), p.resolve(pg, off + 0x50)
        verts = bytearray()
        if rv and nv:
            vb = p.d[p.pbase(rv[0]) + rv[1]:p.pbase(rv[0]) + rv[1] + VERT * nv]
            for k in range(nv):
                for j in range(10):
                    verts += bytes(vb[VERT * k + 4 * j + int(c)] for c in VPERM[j])
        idx = b''
        if ri and nt:
            ib = p.pbase(ri[0]) + ri[1]
            idx = struct.pack('<%dH' % (3 * nt), *struct.unpack_from('>%dH' % (3 * nt), p.d, ib))
        body.append((out, bytes(verts) if rv else None, idx if ri else None))
    objstart = B.pos
    for out, _v, _i in body:
        B.raw(bytes(out))
    for k, (_o, verts, idx) in enumerate(body):
        o = objstart + PS4_OBJ * k
        if verts is not None:
            B.pad(16, 0xaa)
            B.relocs.append((o + 0x50, B, B.pos))
            B.raw(verts)
        if idx is not None:
            B.pad(16, 0xaa)
            B.relocs.append((o + 0x58, B, B.pos))
            B.raw(idx)
    return B
