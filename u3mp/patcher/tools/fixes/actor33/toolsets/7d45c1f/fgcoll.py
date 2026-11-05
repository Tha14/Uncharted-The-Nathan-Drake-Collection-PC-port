#!/usr/bin/env python3
"""COLLISION_DATA_FOREGROUND: PS3 (32-bit BE) -> PS4 (64-bit LE) item converter.

The item is an ND wrapper around Havok packfiles (converted by hkconvert.convert_packfile) and
ND collision meshes (converted to hkpBvCompressedMeshShape packfiles by collision.py).
Layouts (PS3 offset / PS4 offset), verified byte for byte against retail pairs (FINDINGS.md 13):

item      ver u32 0/0, nParts 4/4, parts** 8/8, nExtra c/10, extras** 10/18, flags 14/20,
          nCons 18/24, cons** 1c/28                               (0x20 / 0x30)
part      xf f32[8] 0/0, joint 20/20, u32 24/28(u64), hk* 28/30, u32 2c/38 (0x30 / 0x40)
hk        size 0/0, type 4/4 (PS3 3 body, 5 breakable -> PS4 2, 4), trailer* 10/18, u32 14/20,
          u32 18/28, f32 1c/30, f32 20/34, mesh*|int id 24/38, packfile 30/40
trailer3  3 x {ptr,count} 0/0 (elements 0x40/0x40, 0x38/0x38, 0x28/0x28), u32 18/30,
          sphere trees T0..T2 1c/38, 20/40, 24/48                    (0x28 / 0x50)
trailer5  (breakable) zeros 0..18/0..28, trailer3* 18/28, records[4] at 30/50 of
          {trailer3*, u16 pair, u32 bitfield (bit order reversed), zeros} (0x18 / 0x28)
node      sphere f32[4], children** 10/10 (inline after the node), u16 n 14/18, u16 tag 16/1a,
          u32 18/1c                                                   (0x1c+4n / 0x20+8n)
con       size 0/0, type 4/4 (1 -> 0), u32 8/8, u16 pair c/10, packfile 10/20
mesh      PS3 ND mesh (collision.decode_mesh) -> {mats*, packfile*, u32 size, u32 nmats}
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ndpak import Pak
from ps4writer import Blob
import hkconvert

AA = 0xaaaaaaaa


def _rev32(x):
    return int('{:032b}'.format(x)[::-1], 2)


class R:
    """PS3 reader on (page, off) addresses"""

    def __init__(self, p):
        self.p = p

    def u32(self, a, o=0):
        return self.p.pu32(a[0], a[1] + o)

    def raw(self, a, o, n):
        b = self.p.pbase(a[0]) + a[1] + o
        return self.p.d[b:b + n]

    def ptr(self, a, o=0):
        return self.p.resolve(a[0], a[1] + o)


def swap_words(b, u16pairs=()):
    """PS3 big-endian 32-bit words -> little-endian; word indices in u16pairs hold two u16."""
    out = bytearray()
    for i in range(0, len(b), 4):
        w = b[i:i + 4]
        if i // 4 in u16pairs:
            out += w[1::-1] + w[3:1:-1]
        else:
            out += w[::-1]
    return bytes(out)


# element records of the three trailer arrays: (size, word indices that are u16 pairs)
ELEM = [(0x40, (14, 15)), (0x38, (12,)), (0x28, (9,))]


# ------------------------------------------------------------------ PS3 reading
def read_node(r, a):
    n = dict(sphere=r.raw(a, 0, 16), cnt=struct.unpack('>H', r.raw(a, 0x14, 2))[0],
             tag=struct.unpack('>H', r.raw(a, 0x16, 2))[0], u=r.u32(a, 0x18), kids=[])
    if n['cnt']:
        ka = r.ptr(a, 0x10)
        for i in range(n['cnt']):
            n['kids'].append(read_node(r, r.ptr(ka, 4 * i)))
    return n


def read_tr3(r, a):
    t = dict(arrs=[], u=r.u32(a, 0x18), trees=[])
    for k in range(3):
        cnt = r.u32(a, 8 * k + 4)
        data = b''
        if cnt:
            data = r.raw(r.ptr(a, 8 * k), 0, ELEM[k][0] * cnt)
        t['arrs'].append((cnt, data))
    for o in (0x1c, 0x20, 0x24):
        x = r.ptr(a, o)
        t['trees'].append(read_node(r, x) if x else None)
    return t


def read_tr5(r, a, nrec):
    """breakable trailer: nrec records (nrec = hk +0x24) {trailer3*, u16 pair, u32 bits, 12 bytes}"""
    t = dict(recs=[])
    for i in range(nrec):
        o = 0x18 * i
        x = r.ptr(a, o)
        t['recs'].append(dict(tr=read_tr3(r, x) if x else None, pair=r.raw(a, o + 4, 4), bits=r.u32(a, o + 8),
                              tail=r.raw(a, o + 0xc, 0xc)))
    return t


def read_hk(r, a, mesh_cb=None):
    h = dict(size=r.u32(a, 0), type=r.u32(a, 4), u14=r.u32(a, 0x14), u18=r.u32(a, 0x18),
             f=r.raw(a, 0x1c, 8), mesh=None, ival=0)
    for o in (8, 0xc, 0x28, 0x2c):
        if r.u32(a, o):
            raise NotImplementedError('hk block: non-zero word at +%#x' % o)
    m = r.ptr(a, 0x24)
    if m:
        h['mesh'] = mesh_cb(m) if mesh_cb else m
    else:
        h['ival'] = r.u32(a, 0x24)
    h['pf'] = r.raw(a, 0x30, h['size'])
    t = r.ptr(a, 0x10)
    h['tr'] = None
    if t:
        h['tr'] = read_tr5(r, t, h['ival']) if h['type'] == 5 else read_tr3(r, t)
    return h


def read_fg(p, it, mesh_cb=None):
    r = R(p)
    a = (it.page, it.data_off)
    m = dict(ver=r.u32(a, 0), flags=r.u32(a, 0x14), parts=[], extras=[], cons=[])
    n, ne, nc = r.u32(a, 4), r.u32(a, 0xc), r.u32(a, 0x18)
    if n:
        pa = r.ptr(a, 8)
        for i in range(n):
            pp = r.ptr(pa, 4 * i)
            hk = r.ptr(pp, 0x28)
            if r.u32(pp, 0x24) or r.u32(pp, 0x2c):
                raise NotImplementedError('part %d: non-zero +24/+2c' % i)
            m['parts'].append(dict(xf=r.raw(pp, 0, 0x20), joint=r.u32(pp, 0x20),
                                   hk=read_hk(r, hk, mesh_cb) if hk else None))
    if ne:
        ea = r.ptr(a, 0x10)
        for i in range(ne):
            m['extras'].append(read_hk(r, r.ptr(ea, 4 * i), mesh_cb))
    if nc:
        ca = r.ptr(a, 0x1c)
        for i in range(nc):
            c = r.ptr(ca, 4 * i)
            sz = r.u32(c, 0)
            m['cons'].append(dict(size=sz, type=r.u32(c, 4), u8=r.u32(c, 8), pair=r.raw(c, 0xc, 4),
                                  pf=r.raw(c, 0x10, sz)))
    return m


def ps3_mesh(p, check=False, log=None):
    """mesh callback: PS3 ND mesh -> (packfile bytes, mats [(friction, flags)]).  check: decode the
    built hkpBvCompressedMeshShape again and compare with the PS3 triangles."""
    import collision

    def cb(addr):
        c = collision.decode_mesh(p, addr[0], addr[1])
        if c['tris'] is None or len(c['tris']) == 0:
            raise ValueError('empty FG collision mesh')
        pf, mats = collision.build_from_ps3(c)
        if check:
            errs, info = collision.check(pf, c)
            if errs or info['unmatched'] or info['max_vertex_error'] > 1e-3:
                raise ValueError('mesh check failed: %s %s' % (errs, info))
            if log is not None:
                log.append(info)
        return pf, mats
    return cb


# ------------------------------------------------------------------ PS4 writing
class W:
    """one Blob for the whole item; pointers are self relocations"""

    def __init__(self):
        self.b = Blob('fgcoll')

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
        o = self.pos
        self.b.raw(bytes([fill]) * n)
        return o


def w_node(w, n):
    o = w.reserve(0x20)
    w.b.b[o:o + 16] = swap_words(n['sphere'])
    w.put(o + 0x18, 'HHI', n['cnt'], n['tag'], n['u'])
    if n['cnt']:
        ka = w.reserve(8 * n['cnt'])
        w.pad()
        w.ptr_at(o + 0x10, ka)
        for i, k in enumerate(n['kids']):
            w.ptr_at(ka + 8 * i, w_node(w, k))
    return o


def w_tr3(w, t):
    w.pad()
    o = w.reserve(0x50)
    for k in range(3):
        w.put(o + 0x10 * k + 0xc, 'I', AA)
    w.put(o + 0x30, 'I', t['u'])
    for k, (cnt, data) in enumerate(t['arrs']):
        w.put(o + 0x10 * k + 8, 'I', cnt)
        if cnt:
            a = w.pos
            sz, pairs = ELEM[k]
            for i in range(cnt):
                w.b.raw(swap_words(data[sz * i:sz * (i + 1)], pairs))
            w.pad()
            w.ptr_at(o + 0x10 * k, a)
    for k, tr in enumerate(t['trees']):
        if tr is not None:
            w.ptr_at(o + 0x38 + 8 * k, w_node(w, tr))
    return o


def w_tr5(w, t):
    o = w.reserve(0x28 * len(t['recs']))
    for i, rc in enumerate(t['recs']):
        ro = o + 0x28 * i
        w.b.b[ro + 8:ro + 12] = swap_words(rc['pair'], (0,))
        w.put(ro + 0xc, 'I', _rev32(rc['bits']))
        if any(rc['tail']):
            w_tail(w, ro, rc['tail'])
    for i, rc in enumerate(t['recs']):
        if rc['tr'] is not None:
            w.ptr_at(o + 0x28 * i, w_tr3(w, rc['tr']))
    return o


def w_tail(w, ro, tail):
    raise NotImplementedError('breakable trailer record tail %s' % tail.hex())


def w_mesh(w, mesh):
    pf, mats = mesh
    w.pad()
    pfo = w.pos
    w.b.raw(pf)
    w.pad()
    mo = w.pos
    for fr, fl in mats:
        w.b.raw(struct.pack('<IfI', 1, fr if fr else 1.0, fl))
    w.pad()
    so = w.reserve(0x20)
    w.put(so + 0x10, 'IIII', len(pf), len(mats), AA, AA)
    w.ptr_at(so, mo)
    w.ptr_at(so + 8, pfo)
    return so


def w_hk(w, h, convert=True):
    so = w_mesh(w, h['mesh']) if h['mesh'] is not None else None
    w.pad()
    pf = hkconvert.convert_packfile(h['pf']) if convert else h['pf']
    brk = h['type'] == 5
    o = w.reserve(0x40)
    w.put(o, 'II', len(pf), h['type'] - 1)
    w.put(o + 0x20, 'II', h['u14'], AA if brk else 0)
    w.put(o + 0x28, 'I', h['u18'])
    w.b.b[o + 0x30:o + 0x38] = swap_words(h['f'])
    if so is not None:
        w.ptr_at(o + 0x38, so)
    else:
        w.put(o + 0x38, 'I', h['ival'])
    w.b.raw(pf)
    w.pad()
    if h['tr'] is not None:
        w.ptr_at(o + 0x18, w_tr5(w, h['tr']) if brk else w_tr3(w, h['tr']))
    return o


def build(m, convert=True):
    w = W()
    o = w.reserve(0x30)
    w.put(o, 'II', m['ver'], len(m['parts']))
    w.put(o + 0x10, 'II', len(m['extras']), AA)
    w.put(o + 0x20, 'II', m['flags'], len(m['cons']))
    if m['parts']:
        pa = w.reserve(8 * len(m['parts']))
        w.pad()
        w.ptr_at(o + 8, pa)
        for i, pt in enumerate(m['parts']):
            po = w.reserve(0x40)
            w.ptr_at(pa + 8 * i, po)
            w.b.b[po:po + 0x20] = swap_words(pt['xf'])
            w.put(po + 0x20, 'II', pt['joint'], AA)
            if pt['hk'] is not None:
                w.ptr_at(po + 0x30, w_hk(w, pt['hk'], convert))
    if m['extras']:
        ea = w.reserve(8 * len(m['extras']))
        w.pad()
        w.ptr_at(o + 0x18, ea)
        for i, h in enumerate(m['extras']):
            w.ptr_at(ea + 8 * i, w_hk(w, h, convert))
    if m['cons']:
        ca = w.reserve(8 * len(m['cons']))
        w.pad()
        w.ptr_at(o + 0x28, ca)
        for i, c in enumerate(m['cons']):
            pf = hkconvert.convert_packfile(c['pf']) if convert else c['pf']
            co = w.reserve(0x20)
            w.ptr_at(ca + 8 * i, co)
            w.put(co, 'III', len(pf), c['type'] - 1, c['u8'])
            w.b.b[co + 0x10:co + 0x14] = swap_words(c['pair'], (0,))
            w.b.raw(pf)
            w.pad()
    w.pad(8, 0)
    return w.b


def fg_blob(p, it, check=True, log=None):
    """PS3 pak + COLLISION_DATA_FOREGROUND item -> ps4writer Blob for the PS4 item"""
    return build(read_fg(p, it, ps3_mesh(p, check, log)))


# ------------------------------------------------------------------ verification
def place(blob, data_off):
    """bytes of the blob placed at page offset data_off (pointers = page offsets)"""
    b = bytearray(blob.b)
    for pos, _t, toff in blob.relocs:
        struct.pack_into('<Q', b, pos, data_off + toff)
    return bytes(b)


def retail_meshes(q, it):
    """mesh callback replacement for checks: the retail PS4 mesh packfiles + mats, in part order"""
    out = []
    a = (it.page, it.data_off)
    n = q.pu32(a[0], a[1] + 4)
    pa = q.resolve(a[0], a[1] + 8)
    for i in range(n):
        pp = q.resolve(pa[0], pa[1] + 8 * i)
        hk = q.resolve(pp[0], pp[1] + 0x30)
        ms = hk and q.resolve(hk[0], hk[1] + 0x38)
        if not ms:
            continue
        mats = q.resolve(ms[0], ms[1])
        pfa = q.resolve(ms[0], ms[1] + 8)
        size, nm = q.pu32(ms[0], ms[1] + 0x10), q.pu32(ms[0], ms[1] + 0x14)
        pf = q.d[q.pbase(pfa[0]) + pfa[1]:q.pbase(pfa[0]) + pfa[1] + size]
        mt = [struct.unpack_from('<IfI', q.d, q.pbase(mats[0]) + mats[1] + 12 * k)[1:] for k in range(nm)]
        out.append((pf, mt))
    return out


def check_pair(p3path, p4path, verbose=True):
    """convert every FG item of the PS3 pak (with the retail mesh packfiles substituted, since the BV
    tree builder differs) and compare with the retail PS4 item bytes."""
    p, q = Pak(p3path), Pak(p4path)
    res = []
    for i3, i4 in zip(p.items_of('COLLISION_DATA_FOREGROUND'), q.items_of('COLLISION_DATA_FOREGROUND')):
        meshes = iter(retail_meshes(q, i4))
        m = read_fg(p, i3, lambda a: next(meshes))
        b = place(build(m), i4.data_off)
        ref = q.d[q.pbase(i4.page) + i4.data_off:q.pbase(i4.page) + i4.data_off + i4.size]
        nd = sum(1 for x, y in zip(b, ref) if x != y) + abs(len(b) - len(ref))
        first = next((k for k in range(min(len(b), len(ref))) if b[k] != ref[k]), None)
        res.append((i3.name, len(b), len(ref), nd, first))
        if verbose:
            print('%-50s mine %#x retail %#x diff %d first %s' % (i3.name, len(b), len(ref), nd,
                                                                 None if first is None else hex(first)))
    return res


if __name__ == '__main__':
    if sys.argv[1] == 'check':
        check_pair(sys.argv[2], sys.argv[3])
    else:
        p = Pak(sys.argv[1])
        for it in p.items_of('COLLISION_DATA_FOREGROUND'):
            log = []
            b = fg_blob(p, it, True, log)
            print(it.name, len(b.b), 'meshes', [(x['tris'], round(x['max_vertex_error'], 6)) for x in log])
