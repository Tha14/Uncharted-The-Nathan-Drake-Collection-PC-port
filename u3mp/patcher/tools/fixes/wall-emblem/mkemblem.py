#!/usr/bin/env python3
"""air-multi wall emblem prototype: PS3 patch -> foreground actor pak on the retail loading-menu-cloth effects, spawner
records into a rebuilt air-multi-ingame12, collapsed background patches in a copy of air-multi.pak.
usage: mkemblem.py <overlay main dir> <retail actor33/ps4 dir>   (writes only below ./out)"""
from lib import *
import ps4actor as A
from ps4actor import AutoPages, bindfull, bc1_flat, copy_item_blob, tmpl_materials, f16, PAD32
from ps4writer import PakWriter, Blob, make_login_and_vram_page
from convert_net import extract_effect_span, PS4_COOK_PREFIX
import texcodec as tc, json, hashlib, ps4ingame
OV, RET = sys.argv[1].rstrip('/') + '/', sys.argv[2].rstrip('/') + '/'
T_ = Pak(RET + 'loading-menu-cloth.pak')
SK = Pak(RET + 'cargo-plane-int-moving-multi.pak')     # retail NdSimpleObject: 1 joint, no ANIM
MAP = Pak(OV + 'pak23/ps4/air-multi.pak')
P3 = Pak('p3/air-multi.pak')
MAPTX = {t['path']: t for t in MAP.textures()}
MAPM = {m['name'].split(',')[0]: m for m in tmpl_materials(MAP)}


def clip(v, tris):
    """split on uv1 = 0 / 1 so nothing relies on the overlay sampler's wrap mode; outside pieces get uv1 (0,0)"""
    V = {k: list(a) for k, a in v.items()}

    def lerp(a, b, t):
        for k in V:
            V[k].append(np.asarray(V[k][a]) * (1 - t) + np.asarray(V[k][b]) * t)
        return len(V['pos']) - 1
    cur = [(tuple(t), True) for t in tris]
    for ax, lim, keep in ((0, 0.0, 1), (0, 1.0, -1), (1, 0.0, 1), (1, 1.0, -1)):
        nxt = []
        for t, ins in cur:
            if not ins:
                nxt.append((t, False))
                continue
            d = [keep * (V['uv1'][i][ax] - lim) for i in t]
            if min(d) >= -1e-5:
                nxt.append((t, True))
                continue
            if max(d) <= 1e-5:
                nxt.append((t, False))
                continue
            pi_, po_ = [], []
            for j in range(3):
                a, b = t[j], t[(j + 1) % 3]
                da, db = d[j], d[(j + 1) % 3]
                (pi_ if da >= 0 else po_).append(a)
                if (da >= 0) != (db >= 0):
                    m = lerp(a, b, da / (da - db))
                    pi_.append(m)
                    po_.append(m)
            for pl, fl in ((pi_, True), (po_, False)):
                for k in range(1, len(pl) - 1):
                    nxt.append(((pl[0], pl[k], pl[k + 1]), fl))
        cur = nxt
    out, nout = [], 0
    for t, ins in cur:
        if ins:
            out.append(t)
            continue
        nt = []
        for i in t:
            for k in V:
                V[k].append(np.asarray(V[k][i]))
            V['uv1'][-1] = np.zeros(2)
            nt.append(len(V['pos']) - 1)
        out.append(tuple(nt))
        nout += 1
    return {k: np.array(a) for k, a in V.items()}, out, nout


def patches():
    res = []
    for pi, c in enumerate(G.collection(P3)):
        for m3, fi, cnt in c['groups']:
            if 'emblem' not in m3:
                continue
            for ii, ia in enumerate(c['insts']):
                I = G.instance(P3, ia)
                M = I['mat']
                vs, ts, base = [], [], 0
                for k in range(fi, fi + cnt):
                    d = G.decode_block(P3, *c['blocks'][k])
                    n = d['nverts']
                    pos = np.c_[d['pos'], np.ones(n)] @ M
                    R = M[:3, :3]
                    Rn = R / np.cbrt(abs(np.linalg.det(R)))
                    nrm = d['normal'][:, :3] @ Rn
                    tan = d['tangent'][:, :3] @ Rn
                    tw = d['tangent'][:, 3] if d['tangent'].shape[1] > 3 else np.ones(n)
                    vs.append(dict(pos=pos[:, :3], uv0=d['uv0'], uv1=d['uv1'], nrm=nrm, tan=np.c_[tan, tw]))
                    ts += [(a + base, b + base, e + base) for a, b, e in d['tris']]
                    base += n
                v = {k: np.vstack([x[k] for x in vs]) for k in vs[0]}
                v, tr, nout = clip(v, ts)
                res.append(dict(proto=pi, inst=ii, mat=m3, v=v, tris=tr, nout=nout, x8=I['x8'],
                                blackboard='blackboard' in m3))
    return res


