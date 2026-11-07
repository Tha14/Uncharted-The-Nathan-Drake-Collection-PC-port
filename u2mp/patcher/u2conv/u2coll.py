"""Uncharted 2 background collision (COLLISION_DATA_HAVOK_BG version 3): PS3 -> PS4.

U2 keeps its Havok-6 era layout on PS4 (no Havok packfile, unlike U3's hkpBvCompressedMeshShape): the cooker widened
the same structures to 64-bit pointers and put every block inline in the item, in a fixed order.  Found on the
retail pairs of the base multiplayer levels (dig, stage, village, treasure, gompa, warzone, shambhala, sham-wide,
ice-cave, train-wreck, the-fort); check: validate.py coll.

PS3 item (32-bit BE), PS4 item (64-bit LE, header 0x70):
  PS3   PS4
  +00   +00   u32 version 3 (+04 aa)
  +04   +08   ptr A[n]          per subpart {ptr matIndex u8[nTri], ptr mats, u32 nMats} (PS3 12 B, PS4 0x18)
  +08   +10   ptr subpart*[n]   (PS3: the array sits inline at item +0x40)
  +0c   +18   u32 n subparts
  +10   +1c   u32 (0xc in every sample)
  +14   +20   ptr MOPP {f32 offset[4], ptr code, u32 size} (PS4 0x20); code bytes copied as they are
  +18   +28   ptr welding u16[nWeld]
  +1c   +30   u32 nWeld
  +20   +34   4 raw bytes (flags)
  +24/28 +38/40 zero
  +2c   +48   ptr (PS3: a stale pointer into other data; PS4: null)
  +30   +50   ptr u32[n] (runtime, zero; PS4 inline right after the header)
  +34/38 +58/60 zero
  +3c   +68   ptr (PS3: a shared float table; PS4: null)
  mats (Havok-6 hkpMaterial-like): PS3 {u32 a, u32 b} -> PS4 {u32 a, f32 1.0, u32 b}
  subpart (hkpExtendedMeshShape triangles subpart) PS3 0x40 -> PS4 0x90:
    PS3 +00..0f zero          -> PS4 +00..1f zero
        +10 numTriangles      -> +20 (+24 aa)        +14 vertexBase -> +28      +1c numVertices -> +30 (+34 aa)
        +30 indexBase         -> +38                 +18 vertexStriding (u32) -> +40 u16
        +3c triangleOffset    -> +44                 +34 indexStriding -> +48 u16
        +38 u8 stridingType   -> +4a u8 (+1: Havok 2011 inserted INDICES_INT8)   +39 u8 flip -> +4b (+4c aa)
        +20 extrusion vec4    -> +50                 PS4 +60 hkQsTransform identity (t 0, q (0,0,0,1), s 1)
  vertices f32[3 * numVertices] (stride 12), indices u16 / u32 (stride 6 / 12): element-wise byte swap.
PS4 order: header, u32[n], A[n], subpart*[n], per subpart (matIndex, mats, vertices, indices, subpart), MOPP, MOPP
code, welding.  Blocks are 16-aligned with 0xaa fill, vertices and indices 128-aligned (the item data itself is
128-aligned in its page; u2paktools makes the page writer honour Blob.data_align).
"""
import struct

AA = 0xaaaaaaaa


class Unsupported(Exception):
    pass


def _sw32(b):
    n = len(b) // 4
    return struct.pack('<%dI' % n, *struct.unpack('>%dI' % n, b[:4 * n]))


def _sw16(b):
    n = len(b) // 2
    return struct.pack('<%dH' % n, *struct.unpack('>%dH' % n, b[:2 * n]))


class _R:
    def __init__(self, p, pg, off):
        self.p, self.pg, self.off = p, pg, off

    def u32(self, o):
        return self.p.pu32(self.pg, self.off + o)

    def ptr(self, o):
        return self.p.resolve(self.pg, self.off + o)

    def raw(self, o, n):
        b = self.p.pbase(self.pg) + self.off + o
        return self.p.d[b:b + n]


def _at(p, r):
    return _R(p, r[0], r[1])


