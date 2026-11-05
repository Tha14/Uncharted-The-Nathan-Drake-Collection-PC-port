#!/usr/bin/env python3
"""PS3 -> PS4 background collision converter (Uncharted 3, COLLISION_DATA_HAVOK_BG).

PS3 (ND custom, read by the SPU raycast jobs; PPU reader f_8db848 / f_b506c8):
  item (32-bit ptrs): +00 u32 version 6, +0c u32 count, +21 u8 flag, +2c ptr sid[count],
  +30 ptr u32[count], +3c ptr (chunk float pairs, unused here), +40 ptr mesh
  mesh: +00 f32 aabb[6], +18 u32 ?, +1c u32 numChunks, +20 u32 numGroups, +24 u32 numMats,
        +28 ptr groups {u32 firstChunk, u32 count}, +2c ptr group aabbs (6 f32),
        +30 ptr chunk aabbs (6 f32), +34 ptr chunk records, +38 ptr chunk quant boxes (6 f32),
        +3c ptr materials {u32 0, u32 bf16 friction, u32 flags}
  chunk record 16 B: {ptr data, u32 0, u16 numIndices, u16 numVerts, u32 material index}
  chunk data: indices (21 x 6-bit MSB-first per 16 bytes), verts (3 x u16 BE, v = min +
  (max-min)*q/65536 over the chunk quant box), per-triangle u16 = Havok welding code.

PS4 (Havok 2011.3 "ND_Toaster"): the item holds a 64-bit LE packfile whose root object is an
hkpBvCompressedMeshShape (hkcdStaticMeshTree: top tree of sections with 5-byte nodes, each
section a 4-byte-node tree over <=127 primitives, 11/11/10-bit packed vertices + 21/21/22-bit
shared vertices, per-primitive data runs {u32 filter|userData<<8|welding<<16, u8 first, u8 count}).
The layout was decoded from retail ldn-train-stage (all 6324 primitives / 19125 vertices / both
trees verified by containment) - see FINDINGS.md section 12.

usage: collision.py <ps3 stage.pak> <out.hkx> [--check]      (writes the packfile only)
       import collision; collision.havok_bg_blob(ps3pak) -> ps4writer Blob for the item
"""
import os, struct, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ndpak import Pak

# ------------------------------------------------------------------ PS3 side
def _unpack_idx(b, n):
    out = []
    for i in range(0, len(b), 16):
        bits = int.from_bytes(b[i:i + 16], 'big')
        for k in range(21):
            out.append((bits >> (122 - 6 * k)) & 63)
    return out[:n]


def ps3_collision(p, index=0):
    """-> dict(header bytes info, tris (N,3,3) float64, weld (N,), mat (N,), mats [(friction, flags)])
    index: which COLLISION_DATA_HAVOK_BG item (levels may carry several, e.g. cave-multi has 2)"""
    items = p.items_of('COLLISION_DATA_HAVOK_BG')
    if len(items) <= index:
        return None
    it = items[index]
    pg, b = it.page, it.data_off
    B = p.pbase(pg) + b
    ver, w4, w8, count = struct.unpack_from('>4I', p.d, B)
    misc = p.d[B + 0x10:B + 0x24]
    arr = []
    r = p.resolve(pg, b + 0x30)
    if r and count:
        arr = list(struct.unpack_from('>%dI' % count, p.d, p.pbase(r[0]) + r[1]))
    out = dict(version=ver, w4=w4, w8=w8, count=count, misc=misc, arr=arr, tris=None)
    r = p.resolve(pg, b + 0x40)
    if r is None:
        return out
    out.update(decode_mesh(p, *r))
    return out