def proto_caps():
    a3, a4 = 8, 1
    for i in T_.items:
        if i.type == 'RAW_DATA' and i.size >= 0x38:
            o = T_.pbase(i.page) + i.data_off
            if struct.unpack_from('<I', T_.d, o)[0] == 3 and struct.unpack_from('<I', T_.d, o + 12)[0] == 0x5791a421:
                x, y = struct.unpack_from('<II', T_.d, o + 0x30)
                a3, a4 = max(a3, x), max(a4, y)
    return a3, a4


def build(art, pt):
    w = PakWriter()
    span, sit = extract_effect_span(T_)
    p0 = AutoPages(w, max_entries=max(1, (sit[0].off - 0x14) // 16))
    span.modulo = 1 << 40
    span.phase = sit[0].off
    p0.span(span)
    vram = bytearray()
    tex = []

    def add(short, h, fmt, tw, th, mips, tiled, flags=3, unk20=None):
        nonlocal vram
        while len(vram) % 0x100:
            vram.append(0)
        off = len(vram)
        vram += tiled
        nb = Blob('texname')
        nb.cstr(short)
        ni_ = p0.item('RAW_DATA', nb)
        full = PS4_COOK_PREFIX + short
        db = Blob('vramdesc')
        db.u64(0).u64(off).u32(len(tiled)).u32(h)
        db.u32(1).u32(fmt).u32(unk20 if unk20 is not None else (0x010f0000 if fmt in (0x48, 0x4b, 0x4e) else 0x01000000))
        db.u32(mips).u32(tw).u32(th).u32(0).u32(flags)
        db.ptr(db, 0x40)
        db.cstr(full)
        e = dict(hash=h, short=short, full=full, desc=p0.item('VRAM_DESC', db), name=ni_, fmt=fmt)
        tex.append(e)
        return e

    def flat(tag, rgb):
        lin, m = bc1_flat(64, 64, rgb)
        ti_ = tc.ps3_to_ps4_texels(lin, 64, 64, m, 8)
        sh = 'art/generated/u3mp/emblem-%s.tga/%016X.ndb' % (tag, sid(tag))
        return add(sh, sid(sh), 0x47, 64, 64, m, ti_)
    key = pt['mat'].rsplit('/', 1)[0] + ':' + pt['mat'].rsplit('/', 1)[1]
    mm = MAPM[key]
    cb = [b for b in mm['binds'] if b[0] == 'g_tNdFetchColor1Map'][0]
    t = MAPTX.get(cb[2]) or MAPTX.get(cb[2] + '.ndb')
    full = t['path']
    short = full.split(PS4_COOK_PREFIX, 1)[1] if PS4_COOK_PREFIX in full else full
    ecol = add(short, t['hash'], t['format'], t['width'], t['height'], t['mips'],
               MAP.vram_bytes(t['vramOffset'], t['vramSize']), flags=t['unk34'], unk20=t['unk20'])
    ecol['full'] = full
    # shared map: flat normal in RG, B = 0 (cloth uses B as AO and as the COLOR_4 mask: no second layer; AO tint below = 1)
    eshared = flat('shared-flat', (128, 128, 0))
    ewhite = flat('white', (255, 255, 255))
    tm = tmpl_materials(T_)
    pv = {a: struct.unpack('<f', d[:4])[0] for a, c, d in mm['params']}
    pv3 = {a: struct.unpack('<3f', d[:12]) for a, c, d in mm['params'] if c == 3}

    def ov(nm, vals):
        f = lambda *x: struct.pack('<4f', *x)
        one = lambda x: f(x, 0, 0, 0)
        if nm == 'g_ambientOcclusionTint' or nm.startswith(('g_facingTintIn', 'g_facingTintOut')):
            return f(1, 1, 1, 0)
        if nm == 'g_colorTint1':
            return f(*pv3.get('g_colorTint1', pv3.get('g_colorTint2', (1, 1, 1))), 0)
        if nm == 'g_colorBrightness1':
            return one(pv.get('g_colorBrightness1', pv.get('g_colorBrightness2', 1.0)))
        if nm in ('g_normalStrength1', 'g_specularMetalness', 'g_uvOffsetU2', 'g_uvOffsetV2'):
            return one(0.0)
        if nm == 'g_specularBrightness':
            return one(pv.get('g_specularBrightness', 0.0))
        if nm == 'g_specularExponentMax':
            return one(pv.get('g_specularExponentMax', 10.0))
        if nm.startswith('g_uvScale'):
            return one(1.0)
        return vals
    b0 = {'g_tNdFetchSharedTexture1Map': eshared, 'g_tNdFetchColor1Map': ecol, 'g_tNdFetchOverlay1Map': ewhite,
          'g_tNdFetchOverlay4Map': ewhite, 'g_tNdFetchTransparencyMap': ewhite}
    hx = hashlib.md5(art.encode()).hexdigest()
    guid = '%s-%s-%s-%s-%s' % (hx[:8], hx[8:12], hx[12:16], hx[16:20], hx[20:32])
    recs = [dict(name='objects/levels/multiplayer/%s:default,%s,%s' % (art, guid, tm[0]['shader']), tm=tm[0],
                 params=[(a, c, ov(a, d)) for a, c, d in tm[0]['params']],
                 binds=[(a, b0[a]) for a, _, _ in tm[0]['binds']]),
            dict(name=tm[1]['name'], tm=tm[1], params=tm[1]['params'],
                 binds=[(a, ewhite if 'Color' in a else eshared) for a, _, _ in tm[1]['binds']])]
    ttab = p0.item('TEXTURE_TABLE')
    order = sorted(tex, key=lambda e: e['hash'])
    nb = Blob('ttnames')
    for e in order:
        nb.ptr(e['name'].data)
    nit = p0.item('RAW_DATA', nb)
    hb = Blob('tthash')
    for e in order:
        hb.u32(e['hash'])
    hit = p0.item('RAW_DATA', hb)
    tb = ttab.data
    tb.u64(len(order)).ptr(hit.data).ptr(nit.data)
    tb.ptr(tb, 0x20)
    for e in order:
        tb.ptr(e['desc'].data)
    strs = Blob('geostrings')
    sp = {}

    def S(s):
        if s not in sp:
            sp[s] = strs.cstr(s)
        return sp[s]

    def raw(d):
        b = Blob('raw')
        b.raw(bytes(d))
        return p0.item('RAW_DATA', b).data
    v = pt['v']
    c = (v['pos'].min(0) + v['pos'].max(0)) / 2
    pos = (v['pos'] - c).astype('<f4')
    nv = len(pos)
    rad = float(np.linalg.norm(pos, axis=1).max())
    fmtb = Blob('vtxfmt')
    fo = {}
    for s_, f_ in {0: (0, 0, 12, 6), 1: (1, 0, 4, 0x22), 12: (12, 0, 4, 0x22), 2: (2, 0, 8, 0x0a), 3: (3, 0, 8, 0x0a),
                   4: (4, 0, 8, 0x0a)}.items():
        fo[s_] = fmtb.pos
        fmtb.raw(bytes(f_))
    p0.item('RAW_DATA', fmtb)
    streams = [(0, 12, pos.tobytes()), (1, 4, f16(v['uv0']).astype('<f2').tobytes()),
               (12, 4, f16(v['uv1']).astype('<f2').tobytes()),
               (2, 8, f16(np.c_[v['nrm'], np.ones(nv)]).astype('<f2').tobytes()),
               (3, 8, f16(v['tan']).astype('<f2').tobytes()), (4, 8, np.ones((nv, 4), '<f2').tobytes())]
    st = Blob('streams')
    for s_, sd, d in streams:
        db = raw(d)
        st.u32((sd << 16) | 1).u32(len(d)).ptr(fmtb, fo[s_]).ptr(db)
    sti = p0.item('RAW_DATA', st)
    ib = raw(np.array(pt['tris'], '<u2').reshape(-1).tobytes())
    ni = len(pt['tris']) * 3
    cnts, offs, js = raw(np.ones(nv, '<u2').tobytes()), raw(np.arange(nv, dtype='<u2').tobytes()), raw(np.zeros(nv, '<u2').tobytes())
    ws = raw(np.ones(nv, '<f4').tobytes())
    skb = Blob('skin')
    skb.u32(0).u32(nv).ptr(cnts).ptr(offs).ptr(js).ptr(ws)
    ski = p0.item('RAW_DATA', skb)
    jh = SK.items_of('JOINT_HIERARCHY')[0]
    fgS = SK.items_of('FOREGROUND_SECTION_2')[0]
    skel, hier = struct.unpack_from('<II', SK.d, SK.pbase(fgS.page) + fgS.data_off + 8)
    jr = SK.resolve(fgS.page, fgS.data_off + 0x10)
    jit = SK.item_at(jr[0], jr[1] - 0x20)
    jnames = SK.d[SK.pbase(jr[0]) + jr[1]:SK.pbase(jr[0]) + jr[1] + jit.size]
    subs = Blob('submeshes')
    sname = '%s.Prototype0.Submesh0' % art
    subs.u64(0).ptr(strs, S(sname)).u32(1).u32(0).u32(0).u32(0).u32(nv).u32(ni).u32(len(streams)).u32(0).ptr(sti.data).u64(0).ptr(ib)
    subs.ptr(strs, S(recs[0]['name'])).i32(-1).u32(0).u32(1).u32(0).ptr(ski.data).raw(b'\0' * 0x20)
    p0.item('RAW_DATA', subs)
    cb3, cb4 = proto_caps()
    pname = 'Prototype0.%s' % art
    fl = Blob('pf')
    fl.raw(bytes([1, 0, 1, 1, 0, 0, 1, 1]) + b'\0' * 8)
    sn = Blob('sn')
    sn.ptr(strs, S(sname)).i32(-1).u32(0)
    pb = Blob('prototypes')
    pb.u32(3).u32(1).u32(0).u32(0x5791a421).ptr(strs, S(pname)).ptr(sn).ptr(fl).u64(0).u32(cb3).u32(cb4).raw(b'\0' * (0x78 - 0x38))
    pl = Blob('pl')
    pl.ptr(pb)
    for b in (pb, fl, sn, pl):
        p0.item('RAW_DATA', b)
    gt = []
    for m in recs:
        for _, e in m['binds']:
            if e not in gt:
                gt.append(e)
    texb = Blob('gt')
    for e in gt:
        texb.ptr(strs, S(bindfull(e))).u32(e['hash']).u32(0x200).u32(0x101).i32(-1).u32(0).u32(4).u64(0)
    fe, pa, va, bi, ma = Blob('f'), Blob('p'), Blob('v'), Blob('b'), Blob('m')
    for m in recs:
        tmm = m['tm']
        fo_ = fe.pos
        for f in tmm['feats']:
            fe.ptr(strs, S(f))
        po = pa.pos
        for n_, c_, vals in m['params']:
            vo = va.pos
            va.raw(vals)
            pa.ptr(strs, S(n_)).ptr(va, vo).u32(c_).u32(0)
        bo = bi.pos
        for pn, e in m['binds']:
            sh = e['short'].rsplit('/', 1)[0] if e['short'].endswith('.ndb') else e['short']
            bi.ptr(strs, S(pn)).ptr(strs, S(sh)).ptr(strs, S(bindfull(e))).i32(-1).u32(0).u64(0)
        ma.ptr(strs, S(m['name'])).ptr(strs, S(tmm['shader'])).ptr(fe, fo_).ptr(strs, S(tmm['hash'])).ptr(pa, po).ptr(bi, bo)
        ma.u32(len(tmm['feats'])).u32(len(m['params'])).u32(len(m['binds'])).u32(tmm['x3']).u32(tmm['tail']).u32(0)
    for b in (fe, pa, va, bi, ma, texb):
        p0.item('RAW_DATA', b)
    p0.item('RAW_DATA', strs)
    gb = Blob('geometry')
    gb.u32(0x10).u32(1).u64(1).ptr(subs).u64(1).ptr(pl).u64(0).ptr(None).u64(len(gt)).ptr(texb).u64(2).ptr(ma).u64(0).u64(0)
    geo = p0.item('GEOMETRY_1', gb)
    jn = raw(bytes(jnames))
    fb = Blob('fg')
    fb.ptr(fb, 0x1c).u32(skel).u32(hier).ptr(jn)
    fb.raw(struct.pack('<f', 0.0))
    fb.cstr(pname)
    fg = p0.item('FOREGROUND_SECTION_2', fb, name=art)
    ji = p0.item('JOINT_HIERARCHY', copy_item_blob(SK, jh), name=jh.name)
    tv = Blob('t')
    tv.raw(struct.pack('<4f', 0, 0, 0, rad))
    tv_ = p0.item('TAG_VEC4', tv, name='vis_sphere')
    ti = Blob('t')
    ti.raw(struct.pack('<i', 0))
    ti_ = p0.item('TAG_INT', ti, name='vis_joint_index')
    keys = [sid(jh.name), sid('geo.' + art)]
    nb_ = 7
    ab = Blob('artgroup')
    n = 2
    ab.u32(n).u32(1).ptr(ab, 0x50).ptr(ab, 0x30).u64(0).u64(0).u32(PAD32).u32(PAD32)
    tbl = 0x50 + 16 * n
    x = tbl + 16 * nb_
    ab.u64(0).u32(1).u32(0x12345).ptr(ab, x).u32(PAD32).u32(PAD32)
    assert ab.pos == 0x50
    ab.ptr(ji.data, -0x20).u64(0)
    ab.u64(0).u64(0)
    bk = [None] * nb_
    for k, key_ in enumerate(keys):
        i = key_ % nb_
        while bk[i] is not None:
            i = (i + 1) % nb_
        bk[i] = (key_, k)
    for e in bk:
        if e is None:
            ab.u32(0).u32(PAD32).u64(0)
        else:
            ab.u32(e[0]).u32(PAD32).ptr(ab, 0x50 + 16 * e[1])
    ab.u32(0).u32(PAD32).u64(0).u64(0).u32(0).u32(nb_).ptr(ab, tbl)
    ag = p0.item('ARTGROUP_DATA', ab, name=art)
    lp = Blob('lp')
    lp.u64(0)
    lpi = p0.item('LEVEL_PATS_1', lp)
    pk = Blob('pk')
    pk.ptr(pk, 0x20).u32(0).u32(1).ptr(pk, 0x20 + ((len(art) + 1 + 15) // 16) * 16).u32(PAD32).u32(PAD32)
    pk.cstr(art)
    pk.pad(16, 0xaa)
    pk.u32(sid(art)).u32(PAD32).ptr(ag.data, -0x20)
    pki = p0.item('PACKAGE_ITEM_LOOKUP', pk)
    p1 = w.page()
    make_login_and_vram_page(w, p1, [None] * 3, [e['desc'] for e in tex])
    w.vram = bytes(vram)

    class SI:
        def __init__(s, sp_, pos_):
            s.span, s.pos = sp_, pos_
        page = property(lambda s: s.span.page)
        off = property(lambda s: s.span.off + s.pos)
    et = [i.off for i in sit if i.type == 'EFFECT_TABLE'][0] - sit[0].off
    w.login_targets = [lpi, ag, SI(span, et), ttab, geo, ji, fg, pki, tv_, ti_]
    w.login_item.data.b[:] = struct.pack('<II', len(w.login_targets), 0) + b'\0' * (8 * len(w.login_targets))
    d = w.build()
    open('out/actor33/ps4/%s.pak' % art, 'wb').write(d)
    return dict(art=art, center=c.tolist(), radius=rad, verts=nv, tris=len(pt['tris']), outside_tris=pt['nout'],
                color=short.split('/')[-2], bytes=len(d), uv1=[v['uv1'].min(0).tolist(), v['uv1'].max(0).tolist()])


SHORT = {'ground-concrete': 'ground', 'metal-diamond': 'diamond', 'corrugated-main-blue': 'corr-blue',
         'corrugated-main': 'corr', 'control-tower': 'tower', 'stucco-brown': 'stucco', 'blackboard': 'board'}
plan = []
for pt in patches():
    s = [v for k, v in SHORT.items() if k in pt['mat']][0]
    art = 'air-multi-emblem-%s%s' % (s, ('-%d' % pt['inst']) if pt['blackboard'] else '')
    r = build(art, pt)
    r.update(proto=pt['proto'], inst=pt['inst'], mat=pt['mat'], record='np+0x7e8' if pt['blackboard'] else 'np+0x7f0',
             spawner=art + '-spawner', spawner_sid=sid(art + '-spawner'))
    plan.append(r)
    print(art, r['verts'], r['tris'], 'outside', r['outside_tris'], r['color'], r['bytes'], hex(r['spawner_sid']))
ZERO = [0x560619f3, 0x56da20e1, 0x65c281d4, 0x66c6d3fe, 0x78812e7e, 0x871aab05, 0x97587862, 0xa88f1ca6, 0xbaa50d5b,
        0xc867d993, 0xc87fbb93]
ADD = False


class C2(ps4ingame.Conv):
    def spawner_group(self, it, root):
        a = (it.page, it.data_off)
        self.memo[a] = root
        arr, n = self.ptr(a, 4), self.u32(a, 8)
        srt, n2 = self.ptr(a, 12), self.u32(a, 16)
        sp = [self.spawner(self.ptr(arr, 4 * k)) for k in range(n)]
        root.u64(0)
        ab = self.new(arr, 'sp[]')
        srtd = [(self.u32(srt, 8 * k), self.spawner(self.ptr(srt, 8 * k + 4))) for k in range(n2)]
        for r in (plan if ADD else []):
            nm = self.new(None, 'str')
            nm.cstr(r['spawner'])
            at = self.new(None, 'str')
            at.cstr(r['art'])
            ents = sorted([(k, 1, struct.pack('<I', 0)) for k in ZERO] + [(0xe07cb817, 13, bytes.fromhex('f3d0ba39'))])
            tt = self.new(None, 'tagtab')
            for k, ty, dat in ents:
                vb = self.new(None, 'tagval')
                db = self.new(None, 'tagdata')
                db.raw(dat)
                vb.u16(ty).u16(4).u32(0xaaaaaaaa).ptr(db)
                tt.u32(k).u32(0xaaaaaaaa).ptr(vb)
            tg = self.new(None, 'tags')
            tg.ptr(tt).u32(len(ents)).u32(0xaaaaaaaa)
            b = self.new(None, 'spawner')
            b.raw(struct.pack('<4f', *r['center'], 0.0))
            b.raw(struct.pack('<4f', 0, 0, 0, 1))
            b.ptr(nm).u32(r['spawner_sid']).u32(0xaaaaaaaa).u64(0).u64(0)
            b.u32(0x2db62a42).u32(0xaaaaaaaa).ptr(at).u32(0).u32(0xaaaaaaaa).ptr(root).u64(0).ptr(tg).u64(0).u64(0).u64(0)
            sp.append(b)
            srtd.append((r['spawner_sid'], b))
        for b in sp:
            ab.ptr(b)
        root.ptr(ab).u32(len(sp)).u32(0xaaaaaaaa)
        srtd.sort(key=lambda e: e[0])
        sb = self.new(srt, 'sorted')
        for k, b in srtd:
            sb.u32(k).u32(0xaaaaaaaa).ptr(b)
        root.ptr(sb).u32(len(srtd)).u32(0xaaaaaaaa)


ps4ingame.Conv = C2
base = ps4ingame.convert('p3/air-multi-ingame12.pak', 'out/_ingame_base.pak')
ov = open(OV + 'pak23/ps4/air-multi-ingame12.pak', 'rb').read()
print('ingame rebuild without additions identical to overlay:', base == ov, len(base), len(ov))
ADD = True
ps4ingame.convert('p3/air-multi-ingame12.pak', 'out/pak23/ps4/air-multi-ingame12.pak')
d = bytearray(open(OV + 'pak23/ps4/air-multi.pak', 'rb').read())
g = MAP.items_of('GEOMETRY_1')[0]
pg, gb = g.page, g.data_off
ns = MAP.u64(MAP.pbase(pg) + gb + 8)
sr = MAP.resolve(pg, gb + 0x10)
hide = []
for i in range(ns):
    o = sr[1] + 0x88 * i
    mat = MAP.pstr(*MAP.resolve(sr[0], o + 0x48))
    if 'emblem' in mat:
        ni = MAP.pu32(sr[0], o + 0x24)
        ib = MAP.resolve(sr[0], o + 0x40)
        f = MAP.pbase(ib[0]) + ib[1]
        assert ni % 3 == 0
        hide.append(dict(sub=MAP.pstr(*MAP.resolve(sr[0], o + 8)), mat=mat.split(',')[0], ni=ni, off=f, orig=bytes(d[f:f + 2 * ni]).hex()))
        d[f:f + 2 * ni] = b'\0' * (2 * ni)
open('out/pak23/ps4/air-multi.pak', 'wb').write(d)
json.dump(dict(actors=plan, hide=hide, txt_lines=['actor ' + r['art'] for r in plan],
               spawner_type='NdSimpleObject 0x2db62a42'), open('out/spawn-plan.json', 'w'), indent=1)
print('hidden', [(h['sub'], h['ni']) for h in hide])