def read(p, it):
    """PS3 item -> dict (raises Unsupported on fields never seen non-zero)"""
    h = _R(p, it.page, it.data_off)
    ver = h.u32(0)
    if ver != 3:
        raise Unsupported('COLLISION_DATA_HAVOK_BG version %d (U2 is 3)' % ver)
    for o in (0x24, 0x28, 0x34, 0x38):
        if h.u32(o) and h.ptr(o) is None:
            raise Unsupported('collision header +%#x = %#x' % (o, h.u32(o)))
    n = h.u32(0xc)
    c = dict(w10=h.u32(0x10), flags=h.raw(0x20, 4), nweld=h.u32(0x1c), subs=[])
    z = h.ptr(0x30)
    c['zarr'] = _sw32(_at(p, z).raw(0, 4 * n)) if z else b'\0' * 4 * n
    A, S = h.ptr(4), h.ptr(8)
    if n and (A is None or S is None):
        raise Unsupported('collision without subpart tables')
    for k in range(n):
        a = _R(p, A[0], A[1] + 12 * k)
        s = _at(p, _R(p, S[0], S[1] + 4 * k).ptr(0))
        if any(s.raw(0, 0x10)):
            raise Unsupported('subpart base fields set (%s)' % s.raw(0, 0x10).hex())
        ntri, nv, vstride = s.u32(0x10), s.u32(0x1c), s.u32(0x18)
        istride, stype, flip = s.u32(0x34), s.raw(0x38, 1)[0], s.raw(0x39, 1)[0]
        if s.raw(0x3a, 2) != b'\0\0' or stype not in (1, 2) or vstride < 12:
            raise Unsupported('subpart striding %d/%d type %d' % (vstride, istride, stype))
        isz = 2 if stype == 1 else 4
        vr, ir = s.ptr(0x14), s.ptr(0x30)
        verts = _sw32(_at(p, vr).raw(0, vstride * nv)) if vr else None
        idx = None
        if ir:
            raw = _at(p, ir).raw(0, istride * (ntri - 1) + 3 * isz) if ntri else b''
            idx = _sw16(raw) if isz == 2 else _sw32(raw)
        nm = a.u32(8)
        xr, yr = a.ptr(0), a.ptr(4)
        mi = _at(p, xr).raw(0, ntri) if xr else None
        mats = None
        if yr:
            m = _at(p, yr)
            mats = [(m.u32(8 * j), m.u32(8 * j + 4)) for j in range(nm)]
        c['subs'].append(dict(ntri=ntri, nv=nv, vstride=vstride, istride=istride, stype=stype, flip=flip,
                              tri_off=s.u32(0x3c), extr=_sw32(s.raw(0x20, 0x10)), verts=verts, idx=idx,
                              nmat=nm, mi=mi, mats=mats))
    mr = h.ptr(0x14)
    c['mopp'] = None
    if mr:
        m = _at(p, mr)
        size = m.u32(0x14)
        cr = m.ptr(0x10)
        code = m.raw(0, 0) if cr is None else _at(p, cr).raw(0, (size + 3) // 4 * 4)
        c['mopp'] = dict(off=_sw32(m.raw(0, 0x10)), size=size, code=code[:size] if cr else None)
    wr = h.ptr(0x18)
    c['weld'] = _sw16(_at(p, wr).raw(0, 2 * c['nweld'])) if wr and c['nweld'] else None
    return c


def blob(c):
    from ps4writer import Blob
    B = Blob('u2coll')
    B.data_align = 0x80
    pad = lambda a=16: B.pad(a, 0xaa)  # noqa: E731
    n = len(c['subs'])
    B.u32(3).u32(AA)
    hdr_ptrs = {}
    B.raw(b'\0' * 0x68)
    struct.pack_into('<II', B.b, 0x18, n, c['w10'])
    struct.pack_into('<I', B.b, 0x30, c['nweld'])
    B.b[0x34:0x38] = c['flags']

    def ptr_at(pos, off):
        B.relocs.append((pos, B, off))
    ptr_at(0x50, B.pos)
    B.raw(c['zarr'])
    pad()
    a_pos = B.pos
    if n:
        ptr_at(0x08, a_pos)
    B.raw(bytes(0x18 * n))
    for k in range(n):
        struct.pack_into('<II', B.b, a_pos + 0x18 * k + 0x10, c['subs'][k]['nmat'], AA)
    pad()
    s_pos = B.pos
    if n:
        ptr_at(0x10, s_pos)
    B.raw(bytes(8 * n))
    pad()
    for k, s in enumerate(c['subs']):
        ap = a_pos + 0x18 * k
        if s['mi'] is not None:
            ptr_at(ap, B.pos)
            B.raw(s['mi'])
            pad()
        if s['mats'] is not None:
            ptr_at(ap + 8, B.pos)
            for a, b in s['mats']:
                B.u32(a).raw(struct.pack('<f', 1.0)).u32(b)
            pad()
        vp = ip = None
        if s['verts'] is not None:
            pad(0x80)
            vp = B.pos
            B.raw(s['verts'])
        if s['idx'] is not None:
            pad(0x80)
            ip = B.pos
            B.raw(s['idx'])
        pad()
        ptr_at(s_pos + 8 * k, B.pos)
        o = B.pos
        B.raw(bytes(0x90))
        struct.pack_into('<II', B.b, o + 0x20, s['ntri'], AA)
        struct.pack_into('<II', B.b, o + 0x30, s['nv'], AA)
        if vp is not None:
            ptr_at(o + 0x28, vp)
        if ip is not None:
            ptr_at(o + 0x38, ip)
        struct.pack_into('<HHiHBBI', B.b, o + 0x40, s['vstride'], 0, struct.unpack('<i', struct.pack('<I', s['tri_off']))[0],
                         s['istride'], s['stype'] + 1, s['flip'], AA)
        B.b[o + 0x50:o + 0x60] = s['extr']
        struct.pack_into('<4f4f4f', B.b, o + 0x60, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1)
    m = c['mopp']
    if m is not None:
        pad()
        ptr_at(0x20, B.pos)
        mo = B.pos
        B.raw(m['off']).u64(0).u32(m['size']).u32(AA)
        if m['code'] is not None:
            pad(0x80)
            ptr_at(mo + 0x10, B.pos)
            B.raw(m['code'])
    if c['weld'] is not None:
        pad(0x80)
        ptr_at(0x28, B.pos)
        B.raw(c['weld'])
    return B


def havok_bg_blobs(p):
    """one Blob per PS3 COLLISION_DATA_HAVOK_BG item (the hook ps4stage calls: collision.havok_bg_blobs)"""
    return [blob(read(p, it)) for it in p.items_of('COLLISION_DATA_HAVOK_BG')]


def check_ps4(q, it):
    """structural check of a PS4 U2 collision item (converted or retail) -> list of problems"""
    errs = []
    pg, b = it.page, it.data_off
    end = b + it.size

    def P(o, what, align=1):
        r = q.resolve(pg, o)
        if r is None:
            return None
        if r[0] != pg or not b <= r[1] < end:
            errs.append('%s points outside the item' % what)
            return None
        if r[1] % align:
            errs.append('%s not %d-aligned' % (what, align))
        return r[1]
    if q.pu32(pg, b) != 3:
        errs.append('version %d' % q.pu32(pg, b))
    if b % 0x80:
        errs.append('item data not 128-aligned')
    n = q.pu32(pg, b + 0x18)
    A, S = P(b + 8, 'A'), P(b + 0x10, 'subparts')
    for k in range(n if A is not None and S is not None else 0):
        s = P(S + 8 * k, 'subpart %d' % k, 16)
        if s is None:
            continue
        ntri, nv = q.pu32(pg, s + 0x20), q.pu32(pg, s + 0x30)
        v, ix = P(s + 0x28, 'vertices %d' % k, 0x80), P(s + 0x38, 'indices %d' % k, 0x80)
        vstride, istride, stype = q.u16(q.pbase(pg) + s + 0x40), q.u16(q.pbase(pg) + s + 0x48), q.d[q.pbase(pg) + s + 0x4a]
        if v is not None and v + vstride * nv > end:
            errs.append('vertices %d overrun' % k)
        if ix is not None and ntri:
            isz = 2 if stype == 2 else 4
            last = ix + istride * (ntri - 1) + 3 * isz
            if last > end:
                errs.append('indices %d overrun' % k)
            else:
                fmt = '<%d%s' % (3, 'H' if isz == 2 else 'I')
                mx = max(max(struct.unpack_from(fmt, q.d, q.pbase(pg) + ix + istride * t)) for t in range(ntri))
                if mx >= nv:
                    errs.append('subpart %d index %d >= %d vertices' % (k, mx, nv))
        a = A + 0x18 * k
        x = P(a, 'material index %d' % k)
        if x is not None and x + ntri > end:
            errs.append('material index %d overrun' % k)
        y = P(a + 8, 'materials %d' % k)
        if y is not None and y + 12 * q.pu32(pg, a + 0x10) > end:
            errs.append('materials %d overrun' % k)
        if len(errs) > 20:
            break
    m = P(b + 0x20, 'MOPP')
    if m is not None:
        c = P(m + 0x10, 'MOPP code', 0x80)
        if c is not None and c + q.pu32(pg, m + 0x18) > end:
            errs.append('MOPP code overrun')
    w = P(b + 0x28, 'welding', 0x80)
    if w is not None and w + 2 * q.pu32(pg, b + 0x30) > end:
        errs.append('welding overrun')
    return errs
