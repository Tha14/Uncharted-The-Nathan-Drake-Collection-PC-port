#!/usr/bin/env python3
"""Convert a PS3 Uncharted 3 foreground actor package (skinned meshes: characters,
accessories, props) into a PS4 / Nathan Drake Collection package.

Geometry is decoded with ps3geom.py.  Materials, compiled effects and the joint-name
table come from a PS4 template pak (an existing PS4 actor that uses the same skeleton and
similar materials); every PS3 material is mapped onto one of the template's materials and
re-bound to the converted PS3 textures.

usage: ps4actor.py <ps3.pak> <ps4-template.pak> <out.pak> [--rename NEW-ARTGROUP-NAME]
"""
import argparse, hashlib, os, re, struct, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ndpak import Pak, sid
from ps4writer import PakWriter, Blob, make_login_and_vram_page
from convert_net import extract_effect_span, PS4_COOK_PREFIX, PS3_COOK_MARK
import ps3geom
import texcodec as tc
SB_EXACT = os.environ.get('PS4ACTOR_SB_EXACT') == '1'
SB_DROP = os.environ.get('PS4ACTOR_SB_DROP') == '1'
FILL_STREAMS = os.environ.get('PS4ACTOR_FILL_STREAMS', '1') == '1'
SPREAD_MATS = os.environ.get('PS4ACTOR_SPREAD_MATS') == '1'   # see slot_of below

PAD32 = 0xaaaaaaaa


# ------------------------------------------------------------------ PS3 side
def ps3_block_materials(p, proto):
    """material name per block of a PS3 prototype.  proto+4 u16 = material count, +6 u16 =
    block count; proto+0x10 = material table {ptr matdesc, u16 first block, u16 nblocks,
    ptr, u32} (matdesc+0 -> material {name, ...})."""
    pg, po = proto
    nmat = p.u16(p.pbase(pg) + po + 4)
    nblk = p.u16(p.pbase(pg) + po + 6)
    t = p.resolve(pg, po + 0x10)
    out = [None] * nblk
    for s in range(nmat):
        e = t[1] + 0x10 * s
        sd = p.resolve(t[0], e)
        first, cnt = struct.unpack_from('>HH', p.d, p.pbase(t[0]) + e + 4)
        m = p.resolve(sd[0], sd[1])
        nm = p.pstr(*p.resolve(m[0], m[1]))
        for k in range(first, first + cnt):
            out[k] = nm
    assert None not in out, out
    return out


def read_tags(p):
    out = []
    for it in p.items:
        if it.type in ('TAG_FLOAT', 'TAG_INT', 'TAG_VEC4'):
            b = p.pbase(it.page) + it.data_off
            out.append((it.type, it.name, p.d[b:b + it.size]))
    return out


def ps3_artgroup(p):
    it = p.items_of('ARTGROUP_DATA')[0]
    pg, b = it.page, it.data_off
    n = p.pu32(pg, b)
    ents = p.resolve(pg, b + 8)[1]
    targets = []
    for k in range(n):
        r = p.resolve(pg, ents + 8 * k)
        tgt = p.item_at(*r) if r else None
        targets.append(tgt)
    hdr2 = p.resolve(pg, b + 0xc)
    x = p.resolve(hdr2[0], hdr2[1] + 0xc)
    nb = p.u32(p.pbase(x[0]) + x[1] + 0x10)
    return it.name, targets, nb


# ------------------------------------------------------------------ PS4 template
def tmpl_materials(q):
    g = q.items_of('GEOMETRY_1')[0]
    pg, b = g.page, g.data_off
    n = q.pu32(pg, b + 0x48)
    mp, mo = q.resolve(pg, b + 0x50)
    out = []
    for i in range(n):
        o = mo + i * 0x48
        S = lambda off: q.pstr(*q.resolve(mp, o + off))
        nf, npar, nb, x3 = struct.unpack_from('<4I', q.d, q.pbase(mp) + o + 0x30)
        tail = q.pu32(mp, o + 0x40)
        feats = []
        fr = q.resolve(mp, o + 0x10)
        for k in range(nf):
            feats.append(q.pstr(*q.resolve(fr[0], fr[1] + 8 * k)))
        params = []
        pr = q.resolve(mp, o + 0x20)
        for k in range(npar):
            po = pr[1] + 0x18 * k
            vr = q.resolve(pr[0], po + 8)
            params.append((q.pstr(*q.resolve(pr[0], po)), q.pu32(pr[0], po + 0x10),
                           q.d[q.pbase(vr[0]) + vr[1]:q.pbase(vr[0]) + vr[1] + 16]))
        binds = []
        br = q.resolve(mp, o + 0x28)
        for k in range(nb):
            bo = br[1] + 0x28 * k
            binds.append((q.pstr(*q.resolve(br[0], bo)), q.pstr(*q.resolve(br[0], bo + 8)),
                          q.pstr(*q.resolve(br[0], bo + 0x10))))
        out.append(dict(name=S(0), shader=S(8), hash=S(0x18), feats=feats, params=params,
                        binds=binds, x3=x3, tail=tail))
    return out


def tmpl_fg_info(q):
    """skeleton sid, hierarchy id and joint-name table of the template FG sections."""
    it = q.items_of('FOREGROUND_SECTION_2')[0]
    pg, b = it.page, it.data_off
    skel, hier = struct.unpack_from('<II', q.d, q.pbase(pg) + b + 8)
    jr = q.resolve(pg, b + 0x10)
    jitem = q.item_at(jr[0], jr[1] - 0x20)
    names = q.d[q.pbase(jr[0]) + jr[1]:q.pbase(jr[0]) + jr[1] + jitem.size]
    g = q.items_of('GEOMETRY_1')[0]
    sm = q.resolve(g.page, g.data_off + 0x10)[1]
    njoints = q.pu32(g.page, sm + 0x58)
    return skel, hier, names, njoints


# ------------------------------------------------------------------ helpers
TEX_KINDS = [  # (suffix in texture name, PS4 binding parameters that take it)
    ('-detail-normal', ('g_tNdFetchDetail3Map', 'g_tNdFetchDetail1Map', 'g_tNdFetchDetail2Map')),
    ('-spec-mask', ('g_tNdFetchMask4Map',)),
    ('-tint-mask', ('g_tNdFetchColorTintMask1Map',)),
    ('-normal', ('g_tNdFetchNormal1Map', 'g_tNdFetchNormal2Map')),
    ('-color', ('g_tNdFetchColor1Map', 'g_tNdFetchColor2Map')),
    ('-shift', ('g_tNdFetchNoiseMap',)),          # hair anisotropic shift
    ('-trans', ('g_tNdFetchHairAlphaMap',)),      # hair alpha (often missing on PS3: see donors)
]


