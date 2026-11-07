"""Uncharted 2 actor collision, PS3 -> PS4: COLLISION_DATA_FOREGROUND items and the Havok packfile of a
JOINT_HIERARCHY D block (ragdoll props).  The packfiles go through u2havok (Havok 6.1 -> 2011.3).

Layouts (PS3 offset / PS4 offset), found on the 660 COLLISION_DATA_FOREGROUND items of the PS3/PS4 actor27 pairs
(IMPORT.md); U2 has none of the U3 extras / constraints / breakable records / ND meshes:
  item     u32 version 0x1001, u32 nParts, parts** (PS3 0xc, the part pointer array follows inline; PS4 0x10)
  part     xf f32[8] 0/0, u32 joint 20/20 (PS4 aa at 24), u64 0 -/28, hk* 28/30, 0 2c/38      (0x30 / 0x40)
  hk       u32 packfile size 0/0, u32 1 4/4, trailer* 10/18, PS4 +2c = 0x57e0e057; packfile 20/30
  trailer  3 x {ptr elements, u32 count} (elements 0x40 / 0x34 / 0x28 bytes: 32-bit words; u16 pairs at words 14-15 / 12 /
           9), PS4 + u64 0                                                                      (0x18 / 0x38)
PS4 order: item header, part pointers, then per part {part, hk + packfile, trailer, element arrays}, 16-aligned
with 0xaa fill.

JOINT_HIERARCHY D block (props with a ragdoll / skeleton mapper): PS3 {u32 3, zeros, +0x4c ptr W}; PS4 {u32 3, ...,
+0x90 ptr W}; W = {u32 packfile size, u32 1, zeros, packfile} (PS3 header 0x10, PS4 0x20 with the 8 packfile magic
bytes at +0x18).
"""
import struct

import u2paktools

u2paktools.setup()
from ps4writer import Blob  # noqa: E402

import u2havok  # noqa: E402

AA = 0xaaaaaaaa
PF_MAGIC = bytes.fromhex('57e0e05710c0c010')
ELEM = [(0x40, (14, 15)), (0x34, (12,)), (0x28, (9,))]       # U2: the second record is 0x34 (U3 0x38); no U2 actor uses the third


def swap_words(b, u16pairs=()):
    out = bytearray()
    for i in range(0, len(b), 4):
        w = b[i:i + 4]
        out += (w[1::-1] + w[3:1:-1]) if i // 4 in u16pairs else w[::-1]
    return bytes(out)


class R:
    def __init__(self, p):
        self.p = p

    def u32(self, a, o=0):
        return self.p.pu32(a[0], a[1] + o)

    def raw(self, a, o, n):
        b = self.p.pbase(a[0]) + a[1] + o
        return self.p.d[b:b + n]

    def ptr(self, a, o=0):
        return self.p.resolve(a[0], a[1] + o)


def packfile_size(b, off=0):
    """size of the (big-endian) packfile at b[off:]"""
    nsec = struct.unpack_from('>I', b, off + 0x14)[0]
    return max(sum(struct.unpack_from('>7I', b, off + 0x40 + 0x30 * s + 20)[k] for k in (0, 6)) for s in range(nsec))


