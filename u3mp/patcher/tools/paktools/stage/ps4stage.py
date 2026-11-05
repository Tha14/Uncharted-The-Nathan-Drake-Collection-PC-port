#!/usr/bin/env python3
"""PS3 -> PS4 converter for Uncharted 3 background stage levels (the multiplayer lobby
backdrops: <map>-multi-stage, ldn-train-stage, ...).

Structure was derived from the two stages that still ship on PS4 (san-upper-multi-stage and
ldn-train-stage, rosetta pairs).  The PS3 PM_COLLECTION_2 / BACKGROUND_SECTION /
MATERIAL_TABLE_1 / INSTANCE_TAGS_1 / SPHERE_TREE_NODE_1 / NAME_TABLE_1 items have no PS4
counterpart: the PS4 build keeps everything in one background GEOMETRY_1 (v0x10, flags 0):

  GEOMETRY_1  +0x08 n/ptr submeshes (0x88)  +0x18 n/ptr prototype*  +0x28 n/ptr instance*
              +0x38 n/ptr textures (0x28)   +0x48 n/ptr materials (0x48)
  submesh     one per run of <= 20 PS3 blocks of one material group; name
              '<stage>.Prototype<p>.Submesh<first block>'; streams = the PS3 block attributes
              in PS3 format order (pos f32x3, uv0/uv1 f16x2, colour/normal/tangent f16x4);
              streams2 = defaults of the per-instance attributes (8 ilight f16x4, 5 lightmap
              uv f16x2, 255 iconst f32x3); +0x10 flags 1 | 0x100 (PS3 group flag 0x80);
              +0x58 joint count / +0x60 skin for skinned prototypes
  prototype   0x78 {3, nsub, 0, hash, name 'Prototype<p>.<stage>', subname table, flags[16],
              0, cb3 capacity, cb4 capacity}
  instance    0xc0 {3, id, proto name, -1, 0, 0x01010101, 1, nsub, njointmats, 0,
              instsub*, jointmats*, ..., world AABB min/max (+0x60/+0x70), matrix (+0x80)}
  instsub     0x50 {submesh name, -1, 0, nstreams, 0, streams*, flags, 0, 0x00ff0000, -1,
              world AABB min, max} - per-instance ilight / lightmap uv / iconst streams

Materials/effects/textures are taken from existing PS4 paks (see mkplan.py, fxextract.py);
EFFECT string indices are remapped into the new pak's string table (fxremap.py).

usage: ps4stage.py <ps3 stage.pak> <plan.json> <bundle.json> <out.pak>
"""
import argparse, csv, hashlib, json, os, re, struct, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
from ndpak import Pak, sid
from ps4writer import PakWriter, Blob, make_login_and_vram_page
import texcodec as tc
import ps3geom
from bggeom import read_format, decode_attr, collection, instance, gen_tangents, block_prec
from fxremap import remap
from mkplan import read_materials, tex_role, tex_role_final, param_role, short_of

PS4_PREFIX = 'd:/dev/CP9/Data/U3/CookData/ND/build/texture2/'
RES = '/mnt/user-data/uploads/Uncharted-windows/_claude_transfer/stages'
PAGE_LIMIT = 0x1c0000
PAD32 = 0xaaaaaaaa
MAX_BLOCKS = 20                       # PS4 splits a PS3 material group every 20 blocks
MAX_TEX = int(os.environ.get('U3_STAGE_MAX_TEX', '0')) or (1 << 30)   # optional cap for PS3-only textures; default: full PS3 resolution

# PS3 attribute id -> (PS4 slot, components, PS4 type) ; type 6 f32x3, 0x22 f16x2, 0x0a f16x4
ATTR = {1: (0, 3, 6), 0x11: (1, 2, 0x22), 0x12: (12, 2, 0x22), 2: (2, 4, 0x0a), 3: (3, 4, 0x0a),
        0x15: (4, 4, 0x0a), 0x4c: (8, 4, 0x0a), 0x13: (5, 2, 0x22), 0x4e: (255, 3, 6)}
INST_ATTR = (0x4c, 0x13, 0x4e)
TSIZE = {6: 12, 0x22: 4, 0x0a: 8}
# uniform-stream sharing (U3_STAGE_NO_UNIFORM=1 disables): convert() runs twice, pass 1 collects the longest
# length of every uniform per-vertex value, pass 2 gives each value one shared buffer of that length
_UNI = dict(collect=None, plan=None, bufs={}, seen={})


RAW_FMT_BPP = {0x1c: 4, 0x1d: 4, 0x57: 4, 0x5b: 4}


def cap_raw_texture(desc, texels, report, short):
    """agent Q (Museum hang, g6d): a retail bundle texture stored uncompressed (R8G8B8A8 / B8G8R8A8) can be huge (retail
    sky-village-pre-color-no-sun: 2048x1024x12 mips = 10.5 MB, put Museum's set at 97 MB VRAM vs <= 85 MB on every map
    that loads).  Drop top mips (2D only) until it is <= U3_STAGE_MAX_RAW bytes (default 4 MB; 0 = off).  The remaining
    mips are the retail texels unchanged; every PS4 mip is padded to 8x8 pixels, so dropping mip k removes exactly its
    padded size."""
    cap = int(os.environ.get('U3_STAGE_MAX_RAW', str(4 << 20)))
    fmt, typ = struct.unpack_from('<I', desc, 0x1c)[0], struct.unpack_from('<I', desc, 0x18)[0]
    if not cap or fmt not in RAW_FMT_BPP or typ != 1 or len(texels) <= cap:
        return desc, texels
    mips, w, h = struct.unpack_from('<III', desc, 0x24)
    bpp = RAW_FMT_BPP[fmt]; cut = 0; n = 0
    while len(texels) - cut > cap and mips - n > 1:
        mw, mh = max(1, w >> n), max(1, h >> n)
        cut += ((mw + 7) // 8 * 8) * ((mh + 7) // 8 * 8) * bpp; n += 1
    if not n:
        return desc, texels
    d = bytearray(desc)
    struct.pack_into('<III', d, 0x24, mips - n, max(1, w >> n), max(1, h >> n))
    report.setdefault('approx', []).append('texture %s: retail uncompressed %dx%d, top %d mip(s) dropped (VRAM cap)' % (short, w, h, n))
    return bytes(d), texels[cut:]


def REMAP_ON():
    """agent Q: honour the PS3 GCM component remap of uncompressed A8R8G8B8 textures (U3_STAGE_NO_REMAP=1 = old behaviour)"""
    return os.environ.get('U3_STAGE_NO_REMAP') != '1'


def UNIFORM_ON():
    return os.environ.get('U3_STAGE_NO_UNIFORM') != '1'


# ============================================================ PS3 side
def attr_values(p, fpg, fmt, attr, data_ref, nv, prec=None):
    aid, af, aoff, w = attr
    val = decode_attr(p, fpg, fmt, af, aoff, w, data_ref[0] if data_ref else 0,
                      data_ref[1] if data_ref else 0, nv, (prec or {}).get(aid))
    n = af & 0xf
    if aid in (0x15, 0x4e) and af & 0xf0 == 0:       # colour / instance constant: unorm
        val = (val + 1.0) / 2.0
    return val, n


def to_ps4(aid, val, nv):
    """PS3 decoded attribute -> PS4 stream array (float64, components per PS4 type)"""
    slot, nc, ty = ATTR[aid]
    v = np.zeros((nv, nc))
    k = min(nc, val.shape[1])
    v[:, :k] = val[:, :k]
    if aid in (2, 0x15) and val.shape[1] < 4:          # normal / colour: w = 1
        v[:, 3] = 1.0
    if aid == 0x15 and val.shape[1] < 3:
        v[:, val.shape[1]:3] = 0.0
    if aid == 3 and val.shape[1] < 4:
        v[:, 3] = 1.0
    if aid == 0x4e:
        v = np.round(v * 16.0) / 16.0                   # PS4 keeps 1/16 steps (1/3 -> 0.3125)
    return v


def decode_block3(p, blk, insts):
    """one PS3 bg block -> dict(nv, tris, attrs [(aid, array)] in format order,
    inst [per instance: {aid: array}], skin, flags...)"""
    pg, bo = blk
    nv = p.pu32(*p.resolve(pg, bo + 0x24))
    fpg, fmt = p.resolve(pg, bo + 0x28)
    spg, st = p.resolve(pg, bo)
    attrs = read_format(p, fpg, fmt)
    prec = block_prec(attrs)
    out = dict(nv=nv, attrs=[], inst=[], order=[a[0] for a in attrs])
    base_inst = {}
    for ai, a in enumerate(attrs):
        aid = a[0]
        if aid not in ATTR:
            continue
        r = p.resolve(spg, st + 8 * ai)
        val, _ = attr_values(p, fpg, fmt, a, r, nv, prec)
        if aid in INST_ATTR:
            base_inst[aid] = to_ps4(aid, val, nv)
        else:
            out['attrs'].append((aid, to_ps4(aid, val, nv)))
    out['base_inst'] = base_inst
    ii = p.resolve(pg, bo + 4)
    ntris, _, isz, _ = struct.unpack_from('>4H', p.d, p.pbase(ii[0]) + ii[1])
    idat = p.resolve(ii[0], ii[1] + 8)
    bi = p.pbase(idat[0])
    out['tris'] = ps3geom.decode_indices(p.d[bi + idat[1]:bi + idat[1] + isz], ntris)
    # skin
    sk = p.resolve(pg, bo + 0xc)
    if sk is not None:
        try:
            out['skin'], out['palette'] = ps3geom.decode_skin(p, sk[0], sk[1], nv)
        except Exception as ex:
            out['skin_err'] = repr(ex)
    # per-instance attributes
    for (stbl, ifmt) in insts:
        d = {}
        if ifmt is not None:
            iattrs = read_format(p, *ifmt)
            iprec = block_prec(iattrs)
            k = 0
            for a in iattrs:
                if a[0] not in INST_ATTR:
                    continue
                r = p.resolve(stbl[0], stbl[1] + 4 * k) if stbl else None
                k += 1
                val, _ = attr_values(p, ifmt[0], ifmt[1], a, r, nv, iprec)
                d[a[0]] = to_ps4(a[0], val, nv)
                if a[0] == 0x13 and r is not None:
                    d['_lm'] = True          # instance carries its own lightmap uvs
        out['inst'].append(d)
    return out


def ps3_light_table(p):
    it = p.items_of('LIGHT_TABLE_1')
    return it[0] if it else None


# ============================================================ resources
class Resources:
    """PS4 materials / effects / textures from the device bundle"""

    def __init__(self, bundle, plan, res=RES):
        self.techs = bundle['techs']
        self.mats = {}
        for m in bundle['mats']:
            if m.get('rec'):
                self.mats[(m['pak'], m['key'])] = m['rec']
        self.tex = {}
        for t in bundle['tex']:
            if 'desc' in t:
                self.tex[t['path']] = t
        # agent A (TEST gen3 Fort crash): material lightmap slots (g_tLightMap/1) of unlit props fall back to the retail
        # default-lightmap0/1 textures; a bundle without them (selfbuild, re-extract) left the binding dangling ->
        # runtime 'Could not find texture ... default-lightmap0' assert. Always carry the two retail copies.
        dl = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'default_lightmaps.json')
        if os.path.exists(dl):
            for t in json.load(open(dl))['tex']:
                if 'desc' in t and t['path'] not in self.tex:
                    self.tex[t['path']] = t
        for r in self.mats.values():
            for full, e in r.get('textures', {}).items():
                if 'desc' in e and e['path'] not in self.tex:
                    self.tex[e['path']] = dict(path=e['path'], desc=e['desc'], texels=e['texels'], gtex=e.get('gtex'))
        self.gtex = {}
        for r in self.mats.values():
            for full, e in r.get('textures', {}).items():
                if e.get('gtex'):
                    self.gtex[full] = e['gtex']
        self.plan = plan
        self.dict = {}
        dp = os.path.join(res, '..', '..', '..', 'Uncharted - The Nathan Drake Collection [CUSA02320]',
                          'u3data', 'build', 'main', 'texturedict1', 'ps4', 'TextureDictionaries.csv')
        dp = os.path.normpath(dp)
        if not os.path.exists(dp):
            dp = '/mnt/user-data/uploads/Uncharted - The Nathan Drake Collection [CUSA02320]/u3data/build/main/texturedict1/ps4/TextureDictionaries.csv'
        if os.environ.get('U3_TEXDICT'):
            dp = os.environ['U3_TEXDICT']
        if os.path.exists(dp):
            for row in csv.reader(open(dp)):
                if len(row) < 11 or row[0] == 'dictionary':
                    continue
                row = [x.strip() for x in row]
                self.dict[row[2]] = row

    def by_short(self, short):
        for path, t in self.tex.items():
            if short_of(path) == short:
                return t
        return None