def decode_mesh(p, mpg, mo):
    """decode a PS3 ND collision mesh (BG item +0x40, FG proto record +0x24) at page/offset."""
    MB = p.pbase(mpg) + mo
    aabb = struct.unpack_from('>6f', p.d, MB)
    n18, nchunk, ngroup, nmat = struct.unpack_from('>4I', p.d, MB + 0x18)

    def at(off):
        rr = p.resolve(mpg, mo + off)
        return None if rr is None else (rr, p.pbase(rr[0]) + rr[1])
    (crr, chunks), (_, qbox), (_, mats_b) = at(0x34), at(0x38), at(0x3c)
    tris, weld, mat = [], [], []
    for i in range(nchunk):
        rec = p.d[chunks + 16 * i:chunks + 16 * i + 16]
        dr = p.resolve(crr[0], crr[1] + 16 * i)
        nidx, nv = struct.unpack_from('>HH', rec, 8)
        mi = struct.unpack_from('>I', rec, 12)[0]
        if dr is None or nidx == 0:
            continue
        D = p.pbase(dr[0]) + dr[1]
        ib = ((nidx + 20) // 21) * 16
        vb = (nv * 6 + 15) & ~15
        idx = _unpack_idx(p.d[D:D + ib], nidx)
        q = np.array(struct.unpack_from('>%dH' % (3 * nv), p.d, D + ib), dtype=np.float64).reshape(nv, 3)
        box = np.array(struct.unpack_from('>6f', p.d, qbox + 24 * i), dtype=np.float64)
        V = box[:3] + (box[3:] - box[:3]) * q / 65536.0
        nt = nidx // 3
        td = struct.unpack_from('>%dH' % nt, p.d, D + ib + vb)
        for t in range(nt):
            a, b2, c = idx[3 * t:3 * t + 3]
            if max(a, b2, c) >= nv:
                raise ValueError('chunk %d: index out of range' % i)
            tris.append(V[[a, b2, c]])
            weld.append(td[t])
            mat.append(mi)
    mats = []
    for k in range(nmat):
        z, fr, fl = struct.unpack_from('>III', p.d, mats_b + 12 * k)
        mats.append((struct.unpack('>f', struct.pack('>I', (fr & 0xffff) << 16))[0], fl))
    return dict(aabb=aabb, tris=np.array(tris).reshape(-1, 3, 3), weld=np.array(weld, dtype=np.int64),
                mat=np.array(mat, dtype=np.int64), mats=mats)


# ------------------------------------------------------------------ hkcdStaticMeshTree encoder
F32 = np.float32


def _enc_box(cmin, cmax, pmin, pmax):
    """conservative Codec3Axis byte triple + the decoded box (as the runtime decodes it)."""
    xyz = []
    dmin = np.empty(3, F32)
    dmax = np.empty(3, F32)
    for a in range(3):
        span = F32(pmax[a] - pmin[a])
        lo = hi = 0
        for k in range(15, -1, -1):
            v = F32(pmin[a] + span * F32(k * k) * F32(1.0 / 226.0))
            if v <= cmin[a] - 1e-6 * max(1.0, abs(cmin[a])):
                lo = k
                break
        for k in range(15, -1, -1):
            v = F32(pmax[a] - span * F32(k * k) * F32(1.0 / 226.0))
            if v >= cmax[a] + 1e-6 * max(1.0, abs(cmax[a])):
                hi = k
                break
        xyz.append((lo << 4) | hi)
        dmin[a] = F32(pmin[a] + span * F32(lo * lo) * F32(1.0 / 226.0))
        dmax[a] = F32(pmax[a] - span * F32(hi * hi) * F32(1.0 / 226.0))
    return bytes(xyz), dmin, dmax


def _split(ids, bmin, bmax, cent):
    """median split along the longest axis of the centroid bounds"""
    c = cent[ids]
    ext = c.max(0) - c.min(0)
    ax = int(np.argmax(ext))
    order = ids[np.argsort(c[:, ax], kind='stable')]
    h = len(order) // 2
    return order[:h], order[h:]


def _build_tree(ids, lbmin, lbmax, cent, leaf_cb):
    """returns nested ('leaf', id) / ('node', left, right) with box (min,max)"""
    if len(ids) == 1:
        i = int(ids[0])
        return ('leaf', i, lbmin[i], lbmax[i])
    a, b = _split(ids, lbmin, lbmax, cent)
    L = _build_tree(a, lbmin, lbmax, cent, leaf_cb)
    R = _build_tree(b, lbmin, lbmax, cent, leaf_cb)
    return ('node', L, R, np.minimum(L[-2], R[-2]), np.maximum(L[-1], R[-1]))


def _nleaves(n):
    return 1 if n[0] == 'leaf' else _nleaves(n[1]) + _nleaves(n[2])


def _emit_tree(root, pmin, pmax, nbytes, leafval):
    """depth-first node bytes; nbytes 4 (Codec3Axis4) or 5 (Codec3Axis5).
    returns (bytes, {leaf id: node index})"""
    out = []
    leafnode = {}

    def rec(n, pmin, pmax):
        xyz, dmin, dmax = _enc_box(n[-2], n[-1], pmin, pmax)
        idx = len(out)
        out.append(None)
        if n[0] == 'leaf':
            v = leafval(n[1])
            leafnode[n[1]] = idx
            if nbytes == 4:
                assert v < 128
                out[idx] = xyz + bytes([v << 1])
            else:
                assert v < 0x8000
                out[idx] = xyz + bytes([v >> 8, v & 0xff])
        else:
            nl = _nleaves(n[1])
            if nbytes == 4:
                assert nl < 128
                out[idx] = xyz + bytes([(nl << 1) | 1])
            else:
                assert nl < 0x8000
                out[idx] = xyz + bytes([0x80 | (nl >> 8), nl & 0xff])
            rec(n[1], dmin, dmax)
            rec(n[2], dmin, dmax)
    rec(root, F32(pmin), F32(pmax))
    return b''.join(out), leafnode


def _sections(tris, maxprims=127, maxverts=255):
    """spatially split triangle ids into sections (<= maxprims tris, <= maxverts unique verts)"""
    cent = tris.mean(1)
    keys = [tuple(np.round(v, 5)) for v in tris.reshape(-1, 3)]
    vid = {}
    tv = np.array([vid.setdefault(k, len(vid)) for k in keys]).reshape(-1, 3)
    out = []

    def rec(ids):
        if len(ids) <= maxprims and len(set(tv[ids].ravel())) <= maxverts:
            out.append(ids)
            return
        c = cent[ids]
        ax = int(np.argmax(c.max(0) - c.min(0)))
        order = ids[np.argsort(c[:, ax], kind='stable')]
        h = len(order) // 2
        rec(order[:h])
        rec(order[h:])
    rec(np.arange(len(tris)))
    return out, tv


def build_bvcm(tris, weld, uidx, userpal, filterpal=(1,), header=None):
    """tris (N,3,3); weld (N,) u16 welding codes; uidx (N,) userData palette index.
    returns the __data__ section (bytes, local fixups list, virtual fixup offset)."""
    tris = np.asarray(tris, dtype=np.float64)
    N = len(tris)
    secs, tv = _sections(tris)
    allv = tris.reshape(-1, 3)
    dmin = F32(allv.min(0))
    dmax = F32(allv.max(0))
    # vertices referenced by more than one section become shared (crack free borders)
    vsec = {}
    for si, ids in enumerate(secs):
        for v in set(tv[ids].ravel()):
            vsec.setdefault(int(v), set()).add(si)
    vpos = {}
    for t in range(N):
        for k in range(3):
            vpos.setdefault(int(tv[t, k]), allv[3 * t + k])
    if len(vsec) <= 0xffff:
        # everything fits the shared-vertex index: 21/21/22-bit precision over the whole mesh
        shared_ids = sorted(vsec)
    else:
        shared_ids = sorted(v for v, s in vsec.items() if len(s) > 1)
        if len(shared_ids) > 0xffff:
            shared_ids = shared_ids[:0xffff]
    shared_index = {v: i for i, v in enumerate(shared_ids)}
    span = (dmax - dmin).astype(np.float64)
    span[span == 0] = 1.0

    def enc_shared(p):
        q = np.clip(np.round((p - dmin) / span * np.array([0x1fffff, 0x1fffff, 0x3fffff])), 0,
                    [0x1fffff, 0x1fffff, 0x3fffff]).astype(np.int64)
        return int(q[0]) | (int(q[1]) << 21) | (int(q[2]) << 42)

    def dec_shared(v):
        return dmin + span * np.array([(v & 0x1fffff) / 0x1fffff, ((v >> 21) & 0x1fffff) / 0x1fffff,
                                       ((v >> 42) & 0x3fffff) / 0x3fffff])
    sharedV = [enc_shared(vpos[v]) for v in shared_ids]
    packedV, prims, svi, runs = [], [], [], []
    srec = []
    sec_boxes = []
    for si, ids in enumerate(secs):
        vl = []
        for t in ids:
            for k in range(3):
                v = int(tv[t, k])
                if v not in vl:
                    vl.append(v)
        loc_sh = [v for v in vl if v in shared_index]
        loc_pk = [v for v in vl if v not in shared_index]
        P = np.array([vpos[v] for v in loc_pk]) if loc_pk else np.zeros((0, 3))
        if loc_pk:
            pmin = F32(P.min(0))
            pmax = F32(P.max(0))
        else:
            pmin = pmax = F32(np.zeros(3))
        scale = F32(np.maximum((pmax - pmin).astype(np.float64) / np.array([2047.0, 2047.0, 1023.0]), 1e-7))
        dec = {}
        fpv = len(packedV)
        for v in loc_pk:
            q = np.clip(np.round((vpos[v] - pmin) / scale), 0, [2047, 2047, 1023]).astype(np.int64)
            packedV.append(int(q[0]) | (int(q[1]) << 11) | (int(q[2]) << 22))
            dec[v] = pmin.astype(np.float64) + scale.astype(np.float64) * q
        so = len(svi)
        for v in loc_sh:
            svi.append(shared_index[v])
            dec[v] = dec_shared(sharedV[shared_index[v]])
        lidx = {v: i for i, v in enumerate(loc_pk)}
        lidx.update({v: len(loc_pk) + i for i, v in enumerate(loc_sh)})
        po = len(prims)
        tb_min, tb_max = [], []
        for t in ids:
            a, b, c = (lidx[int(tv[t, k])] for k in range(3))
            prims.append(bytes([a, b, c, c]))
            q3 = np.array([dec[int(tv[t, k])] for k in range(3)])
            tb_min.append(F32(q3.min(0)))
            tb_max.append(F32(q3.max(0)))
        tb_min = np.array(tb_min)
        tb_max = np.array(tb_max)
        smin = F32(tb_min.min(0))
        smax = F32(tb_max.max(0))
        sec_boxes.append((smin, smax))
        # data runs
        ro = len(runs)
        vals = [(int(filterpal_index(0))) | (int(uidx[t]) << 8) | (int(weld[t]) << 16) for t in ids]
        j = 0
        while j < len(vals):
            k = j
            while k + 1 < len(vals) and vals[k + 1] == vals[j] and k + 1 - j < 255:
                k += 1
            runs.append((vals[j], j, k - j + 1))
            j = k + 1
        # section tree
        local = np.arange(len(ids))
        tcent = (tb_min + tb_max) * 0.5
        root = _build_tree(local, tb_min, tb_max, tcent, None)
        nodes, _ = _emit_tree(root, smin, smax, 4, lambda i: i)
        srec.append(dict(nodes=nodes, smin=smin, smax=smax, off=pmin, scale=scale, fpv=fpv,
                         sh=(so << 8) | len(loc_pk), pr=(po << 8) | len(ids), dr=(ro << 8) | (len(runs) - ro),
                         npv=len(loc_pk), nsi=len(loc_sh)))
        assert len(loc_pk) <= 255 and len(loc_sh) <= 255 and len(ids) <= 127 and len(runs) - ro <= 255
    # top tree over sections
    smins = np.array([b[0] for b in sec_boxes])
    smaxs = np.array([b[1] for b in sec_boxes])
    root = _build_tree(np.arange(len(secs)), smins, smaxs, (smins + smaxs) * 0.5, None)
    top, leafnode = _emit_tree(root, dmin, dmax, 5, lambda i: i)
    nkeys = N
    maxkey = max(((si << 8) | ((len(ids) - 1) << 1)) for si, ids in enumerate(secs))
    # ---------------------------------------------------------- serialize
    D = bytearray()
    loc = []           # (src offset, dst offset)

    def al(a=16):
        while len(D) % a:
            D.append(0)
    hdr = bytearray(header if header is not None else b'\0' * 0x30)
    D += hdr[:0x30]
    D += b'\0' * (0xf0 - 0x30)

    def arr(at, n):
        struct.pack_into('<QII', D, at, 0, n, 0x80000000 | n)
    # palettes
    al()
    loc.append((0x30, len(D)))
    arr(0x30, len(filterpal))
    D += struct.pack('<%dI' % len(filterpal), *filterpal)
    al()
    loc.append((0x40, len(D)))
    arr(0x40, len(userpal))
    D += struct.pack('<%dI' % len(userpal), *userpal)
    al()
    loc.append((0x50, len(D)))
    arr(0x50, len(top) // 5)
    D += top
    al()
    struct.pack_into('<4f', D, 0x60, *[float(x) for x in dmin], 1.0)
    struct.pack_into('<4f', D, 0x70, *[float(x) for x in dmax], 1.0)
    struct.pack_into('<iiI', D, 0x80, nkeys, max(1, maxkey.bit_length()), maxkey)
    secbase = len(D)
    loc.append((0x90, secbase))
    arr(0x90, len(secs))
    D += b'\0' * (0x60 * len(secs))
    for si, s in enumerate(srec):
        o = secbase + 0x60 * si
        struct.pack_into('<QII', D, o, 0, len(s['nodes']) // 4, 0x80000000 | (len(s['nodes']) // 4))
        struct.pack_into('<4f', D, o + 0x10, *[float(x) for x in s['smin']], 1.0)
        struct.pack_into('<4f', D, o + 0x20, *[float(x) for x in s['smax']], 1.0)
        struct.pack_into('<6f', D, o + 0x30, *[float(x) for x in s['off']], *[float(x) for x in s['scale']])
        struct.pack_into('<IIIIBBHBBBB', D, o + 0x48, s['fpv'], s['sh'], s['pr'], s['dr'], s['npv'], s['nsi'],
                         leafnode[si], 0, 0, 0, 0)
    for si, s in enumerate(srec):
        al()
        loc.append((secbase + 0x60 * si, len(D)))
        D += s['nodes']
    for at, data, n in ((0xa0, b''.join(prims), len(prims)), (0xb0, struct.pack('<%dH' % len(svi), *svi), len(svi)),
                        (0xc0, struct.pack('<%dI' % len(packedV), *packedV), len(packedV)),
                        (0xd0, struct.pack('<%dQ' % len(sharedV), *sharedV), len(sharedV)),
                        (0xe0, b''.join(struct.pack('<IBBH', v, i, c, 0) for v, i, c in runs), len(runs))):
        al()
        arr(at, n)
        if n:
            loc.append((at, len(D)))
            D += data
    al()
    return bytes(D), loc


def filterpal_index(_):
    return 0


CLASSNAMES = (b"\xf6^Xu\thkClass\x00\xc2\xa4~\\\thkClassMember\x00\xcf\t6\x8a\thkClassEnum\x00"
              b"l\x8ao\xce\thkClassEnumItem\x00'?;\xb6\thkpBvCompressedMeshShape\x00" + b'\xff' * 12)
# retail shape header (vtable/refcount/hkpShape/hkpBvTreeShape/flags), from ldn-train-stage
SHAPE_HDR = bytes.fromhex('000000000000000000000000000400000000000000000000030000000000000000000000000000000000000000010100')


def packfile(data, loc):
    """wrap a __data__ section into a 2011.3 64-bit LE packfile (same layout as retail)."""
    out = bytearray(bytes.fromhex('57e0e05710c0c010000000000900000008010101030000000200000000000000'
                                  '000000004b000000') + b'hk_2011.3.0-r1\x00\xff' + bytes.fromhex('00000000ffffffff'))
    assert len(out) == 0x40
    lf = bytearray()
    for s, d in loc:
        lf += struct.pack('<II', s, d)
    while len(lf) % 16:
        lf += b'\xff'
    vf = struct.pack('<III', 0, 0, 0x4b) + b'\xff' * 4
    start_cn = 0xd0
    start_data = start_cn + len(CLASSNAMES)
    dsz = len(data)
    secs = [(b'__classnames__', start_cn, [len(CLASSNAMES)] * 6),
            (b'__types__', start_data, [0] * 6),
            (b'__data__', start_data, [dsz, dsz + len(lf), dsz + len(lf), dsz + len(lf) + len(vf),
                                      dsz + len(lf) + len(vf), dsz + len(lf) + len(vf)])]
    for tag, st, offs in secs:
        out += tag.ljust(19, b'\0') + b'\xff' + struct.pack('<7I', st, *offs)
    assert len(out) == start_cn
    out += CLASSNAMES
    out += data + lf + vf
    return bytes(out)


def build_from_ps3(c):
    """c = ps3_collision(); returns (packfile bytes, mats table [(friction, flags)])"""
    mats = c['mats']
    flags_used = []
    for m in c['mat']:
        f = mats[m][1]
        if f not in flags_used:
            flags_used.append(f)
    uidx = np.array([flags_used.index(mats[m][1]) for m in c['mat']])
    data, loc = build_bvcm(c['tris'], c['weld'], uidx, flags_used, header=SHAPE_HDR)
    return packfile(data, loc), mats


def havok_bg_blobs(p):
    """one Blob per PS3 COLLISION_DATA_HAVOK_BG item that has a mesh, in PS3 order"""
    out = []
    for k in range(len(p.items_of('COLLISION_DATA_HAVOK_BG'))):
        b = havok_bg_blob(p, k)
        if b is not None:
            out.append(b)
    return out


def havok_bg_blob(p, index=0):
    """ps4writer Blob for the PS4 COLLISION_DATA_HAVOK_BG item, or None when the PS3 item has no mesh."""
    from ps4writer import Blob
    c = ps3_collision(p, index)
    if c is None or c['tris'] is None or len(c['tris']) == 0:
        return None
    pf, mats = build_from_ps3(c)
    B = Blob('havokbg')
    B.u32(c['version']).u32(0xaaaaaaaa).u64(0).u64(0)          # +08/+10: PS3 +04/+08 (zero)
    B.u32(c['count'])                                          # +18
    B.raw(b'\0' * (0x34 - B.pos))                              # PS3 +10..+20 are zero in every sample
    B.raw(c['misc'][0x10:0x14])                                # +34..+38 = PS3 +20..+24 (+21 -> +35, read by
                                                               # HavokLoginLevel 0x1133bf0 / PS3 f_8dc0e8)
    B.u64(0).u64(0)                                            # +38/+40 runtime
    B.u64(0)                                                   # +48 sid list (retail: null)
    arr_pos = B.pos
    B.u64(0)                                                   # +50 -> u32[count]
    B.u64(0).u64(0).u64(0)                                     # +58/+60 runtime, +68 null
    tr_pos = B.pos
    B.u64(0)                                                   # +70 -> trailer
    B.u32(0xaaaaaaaa).u32(0xaaaaaaaa)
    assert B.pos == 0x80
    a_off = B.pos
    B.raw(struct.pack('<%dI' % len(c['arr']), *c['arr']) if c['arr'] else b'')
    B.raw(b'\0' * ((-len(B.b)) % 16))
    B.raw(b'\0' * 0x100 if not c['arr'] else b'')
    while B.pos < 0x180:
        B.raw(b'\0' * 16)
    B.pad(16, 0xaa)
    pf_off = B.pos
    B.raw(pf)
    B.pad(16, 0xaa)
    m_off = B.pos
    for fr, fl in mats:
        B.u32(1).raw(struct.pack('<f', fr if fr else 1.0)).u32(fl)
    B.pad(8, 0xaa)
    t_off = B.pos
    B.u64(0).u64(0).u32(len(pf)).u32(len(mats))
    B.relocs += [(arr_pos, B, a_off), (tr_pos, B, t_off), (t_off, B, m_off), (t_off + 8, B, pf_off)]
    return B


class _Pf:
    def __init__(self, b):
        st, lf, gf = struct.unpack_from('<III', b, 0x40 + 0x30 * 2 + 20)
        self.D = b[st:st + lf]
        self.loc = {}
        for o in range(st + lf, st + gf, 8):
            a_, t = struct.unpack_from('<II', b, o)
            if a_ != 0xffffffff:
                self.loc[a_] = t

    def arr(self, o):
        return self.loc.get(o), struct.unpack_from('<I', self.D, o + 8)[0]


def _dec_box(xyz, pmin, pmax):
    span = (pmax - pmin).astype(F32)
    mn = pmin.astype(F32).copy()
    mx = pmax.astype(F32).copy()
    for a in range(3):
        lo, hi = xyz[a] >> 4, xyz[a] & 15
        mn[a] = F32(pmin[a] + span[a] * F32(lo * lo) * F32(1.0 / 226.0))
        mx[a] = F32(pmax[a] - span[a] * F32(hi * hi) * F32(1.0 / 226.0))
    return mn, mx


def decode_bvcm(pf):
    """independent decoder (validated on retail ldn-train): returns dict with triangles per section,
    run values and tree containment errors."""
    P = _Pf(pf)
    D = P.D
    T = 0x50
    tp, tn = P.arr(T)
    dmin = np.array(struct.unpack_from('<3f', D, T + 0x10), F32)
    dmax = np.array(struct.unpack_from('<3f', D, T + 0x20), F32)
    sp, sn = P.arr(T + 0x40)
    pp, pn = P.arr(T + 0x50)
    vp, vn = P.arr(T + 0x60)
    kp, kn = P.arr(T + 0x70)
    hp, hn = P.arr(T + 0x80)
    rp, rn = P.arr(T + 0x90)
    prims = [struct.unpack_from('4B', D, pp + 4 * i) for i in range(pn)] if pn else []
    svi = struct.unpack_from('<%dH' % vn, D, vp) if vn else ()
    pv = struct.unpack_from('<%dI' % kn, D, kp) if kn else ()
    sv = struct.unpack_from('<%dQ' % hn, D, hp) if hn else ()
    runs = [struct.unpack_from('<IBBH', D, rp + 8 * i) for i in range(rn)] if rn else []
    span = (dmax - dmin).astype(np.float64)
    out_tris, out_vals, bad = [], [], 0
    secs = []
    for s in range(sn):
        o = sp + 0x60 * s
        snp, snn = P.arr(o)
        smin = np.array(struct.unpack_from('<3f', D, o + 0x10), F32)
        smax = np.array(struct.unpack_from('<3f', D, o + 0x20), F32)
        cp = np.array(struct.unpack_from('<6f', D, o + 0x30), np.float64)
        fpv, shv, pr, dr = struct.unpack_from('<IIII', D, o + 0x48)
        # runtime (section accessor 0x12fba70): numPacked = low byte of the shared-vertices word,
        # shared vertex k (k >= numPacked) = svi[(word >> 8) - numPacked + k]; +0x58 repeats numPacked
        npv = shv & 0xff
        if D[o + 0x58] != npv:
            bad += 1
        secs.append((smin, smax))

        def vert(k):
            if k < npv:
                v = pv[fpv + k]
                return cp[:3] + cp[3:] * np.array([v & 0x7ff, (v >> 11) & 0x7ff, v >> 22])
            v = sv[svi[(shv >> 8) + k - npv]]
            return dmin + span * np.array([(v & 0x1fffff) / 0x1fffff, ((v >> 21) & 0x1fffff) / 0x1fffff,
                                           ((v >> 42) & 0x3fffff) / 0x3fffff])
        stris = []
        for j in range(pr & 0xff):
            a_, b_, c_, d_ = prims[(pr >> 8) + j]
            stris.append(np.array([vert(a_), vert(b_), vert(c_)]))
        vals = []
        for v, i0, cnt, _ in runs[dr >> 8:(dr >> 8) + (dr & 0xff)]:
            vals += [v] * cnt
        # section tree containment
        def rec(i, pmin, pmax):
            nonlocal bad
            xyz = D[snp + 4 * i:snp + 4 * i + 3]
            d = D[snp + 4 * i + 3]
            mn, mx = _dec_box(xyz, pmin, pmax)
            if d & 1:
                rec(i + 1, mn, mx)
                rec(i + 2 * (d >> 1), mn, mx)
            else:
                t = stris[d >> 1]
                if np.any(t < mn - 1e-4) or np.any(t > mx + 1e-4):
                    bad += 1
        if stris:
            rec(0, smin, smax)
        out_tris.append(stris)
        out_vals.append(vals)

    def rect(i, pmin, pmax):
        nonlocal bad
        xyz = D[tp + 5 * i:tp + 5 * i + 3]
        hi, lo = D[tp + 5 * i + 3], D[tp + 5 * i + 4]
        mn, mx = _dec_box(xyz, pmin, pmax)
        v = ((hi & 0x7f) << 8) | lo
        if hi & 0x80:
            rect(i + 1, mn, mx)
            rect(i + 2 * v, mn, mx)
        else:
            smin, smax = secs[v]
            if np.any(smin < mn - 1e-4) or np.any(smax > mx + 1e-4):
                bad += 1
    rect(0, dmin, dmax)
    return dict(tris=out_tris, vals=out_vals, bad=bad, nsec=sn, npk=struct.unpack_from('<iiI', D, T + 0x30))


def check(pf, c):
    """decode our packfile and compare with the PS3 source triangles (as a set, order-insensitive)"""
    r = decode_bvcm(pf)
    got = np.array([t for s in r['tris'] for t in s])
    errs = []
    if len(got) != len(c['tris']):
        errs.append('triangle count %d != %d' % (len(got), len(c['tris'])))
    if r['bad']:
        errs.append('%d tree containment failures' % r['bad'])
    # nearest source triangle by centroid (brute force in blocks), error over matching vertices
    src = np.asarray(c['tris'])
    sc = src.mean(1)
    maxerr = 0.0
    miss = 0
    for i0 in range(0, len(got), 512):
        g = got[i0:i0 + 512]
        gc = g.mean(1)
        d = ((gc[:, None, :] - sc[None, :, :]) ** 2).sum(-1)
        jj = np.argsort(d, axis=1)[:, :4]          # coincident twins: take the best of the 4 nearest
        e = np.min([np.abs(g - src[jj[:, k]]).max(axis=(1, 2)) for k in range(jj.shape[1])], axis=0)
        miss += int((e > 0.05).sum())
        maxerr = max(maxerr, float(e.max()))
    return errs, dict(tris=len(got), sections=r['nsec'], max_vertex_error=maxerr, unmatched=miss)


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('out')
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    p = Pak(a.src)
    c = ps3_collision(p)
    if c is None or c['tris'] is None:
        print('no PS3 collision mesh in', a.src)
        sys.exit(1)
    pf, mats = build_from_ps3(c)
    open(a.out, 'wb').write(pf)
    print('wrote %s: %d bytes, %d triangles, %d materials' % (a.out, len(pf), len(c['tris']), len(mats)))
    if a.check:
        print('check:', check(pf, c))