def read_fg(p, it):
    r = R(p)
    a = (it.page, it.data_off)
    m = dict(ver=r.u32(a, 0), parts=[])
    n = r.u32(a, 4)
    pa = r.ptr(a, 8) if n else None
    for i in range(n):
        pp = r.ptr(pa, 4 * i)
        if r.u32(pp, 0x24) or r.u32(pp, 0x2c):
            raise NotImplementedError('FG collision part %d: non-zero +24/+2c' % i)
        hk = r.ptr(pp, 0x28)
        h = None
        if hk:
            for o in (8, 0xc, 0x14, 0x18, 0x1c):
                if r.u32(hk, o):
                    raise NotImplementedError('FG collision hk block: non-zero word at +%#x' % o)
            size, typ = r.u32(hk, 0), r.u32(hk, 4)
            if typ != 1:
                raise NotImplementedError('FG collision hk block type %d' % typ)
            tr = None
            t = r.ptr(hk, 0x10)
            if t:
                tr = []
                for k in range(3):
                    cnt = r.u32(t, 8 * k + 4)
                    tr.append((cnt, r.raw(r.ptr(t, 8 * k), 0, ELEM[k][0] * cnt) if cnt else b''))
            h = dict(size=size, type=typ, pf=r.raw(hk, 0x20, size), tr=tr)
        m['parts'].append(dict(xf=r.raw(pp, 0, 0x20), joint=r.u32(pp, 0x20), hk=h))
    return m


class W:
    def __init__(self):
        self.b = Blob('fgcoll')
        self.relocs = []

    @property
    def pos(self):
        return self.b.pos

    def pad(self, a=16, fill=0xaa):
        self.b.pad(a, fill)

    def ptr_at(self, at, target):
        self.b.relocs.append((at, self.b, target))

    def put(self, at, fmt, *v):
        struct.pack_into('<' + fmt, self.b.b, at, *v)

    def reserve(self, n, fill=0):
        o = self.b.pos
        self.b.raw(bytes([fill]) * n)
        return o


def build_fg(m, convert=u2havok.convert_packfile):
    w = W()
    o = w.reserve(0x10)
    w.put(o, 'II', m['ver'], len(m['parts']))
    if m['parts']:
        pa = w.reserve(8 * len(m['parts']))
        w.pad()
        w.ptr_at(o + 8, pa)
        for i, pt in enumerate(m['parts']):
            po = w.reserve(0x40)
            w.ptr_at(pa + 8 * i, po)
            w.b.b[po:po + 0x20] = swap_words(pt['xf'])
            w.put(po + 0x20, 'II', pt['joint'], AA)
            h = pt['hk']
            if h is None:
                continue
            pf = convert(h['pf'])
            ho = w.reserve(0x30)
            w.ptr_at(po + 0x30, ho)
            w.put(ho, 'II', len(pf), h['type'])
            w.b.b[ho + 0x2c:ho + 0x30] = pf[:4]
            w.b.raw(pf)
            w.pad()
            if h['tr'] is not None:
                to = w.reserve(0x38)
                w.ptr_at(ho + 0x18, to)
                for k, (cnt, data) in enumerate(h['tr']):
                    w.put(to + 0x10 * k + 8, 'II', cnt, AA if k < 2 else 0)
                    if cnt:
                        w.pad()
                        eo = w.pos
                        size, pairs = ELEM[k]
                        w.b.raw(b''.join(swap_words(data[j:j + size], pairs) for j in range(0, len(data), size)))
                        w.ptr_at(to + 0x10 * k, eo)
            if i + 1 < len(m['parts']):
                w.pad()
    return w.b


def fg_blob(p, it):
    """PS3 pak + COLLISION_DATA_FOREGROUND item -> ps4writer Blob of the PS4 item"""
    return build_fg(read_fg(p, it))


# ------------------------------------------------------------------ JOINT_HIERARCHY D block
D_PF_PS3 = 0x4c
D_PF_PS4 = 0x90


def jh_d_packfile(p, it):
    """the PS3 packfile a JOINT_HIERARCHY D block points at (None when the D block is the plain {3, 0...})"""
    d = p.resolve(it.page, it.data_off + 0x18)
    if not d:
        return None
    base = p.pbase(d[0]) + d[1]
    if struct.unpack_from('>I', p.d, base)[0] != 3:
        raise NotImplementedError('JOINT_HIERARCHY D block version %d' % struct.unpack_from('>I', p.d, base)[0])
    for o in range(4, 0x5c, 4):
        if o != D_PF_PS3 and struct.unpack_from('>I', p.d, base + o)[0]:
            raise NotImplementedError('JOINT_HIERARCHY D block: non-zero word at +%#x' % o)
    wr = p.resolve(d[0], d[1] + D_PF_PS3)
    if not wr:
        return None
    wb = p.pbase(wr[0]) + wr[1]
    size, one, z0, z1 = struct.unpack_from('>4I', p.d, wb)
    if one != 1 or z0 or z1 or p.d[wb + 0x10:wb + 0x18] != PF_MAGIC:
        raise NotImplementedError('JOINT_HIERARCHY D block packfile wrapper %s' % p.d[wb:wb + 0x18].hex())
    return p.d[wb + 0x10:wb + 0x10 + size]