# ============================================================ writer helpers
class AutoPages:
    def __init__(self, w):
        self.w = w
        self.cur = w.page()
        self.size = 0x200

    def room(self, n):
        if self.size + n > PAGE_LIMIT and self.size > 0x200:
            self.cur = self.w.page()
            self.size = 0x200
        self.size += n

    def item(self, type_, data=None, name='', phase=None):
        n = (len(data) if data is not None else 0) + 0x60 + len(type_) + len(name)
        if phase:
            n += phase[0]
        self.room((n + 15) & ~15)
        it = self.cur.item(type_, data, name=name)
        if phase:
            it.phase = phase
        return it


def f16b(a):
    return np.asarray(a, dtype='<f2').tobytes()


def f32b(a):
    return np.asarray(a, dtype='<f4').tobytes()


def stream_bytes(ty, arr):
    if ty == 6:
        return f32b(arr[:, :3])
    if ty == 0x22:
        return f16b(arr[:, :2])
    return f16b(arr[:, :4])


# ============================================================ prototype helpers
def lod0_groups(groups, nlod):
    """PS3 prototypes with header byte0 = n LODs > 1 list the material groups of every LOD in
    turn; the PS4 build keeps LOD0 only (retail ldn-train-stage Prototype2/3).  LOD1 starts at
    the first group whose material repeats an earlier one."""
    if nlod <= 1:
        return groups
    seen = set()
    for i, g in enumerate(groups):
        if g[0] in seen:
            return groups[:i]
        seen.add(g[0])
    return groups


def proto_flags(hw0, hw14, skinned, report, name):
    """16 prototype flag bytes, from retail rosetta pairs: [0] skinned (PS3 hdr byte1 0x02),
    [1] byte1 0x04, [2] byte1 0x08, [6]/[7] PS3 hdr+0x38 bits 0x01/0x02, [10] bit 0x20"""
    b1 = (hw0 >> 16) & 0xff
    b38 = hw14 >> 24
    fl = bytearray(16)
    fl[0] = 1 if skinned else 0
    fl[1] = 1 if b1 & 4 else 0
    fl[2] = 1 if b1 & 8 else 0
    fl[6] = 1 if b38 & 1 else 0
    fl[7] = 1 if b38 & 2 else 0
    fl[10] = 1 if b38 & 0x20 else 0
    unk = (b1 & ~0x0e) | ((b38 & ~0x63) << 8)
    if unk:
        report['approx'].append('%s: unmapped PS3 prototype flag bits %#x' % (name, unk))
    return bytes(fl)


# ============================================================ converter
def convert(ps3_path, plan, bundle, out_path, verbose=True, no_skin=False, keep_lights=False):
    if not UNIFORM_ON():
        return _convert_once(ps3_path, plan, bundle, out_path, verbose, no_skin, keep_lights)
    try:
        _UNI.update(collect={}, plan=None, bufs={}, seen={})
        _convert_once(ps3_path, plan, bundle, out_path, False, no_skin, keep_lights)
        _UNI.update(plan=_UNI['collect'], collect=None, bufs={}, seen={})
        d, r = _convert_once(ps3_path, plan, bundle, out_path, verbose, no_skin, keep_lights)
        r['uniform_values'] = len(_UNI['bufs'])
        return d, r
    finally:
        _UNI.update(collect=None, plan=None, bufs={}, seen={})