def tex_kind(short):
    base = short.split('.tga')[0].split('.dds')[0]
    for suf, _ in TEX_KINDS:
        if base.endswith(suf) or suf + '-' in base:
            return suf
    return None


def bc1_flat(w, h, rgb):
    """BC1 texels (PS3 linear order, all mips) of a constant colour."""
    r, g, b = rgb
    c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    blk = struct.pack('<HHI', c, c, 0)
    out = bytearray()
    mips = 0
    while True:
        bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
        out += blk * (bw * bh)
        mips += 1
        if w == 1 and h == 1:
            break
        w, h = max(1, w // 2), max(1, h // 2)
    return bytes(out), mips


def f16(a):
    return np.asarray(a, dtype=np.float16)


def bindfull(e):
    """GEOMETRY_1 texture entries and material bindings name a texture by its full cooked
    path WITHOUT the '.ndb' extension (VRAM_DESC / TEXTURE_TABLE keep it)."""
    f = e['full']
    return f[:-4] if f.endswith('.ndb') else f


PAGE_LIMIT = 0x180000   # (margin: tables filled after item() grow past the estimate)
# package-mgr asserts pageEntry.m_pageSize < loading-heap page (2 MB)


class AutoPages:
    """Page.item()/span() look-alike that starts a new page when the current one fills up.
    Pointers between pages are fine (fixups carry the destination page)."""

    def __init__(self, w, max_entries=None):
        self.w = w
        self.cur = w.page()
        self.size = 0x200
        # page-entry cap of the current page: the effect span is pinned to the template's page-0
        # offset, so page 0 may not get a longer entry table than the room before that offset
        self.max_entries = max_entries
        self.nent = 0

    def _new_page(self):
        self.cur = self.w.page()
        self.size = 0x200
        self.max_entries = None
        self.nent = 0

    def _room(self, n, nent=0):
        if self.size + n > PAGE_LIMIT and self.size > 0x200:
            self._new_page()
        if self.max_entries is not None and self.nent + nent > self.max_entries:
            self._new_page()
        self.size += n
        self.nent += nent

    def item(self, type_, data=None, name='', listed=True):
        est = (len(data) if data is not None else 0) + 0x40 + len(type_) + len(name)
        if data is None or len(data) < 0x100:
            est += 0x400          # items filled in later (tables) stay small
        self._room((est + 15) & ~15, 1 if listed else 0)
        return self.cur.item(type_, data, name=name, listed=listed)

    def span(self, span):
        self._room(len(span.data) + 0x1000, len(span.entries))
        return self.cur.span(span)


# ------------------------------------------------------------------ conversion
GRAFT_TYPES = ('JOINT_HIERARCHY', 'ANIM', 'COLLISION_DATA_FOREGROUND')


def copy_item_blob(T, it):
    """Template item data as a Blob; its pointers must stay inside the item (true for
    JOINT_HIERARCHY / ANIM / COLLISION_DATA_FOREGROUND / SOUND_BANK_TABLE of retail props)."""
    b = Blob(it.type)
    base = T.pbase(it.page)
    data = bytearray(T.d[base + it.data_off:base + it.data_off + it.size])
    relocs = []
    for (pg, o), dp in T.ptrmap.items():
        if pg == it.page and it.data_off <= o < it.data_off + it.size:
            tgt = T.pptr(pg, o)
            if dp != it.page or not (it.data_off <= tgt < it.data_off + it.size):
                raise ValueError('%s %s points outside itself' % (it.type, it.name))
            relocs.append((o - it.data_off, tgt - it.data_off))
            struct.pack_into('<Q', data, o - it.data_off, 0)
    b.raw(bytes(data))
    for pos, toff in relocs:
        b.relocs.append((pos, b, toff))
    return b


def template_streams(q):
    """material name -> [(slot, stride, fmt bytes)] of the first template submesh using it"""
    g = q.items_of('GEOMETRY_1')[0]
    pg, b = g.page, g.data_off
    nsub = q.u64(q.pbase(pg) + b + 8)
    sr = q.resolve(pg, b + 0x10)
    out = {}
    for i in range(nsub):
        o = sr[1] + 0x88 * i
        mat = q.pstr(*q.resolve(sr[0], o + 0x48))
        ns = q.pu32(sr[0], o + 0x28)
        st = q.resolve(sr[0], o + 0x30)
        sl = []
        for k in range(ns):
            so = st[1] + 0x18 * k
            f = q.resolve(st[0], so + 8)
            fb = q.d[q.pbase(f[0]) + f[1]:q.pbase(f[0]) + f[1] + 4]
            sl.append((fb[0], q.pu32(st[0], so) >> 16, bytes(fb)))
        out.setdefault(mat, sl)
    return out


def convert(ps3_path, tmpl_path, out_path, rename=None, stream_flags=3, verbose=True,
            mat_choice=None, lod0_only=False, max_tex=0, graft=False, mat_by_file=False,
            skel_tmpl=None, match_streams=False, own_skel=False):
    """graft: copy JOINT_HIERARCHY / ANIM / COLLISION_DATA_FOREGROUND items from the template
    (props that carry their own skeleton; PS3 anims and Havok collision cannot be converted) for
    every PS3 artgroup target of the same type (ANIMs matched by name), with the PS3 skeleton id
    and names; SOUND_BANK_TABLE is rebuilt from the PS3 bank names.
    mat_by_file: map a PS3 material onto the template material of the same material file
    (dir/file) before falling back to the first one.
    skel_tmpl: take skeleton sid / hierarchy id / joint names from this pak instead of the template.
    own_skel: prop with its own (multi-joint) skeleton, converted from the PS3 data: JOINT_HIERARCHY
    (skel.py), ANIMs (anim.py), COLLISION_DATA_FOREGROUND (fgcoll.py); the template only supplies
    materials/effects.  COLLISION_DATA_BACKGROUND/EMPTY and SKELETON_FLIPDATA have no PS4 item.
    match_streams: give every submesh the vertex streams the template material's submesh has
    (missing vertex colour = white, missing uv/normal/tangent = constants), in template order."""
    src = Pak(ps3_path)
    T = Pak(tmpl_path)
    assert src.platform == 'ps3' and T.platform == 'ps4'
    art_old, ag_targets, ag_buckets = ps3_artgroup(src)
    art = rename or art_old
    ren = (lambda s: s.replace(art_old, art)) if rename else (lambda s: s)

    tmats = tmpl_materials(T)
    # constant-buffer table capacities of the template prototypes (u32 at +0x30/+0x34)
    cb3_cap, cb4_cap = 2, 1
    for it in T.items:
        if it.type == 'RAW_DATA' and it.size >= 0x38:
            o = T.pbase(it.page) + it.data_off
            if struct.unpack_from('<I', T.d, o)[0] == 3 and struct.unpack_from('<I', T.d, o + 12)[0] == 0x5791a421:
                a, b = struct.unpack_from('<II', T.d, o + 0x30)
                cb3_cap, cb4_cap = max(cb3_cap, a), max(cb4_cap, b)
    cb3_cap = max(cb3_cap, int(os.environ.get('PS4ACTOR_CB3_MIN', '4')))
    # retail material names (path -> 'path,GUID,shader') of every PS4 actor next to the template
    retail_names = {}
    tdir = os.path.dirname(os.path.abspath(tmpl_path))
    for fn in sorted(os.listdir(tdir)):
        if fn.endswith('.pak') and fn.startswith(('mp-', 'hero')):
            try:
                for m in tmpl_materials(Pak(os.path.join(tdir, fn))):
                    retail_names.setdefault(m['name'].split(',')[0], m['name'])
            except Exception:
                pass
    skel_sid, hier_id, joint_names, njoints = tmpl_fg_info(Pak(skel_tmpl) if skel_tmpl else T)
    tstreams = template_streams(T) if match_streams else {}
    # grafted skeleton: FG sections reference the PS3 skeleton id (patched into the copies)
    graft_items = []
    ps3_skel = None
    if graft:
        jh3 = src.items_of('JOINT_HIERARCHY')
        jh4 = T.items_of('JOINT_HIERARCHY')
        if jh3 and jh4:
            b3 = src.pbase(jh3[0].page) + jh3[0].data_off
            b4 = T.pbase(jh4[0].page) + jh4[0].data_off
            n3, n4 = src.u32(b3 + 4), struct.unpack_from('<I', T.d, b4 + 4)[0]
            assert n3 == n4, 'joint count PS3 %d != template %d' % (n3, n4)
            ps3_skel = src.u32(b3 + 0x1c)
            t_skel = struct.unpack_from('<I', T.d, b4 + 0x30)[0]
            assert src.u32(b3 + 0x24) == hier_id, 'hierarchy id differs from template'
            skel_sid = ps3_skel
    own_jh = None
    if own_skel:
        graft = True                     # same login/artgroup handling as grafted props
        import skel as _skel
        jh3 = src.items_of('JOINT_HIERARCHY')
        fg3 = src.items_of('FOREGROUND_SECTION_2')
        jsrc = src
        if not jh3 and os.environ.get('PS4ACTOR_SKEL3'):
            # LOD / part paks that are drawn on another actor's skeleton: take it from that PS3 pak
            jsrc = Pak(os.environ['PS4ACTOR_SKEL3'])
            jh3 = jsrc.items_of('JOINT_HIERARCHY')
        if jh3:
            own_jh = _skel.read_jh(jsrc, jh3[0])
            hier_id = struct.unpack_from('>I', own_jh['blob'], 4)[0]
            joint_names = struct.pack('<%dI' % own_jh['nj'], *[s_ for s_, _n in own_jh['names']])
            njoints = own_jh['nj']
        else:
            hier_id, joint_names, njoints = 0, b'', 0
        skel_sid = src.pu32(fg3[0].page, fg3[0].data_off + 8) if fg3 else 0
        ps3_skel = None
    sb_tmpl = [m for m in tmats if 'shadowblocker' in m['name']]
    main_tmpl = [m for m in tmats if 'shadowblocker' not in m['name']]

    # ---------------- decode geometry
    protos = ps3geom.prototypes(src)
    fg_items = src.items_of('FOREGROUND_SECTION_2')
    proto_list = []        # unique prototypes in PM_FG_PROTOTYPES order
    pm = src.items_of('PM_FG_PROTOTYPES')[0]
    for k in range(src.pu32(pm.page, pm.data_off)):
        proto_list.append(src.resolve(pm.page, pm.data_off + 4 + 4 * k))
    if lod0_only:
        fg_items = [it for it in fg_items if not re.search(r'-lod\d+$', it.name)]
        keep = [src.resolve(it.page, it.data_off) for it in fg_items]
        proto_list = [pr for pr in proto_list if pr in keep]
        ag_targets = [t for t in ag_targets if t is None or not re.search(r'-lod\d+$', t.name)]
        if len(ag_targets) <= 2:
            ag_buckets = 7
    pindex = {pr: i for i, pr in enumerate(proto_list)}
    proto_out = []
    used_mats = []
    for pi, pr in enumerate(proto_list):
        info = [v for v in protos.values() if v['proto'] == pr][0]
        mnames = ps3_block_materials(src, pr)
        blocks = [ps3geom.decode_block(src, pg, bo) for pg, bo in info['blocks']]
        # merge consecutive blocks with the same material
        subs = []
        for bi, (blk, mn) in enumerate(zip(blocks, mnames)):
            if subs and subs[-1]['mat'] == mn:
                subs[-1]['blocks'].append(blk)
            else:
                subs.append(dict(mat=mn, first=bi, blocks=[blk]))
        _drop = os.environ.get('PS4ACTOR_DROP_MAT')
        if _drop and any(_drop not in s['mat'] for s in subs):
            subs = [s for s in subs if _drop not in s['mat']]
        if SB_DROP and not SB_EXACT and any('shadowblocker' not in s['mat'] for s in subs):
            # PS3 shadow-blocker submeshes carry positions only; drawn with a lit PS4 material they
            # crash the renderer (missing streams), on the PS4 lib-shadowblocker material the package
            # fails to load. Drop them (the part only loses its shadow-casting proxy).
            subs = [s for s in subs if 'shadowblocker' not in s['mat']]
        for s in subs:
            if s['mat'] not in used_mats:
                used_mats.append(s['mat'])
        proto_out.append(subs)

    # ---------------- textures
    texs = [t for t in src.textures()]
    w = PakWriter()
    span, span_items = extract_effect_span(T)
    p0 = AutoPages(w, max_entries=max(1, (span_items[0].off - 0x14) // 16))
    # effects first, at the template's exact page offset: EFFECT data holds page-relative offsets
    # that are not pointer fixups; a span placed 0x1000 later (same phase) makes the renderer read
    # shader code as a resource pointer (fault at 0xe747e3 once the package is logged in)
    span.modulo = 1 << 40
    span.phase = span_items[0].off
    p0.span(span)
    vram = bytearray()
    tex_entries = []

    def add_texture(short, hash_, fmt4, w_, h_, mips, tiled, flags=None, unk20=None):
        nonlocal vram
        while len(vram) % 0x100:
            vram.append(0)
        voff = len(vram)
        vram += tiled
        nameb = Blob('texname')
        nameb.cstr(short)
        name_item = p0.item('RAW_DATA', nameb)
        full4 = PS4_COOK_PREFIX + short
        db = Blob('vramdesc')
        db.u64(0).u64(voff).u32(len(tiled)).u32(hash_)
        db.u32(1).u32(fmt4)
        db.u32(unk20 if unk20 is not None else (0x010f0000 if fmt4 in (0x48, 0x4b, 0x4e) else 0x01000000))
        db.u32(mips).u32(w_).u32(h_).u32(0).u32(stream_flags if flags is None else flags)
        db.ptr(db, 0x40)
        db.cstr(full4)
        desc_item = p0.item('VRAM_DESC', db)
        e = dict(hash=hash_, short=short, full=full4, desc=desc_item, name=name_item, fmt=fmt4,
                 kind=tex_kind(short))
        tex_entries.append(e)
        return e

    ps3tex = []
    for t in texs:
        full3 = t['path']
        short = full3.split(PS3_COOK_MARK, 1)[1] if PS3_COOK_MARK in full3 else full3
        fmt3 = tc.ps3_format(t)
        if fmt3 is None or not fmt3[2]:
            if verbose:
                print('  skip texture %s (format %#x)' % (short, t['gcmRemapCtl']))
            continue
        kind, bpb, comp = fmt3
        texels = src.vram_bytes(t['vramOffset'], t['vramSize'])
        tw, th, tm = t['width'], t['height'], t['mips']
        # PS4 kit parts keep only small resident mips (the rest streams); a PS3 texture is fully
        # resident, so drop its top mips down to max_tex or the part fails to load (level state 12)
        while max_tex and max(tw, th) > max_tex and tm > 1:
            bw, bh = tc.blocks(tw, th, True)
            texels = texels[bw * bh * bpb:]
            tw, th, tm = max(1, tw >> 1), max(1, th >> 1), tm - 1
        tiled = tc.ps3_to_ps4_texels(texels, tw, th, tm, bpb)
        k = tex_kind(short)
        if kind == 'DXT1':
            fmt4 = 0x48 if k == '-color' else 0x47
        elif kind == 'DXT5':
            fmt4 = 0x4e if k == '-color' else 0x4d
        elif kind == 'DXT3':
            fmt4 = 0x4b if k == '-color' else 0x4a
        else:
            raise NotImplementedError(kind)
        ps3tex.append(dict(short=short, hash=t['hash'], kind=k, full=PS4_COOK_PREFIX + short,
                           lazy=(short, t['hash'], fmt4, tw, th, tm, tiled)))

    def use(e):
        """create the VRAM_DESC for a PS3 texture the first time a material binds it"""
        if e is None or 'lazy' not in e:
            return e
        if 'real' not in e:
            e['real'] = add_texture(*e['lazy'])
        return e['real']

    flat_cache = {}

    def flat_texture(kind):
        if kind in flat_cache:
            return flat_cache[kind]
        rgb = {'-spec-mask': (40, 40, 40), '-normal': (128, 128, 255), '-color': (128, 128, 128),
               '-tint-mask': (0, 0, 0), '-detail-normal': (128, 128, 255)}[kind]
        lin, mips = bc1_flat(64, 64, rgb)
        tiled = tc.ps3_to_ps4_texels(lin, 64, 64, mips, 8)
        short = 'art/generated/u3mp/flat%s.tga/%016X.ndb' % (kind, sid(kind))
        e = add_texture(short, sid(short), 0x47, 64, 64, mips, tiled)
        flat_cache[kind] = e
        return e

    ttex = {t['path']: t for t in T.textures()}
    tcache = {}

    # donor paks (PS4ACTOR_DONORS, ':'-separated retail PS4 paks): textures the PS3 pak lacks (its
    # cooked '-trans' hair alpha is usually a missingtexture) are copied from the retail pak that
    # has the same texture, instead of the template's unrelated one
    donors = []
    for dp in filter(None, os.environ.get('PS4ACTOR_DONORS', '').split(':')):
        try:
            D = Pak(dp)
            donors.append((D, {t['path'].split('texture2/', 1)[-1].split('.tga')[0]: t for t in D.textures()}))
        except Exception:
            pass

    def donor_texture(short_noext):
        for D, idx in donors:
            t = idx.get(short_noext)
            if t is not None:
                return tmpl_texture(t['path'], D, t)
        return None

    def tmpl_texture(tfull, src_pak=None, t=None, force=False):
        """copy a texture of the PS4 template (or a donor pak) verbatim (low mips + streaming flag/hash).
        force: copy even with PS4ACTOR_NO_TMPL_TEX (bindings without a PS3 kind: env maps, overlays)"""
        if os.environ.get('PS4ACTOR_NO_TMPL_TEX') == '1' and not force:
            return None
        if tfull in tcache:
            return tcache[tfull]
        P = src_pak or T
        if t is None:
            t = ttex.get(tfull) or ttex.get(tfull + '.ndb')
        if t is not None:
            tfull = t['path']
        if t is None:
            tcache[tfull] = None
            return None
        short = tfull.split(PS4_COOK_PREFIX, 1)[1] if PS4_COOK_PREFIX in tfull else tfull
        e = add_texture(short, t['hash'], t['format'], t['width'], t['height'], t['mips'],
                        P.vram_bytes(t['vramOffset'], t['vramSize']), flags=t['unk34'], unk20=t['unk20'])
        e['full'] = tfull
        tcache[tfull] = e
        return e

    # texture table
    ttab = p0.item('TEXTURE_TABLE')

    # ---------------- materials (template copies, re-bound)
    def pick_template(mname):
        key = mname.rsplit('/', 1)[0] + ':' + mname.rsplit('/', 1)[1]
        exact = [m for m in tmats if m['name'].split(',')[0] == key]
        if exact:
            return exact[0]
        if 'shadowblocker' in mname and sb_tmpl:
            return sb_tmpl[0]
        if mat_choice and mname in mat_choice:
            return [m for m in tmats if mat_choice[mname] in m['name']][0]
        if mat_by_file:
            same = [m for m in main_tmpl if m['name'].split(':')[0] == mname.rsplit('/', 1)[0]]
            if same:
                return same[0]
        if SPREAD_MATS:
            # hair cards need the hair (alpha) shader, hats/masks/goggles must not get it
            hair = [m for m in main_tmpl if 'GLOBAL_HAIR' in m['feats']]
            other = [m for m in main_tmpl if 'GLOBAL_HAIR' not in m['feats']]
            if 'hair' in mname.rsplit('/', 2)[-2].lower():
                if hair:
                    return hair[0]
            elif other:
                return other[0]
        return main_tmpl[0]

    def textures_for(mname):
        """PS3 textures that belong to material mname (name heuristics)."""
        stem = mname.rsplit('/', 1)[0].rsplit('/', 1)[-1]          # .../<file>/<mat>
        cands = ps3tex
        own = [e for e in cands if stem in e['short']]
        if not own:
            toks = set(re.split(r'[-/]', stem)) - {'mp', 'a', 'b', 'c', '00', '01'}
            scored = sorted(cands, key=lambda e: -len(toks & set(re.split(r'[-/.]', e['short']))))
            own = scored[:4]
        return own

    # One material per template material, in the template's order: the copied EFFECT items belong
    # to the template materials by position (one effect per material), so a different count or order
    # pairs materials with the wrong effect (load failure, level state 12, or a renderer fault).
    slot_of = {}
    for mn in used_mats:
        tm = pick_template(mn)
        slot_of[mn] = next(i for i, m in enumerate(tmats) if m is tm)
    if SPREAD_MATS:
        # PS3 materials that fell back onto a slot another PS3 material already uses would share
        # its textures; give them a free (unused, non-shadowblocker) template slot instead
        taken = set()
        free = [i for i, m in enumerate(tmats) if 'shadowblocker' not in m['name']
                and i not in set(slot_of.values())]
        for mn in used_mats:
            if slot_of[mn] in taken and free and 'shadowblocker' not in mn:
                slot_of[mn] = free.pop(0)
            taken.add(slot_of[mn])
    by_path = {x['short'].rsplit('/', 1)[0]: x for x in ps3tex}

    def binds_for(mn, tm):
        own = textures_for(mn) if mn else []
        exact = bool(mn) and tm['name'].split(',')[0] == mn.rsplit('/', 1)[0] + ':' + mn.rsplit('/', 1)[1]
        binds = []
        for pname, tshort, tfull in tm['binds']:
            want = [suf for suf, ps in TEX_KINDS if pname in ps]
            e = None
            if exact or not mn:
                # same material on PS4 (or a template material no PS3 mesh uses): its texture
                e = tmpl_texture(tfull) or by_path.get(tshort)
            if e is not None:
                pass
            elif tshort in by_path:
                e = by_path[tshort]
            elif want:
                c = [x for x in own if x['kind'] == want[0]]
                if not c and donors:
                    col = [x for x in own if x['kind'] == '-color']
                    if col:
                        stem = col[0]['short'].split('.tga')[0]
                        e = donor_texture(stem[:stem.rfind('-color')] + want[0]) if '-color' in stem else None
                e = c[0] if c else (e or tmpl_texture(tfull) or flat_texture(want[0]))
            else:
                # environment maps / cornea / hair noise etc: copy the template's texture
                e = tmpl_texture(tfull, force=True)
            binds.append((pname, use(e), tshort, tfull))
        return binds

    mat_recs = []
    for ti, tm in enumerate(tmats):
        users = [mn for mn in used_mats if slot_of[mn] == ti]
        # several PS3 materials on one template slot: name/textures follow the exact match if any
        users.sort(key=lambda u: tm['name'].split(',')[0] != u.rsplit('/', 1)[0] + ':' + u.rsplit('/', 1)[1])
        mn = users[0] if users else None
        binds = binds_for(mn, tm)
        tpath = tm['name'].split(',')[0]
        upath = (mn.rsplit('/', 1)[0] + ':' + mn.rsplit('/', 1)[1]) if mn else tpath
        if upath in retail_names and upath != tpath:
            ps4name = retail_names[upath].rsplit(',', 1)[0] + ',' + tm['shader']
        elif upath == tpath or 'lib-shadowblocker' in tpath or not mn:
            # retail material GUIDs identify the material asset (same path -> same GUID in every
            # pak; e.g. one shared lib-shadowblocker instance). Reuse it for the same material.
            ps4name = tm['name']
        else:
            hx = hashlib.md5(upath.encode()).hexdigest()
            guid = '%s-%s-%s-%s-%s' % (hx[:8], hx[8:12], hx[12:16], hx[16:20], hx[20:32])
            ps4name = '%s,%s,%s' % (upath, guid, tm['shader'])
        mat_recs.append(dict(ps3=users, name=ps4name, tm=tm, binds=binds))
        if verbose:
            print('  material %d %-50s <- %s  [%s]' % (ti, tpath, ', '.join(users) or '(unused)',
                  ', '.join('%s=%s' % (b[0][9:], (b[1]['short'].rsplit('/', 1)[-1] if b[1] else 'tmpl'))
                            for b in binds)))

    # texture table body (all textures)
    order = sorted(tex_entries, key=lambda e: e['hash'])
    namesb = Blob('ttnames')
    for e in order:
        namesb.ptr(e['name'].data)
    names_item = p0.item('RAW_DATA', namesb)
    hashb = Blob('tthash')
    for e in order:
        hashb.u32(e['hash'])
    hash_item = p0.item('RAW_DATA', hashb)
    tb = ttab.data
    tb.u64(len(order)).ptr(hash_item.data).ptr(names_item.data)
    tb.ptr(tb, 0x20)
    for e in order:
        tb.ptr(e['desc'].data)

    # ---------------- effects (verbatim from template)

    # ---------------- geometry buffers
    strs = Blob('geostrings')
    spool = {}

    def S(s):
        if s not in spool:
            spool[s] = strs.cstr(s)
        return spool[s]

    def raw(data, name=''):
        b = Blob('raw')
        b.raw(bytes(data))
        return p0.item('RAW_DATA', b, name=name).data

    fmtb = Blob('vtxfmt')
    FMT = {0: (0, 0, 0x0c, 0x06), 1: (1, 0, 0x04, 0x22), 2: (2, 0, 0x08, 0x0a), 3: (3, 0, 0x08, 0x0a),
           4: (4, 0, 0x08, 0x0a)}
    fmt_off = {}
    for slot, f in FMT.items():
        fmt_off[slot] = fmtb.pos
        fmtb.raw(bytes(f))
    fmt_item = p0.item('RAW_DATA', fmtb)

    subs_blob = Blob('submeshes')
    sub_records = []
    proto_records = []
    for pi, subs in enumerate(proto_out):
        pname = 'Prototype%d.%s' % (pi, art)
        names = []
        for s in subs:
            nv = sum(b['nverts'] for b in s['blocks'])
            pos = np.concatenate([b['pos'] for b in s['blocks']]).astype('<f4')
            tris = []
            base = 0
            for b in s['blocks']:
                tris += [(a + base, c + base, d + base) for a, c, d in b['tris']]
                base += b['nverts']
            idx = np.array(tris, dtype='<u2').reshape(-1)
            streams = [(0, 12, pos.tobytes())]
            if all('uv0' in b for b in s['blocks']):
                uv = np.concatenate([b['uv0'] for b in s['blocks']])
                streams.append((1, 4, f16(uv).astype('<f2').tobytes()))
            elif FILL_STREAMS and 'shadowblocker' not in s['mat']:
                # PS3 shadow-blocker blocks carry positions only; the PS4 lit material reads uv,
                # normal and tangent streams, and a draw without them faults in the renderer
                streams.append((1, 4, np.zeros((nv, 2), '<f2').tobytes()))
            if all('normal' in b for b in s['blocks']):
                n = np.concatenate([b['normal'] for b in s['blocks']])
                n4 = np.concatenate([n, np.ones((nv, 1))], 1)
                streams.append((2, 8, f16(n4).astype('<f2').tobytes()))
            elif FILL_STREAMS and 'shadowblocker' not in s['mat']:
                n4 = np.tile(np.array([0, 0, 1, 1], '<f2'), (nv, 1))
                streams.append((2, 8, n4.tobytes()))
            if all('tangent' in b for b in s['blocks']):
                tg = np.concatenate([b['tangent'] for b in s['blocks']])
                if tg.shape[1] == 3:
                    tg = np.concatenate([tg, np.ones((nv, 1))], 1)
                streams.append((3, 8, f16(tg).astype('<f2').tobytes()))
            elif FILL_STREAMS and 'shadowblocker' not in s['mat']:
                t4 = np.tile(np.array([1, 0, 0, 1], '<f2'), (nv, 1))
                streams.append((3, 8, t4.tobytes()))
            if match_streams:
                want = tstreams.get(mat_recs[slot_of[s['mat']]]['tm']['name'])
                if want:
                    have = {sl: (sl, st_, d_) for sl, st_, d_ in streams}
                    fill = {1: lambda: np.zeros((nv, 2), '<f2').tobytes(),
                            2: lambda: np.tile(np.array([0, 0, 1, 1], '<f2'), (nv, 1)).tobytes(),
                            3: lambda: np.tile(np.array([1, 0, 0, 1], '<f2'), (nv, 1)).tobytes(),
                            4: lambda: np.ones((nv, 4), '<f2').tobytes()}
                    new = []
                    for sl, stride, _fb in want:
                        if sl in have:
                            new.append(have.pop(sl))
                        elif sl in fill:
                            new.append((sl, stride, fill[sl]()))
                        else:
                            raise NotImplementedError('template stream slot %d' % sl)
                    streams = new + list(have.values())
            # skin
            skin = None
            if all('skin' in b for b in s['blocks']):
                infl = [x for b in s['blocks'] for x in b['skin']]
                counts = np.array([len(x) for x in infl], dtype='<u2')
                offs = np.concatenate([[0], np.cumsum(counts)[:-1]]).astype('<u2')
                joints = np.array([j for x in infl for j, _w in x], dtype='<u2')
                wts = np.array([wv / 255.0 for x in infl for _j, wv in x], dtype='<f4')
                skin = dict(total=int(counts.sum()), counts=raw(counts.tobytes()), offs=raw(offs.tobytes()),
                            joints=raw(joints.tobytes()), wts=raw(wts.tobytes()))
            ib = raw(idx.tobytes())
            st = Blob('streams')
            for slot, stride, data in streams:
                db = raw(data)
                st.u32((stride << 16) | 1).u32(len(data)).ptr(fmtb, fmt_off[slot]).ptr(db)
            st_item = p0.item('RAW_DATA', st)
            sk_item = None
            if skin:
                skb = Blob('skin')
                skb.u32(0).u32(skin['total']).ptr(skin['counts']).ptr(skin['offs']).ptr(skin['joints']).ptr(skin['wts'])
                sk_item = p0.item('RAW_DATA', skb)
            mrec = mat_recs[slot_of[s['mat']]]
            sname = '%s.Prototype%d.Submesh%d' % (art, pi, s['first'])
            names.append(sname)
            sb = 'shadowblocker' in s['mat']
            last_lod = (pi == len(proto_out) - 1)
            flags = 0x01000100 if sb else (0 if last_lod else 0x00010000)
            sub_records.append(dict(name=sname, nv=nv, ni=len(idx), ns=len(streams), st=st_item, ib=ib,
                                    mat=mrec['name'], skin=sk_item, flags=flags, sb=sb))
        proto_records.append(dict(name=pname, subnames=names))

    for r in sub_records:
        b = subs_blob
        b.u64(0).ptr(strs, S(r['name'])).u32(1).u32(r['flags']).u32(1 if r['sb'] else 0).u32(0)
        b.u32(r['nv']).u32(r['ni']).u32(r['ns']).u32(0).ptr(r['st'].data).u64(0).ptr(r['ib'])
        b.ptr(strs, S(r['mat'])).i32(-1).u32(0).u32(njoints).u32(0)
        b.ptr(r['skin'].data if r['skin'] else None)
        b.raw(b'\0' * 0x20)
    subs_item = p0.item('RAW_DATA', subs_blob)

    # prototypes
    protob = Blob('prototypes')
    plist = Blob('protolist')
    flagsb = Blob('protoflags')
    snl = Blob('subnames')
    for pr in proto_records:
        foff = flagsb.pos
        flagsb.raw(bytes([1, 0, 1, 1, 0, 0, 1, 1]) + b'\0' * 8)
        soff = snl.pos
        for n in pr['subnames']:
            snl.ptr(strs, S(n)).i32(-1).u32(0)
        pr['off'] = protob.pos
        protob.u32(3).u32(len(pr['subnames'])).u32(0).u32(0x5791a421)
        protob.ptr(strs, S(pr['name'])).ptr(snl, soff).ptr(flagsb, foff).u64(0)
        # +0x30: capacity of the instance's scope-3 (per-instance) constant buffer table. The game
        # allocates exactly this many slots (0xe86b60) and registers every distinct scope-3 cbuffer of
        # the prototype's materials without a bounds check (0xe86f10), so a short count overflows into
        # the entry array (resource pointer = a cbuffer key -> faults in Material::Apply).
        protob.u32(cb3_cap).u32(cb4_cap)
        protob.raw(b'\0' * (0x78 - 0x38))
    for pr in proto_records:
        plist.ptr(protob, pr['off'])
    for blob in (protob, flagsb, snl, plist):
        p0.item('RAW_DATA', blob)

    # textures referenced by materials (GEOMETRY_1 texture entries)
    texb = Blob('geotextures')
    gtex = []
    for m in mat_recs:
        for _p, e, _s, _f in m['binds']:
            if e is not None and e not in gtex:
                gtex.append(e)
    for e in gtex:
        texb.ptr(strs, S(bindfull(e))).u32(e['hash']).u32(0x200).u32(0x101).i32(-1).u32(0).u32(4).u64(0)

    # materials
    featb = Blob('features')
    parb = Blob('params')
    valb = Blob('paramvalues')
    bindb = Blob('bindings')
    matb = Blob('materials')
    for m in mat_recs:
        tm = m['tm']
        foff = featb.pos
        for f in tm['feats']:
            featb.ptr(strs, S(f))
        poff = parb.pos
        for nm, cnt, vals in tm['params']:
            voff = valb.pos
            valb.raw(vals)
            parb.ptr(strs, S(nm)).ptr(valb, voff).u32(cnt).u32(0)
        boff = bindb.pos
        for pname, e, tshort, tfull in m['binds']:
            if e is not None:
                short = e['short'].rsplit('/', 1)[0] if e['short'].endswith('.ndb') else e['short']
                full = bindfull(e)
            else:
                short, full = tshort, tfull
            bindb.ptr(strs, S(pname)).ptr(strs, S(short)).ptr(strs, S(full)).i32(-1).u32(0).u64(0)
        matb.ptr(strs, S(m['name'])).ptr(strs, S(tm['shader']))
        matb.ptr(featb if tm['feats'] else None, foff).ptr(strs, S(tm['hash']))
        matb.ptr(parb if tm['params'] else None, poff).ptr(bindb if m['binds'] else None, boff)
        matb.u32(len(tm['feats'])).u32(len(tm['params'])).u32(len(m['binds'])).u32(tm['x3'])
        matb.u32(tm['tail']).u32(0)
    for blob in (featb, parb, valb, bindb, matb, texb):
        if len(blob):
            p0.item('RAW_DATA', blob)
    p0.item('RAW_DATA', strs)

    gb = Blob('geometry')
    gb.u32(0x10).u32(1)
    gb.u64(len(sub_records)).ptr(subs_blob)
    gb.u64(len(proto_records)).ptr(plist)
    gb.u64(0).ptr(None)
    gb.u64(len(gtex)).ptr(texb if gtex else None)
    gb.u64(len(mat_recs)).ptr(matb)
    gb.u64(0).u64(0)
    geo = p0.item('GEOMETRY_1', gb)

    # ---------------- FOREGROUND_SECTION_2 (+ joint names)
    fg_out = []
    for it in fg_items:
        pg, b = it.page, it.data_off
        pr = src.resolve(pg, b)
        lod = struct.unpack_from('>f', src.d, src.pbase(pg) + b + 0xc)[0]
        pi = pindex[pr]
        jn = raw(joint_names, name='debugMeshJointNameLoc')
        fb = Blob('fg')
        fb.ptr(fb, 0x1c).u32(skel_sid).u32(hier_id).ptr(jn)
        fb.raw(struct.pack('<f', lod))
        assert fb.pos == 0x1c
        fb.cstr(proto_records[pi]['name'])
        fg_out.append(p0.item('FOREGROUND_SECTION_2', fb, name=ren(it.name)))

    # ---------------- tags
    tag_items = []
    tags = read_tags(src)
    if lod0_only:
        seen = set()
        tags = [t for t in tags if t[0] != 'TAG_FLOAT' and not (t[:2] in seen or seen.add(t[:2]))]
    for ty, nm, data in tags:
        tb_ = Blob('tag')
        if ty == 'TAG_VEC4':
            tb_.raw(struct.pack('<4f', *struct.unpack('>4f', data)))
        elif ty == 'TAG_FLOAT':
            tb_.raw(struct.pack('<f', *struct.unpack('>f', data)))
        else:
            tb_.raw(struct.pack('<i', *struct.unpack('>i', data)))
        tag_items.append(p0.item(ty, tb_, name=nm))

    # ---------------- grafted template items (skeleton, anims, collision) and sound banks
    graft_by_type = {}
    if own_skel:
        import anim as _anim
        import fgcoll as _fg
        for tgt in ag_targets:
            if tgt is None:
                continue
            if tgt.type == 'JOINT_HIERARCHY' and tgt in src.items:
                blob = _skel.build_jh(own_jh)
            elif tgt.type == 'ANIM':
                a_ = _anim.read_ps3_anim(src, tgt)
                if a_['clip'] is not None and _anim.is_streamed(a_['clip']):
                    raise NotImplementedError('streamed clip %s' % tgt.name)
                blob = _anim.build_ps4_anim(a_)
            elif tgt.type == 'COLLISION_DATA_FOREGROUND':
                blob = _fg.fg_blob(src, tgt)
            else:
                continue
            gi = p0.item(tgt.type, blob, name=ren(tgt.name))
            graft_by_type.setdefault(tgt.type, []).append(gi)
            graft_items.append(gi)
            if verbose:
                print('  converted %s %r (%d bytes)' % (tgt.type, ren(tgt.name), len(blob.b)))
    if graft and not own_skel:
        t_by = {}
        for it in T.items:
            if it.type in GRAFT_TYPES:
                t_by.setdefault(it.type, []).append(it)
        for tgt in ag_targets:
            if tgt is None or tgt.type not in GRAFT_TYPES:
                continue
            cands = t_by.get(tgt.type, [])
            if tgt.type == 'ANIM':
                cands = [c for c in cands if c.name == tgt.name]
            if not cands:
                raise ValueError('template has no %s %r to graft' % (tgt.type, tgt.name))
            blob = copy_item_blob(T, cands[0])
            if ps3_skel is not None:
                off = {'JOINT_HIERARCHY': 0x30, 'ANIM': 0x20}.get(tgt.type)
                if off is not None:
                    assert struct.unpack_from('<I', blob.b, off)[0] == t_skel
                    struct.pack_into('<I', blob.b, off, ps3_skel)
            gi = p0.item(tgt.type, blob, name=ren(tgt.name))
            graft_by_type.setdefault(tgt.type, []).append(gi)
            graft_items.append(gi)
            if verbose:
                print('  graft %s %r <- template %r' % (tgt.type, ren(tgt.name), cands[0].name))
    if graft:
        sbt = src.items_of('SOUND_BANK_TABLE')
        if sbt:
            it = sbt[0]
            banks = [src.pstr(*src.resolve(it.page, o)) for o in range(it.data_off, it.data_off + it.size, 4)
                     if src.resolve(it.page, o)]
            sb = Blob('soundbanks')
            sb.u32(len(banks)).u32(PAD32)
            strs_ = Blob('banknames')
            offs = [strs_.cstr(n) for n in banks]
            for o in offs:
                sb.ptr(sb, 8 + 8 * len(banks) + o)
            sb.raw(bytes(strs_.b))
            gi = p0.item('SOUND_BANK_TABLE', sb)
            graft_by_type['SOUND_BANK_TABLE'] = [gi]
            if verbose:
                print('  sound banks %s' % banks)

    # ---------------- artgroup
    keys = []
    for tgt in ag_targets:
        if tgt is None:
            keys.append(None)
        elif tgt.type == 'FOREGROUND_SECTION_2':
            keys.append(sid('geo.' + ren(tgt.name)))
        else:
            keys.append(sid(ren(tgt.name)))
    nb = ag_buckets
    ab = Blob('artgroup')
    n = len(keys)
    ab.u32(n).u32(1).ptr(ab, 0x50).ptr(ab, 0x30).u64(0).u64(0).u32(PAD32).u32(PAD32)
    tbl_off = 0x50 + 16 * n
    x_off = tbl_off + 16 * nb
    ab.u64(0).u32(1).u32(0x12345).ptr(ab, x_off).u32(PAD32).u32(PAD32)
    assert ab.pos == 0x50
    # entries point at the item headers of the skeleton / collision / anims (retail: FG entry null)
    by_tn = {(gi.type, gi.name): gi for gi in graft_items}
    for tgt in ag_targets:
        gi = by_tn.get((tgt.type, ren(tgt.name))) if tgt is not None else None
        if gi is not None:
            ab.ptr(gi.data, -0x20).u64(0)
        else:
            ab.u64(0).u64(0)
    buckets = [None] * nb
    for k, key in enumerate(keys):
        if key is None:
            continue
        bi = key % nb
        while buckets[bi] is not None:
            bi = (bi + 1) % nb
        buckets[bi] = (key, k)
    for bk in buckets:
        if bk is None:
            ab.u32(0).u32(PAD32).u64(0)
        else:
            ab.u32(bk[0]).u32(PAD32).ptr(ab, 0x50 + 16 * bk[1])
    ab.u32(0).u32(PAD32).u64(0).u64(0).u32(0).u32(nb).ptr(ab, tbl_off)
    artgroup = p0.item('ARTGROUP_DATA', ab, name=art)

    lp = Blob('levelpats')
    lp.u64(0)
    levelpats = p0.item('LEVEL_PATS_1', lp)

    pl = Blob('pkglookup')
    pl.ptr(pl, 0x20).u32(0).u32(1).ptr(pl, 0x20 + ((len(art) + 1 + 15) // 16) * 16).u32(PAD32).u32(PAD32)
    pl.cstr(art)
    pl.pad(16, 0xaa)
    pl.u32(sid(art)).u32(PAD32).ptr(artgroup.data, -0x20)
    pkglookup = p0.item('PACKAGE_ITEM_LOOKUP', pl)

    # ---------------- page 1 + login
    p1 = w.page()
    make_login_and_vram_page(w, p1, [None] * 3, [e['desc'] for e in tex_entries])
    w.vram = bytes(vram)

    class SpanItem:
        def __init__(self, span, pos):
            self.span, self.pos = span, pos

        @property
        def page(self):
            return self.span.page

        @property
        def off(self):
            return self.span.off + self.pos
    et_pos = [i.off for i in span_items if i.type == 'EFFECT_TABLE'][0] - span_items[0].off
    if graft:
        # follow the template's login order (retail props: LEVEL_PATS, SOUND_BANK_TABLE, ARTGROUP,
        # EFFECT_TABLE, TEXTURE_TABLE, GEOMETRY_1, JOINT_HIERARCHY, ANIM*, FG*, COLLISION, LOOKUP, TAG*)
        by = dict(graft_by_type)
        by.update({'ARTGROUP_DATA': [artgroup], 'LEVEL_PATS_1': [levelpats],
                   'EFFECT_TABLE': [SpanItem(span, et_pos)], 'TEXTURE_TABLE': [ttab], 'GEOMETRY_1': [geo],
                   'FOREGROUND_SECTION_2': fg_out, 'PACKAGE_ITEM_LOOKUP': [pkglookup]})
        order, used = [], set()
        tags_done = False
        for pg_, off_ in T.login['entries']:
            ty = T.item_at(pg_, off_).type
            if ty.startswith('TAG_'):
                if not tags_done:
                    order += tag_items
                    tags_done = True
                continue
            if ty not in used:
                used.add(ty)
                order += by.get(ty, [])
        for ty, its in by.items():
            if ty not in used:
                order += its
        if not tags_done:
            order += tag_items
        w.login_targets = order
    else:
        w.login_targets = [artgroup, levelpats, SpanItem(span, et_pos), ttab, geo] + fg_out + [pkglookup] + tag_items
    w.login_item.data.b[:] = struct.pack('<II', len(w.login_targets), 0) + b'\0' * (8 * len(w.login_targets))
    data = w.build()
    open(out_path, 'wb').write(data)
    if verbose:
        print('wrote %s: %d bytes, %d textures, %d materials, %d submeshes, %d prototypes, vram %#x' % (
            out_path, len(data), len(tex_entries), len(mat_recs), len(sub_records), len(proto_records), len(vram)))
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ps3')
    ap.add_argument('template')
    ap.add_argument('out')
    ap.add_argument('--rename')
    ap.add_argument('--stream-flags', type=int, default=3)
    ap.add_argument('--lod0-only', action='store_true')
    ap.add_argument('--max-tex', type=int, default=0, help='drop top mips of PS3 textures above this size')
    ap.add_argument('--graft', action='store_true', help='prop with its own skeleton: copy template skeleton/anims/collision')
    ap.add_argument('--mat-by-file', action='store_true')
    ap.add_argument('--match-streams', action='store_true')
    ap.add_argument('--skel-template')
    ap.add_argument('--own-skel', action='store_true', help='prop with its own skeleton: convert PS3 skeleton/anims/collision')
    a = ap.parse_args()
    convert(a.ps3, a.template, a.out, rename=a.rename, stream_flags=a.stream_flags, lod0_only=a.lod0_only, max_tex=a.max_tex,
            graft=a.graft, mat_by_file=a.mat_by_file, skel_tmpl=a.skel_template, match_streams=a.match_streams,
            own_skel=a.own_skel)


if __name__ == '__main__':
    main()