def d_wrapper(pf4):
    """PS4 wrapper of a converted packfile: {size, 1, zeros, magic} + packfile"""
    return struct.pack('<II', len(pf4), 1) + b'\0' * 16 + pf4[:8] + pf4


# ------------------------------------------------------------------ COLLISION_HAVOK_JOINTED paks (chloe-ponytail)
def havok_jointed_blob(p, it):
    """COLLISION_HAVOK_JOINTED: PS3 {u32 version 0x2001, 0, 0, 0, wrapper {size, 1, 0, 0} + packfile} ->
    PS4 {u32 version, 0, 0, 0} + the PS4 wrapper (d_wrapper) + packfile (the 3 retail pairs)"""
    b = p.pbase(it.page) + it.data_off
    ver, z0, z1, z2, size, one, z3, z4 = struct.unpack_from('>8I', p.d, b)
    if z0 or z1 or z2 or one != 1 or z3 or z4 or p.d[b + 0x20:b + 0x28] != PF_MAGIC:
        raise NotImplementedError('COLLISION_HAVOK_JOINTED header %s' % p.d[b:b + 0x28].hex())
    blob = Blob('havokjointed')
    blob.raw(struct.pack('<4I', ver, 0, 0, 0) + d_wrapper(u2havok.convert_packfile(p.d[b + 0x20:b + 0x20 + size])))
    return blob


def convert_havok_jointed_pak(src_path, out_path):
    """a pak that holds only COLLISION_HAVOK_JOINTED items + artgroup (loose ragdolls: hair, necklaces)"""
    import u2actorparts as parts
    from ndpak import Pak
    from ps4writer import PakWriter, make_login_and_vram_page
    p = Pak(src_path)
    w = PakWriter()
    pg = w.page()
    lookup_pos = pg.item('PACKAGE_ITEM_LOOKUP', Blob('placeholder'))   # retail: lookup first in the page
    made = {}
    for it in p.items_of('COLLISION_HAVOK_JOINTED'):
        made[('COLLISION_HAVOK_JOINTED', it.name)] = pg.item('COLLISION_HAVOK_JOINTED', havok_jointed_blob(p, it),
                                                             name=it.name)
    lp = Blob('levelpats')
    lp.raw(parts.level_pats(p))
    levelpats = pg.item('LEVEL_PATS_1', lp)
    art, targets, nb, lists = parts.read_artgroup(p)
    artgroup = pg.item('ARTGROUP_DATA', parts.build_artgroup(art, parts.artgroup_entries(targets, made),
                                                             parts.keys_for(targets), nb, lists), name=art)
    lookup_pos.data = parts.lookup_blob(art, artgroup)
    by = {'ARTGROUP_DATA': [artgroup], 'LEVEL_PATS_1': [levelpats], 'PACKAGE_ITEM_LOOKUP': [lookup_pos],
          'COLLISION_HAVOK_JOINTED': list(made.values())}
    order = []
    for pgn, off in p.login['entries']:
        t = p.item_at(pgn, off).type
        for x in by.pop(t, []):
            order.append(x)
    for v in by.values():
        order += v
    make_login_and_vram_page(w, w.page(), order, [])
    data = w.build()
    with open(out_path, 'wb') as f:
        f.write(data)
    return data