def _convert_once(ps3_path, plan, bundle, out_path, verbose=True, no_skin=False, keep_lights=False):
    p = Pak(ps3_path)
    stage = os.path.basename(ps3_path)[:-4]
    R = Resources(bundle, plan)
    C = collection(p)
    mats3, texs3 = read_materials(p)
    m3 = {m['name']: m for m in mats3}
    report = dict(stage=stage, approx=[], reused=[], converted=[])

    # ---------------- PS3 textures
    ps3tex = {}
    for t in p.textures():
        s = short_of(t['path'])
        ps3tex.setdefault(s, t)

    # ---------------- materials
    w = PakWriter()
    P = AutoPages(w)
    strs = Blob('strings')
    spool = {}

    def S(s):
        if s not in spool:
            spool[s] = strs.cstr(s)
        return spool[s]

    def raw(data, name='', phase=None):
        b = Blob('raw')
        b.raw(bytes(data))
        return P.item('RAW_DATA', b, name=name, phase=phase).data

    # textures ---------------------------------------------------------
    vram = bytearray()
    tex_entries = {}          # full path without .ndb -> entry

    def add_texture_desc(path_ndb, descraw, texels, gtex=None):
        """copy a PS4 VRAM_DESC (0x40 raw bytes) with new vram offset / path"""
        nonlocal vram
        full = path_ndb[:-4] if path_ndb.endswith('.ndb') else path_ndb
        if full in tex_entries:
            return tex_entries[full]
        while len(vram) % 0x100:
            vram.append(0)
        voff = len(vram)
        vram += texels
        d = bytearray(descraw[:0x38])
        struct.pack_into('<QQI', d, 0, 0, voff, len(texels))
        db = Blob('vramdesc')
        db.raw(bytes(d))
        db.ptr(db, 0x40)
        db.cstr(path_ndb)
        short = path_ndb.split(PS4_PREFIX, 1)[-1]
        nb = Blob('texname')
        nb.cstr(short)                     # retail: short path incl. '/<HASH>.ndb', sid(name) == hash
        name_item = P.item('RAW_DATA', nb)
        desc_item = P.item('VRAM_DESC', db)
        hsh = struct.unpack_from('<I', d, 0x14)[0]
        e = dict(full=full, path=path_ndb, hash=hsh, desc=desc_item, name=name_item,
                 gtex=bytes.fromhex(gtex) if gtex else None, fmt=struct.unpack_from('<I', d, 0x1c)[0])
        tex_entries[full] = e
        return e

    def convert_ps3_cubemap(t, texels, kind, bpb, path_ndb, short, flag=3):
        """PS3 cube map (desc type 2) -> PS4 retail cube (see cubefix.py: mip-major, 8 slots per mip, PS4-tiled,
        sRGB BC, desc +0x20 0x060f0000, GEOMETRY entry +4 0x100); uncompressed A8R8G8B8 cubes are BC3-encoded"""
        import cubefix
        if kind == 'A8R8G8B8':
            out = cubefix.cube_texels_argb(t, texels); kind = 'DXT5'
        else:
            out = cubefix.cube_texels(t, texels, bpb)
        if out is None:
            report.setdefault('approx', []).append('cube %s: short PS3 data, skipped' % short)
            return None
        w_, h_, mips_ = t['width'], t['height'], t['mips']
        fmt4 = {'DXT1': 0x48, 'DXT5': 0x4e, 'DXT3': 0x4b}[kind]
        hsh = sid(path_ndb.split(PS4_PREFIX, 1)[-1])
        d = bytearray(0x38)
        struct.pack_into('<QQIIIIIIIIII', d, 0, 0, 0, 0, hsh, 2, fmt4, 0x060f0000, mips_, w_, h_, 0, flag)
        cls = {0x48: 4, 0x4b: 5, 0x4e: 5}[fmt4]
        gt = struct.pack('<IIIiIIQ', hsh, 0x100, 0x101, -1, 0, cls, 0)
        e = add_texture_desc(path_ndb, bytes(d), bytes(out), gt.hex())
        report['converted'].append(short)
        report.setdefault('cubes', []).append(short)
        return e

    def convert_ps3_texture(t, path_ndb, short, flag=3, srgb=None):
        fmt3 = tc.ps3_format(t)
        if fmt3 is None:
            return None
        kind, bpb, comp = fmt3
        texels = p.vram_bytes(t['vramOffset'], t['vramSize'])
        w_, h_, mips_ = t['width'], t['height'], t['mips']
        if t.get('type') == 2 and (comp or kind == 'A8R8G8B8'):
            return convert_ps3_cubemap(t, texels, kind, bpb, path_ndb, short, flag)
        cap = MAX_TEX if not short.startswith('lightmaps/') else (int(os.environ.get('U3_STAGE_MAX_LM', '0')) or (1 << 30))
        if '/textures/sky/' in short or '/sky/textures/' in short:
            cap = max(cap, 1024)            # sky domes fill the screen
        if not comp:
            # uncompressed swizzled A8R8G8B8 (sky colour maps): deswizzle mip 0, halve down to the cap,
            # rebuild the mip chain and encode BC3 (DXT5) so it goes through the BC path below
            if kind != 'A8R8G8B8' or (w_ & (w_ - 1)) or (h_ & (h_ - 1)):
                return None
            img = tc.ps3_deswizzle_argb8(texels, w_, h_, REMAP_ON() and (t['gcmRemapCtl'] & 0xffff) or 0xaae4).astype(np.float32)   # agent Q: GCM remap
            while max(img.shape[:2]) > cap and min(img.shape[:2]) > 4:
                img = (img[0::2, 0::2] + img[1::2, 0::2] + img[0::2, 1::2] + img[1::2, 1::2]) / 4
            levels = []
            lv = img
            for k in range(max(1, mips_)):
                levels.append(tc.encode_bc3(np.clip(np.round(lv), 0, 255).astype(np.uint8)))
                if min(lv.shape[:2]) < 8:
                    break
                lv = (lv[0::2, 0::2] + lv[1::2, 0::2] + lv[0::2, 1::2] + lv[1::2, 1::2]) / 4
            texels = b''.join(levels)
            w_, h_, mips_ = img.shape[1], img.shape[0], len(levels)
            kind, bpb = 'DXT5', 16
            report.setdefault('approx', []).append('texture %s: PS3 A8R8G8B8 re-encoded as BC3 (%dx%d)' % (short, w_, h_))
        rm_ = t['gcmRemapCtl'] & 0xffff
        if comp and REMAP_ON() and rm_ != 0xaae4 and kind in ('DXT5', 'DXT3'):
            # agent Q: DXT with a non-identity GCM remap (0xaae1 = R<->A on 50 PS3 normal maps): decode every mip, apply
            # the remap, re-encode BC3 (PS4 has no sampler remap)
            levels, o = [], 0
            for mw, mh in tc.mip_dims(w_, h_, mips_):
                n_ = tc.ps3_linear_size(mw, mh, 1, bpb)
                img = tc.decode(texels[o:o + n_], mw, mh, kind); o += n_
                argb = img[..., [3, 0, 1, 2]]
                levels.append(tc.encode_bc3(np.ascontiguousarray(tc.gcm_remap(argb, rm_))))
            texels = b''.join(levels); kind, bpb = 'DXT5', 16
            report.setdefault('approx', []).append('texture %s: PS3 remap %#x applied (BC3 re-encode)' % (short, rm_))
        while max(w_, h_) > cap and mips_ > 1:
            # drop the top mip: retail stages keep only small resident mips (the rest streams), and
            # PS3-only textures have no stream, so the cap bounds the stage's VRAM
            texels = texels[tc.ps3_linear_size(w_, h_, 1, bpb):]
            w_, h_, mips_ = max(1, w_ >> 1), max(1, h_ >> 1), mips_ - 1
        if (w_, h_) != (t['width'], t['height']):
            report.setdefault('capped', []).append(short)
        tiled = tc.ps3_to_ps4_texels(texels, w_, h_, mips_, bpb)
        role = tex_role_final(short) or 'color'
        if srgb is None:
            srgb = role in ('color', 'spec', 'detail')
        fmt4 = {'DXT1': (0x47, 0x48), 'DXT5': (0x4d, 0x4e), 'DXT3': (0x4a, 0x4b)}[kind][1 if srgb else 0]
        hsh = sid(path_ndb.split(PS4_PREFIX, 1)[-1])
        d = bytearray(0x38)
        struct.pack_into('<QQIIIIIIIIII', d, 0, 0, 0, 0, hsh, 1, fmt4, 0x010f0000 if srgb else 0x01000000,
                         mips_, w_, h_, 0, flag)
        # GEOMETRY_1 texture entry: +4 0x1000 for lightmaps (0x400 typical), +0x14 format class
        cls = {0x47: 4, 0x48: 4, 0x4a: 5, 0x4b: 5, 0x4d: 5, 0x4e: 5, 0x50: 3, 0x53: 6}[fmt4]
        gt = struct.pack('<IIIiIIQ', hsh, 0x1000 if short.startswith('lightmaps/') else 0x400, 0x101, -1, 0, cls, 0)
        e = add_texture_desc(path_ndb, bytes(d), tiled, gt.hex())
        report['converted'].append(short)
        return e

    def texture_for_full(full, short_hint=None):
        """PS4 texture by cooked full path (no .ndb): reuse PS4 data, else convert from PS3"""
        if full in tex_entries:
            return tex_entries[full]
        t = R.tex.get(full + '.ndb') or R.tex.get(full)
        if t is not None:
            dsc, txl = cap_raw_texture(bytes.fromhex(t['desc']), bytes.fromhex(t['texels']), report, short_of(t['path']))
            e = add_texture_desc(t['path'], dsc, txl, R.gtex.get(full) or t.get('gtex'))
            report['reused'].append(short_of(t['path']))
            return e
        short = short_hint or short_of(full)
        t3 = ps3tex.get(short)
        if t3 is not None:
            return convert_ps3_texture(t3, full + '.ndb', short)
        return None

    def texture_for_short(short):
        """texture by short path (PS3 texture): PS4 copy if any, else PS3 conversion"""
        t = R.by_short(short)
        if t is not None:
            full = t['path'][:-4] if t['path'].endswith('.ndb') else t['path']
            return texture_for_full(full)
        t3 = ps3tex.get(short)
        if t3 is None:
            return None
        row = R.dict.get(short)
        full = row[1] if row else PS4_PREFIX + short + '/' + t3['path'].rsplit('/', 1)[-1].split('.')[0]
        return convert_ps3_texture(t3, full + '.ndb', short)

    flat_cache = {}

    def flat_texture(role, pname):
        """neutral constant texture for a template binding the PS3 material has no texture for (instead of
        leaking the template's unrelated retail texture): flat normal, mid grey colour, black masks"""
        low = pname.lower()
        if 'normal' in low or role == 'normal':
            kind, rgb, srgb = 'normal', (128, 128, 255), False
        elif role in ('mask', 'spec', 'blend'):
            kind, rgb, srgb = 'mask', (0, 0, 0), False
        elif role == 'trans':
            kind, rgb, srgb = 'trans', (255, 255, 255), False
        elif role == 'detail':
            kind, rgb, srgb = 'detail', (128, 128, 128), False
        else:
            kind, rgb, srgb = 'color', (128, 128, 128), True
        if kind in flat_cache:
            return flat_cache[kind]
        r_, g_, b_ = rgb
        c = ((r_ >> 3) << 11) | ((g_ >> 2) << 5) | (b_ >> 3)
        lin = bytearray(); w_ = h_ = 64; mips_ = 0
        while True:
            lin += struct.pack('<HHI', c, c, 0) * (max(1, (w_ + 3) // 4) * max(1, (h_ + 3) // 4))
            mips_ += 1
            if w_ == 1:
                break
            w_ //= 2; h_ //= 2
        t = dict(width=64, height=64, mips=mips_)
        short = 'art/generated/u3mp/flat-%s.tga' % kind
        tiled = tc.ps3_to_ps4_texels(bytes(lin), 64, 64, mips_, 8)
        path_ndb = '%s%s/%016X.ndb' % (PS4_PREFIX, short, sid(short) & 0xffffffffffffffff)
        hsh = sid(path_ndb.split(PS4_PREFIX, 1)[-1])
        fmt4 = 0x48 if srgb else 0x47
        d = bytearray(0x38)
        struct.pack_into('<QQIIIIIIIIII', d, 0, 0, 0, 0, hsh, 1, fmt4, 0x010f0000 if srgb else 0x01000000,
                         mips_, 64, 64, 0, 3)
        gt = struct.pack('<IIIiIIQ', hsh, 0x400, 0x101, -1, 0, 4, 0)
        e = add_texture_desc(path_ndb, bytes(d), tiled, gt.hex())
        flat_cache[kind] = e
        report.setdefault('flat', []).append(kind)
        return e

    # lightmap textures of the stage (resident, renamed like retail)
    lm_entries = {}
    for s, t in ps3tex.items():
        if '.geom.atgi/' in s or '/lightmap0/' in t['path']:
            base = s.rsplit('/', 1)[-1]
            hx = t['path'].rsplit('/', 1)[-1].split('.')[0]
            path_ndb = '%slightmaps/%s/%s/%s.ndb' % (PS4_PREFIX, stage, base, hx)
            lm_entries[base] = convert_ps3_texture(t, path_ndb, 'lightmaps/%s/%s' % (stage, base), flag=3, srgb=False)

    LM_PARAM = {'g_tLightMap': 'indirect.rgbmax.tga', 'g_tLightMap1': 'indirect.rgbmaxdelta.tga'}
    if lm_entries and not all(v in lm_entries for v in LM_PARAM.values()):
        report['approx'].append('lightmap set incomplete: %s' % sorted(lm_entries))

    def lightmap_entry(pname, full, short):
        """retail: every g_tLightMap/g_tLightMap1 binding of a lightmapped stage points at the
        stage's own indirect.rgbmax / indirect.rgbmaxdelta; otherwise the default lightmaps"""
        if pname in LM_PARAM and lm_entries.get(LM_PARAM[pname]) is not None:
            return lm_entries[LM_PARAM[pname]]
        if pname in LM_PARAM:
            d = 'default-lightmap0.tga' if pname == 'g_tLightMap' else 'default-lightmap1.tga'
            t = R.by_short('art/general/textures/default/' + d)
            if t is not None:
                return texture_for_full(t['path'][:-4] if t['path'].endswith('.ndb') else t['path'])
        return None

    # material records ------------------------------------------------
    names3 = []
    for c in C:
        for g in c['groups']:
            if g[0] not in names3:
                names3.append(g[0])
    # water surfaces (WATER_DATA): their materials join the material table through one dummy prototype each
    # (retail grave-water: Prototype0 with one 1-vertex / 1-triangle submesh and one instance)
    import waterdata
    water_mats = [m for m in waterdata.materials(p) if m in plan['mats']]
    if waterdata.objects(p) and len(water_mats) < len(waterdata.materials(p)):
        report['approx'].append('water: materials missing from the plan/bundle (re-plan): %s'
                                % [m for m in waterdata.materials(p) if m not in plan['mats']])
    for n in water_mats:
        if n not in names3:
            names3.append(n)
    matrec = {}            # ps3 name -> dict(name, rec, binds[(param, short, full, entry)])
    for n in names3:
        pe = plan['mats'][n]
        rec = R.mats.get((pe['pak'], pe['key']))
        if rec is None or 'fx' not in rec:
            raise SystemExit('bundle lacks material %s from %s' % (pe['key'], pe['pak']))
        binds = []
        if pe['exact']:
            name = rec['name']
            for pname, short, full, tail in rec['binds']:
                e = lightmap_entry(pname, full, short) or texture_for_full(full, short)
                binds.append((pname, short, full, e))
        else:
            # template material re-bound to the PS3 material's textures (same role)
            m = m3.get(n)
            own = {}               # role -> [PS3 short paths] in material order
            if m:
                idxs = list(m['textures'])
                if len(m3) == 1:
                    # single-material level (skies): the material's texture walk misses the sky colour map,
                    # so every texture of the pak belongs to it
                    idxs += [i for i in range(len(texs3)) if i not in idxs]
                for i in idxs:
                    s3 = re.sub(r'/[0-9A-F]{16}\.ndb(\.error)?$', '', short_of(texs3[i]['name']))
                    r = tex_role_final(s3)
                    if r == 'spec':
                        r = 'mask'
                    if r and s3 not in own.setdefault(r, []):
                        own[r].append(s3)
            used_role = {}
            for pname, short, full, tail in rec['binds']:
                role = param_role(pname)
                if role == 'spec':
                    role = 'mask'
                e = lightmap_entry(pname, full, short)
                # agent A batch57: explicit PS3 texture per template binding (plan 'bindmap' {pname: PS3 short path};
                # '' = neutral flat texture), used where role order cannot place them (sky-clouds.fx skies)
                if e is None and pname in pe.get('bindmap', {}):
                    s_ = pe['bindmap'][pname]
                    e = texture_for_short(s_) if s_ else flat_texture(role, pname)
                if e is None and own.get(role) and role not in ('lightmap',):
                    k = used_role.get(role, 0)
                    used_role[role] = k + 1
                    e = texture_for_short(own[role][min(k, len(own[role]) - 1)])
                if e is None and role in ('env', None) or e is None and 'LightMap' in pname:
                    e = texture_for_full(full, short)          # shared environment / default lightmap: keep
                if e is None:
                    e = flat_texture(role, pname)              # PS3 material has no such texture: neutral, not the template's
                binds.append((pname, short, full, e))
            key = pe['key']
            k3 = n.rsplit('/', 1)[0] + ':' + n.rsplit('/', 1)[1]
            hx = hashlib.md5(k3.encode()).hexdigest()
            guid = '%s-%s-%s-%s-%s' % (hx[:8], hx[8:12], hx[12:16], hx[16:20], hx[20:32])
            name = '%s,%s,%s' % (k3, guid, rec['shader'])
            nre = sum(1 for (pn_, sh_, fu_, e_) in binds if e_ is not None and e_['full'] != fu_ and 'LightMap' not in pn_)
            report['approx'].append('material %s -> template %s (%s), %d/%d own textures rebound'
                                    % (n, key, pe['pak'], nre, sum(1 for b_ in binds if 'LightMap' not in b_[0])))
        p3 = pe.get('ps3params') or {}
        if p3:
            # agent A (Fort sea band): PS3 water.fx packs constants into one vector named after its fields
            # ('g_fFoamAmount_fFoamDensity_fFoamMidPoint_fFoamSmoothness'); PS4 water templates split them (and vice
            # versa). Map component-wise by field name so e.g. PS3 foam amount 0 replaces the template's 0.645.
            p3 = dict(p3)
            tnames = {q[0] for q in rec['params']} if os.environ.get('U3_STAGE_WATERPACK', '0') == '1' else set()   # batch84 A/B: worse, off
            for k3_, v3_ in list(p3.items()):
                parts = k3_[2:].split('_') if k3_.startswith('g_') else []
                if len(parts) > 1 and k3_ not in tnames and all(re.match(r'^[a-z][A-Z0-9]', s_) for s_ in parts):
                    for j_, s_ in enumerate(parts):
                        if 'g_' + s_ in tnames and j_ < len(v3_) and ('g_' + s_) not in p3:
                            p3['g_' + s_] = [v3_[j_]]
            for t_ in tnames:
                parts = t_[2:].split('_') if t_.startswith('g_') else []
                if len(parts) > 1 and t_ not in p3 and all(('g_' + s_) in p3 for s_ in parts):
                    p3[t_] = [p3['g_' + s_][0] for s_ in parts]
            rec = dict(rec)
            newp = []
            nset = 0
            for pname_, cnt_, x_, vals_ in rec['params']:
                v3 = p3.get(pname_)
                if v3 is not None:
                    old = list(struct.unpack('<%df' % (len(vals_) // 8), bytes.fromhex(vals_)))
                    k_ = min(len(old), len(v3))
                    old[:k_] = v3[:k_]
                    vals_ = struct.pack('<%df' % len(old), *old).hex()
                    nset += 1
                newp.append([pname_, cnt_, x_, vals_])
            rec['params'] = newp
            report.setdefault('ps3params', 0)
            report['ps3params'] += nset
            miss = sorted(set(p3) - {q[0] for q in newp})
            if miss and not pe['exact']:
                report['approx'].append('material %s: PS3 params without a template slot %s' % (n, miss[:6]))
        matrec[n] = dict(name=name, rec=rec, binds=binds, pak=pe['pak'])
    # ---------------- effects
    eff_by_hash = {}
    order_hash = []
    for n in names3:
        rec = matrec[n]['rec']
        h = rec['hash']
        if h not in eff_by_hash:
            eff_by_hash[h] = (matrec[n]['pak'], rec)
            order_hash.append(h)
    # string table: start from the source with most effects
    from collections import Counter
    cnt = Counter(eff_by_hash[h][0] for h in order_hash)
    base_pak = cnt.most_common(1)[0][0]
    table = list(R.techs[base_pak])
    tpos = {s: i for i, s in enumerate(table)}
    shaderfiles = []
    fx_items = {}
    for h in order_hash:
        pak, rec = eff_by_hash[h]
        src = R.techs[pak]
        mp = {}
        for i, s in enumerate(src):
            if s not in tpos:
                tpos[s] = len(table)
                table.append(s)
            mp[i] = tpos[s]
        clos = rec['fx']
        blobs = []
        for k, it in enumerate(clos):
            data = bytes.fromhex(it['data'])
            if k == 0:
                ptrs = {pp[0]: (pp[2] if pp[1] == 0 else None) for pp in it['ptrs']}
                data = remap(data, ptrs, mp)
            b = Blob('fx')
            b.raw(data)
            blobs.append(b)
        for k, it in enumerate(clos):
            b = blobs[k]
            for pos, ti, toff in it['ptrs']:
                b.relocs.append((pos, blobs[ti], toff))
            struct_zero = b.b
            for pos, ti, toff in it['ptrs']:
                struct.pack_into('<Q', struct_zero, pos, 0)
        items = []
        for k, it in enumerate(clos):
            items.append(P.item(it['type'], blobs[k], name=it['name'], phase=(0x100, it['off'] % 0x100)))
        fxIdx, fxX, eoff = rec['fx_entry']
        sf = rec['shader']
        if sf not in shaderfiles:
            shaderfiles.append(sf)
        fx_items[h] = dict(item=items[0], blob=blobs[0], eoff=eoff, fx=shaderfiles.index(sf), x=fxX)
    # EFFECT_TABLE
    eb = Blob('effecttable')
    eb.u32(len(order_hash)).u32(7)
    ent = Blob('fxentries')
    for h in order_hash:
        f = fx_items[h]
        hv = int(h, 16)
        ent.ptr(f['blob'], f['eoff']).u32(f['fx']).u32(f['x']).u64(hv)
    eb.ptr(ent)
    eb.ptr(eb, 0x20)
    sf_off = 0x20
    # shaderfile table at 0x20, its strings, then the string table
    sfb = Blob('sf')
    sfb.u32(len(shaderfiles)).u32(0)
    sf_tab_size = 8 + 8 * len(shaderfiles)
    names_blob = bytearray()
    sf_offs = []
    for s in shaderfiles:
        sf_offs.append(len(names_blob))
        names_blob += s.encode() + b'\0'
    while len(names_blob) % 16:
        names_blob.append(0)
    tab_off = 0x20 + sf_tab_size + len(names_blob)
    tab_off = (tab_off + 15) & ~15
    eb.ptr(eb, tab_off)
    for o in sf_offs:
        pass
    body = Blob('etbody')
    # build manually: we need pointers into eb itself
    cur = 0x20
    eb.u32(len(shaderfiles)).u32(0)
    for o in sf_offs:
        eb.ptr(eb, 0x20 + sf_tab_size + o)
    eb.raw(bytes(names_blob))
    while eb.pos < tab_off:
        eb.u8(0)
    eb.u32(len(table)).u32(0)
    str_base = tab_off + 8 + 8 * len(table)
    so = []
    sb = bytearray()
    for s in table:
        so.append(len(sb))
        sb += s.encode() + b'\0'
    for o in so:
        eb.ptr(eb, str_base + o)
    eb.raw(bytes(sb))
    et_item = P.item('EFFECT_TABLE', eb)
    P.item('RAW_DATA', ent)

    # ---------------- geometry
    fmt_blob = Blob('vtxfmt')
    fmt_off = {}

    def fmt_entry(slot, ty):
        if (slot, ty) not in fmt_off:
            fmt_off[(slot, ty)] = fmt_blob.pos
            fmt_blob.raw(bytes([slot, 0, TSIZE[ty], ty]))
        return fmt_off[(slot, ty)]
    for aid, (slot, nc, ty) in ATTR.items():
        fmt_entry(slot, ty)
    P.item('RAW_DATA', fmt_blob)

    stream_cache = {}

    def streams_blob(lst):
        """lst: [(slot, ty, bytes)] -> Blob of 0x18 stream records.  Identical vertex data blocks are
        shared (read-only GPU data; per-instance lightmap-uv / iconst streams are often equal), which keeps
        big maps inside the memory budget (U3_STAGE_NO_DEDUP=1 disables)"""
        sb_ = Blob('streams')
        for slot, ty, data in lst:
            es = TSIZE[ty]
            nvv = len(data) // es
            ukey = None
            if UNIFORM_ON() and nvv > 1 and bytes(data[:es]) * nvv == bytes(data):
                ukey = (ty, bytes(data[:es]))
            if os.environ.get('U3_STAGE_NO_DEDUP') == '1':
                db = raw(data)
            elif ukey is not None and _UNI['plan'] is not None and _UNI['plan'].get(ukey, 0) >= nvv:
                # uniform stream (every vertex equal): all streams of this value share one buffer sized for the
                # longest of them; a record only reads its own length (lossless, same bytes per vertex)
                db = _UNI['bufs'].get(ukey)
                if db is None:
                    db = _UNI['bufs'][ukey] = raw(ukey[1] * _UNI['plan'][ukey])
                if len(db) > len(data) or _UNI['seen'].get(ukey):
                    report['dedup'] = report.get('dedup', 0) + len(data)
                _UNI['seen'][ukey] = True
            else:
                if ukey is not None and _UNI['collect'] is not None:
                    _UNI['collect'][ukey] = max(_UNI['collect'].get(ukey, 0), nvv)
                k = hashlib.md5(bytes(data)).digest() + len(data).to_bytes(4, 'little')
                db = stream_cache.get(k)
                if db is None:
                    db = stream_cache[k] = raw(data)
                else:
                    report['dedup'] = report.get('dedup', 0) + len(data)
            sb_.u32((TSIZE[ty] << 16) | 1).u32(len(data)).ptr(fmt_blob, fmt_entry(slot, ty)).ptr(db)
        P.item('RAW_DATA', sb_)
        return sb_

    subs = []            # submesh records
    protos = []
    insts = []
    all_inst = []
    for pi, pr in enumerate(C):
        pname = 'Prototype%d.%s' % (pi, stage)
        hdr = pr['hdr']
        hw0 = p.pu32(*hdr)
        njoints = hw0 & 0xffff if (hw0 >> 16) & 0x2 else 0   # agent A: 16-bit (coop-syria-wide P1 = 1499 joints)
        hw14 = p.u32(p.pbase(hdr[0]) + hdr[1] + 0x38)
        # agent F3: PS3 header byte1 0x04 = draw-distance fade; hdr+8 = u16 fade start, u16 fade end (metres). PS4 keeps the flag as
        # prototype flag[1] and reads {f32 start, f32 end, 0, 0} through prototype +0x48 (instance builder 0xe9b3b5; retail
        # ldn-train-stage P9 260/265, P15 45/48, P18 40/45 = PS3 hdr+8). A NULL pointer gives start 0 / end 1e-8 = never drawn.
        hw8 = p.u32(p.pbase(hdr[0]) + hdr[1] + 8)
        fade = ((hw8 >> 16) & 0xffff, hw8 & 0xffff) if (hw0 >> 16) & 0x4 else None
        inst_list = []
        for x in pr['insts']:
            I = instance(p, x)
            bd = I['blockdata']
            inst_list.append((x, I, bd))
        # instance per-block (stream table, format)
        def inst_tables(bi):
            out = []
            for x, I, bd in inst_list:
                if bd is None:
                    out.append((None, None))
                    continue
                stbl = p.resolve(bd[0], bd[1] + 8 * bi)
                ifmt = p.resolve(bd[0], bd[1] + 8 * bi + 4)
                out.append((stbl, ifmt))
            return out
        blocks = [decode_block3(p, b, inst_tables(bi)) for bi, b in enumerate(pr['blocks'])]
        skinned = njoints > 0 and not no_skin and all('skin' in blocks[bi] for bi in range(len(blocks)))
        if njoints and not skinned:
            report['approx'].append('%s: %d-joint skinned prototype written static' % (pname, njoints))
        # PS3 material table entries: group flags
        t = p.resolve(hdr[0], hdr[1] + 0x10)
        gflags = []
        for s in range(len(pr['groups'])):
            gflags.append(p.u32(p.pbase(t[0]) + t[1] + 0x10 * s + 0xc))
        proto_subs = []
        groups = lod0_groups(pr['groups'], hw0 >> 24)
        if len(groups) < len(pr['groups']):
            report['approx'].append('%s: %d LODs, LOD1+ groups dropped like retail (%d of %d groups kept)'
                                    % (pname, hw0 >> 24, len(groups), len(pr['groups'])))
        for gi, (mn, first, cnt_) in enumerate(groups):
            for c0 in range(first, first + cnt_, MAX_BLOCKS):
                chunk = list(range(c0, min(first + cnt_, c0 + MAX_BLOCKS)))
                bl = [blocks[b] for b in chunk]
                nv = sum(b['nv'] for b in bl)
                tris = []
                base = 0
                for b in bl:
                    tris += [(a + base, c + base, d + base) for a, c, d in b['tris']]
                    base += b['nv']
                idx = np.array(tris, dtype='<u2').reshape(-1)
                # main streams in PS3 attribute order (union over blocks; missing = default)
                order = []
                for b in bl:
                    for aid, _a in b['attrs']:
                        if aid not in order:
                            order.append(aid)
                if 2 in order and 0x11 in order and 3 not in order:
                    order.insert(order.index(2) + 1, 3)
                st = []
                arrays = {}
                for aid in order:
                    slot, nc, ty = ATTR[aid]
                    parts = []
                    for b in bl:
                        d = dict(b['attrs'])
                        if aid in d:
                            parts.append(d[aid])
                        elif aid == 3:
                            dd = dict(b['attrs'])
                            parts.append(gen_tangents(dd[1], dd[2][:, :3], dd.get(0x11), b['tris']))
                            report['approx'].append('%s Submesh%d: generated tangents' % (pname, c0))
                        else:
                            dflt = np.zeros((b['nv'], nc))
                            if aid in (2, 0x15):
                                dflt[:, 3] = 1
                            parts.append(dflt)
                    arr = np.concatenate(parts)
                    arrays[aid] = arr
                    st.append((slot, ty, stream_bytes(ty, arr)))
                # instance attribute defaults (proto level)
                iorder = [a for a in INST_ATTR if any(a in b['base_inst'] for b in bl)]
                iorder = [a for a in bl[0]['order'] if a in iorder]
                # the per-instance writer drops the lightmap-uv stream when the stage has no lightmaps; the
                # engine copies <submesh ns2> records from every instsub stream array (0xe9a490), so both lists
                # must match exactly (retail never differs): drop 0x13 here too, else it reads past the
                # instsub array -> 'desc.GetNumAttributes() <= kMaxAttrDefs' (vertex-stream.cpp:146)
                if not lm_entries:
                    iorder = [a for a in iorder if a != 0x13]
                st2 = []
                for aid in iorder:
                    slot, nc, ty = ATTR[aid]
                    if aid == 0x13:
                        arr = np.concatenate([b['base_inst'].get(aid, np.zeros((b['nv'], nc))) for b in bl])
                    else:
                        arr = np.zeros((nv, nc))
                    st2.append((slot, ty, stream_bytes(ty, arr)))
                skin = None
                infl_arr = None
                if skinned:
                    infl = [x for b in bl for x in b['skin']]
                    counts = np.array([len(x) for x in infl], dtype='<u2')
                    offs = np.concatenate([[0], np.cumsum(counts)[:-1]]).astype('<u2')
                    joints = np.array([j for x in infl for j, _w in x], dtype='<u2')
                    wts = np.array([wv / 255.0 for x in infl for _j, wv in x], dtype='<f4')
                    skin = (int(counts.sum()), raw(counts.tobytes()), raw(offs.tobytes()),
                            raw(joints.tobytes()), raw(wts.tobytes()))
                    infl_arr = (counts, offs, joints, wts)
                sname = '%s.Prototype%d.Submesh%d' % (stage, pi, c0)
                rec = dict(name=sname, nv=nv, ni=len(idx), st=streams_blob(st),
                           st2=streams_blob(st2) if st2 else None, ns=len(st), ns2=len(st2),
                           ib=raw(idx.tobytes()), mat=matrec[mn]['name'], ps3mat=mn,
                           flags=1 | (0x100 if gflags[gi] & 0x80 else 0), skin=skin,
                           njoints=njoints if skinned else 0, chunk=chunk, iorder=iorder, infl=infl_arr,
                           pos=arrays[1][:, :3])
                subs.append(rec)
                proto_subs.append(rec)
        # cbuffer capacities from the materials' per-instance (scope 3) cbuffers
        # retail: 3 when a material of the prototype is alpha blended, else 2
        cb3 = 3 if any('TRANSPARENCY_ALPHA_BLEND' in matrec[rr['ps3mat']]['rec']['feats'] for rr in proto_subs) else 2
        # 0xe86f10 dedupes the scope-3 cbuffers of every material's VS/PS program by id and appends without a
        # bounds check; retail = distinct programs + 1 (+1 alpha).  Our converted materials select more program
        # variants per prototype than retail, so reserve one slot per distinct feature set + 2, at least 8
        # (cap 2 overran the heap: Highrise kMaxAttrDefs assert, Museum fault 0x1281ac6).  ~0.5 MB per map.
        nfs = len({frozenset(matrec[rr['ps3mat']]['rec']['feats']) for rr in proto_subs})
        cb3 = max(cb3, nfs + 2, int(os.environ.get('U3_STAGE_CB3_MIN', 8)))
        protos.append(dict(name=pname, subs=proto_subs, njoints=njoints if skinned else 0, hw14=hw14, fade=fade,
                           skinned=skinned, hw0=hw0, cb3=cb3))
        # instances
        for ii, (x, I, bd) in enumerate(inst_list):
            M = I['mat'].astype(np.float64)
            isubs = []
            J = None
            if skinned:
                jr = p.resolve(x[0], x[1] + 0x10)
                if jr:
                    J = np.frombuffer(p.d[p.pbase(jr[0]) + jr[1]:p.pbase(jr[0]) + jr[1] + 0x30 * njoints],
                                      '>f4').astype(np.float64).reshape(njoints, 3, 4)
            for rr in proto_subs:
                pos = rr['pos']
                if J is not None and rr['infl'] is not None:
                    # retail: skinned prototypes get the AABB of the posed (skinned) vertices
                    cnts, offs_, jn, wt = rr['infl']
                    vi = np.repeat(np.arange(len(pos)), cnts)
                    ph = np.c_[pos, np.ones(len(pos))]
                    contrib = np.einsum('tij,tj->ti', J[jn], ph[vi]) * wt[:, None]
                    posed = np.zeros((len(pos), 3))
                    np.add.at(posed, vi, contrib)
                    mn_, mx_ = posed.min(0), posed.max(0)
                else:
                    mn_, mx_ = pos.min(0), pos.max(0)          # retail: prototype-local AABB
                ist = []
                for aid in rr['iorder']:
                    slot, nc, ty = ATTR[aid]
                    parts = []
                    for b in rr['chunk']:
                        d = blocks[b]['inst'][ii] if ii < len(blocks[b]['inst']) else {}
                        parts.append(d.get(aid, blocks[b]['base_inst'].get(aid, np.zeros((blocks[b]['nv'], nc)))))
                    arr = np.concatenate(parts)
                    if aid == 0x13 and not lm_entries:
                        continue
                    ist.append((slot, ty, stream_bytes(ty, arr)))
                lmf = any(blocks[b]['inst'][ii].get('_lm') for b in rr['chunk'] if ii < len(blocks[b]['inst']))
                # agent R: retail shadow-only flag (0x10000; 0x1010000 for ik-only) on shadow-blocker submeshes, unless the
                # PS3 instance word +8 has 0x10000 (retail keeps those drawn). U3_STAGE_NO_SBFLAG=1 = old behaviour.
                sbf = 0
                if os.environ.get('U3_STAGE_NO_SBFLAG', '0') != '1':
                    pm = rr['ps3mat']
                    if 'ik-only' in pm: sbf = 0x1010000
                    elif re.search(r'shadowblocker|canopy-shadow', pm, re.I) and not (I['x8'] & 0x10000): sbf = 0x10000
                # fixer W w005: retail hidden blockers also carry +0x24 = 0x101 (bytes 4/5: lib-shadowblocker 38/38, ik-only 12/12);
                # with only +0x20 0x10000 the instance builder 0xe9a490 still sets pass bits 4 / 0x10 and the blocker draws black.
                # U3_STAGE_NO_SBVIS=1 = old behaviour.
                sbf2 = 0x101 if sbf and os.environ.get('U3_STAGE_NO_SBVIS', '0') != '1' else 0
                isubs.append(dict(sub=rr, st=streams_blob(ist) if ist else None, n=len(ist), mn=mn_, mx=mx_,
                                  flags=(0x100 if lmf and lm_entries else 0) | sbf, flags2=sbf2))
            jm = None
            nj = 0
            if skinned:
                jr = p.resolve(x[0], x[1] + 0x10)
                if jr:
                    nj = njoints
                    jm = raw(p.d[p.pbase(jr[0]) + jr[1]:p.pbase(jr[0]) + jr[1] + 0x30 * nj][::1])
                    # 3x4 float matrices: endian swap
                    a = np.frombuffer(p.d[p.pbase(jr[0]) + jr[1]:p.pbase(jr[0]) + jr[1] + 0x30 * nj], '>f4')
                    jm = raw(a.astype('<f4').tobytes())
            allmn = np.min([s['mn'] for s in isubs], 0) if isubs else np.zeros(3)
            allmx = np.max([s['mx'] for s in isubs], 0) if isubs else np.zeros(3)
            insts.append(dict(proto=pname, id=I['x8'] & 0xffff, x8=I['x8'], subs=isubs, M=M, mn=allmn, mx=allmx,
                              jm=jm, nj=nj))

    # water dummy prototypes (after the PS3 prototypes, like retail)
    for wk, wm in enumerate(water_mats):
        pi = len(C) + wk
        pname = 'Prototype%d.%s' % (pi, stage)
        z = np.zeros((1, 3))
        st = [(ATTR[1][0], ATTR[1][2], stream_bytes(ATTR[1][2], z))]
        rec = dict(name='%s.Prototype%d.Submesh0' % (stage, pi), nv=1, ni=3, st=streams_blob(st), st2=None, ns=1, ns2=0,
                   ib=raw(np.zeros(3, dtype='<u2').tobytes()), mat=matrec[wm]['name'], ps3mat=wm, flags=1, skin=None,
                   njoints=0, chunk=[], iorder=[], infl=None, pos=z)
        subs.append(rec)
        protos.append(dict(name=pname, subs=[rec], njoints=0, hw14=0, skinned=False, hw0=0, cb3=1))
        nid = max([i['id'] for i in insts], default=-1) + 1
        insts.append(dict(proto=pname, id=nid, x8=nid, subs=[dict(sub=rec, st=None, n=0, mn=np.zeros(3), mx=np.zeros(3),
                                                                     flags=0)], M=np.eye(4), mn=np.zeros(3),
                          mx=np.zeros(3), jm=None, nj=0))
        report.setdefault('water', []).append(wm)

    # ---------------- material / texture tables
    gtex = []
    for n in names3:
        for _p, _s, _f, e in matrec[n]['binds']:
            if e is not None and e not in gtex:
                gtex.append(e)
    texb = Blob('geotextures')
    for e in gtex:
        g = e['gtex']
        texb.ptr(strs, S(e['full']))
        if g:
            texb.raw(g[:0x20] if len(g) >= 0x20 else g + b'\0' * (0x20 - len(g)))
        else:
            texb.u32(e['hash']).u32(0x400).u32(0x101).i32(-1).u32(0).u32(4).u64(0)
    featb, parb, valb, bindb, matb = Blob('features'), Blob('params'), Blob('paramvalues'), Blob('bindings'), Blob('materials')
    mat_order = []
    for n in names3:
        if matrec[n]['name'] not in [m['name'] for m in mat_order]:
            mat_order.append(matrec[n])
    for m in mat_order:
        rec = m['rec']
        foff = featb.pos
        for f in rec['feats']:
            featb.ptr(strs, S(f))
        poff = parb.pos
        for pname, cnt_, x, vals in rec['params']:
            v = bytes.fromhex(vals)
            voff = valb.pos
            valb.raw(v)
            valb.pad(16)
            parb.ptr(strs, S(pname)).ptr(valb, voff).u32(cnt_).u32(x)
        boff = bindb.pos
        for pname, short, full, e in m['binds']:
            if e is not None:
                full2 = e['full']
                sh = short if 'LightMap' in pname else short_of(e['path'])
            else:
                full2, sh = full, short
            bindb.ptr(strs, S(pname)).ptr(strs, S(sh)).ptr(strs, S(full2)).i32(-1).u32(0).u64(0)
        matb.ptr(strs, S(m['name'])).ptr(strs, S(rec['shader']))
        matb.ptr(featb if rec['feats'] else None, foff).ptr(strs, S(rec['hash']))
        matb.ptr(parb if rec['params'] else None, poff).ptr(bindb if m['binds'] else None, boff)
        matb.u32(len(rec['feats'])).u32(len(rec['params'])).u32(len(m['binds'])).u32(rec['x3'])
        matb.u32(rec['tail'][0]).u32(rec['tail'][1])
    for blob in (featb, parb, valb, bindb, matb, texb):
        if len(blob):
            P.item('RAW_DATA', blob)

    # submesh array
    sb = Blob('submeshes')
    for r in subs:
        sb.u64(0).ptr(strs, S(r['name'])).u32(r['flags']).u32(0).u32(0).u32(0)
        sb.u32(r['nv']).u32(r['ni']).u32(r['ns']).u32(r['ns2']).ptr(r['st']).ptr(r['st2'])
        sb.ptr(r['ib']).ptr(strs, S(r['mat'])).i32(-1).u32(0).u32(r['njoints']).u32(0)
        if r['skin']:
            skb = Blob('skin')
            tot, c_, o_, j_, w_ = r['skin']
            skb.u32(0).u32(tot).ptr(c_).ptr(o_).ptr(j_).ptr(w_)
            P.item('RAW_DATA', skb)
            sb.ptr(skb)
        else:
            sb.u64(0)
        sb.raw(b'\0' * 0x20)
    P.item('RAW_DATA', sb)
    # prototypes
    protob, plist, flagsb, snl = Blob('prototypes'), Blob('protolist'), Blob('protoflags'), Blob('subnames')
    for pr in protos:
        foff = flagsb.pos
        fl = proto_flags(pr['hw0'], pr['hw14'], pr['skinned'], report, pr['name'])
        flagsb.raw(bytes(fl))
        soff = snl.pos
        for r in pr['subs']:
            snl.ptr(strs, S(r['name'])).i32(-1).u32(0)
        pr['off'] = protob.pos
        cb3, cb4 = pr['cb3'], 1
        protob.u32(3).u32(len(pr['subs'])).u32(0).u32(sid(stage + '.' + pr['name']))
        protob.ptr(strs, S(pr['name'])).ptr(snl, soff).ptr(flagsb, foff).u64(0)
        protob.u32(cb3).u32(cb4)
        fb = None
        if pr.get('fade') and os.environ.get('U3_STAGE_NO_FADE', '0') != '1':
            fb = Blob('fade')
            fb.raw(struct.pack('<4f', float(pr['fade'][0]), float(pr['fade'][1]), 0.0, 0.0))
            P.item('RAW_DATA', fb)
        protob.raw(b'\0' * (0x48 - 0x38))
        protob.ptr(fb)                                   # +0x48 fade distances (flag[1]); NULL when unflagged
        protob.raw(b'\0' * (0x78 - 0x50))
    for pr in protos:
        plist.ptr(protob, pr['off'])
    for blob in (protob, flagsb, snl, plist):
        P.item('RAW_DATA', blob)
    # instances
    instb, ilist, isb = Blob('instances'), Blob('instlist'), Blob('instsubs')
    for ins in insts:
        ioff = isb.pos
        for s in ins['subs']:
            isb.ptr(strs, S(s['sub']['name'])).i32(-1).u32(0).u32(s['n']).u32(0).ptr(s['st'])
            isb.u32(s['flags']).u32(s.get('flags2', 0)).u32(0x00ff0000).i32(-1)   # +0x20 0x100: own lightmap uvs (retail); +0x24 w005
            isb.raw(struct.pack('<4f', *s['mn'], 1.0)).raw(struct.pack('<4f', *s['mx'], 1.0))
        ins['off'] = instb.pos
        # agent A: per-instance flag bytes. Retail ldn-train-stage: PS3 instance word x8 bit 16+k set -> PS4 flag byte k = 0
        # (13/13 instances with PS3 bit 18 carry 0x01000101); unflagged = 0x01010101. Fort's hidden/dynamic pieces use bit 18.
        hi = (ins.get('x8', 0) >> 16) & 0xf if os.environ.get('U3_STAGE_INSTFLAGS', '0') == '1' else 0   # held until TEST's batch83 result
        ifl = 0x01010101 & ~sum(0xff << (8 * k) for k in range(4) if hi >> k & 1)
        instb.u32(3).u32(ins['id']).ptr(strs, S(ins['proto'])).i32(-1).u32(0).u32(ifl).u32(1)
        instb.u32(len(ins['subs'])).u32(ins['nj']).u64(0).ptr(isb, ioff).ptr(ins['jm'])
        instb.raw(b'\0' * 0x20)
        instb.raw(struct.pack('<4f', *ins['mn'], 1.0)).raw(struct.pack('<4f', *ins['mx'], 1.0))
        instb.raw(np.asarray(ins['M'], '<f4').tobytes())
    for ins in insts:
        ilist.ptr(instb, ins['off'])
    for blob in (instb, isb, ilist):
        P.item('RAW_DATA', blob)
    P.item('RAW_DATA', strs)
    gb = Blob('geometry')
    gb.u32(0x10).u32(0)
    gb.u64(len(subs)).ptr(sb)
    gb.u64(len(protos)).ptr(plist)
    gb.u64(len(insts)).ptr(ilist)
    gb.u64(len(gtex)).ptr(texb if gtex else None)
    gb.u64(len(mat_order)).ptr(matb)
    gb.u64(0).u64(0)
    geo = P.item('GEOMETRY_1', gb)

    # ---------------- small items
    li = Blob('levelinfo'); li.u32(0)
    level_info = P.item('LEVEL_INFO_1', li)
    lp = Blob('levelpats'); lp.u64(0)
    level_pats = P.item('LEVEL_PATS_1', lp)
    di = Blob('debuginfo'); di.ptr(di, 0x10).u32(PAD32).u32(PAD32); di.cstr(stage)
    debug_info = P.item('DEBUG_INFO_1', di)
    af = Blob('afm'); af.u32(0).u32(PAD32).u64(0).u64(0)
    afm = P.item('ATTACHED_FG_MAPPING', af)
    tf = Blob('tagfloat'); tf.u32(0)
    tagf = P.item('TAG_FLOAT', tf)
    def gobo(hashes):
        # agent A batch54: each PS3 projector entry names its gobo texture by the PS3 texture hash at entry +0x3c
        # (syria-multi: 2 entries, both 0x9e4a69a6 = light-library/gobos/chromatic-aberative-oval-edge.tga;
        # coop-airport-2: 9 x gobo-quad-color + 1 gobo-front-spot). The old rule (as many 'gobo' textures as
        # entries) dropped every stage whose entries share a texture. The texture is converted into the pak
        # (resident) so the name resolves; None when an entry's texture is not in the PS3 pak.
        byh = {}
        for sh, t3 in ps3tex.items():
            byh.setdefault(t3['hash'], sh)
        # gobo textures that live in another PS3 pak (coop-syria-area1/2: SP syria-area1 / coop-syria-area2-low):
        # bundle 'gobo_tex' {PS3 hash hex: PS4 short}, the PS4 retail copy supplied in bundle 'tex' (b54/gobotex.py)
        for hx, sh in bundle.get('gobo_tex', {}).items():
            byh.setdefault(int(hx, 16), sh)
        names = [byh.get(h) for h in hashes]
        if None in names:
            return None
        for sh in sorted(set(names)):
            if texture_for_short(sh) is None:
                return None
        return names
    light = convert_light_table(p, P, keep_lights, report, gobo)
    # texture table
    order = sorted(tex_entries.values(), key=lambda e: e['hash'])
    namesb = Blob('ttnames')
    for e in order:
        namesb.ptr(e['name'].data)
    hashb = Blob('tthash')
    for e in order:
        hashb.u32(e['hash'])
    P.item('RAW_DATA', namesb)
    P.item('RAW_DATA', hashb)
    ttb = Blob('texturetable')
    ttb.u64(len(order)).ptr(hashb).ptr(namesb).ptr(ttb, 0x20)
    for e in order:
        ttb.ptr(e['desc'].data)
    ttab = P.item('TEXTURE_TABLE', ttb)

    # background collision (PS3 ND mesh -> Havok 2011.3 hkpBvCompressedMeshShape, collision.py)
    colls = []
    if os.environ.get('U3_STAGE_NO_COLL') != '1':
        import collision
        cbs = collision.havok_bg_blobs(p)
        for cb in cbs:
            colls.append(P.item('COLLISION_DATA_HAVOK_BG', cb))
        if cbs:
            report['collision'] = sum(len(cb.b) for cb in cbs)
            report['collision_items'] = len(cbs)
    # occlusion (VIS_INFO_1, visinfo.py; byte-exact on retail cha-entrance-fire-low)
    vis = None
    if os.environ.get('U3_STAGE_NO_VIS') != '1' and p.items_of('VIS_INFO_1'):
        import visinfo
        vbs = visinfo.vis_blobs(p)
        if vbs:
            vis = P.item('VIS_INFO_1', vbs[0])
            for x in vbs[1:]:                   # big VIS (> 1 MB): rest in RAW_DATA items on other pages
                P.item('RAW_DATA', x)
            report['vis'] = sum(len(x.b) for x in vbs)
            if len(vbs) > 1:
                report['vis_parts'] = len(vbs)
    # water surfaces (WATER_DATA, waterdata.py: byte-exact on retail grave-water / grave-01-firstyard-water /
    # grave-03-docks-water except exporter garbage words)
    water = None
    if waterdata.objects(p) and water_mats:
        def wkey(n):
            return matrec[n]['name'] if n in matrec else waterdata.ps4key(n)
        wb = waterdata.water_blob(p, matkey=lambda n: wkey(n).split(',')[0])
        if wb is not None:
            water = P.item('WATER_DATA', wb)
            report['water_objects'] = len(waterdata.objects(p))
    # LEVEL_OFFSET_1 (3 floats): PS3 yemen-multi* carry (200, 0, 0) in the background pak AND the -ingame12 (same
    # value; the login handler sets the level's offset), air-multi* zeros -- kept 1:1, after LEVEL_INFO_1 as in ps4ingame
    loff = None
    lo3 = p.items_of('LEVEL_OFFSET_1')
    if lo3:
        assert lo3[0].size == 12, 'LEVEL_OFFSET_1 size %#x' % lo3[0].size
        lb_ = Blob('leveloffset')
        b0 = p.pbase(lo3[0].page) + lo3[0].data_off
        for k in range(3):
            lb_.raw(p.d[b0 + 4 * k:b0 + 4 * k + 4][::-1])
        loff = P.item('LEVEL_OFFSET_1', lb_)
        report['level_offset'] = list(struct.unpack_from('>3f', p.d, b0))
    # login page
    p1 = w.page()
    make_login_and_vram_page(w, p1, [None] * 9, [e['desc'] for e in order])
    w.vram = bytes(vram)
    # retail order: DEBUG_INFO_1 LEVEL_INFO_1 LEVEL_PATS_1 EFFECT_TABLE TEXTURE_TABLE GEOMETRY_1 VIS_INFO_1
    # LIGHT_TABLE_1 ATTACHED_FG_MAPPING COLLISION_DATA_HAVOK_BG TAG_FLOAT (cha-entrance-fire-low)
    w.login_targets = [level_info] + ([loff] if loff is not None else []) + [debug_info, level_pats, et_item, ttab, geo] + ([vis] if vis is not None else []) + \
        ([light] if light is not None else []) + [afm] + colls + ([water] if water is not None else []) + [tagf]
    w.login_item.data.b[:] = struct.pack('<II', len(w.login_targets), 0) + b'\0' * (8 * len(w.login_targets))
    data = w.build()
    if out_path:
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        open(out_path, 'wb').write(data)
    report.update(size=len(data), submeshes=len(subs), prototypes=len(protos), instances=len(insts),
                  materials=len(mat_order), effects=len(order_hash), textures=len(tex_entries),
                  vram=len(vram), strings=len(table), pages=len(w.pages))
    if verbose:
        print('%s: %d bytes, %d pages, %d submeshes, %d prototypes, %d instances, %d materials, %d effects, '
              '%d textures (vram %#x)' % (out_path, len(data), len(w.pages), len(subs), len(protos), len(insts),
                                          len(mat_order), len(order_hash), len(tex_entries), len(vram)))
    return data, report


# ============================================================ LIGHT_TABLE_1
def convert_light_table(p, P, keep_lights, report, gobo=None):
    """PS3 LIGHT_TABLE_1 (0x60 header) -> PS4 (0xa0 header).  Layout from rosetta pairs
    (ldn-train-stage, san-upper-multi-stage, grave-pirate-ship-e-rs-probes,
    yem-temp-east-wing-corridor, cargo-plane-airport-int, grave-02-hollowhulk-front-low):

      PS3 header u32 +0x08 +0x0c +0x10 +0x14 +0x18 +0x1c +0x20 +0x24 -> PS4 +0x10..+0x2c (same order)
      PS3 ptr  +0x28 records[cnt +0x08] (0x3c each)   -> PS4 +0x30
               +0x30 local lights[cnt +0x10] (0x48)   -> PS4 +0x40
               +0x34 sun (0x3c)                       -> PS4 +0x48
               +0x38 list[cnt +0x18]                  -> PS4 +0x50 (PS4 adds an inner pointer:
                                                         not converted, dropped with its count)
               +0x3c u32 block                        -> PS4 +0x58
               +0x48 vec4[cnt +0x24], +0x4c probe pos vec3[cnt +0x1c]
                                                      -> PS4 +0x70, +0x78
               +0x50 probe SH[cnt +0x1c] (f32 + 28 u8) -> PS4 +0x80
               +0x04 flags u8[cnt +0x00]              -> PS4 +0x08 (at +0xa0)
    Records are big-endian 32-bit words; the word at +0x30 of records / local lights / sun is
    4 raw bytes.  Block sizes are the gaps between the PS3 sub-blocks; PS4 lays them out in
    header order, 16-aligned."""
    its = p.items_of('LIGHT_TABLE_1')
    if not its:
        return None             # sky levels: retail PS4 sky paks carry no LIGHT_TABLE_1 either
    it = its[0]
    pg, b = it.page, it.data_off
    u = lambda o: p.pu32(pg, b + o)
    R = lambda o: p.resolve(pg, b + o)
    base = p.pbase(pg)
    fields = {o: R(o)[1] - b for o in range(0, 0x60, 4) if R(o) is not None and R(o)[0] == pg}
    ends = sorted(set(fields.values()) | {it.size})

    def block(o):
        if o not in fields:
            return None
        t = fields[o]
        n = [x for x in ends if x > t][0] - t
        return p.d[base + b + t:base + b + t + n]

    def words(d, raw_at=None, stride=None):
        d = d[:len(d) // 4 * 4]
        out = bytearray(struct.pack('<%dI' % (len(d) // 4), *struct.unpack('>%dI' % (len(d) // 4), d)))
        if raw_at is not None:
            for k in range(0, len(d) - raw_at - 3, stride):
                out[k + raw_at:k + raw_at + 4] = d[k + raw_at:k + raw_at + 4]
        return bytes(out)

    def sh(d):
        out = bytearray()
        for k in range(0, len(d) // 0x20 * 0x20, 0x20):
            out += struct.pack('<f', *struct.unpack('>f', d[k:k + 4])) + d[k + 4:k + 0x20]
        return bytes(out)

    nflags = u(0)
    cnt8, nlocal, n18 = u(8), u(0x10), u(0x18)
    parts = {}
    if nlocal:
        report.setdefault('lights', []).append('%d local lights converted' % nlocal)
    d = block(0x28)
    if d is not None and cnt8:
        parts[0x30] = words(d[:0x3c * cnt8], 0x30, 0x3c)
    d = block(0x30)
    if d is not None and nlocal:
        parts[0x40] = words(d[:0x48 * nlocal], 0x30, 0x48)
    d = block(0x34)
    if d is not None:
        parts[0x48] = words(d[:0x3c], 0x30, 0x3c) + b'\xaa' * 4
    gobo_names, gobo_str = [], []
    d = block(0x38)
    if n18 and d is not None and len(d) >= 0xa0 * n18 and gobo is not None:
        # projector (gobo) lights: PS3 0xa0 -> PS4 0xb0 {0x3c words (raw +0x30), pad, char* gobo
        # texture, u64 0, 0x60 words (PS3 +0x40..+0xa0)}. agent A batch54: the PS3 stride is 0xa0, not 0xa8
        # (block sizes 0x320/0x3c0/0x640 for 5/6/10 entries; every entry at 0xa0 holds a texture hash at +0x3c);
        # the PS3 entry names its gobo by texture hash at +0x3c. Retail PS4 (syria-area1-entrance, 9 entries)
        # writes one name string per entry (0x50 apart) right after the entries.
        names = gobo([struct.unpack_from('>I', d, 0xa0 * k + 0x3c)[0] for k in range(n18)])
        if names:
            ent = bytearray()
            for k in range(n18):
                e3 = d[0xa0 * k:0xa0 * k + 0xa0]
                ent += words(e3[:0x3c], 0x30, 0x3c) + b'\xaa' * 4 + b'\0' * 16 + words(e3[0x40:0xa0])
            gobo_str = []
            for nm in names:                 # one name per entry, following the entries in the block (retail)
                gobo_str.append(len(ent))
                ent += nm.encode() + b'\0'
                while len(ent) % 16:
                    ent += b'\xaa'
            parts[0x50] = bytes(ent)
            gobo_names = names
            report.setdefault('lights', []).append('%d gobo lights converted (%s)' % (n18, ', '.join(names)))
    if n18 and not gobo_names:
        report['approx'].append('light table: %d gobo (projector) lights dropped (no gobo texture match)' % n18)
        n18 = 0
    d = block(0x3c)
    if d is not None:
        parts[0x58] = words(d)
    for o3, o4 in ((0x48, 0x70), (0x4c, 0x78)):
        d = block(o3)
        if d is not None:
            n = {0x48: 16 * u(0x24), 0x4c: 12 * u(0x1c)}[o3]
            parts[o4] = words(d[:n])
    d = block(0x50)
    if d is not None:
        parts[0x80] = sh(d[:0x20 * u(0x1c)])
    lb = Blob('lighttable')
    lb.u32(nflags).u32(PAD32)
    if nflags:
        lb.ptr(lb, 0xa0)
    else:
        lb.u64(0)
    lb.u32(cnt8).u32(u(0xc)).u32(nlocal).u32(u(0x14)).u32(n18).u32(u(0x1c)).u32(u(0x20)).u32(u(0x24))
    off = 0xa0 + (((nflags + 15) & ~15) if nflags else 0)
    pos = {}
    for o4 in sorted(parts):
        pos[o4] = off
        off = (off + len(parts[o4]) + 15) & ~15
    for o4 in range(0x30, 0xa0, 8):
        if o4 in pos:
            lb.ptr(lb, pos[o4])
        else:
            lb.u64(0)
    assert lb.pos == 0xa0, hex(lb.pos)
    if nflags:
        fl = R(4)
        lb.raw(p.d[p.pbase(fl[0]) + fl[1]:p.pbase(fl[0]) + fl[1] + nflags])
        while lb.pos % 16:
            lb.u8(0xaa)
    for o4 in sorted(parts):
        while lb.pos < pos[o4]:
            lb.u8(0xaa)
        lb.raw(parts[o4])
    while lb.pos % 16:
        lb.u8(0xaa)
    for k, nm in enumerate(gobo_names):
        lb.relocs.append((pos[0x50] + 0xb0 * k + 0x40, lb, pos[0x50] + gobo_str[k]))
    return P.item('LIGHT_TABLE_1', lb)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ps3')
    ap.add_argument('plan')
    ap.add_argument('bundle')
    ap.add_argument('out')
    ap.add_argument('--no-skin', action='store_true')
    ap.add_argument('--report')
    a = ap.parse_args()
    data, rep = convert(a.ps3, json.load(open(a.plan)), json.load(open(a.bundle)), a.out, no_skin=a.no_skin)
    if a.report:
        json.dump(rep, open(a.report, 'w'), indent=1)


if __name__ == '__main__':
    main()
