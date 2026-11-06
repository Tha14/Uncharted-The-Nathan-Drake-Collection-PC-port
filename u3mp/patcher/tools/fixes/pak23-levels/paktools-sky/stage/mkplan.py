#!/usr/bin/env python3
"""Plan the PS4 resources a converted stage reuses, and the device extraction requests.

For every PS3 material of the stage:
  * exact: a PS4 level pak has a material with the same name (PS3 'dir/file/variant' ->
    PS4 'dir/file:variant') -> its record, EFFECT and textures are copied from that pak;
  * fallback: no PS4 material of that name -> a PS4 material with a similar texture-role
    signature (same file other variant first, then same level directory) is used as the
    template and re-bound to the PS3 material's textures.
Every PS3 texture that some PS4 pak carries is requested too (its PS4 VRAM_DESC + low mips).

usage: mkplan.py <ps3 stage pak> [--exclude PAK ...] -o plan.json
Inputs: ps4mats.jsonl.gz (material survey) and ps4tex.tsv (texture index) in --res DIR.
"""
import argparse, gzip, json, os, re, sys, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
from ndpak import Pak
from bggeom import collection

RES = '/mnt/user-data/uploads/Uncharted-windows/_claude_transfer/stages'
ROLE_RE = [('normal', r'-normals?\b|-normal-|normal\.'), ('detail', r'-detail'), ('spec', r'-spec'),
           ('mask', r'-mask'), ('blend', r'-blend'), ('trans', r'-trans|-alpha|-opacity'),
           ('env', r'refle|env'), ('color', r'-color|-diffuse|-col\b')]
PARAM_ROLE = [('Normal', 'normal'), ('Detail', 'detail'), ('Mask', 'mask'), ('Blend', 'blend'),
              ('Transparency', 'trans'), ('Environment', 'env')] + ([('EnvCube', 'env')] if os.environ.get('U3_STAGE_WATERPACK', '0') == '1' else []) + [('Color', 'color'), ('Spec', 'spec'),
              ('LightMap', 'lightmap')]


def ps4key(ps3name):
    d, v = ps3name.rsplit('/', 1)
    return d + ':' + v


def tex_role(short):
    s = short.lower()
    if 'lightmap' in s or 'missingtexture' in s or '.atgi/' in s or 'indirect.rgbmax' in s:
        return None
    for role, rx in ROLE_RE:
        if re.search(rx, s):
            return role
    return 'color'


def tex_role_final(short):
    """role of a PS3 texture for its PS4 encoding (sRGB) and slot -- ps4stage only, plans keep tex_role:
    suffixes match whole words ('-detailed', 'bugenvilia' are not detail / env maps), and a name ending in
    -color/-diffuse/-col is a colour map even after -blend/-detail/-refle/-mask ('dirt-blend-color',
    'tmp-wooden-mask-color' = the colour map of a wooden mask); spec and
    normal roles stay as tex_role gives them (retail keeps '-spec-mask' sRGB, '-normal-blend' is a normal map)."""
    if tex_role(short) is None:
        return None
    b = short.lower().rsplit('/', 1)[-1]
    role = 'color'
    for r, rx in ROLE_FINAL_RE:
        if re.search(rx, b):
            role = r
            break
    if role in ('blend', 'detail', 'env', 'mask') and re.search(r'-(color|diffuse|col)\.[a-z]+$', b):
        role = 'color'
    return role


ROLE_FINAL_RE = [('normal', r'-normals?(?=[-._])'), ('detail', r'-detail(?=[-._])'), ('spec', r'-spec(?=[-._])'),
                 ('mask', r'-mask(?=[-._])'), ('blend', r'-blend(?=[-._])'), ('trans', r'-(trans\w*|alpha|opacity)(?=[-._])'),
                 ('env', r'(-env(?=[-._])|refle)'), ('color', r'-(color|diffuse|col)(?=[-._])')]


def param_role(p):
    for k, r in PARAM_ROLE:
        if k in p:
            return r
    return None


def short_of(full):
    """'.../texture1|2/<short>/<HASH>.ndb' -> short path"""
    m = re.search(r'texture[12]/(.*?)/[0-9A-F]{16}(\.ndb)?$', full)
    return m.group(1) if m else full


def read_materials(p):
    """PS3 MATERIAL_TABLE_1 -> ([{name, textures:[idx]}], [{name, hash}]) (page-aware)"""
    mt = p.items_of('MATERIAL_TABLE_1')
    if not mt:
        return [], []
    mt = mt[0]
    pg, b = mt.page, mt.data_off
    ntex, nmat = p.pu32(pg, b + 4), p.pu32(pg, b + 0xc)
    th = p.resolve(pg, b + 0x18)
    nm = p.resolve(pg, b + 0x30)
    ma = p.resolve(pg, b + 0x20)
    texs = []
    for i in range(ntex):
        r = p.resolve(nm[0], nm[1] + 4 * i)
        texs.append(dict(name=p.pstr(*r) if r else '', hash=p.pu32(th[0], th[1] + 4 * i)))
    out = []
    for i in range(nmat):
        m = p.resolve(ma[0], ma[1] + 4 * i)
        name = p.pstr(*p.resolve(*m))
        idx = []
        try:
            r = p.resolve(m[0], m[1] + 0x10)
            node = p.resolve(*r) if r else None
            bb = p.resolve(node[0], node[1] + 0x10) if node else None
            if bb:
                n = p.pu32(*bb)
                n = p.pu32(bb[0], bb[1] + 4)
                for k in range(min(n, 32)):
                    rr = p.resolve(bb[0], bb[1] + 0x50 + 0x10 * k)
                    if rr and rr[0] == th[0] and th[1] <= rr[1] < th[1] + 4 * ntex:
                        idx.append((rr[1] - th[1]) // 4)
        except Exception:
            pass
        # the pass/sampler tree under +0 also references textures the first node misses (e.g.
        # the '-trans' alpha maps of foliage): bounded walk, appended in walk order
        seen = set()

        def walk(a, depth):
            if depth > 4 or a in seen:
                return
            seen.add(a)
            for k in range(0, 0x80, 4):
                try:
                    r = p.resolve(a[0], a[1] + k)
                except Exception:
                    return
                if not r:
                    continue
                if r[0] == th[0] and th[1] <= r[1] < th[1] + 4 * ntex:
                    j = (r[1] - th[1]) // 4
                    if j not in idx and j not in extra:
                        extra.append(j)
                else:
                    walk(r, depth + 1)
        extra = []
        try:
            # material {name*, 0, 0, 8, passes*[8], ...}: passes beyond the first three can point
            # at structures shared with other materials, so only passes 0..2 are followed
            pl_ = p.resolve(m[0], m[1] + 0x10)
            if pl_:
                seen.add(m)
                for k in range(3):
                    r = p.resolve(pl_[0], pl_[1] + 4 * k)
                    if r:
                        walk(r, 2)
        except Exception:
            pass
        # agent A (2026-10-28): the exact texture set is the union of the binding blocks of the material's 8
        # technique passes (+0x10 -> pass[k] -> +0x10 binding block, textures at +0x50 stride 0x10). The bounded
        # pointer walk above leaks textures of neighbouring materials (gompa-woodfloor-01 picked up
        # gobo-lantern02-trans -> cutoff template + foreign alpha map = see-through floors; 871 materials with a
        # foreign '-trans' map over the MP levels) and misses own maps of later passes (syria brickwall normals).
        # The walk is kept only as a fallback when no pass binding block is readable.
        exact = []
        try:
            pl_ = p.resolve(m[0], m[1] + 0x10)
            for k in range(8 if pl_ else 0):
                r = p.resolve(pl_[0], pl_[1] + 4 * k)
                bb = p.resolve(r[0], r[1] + 0x10) if r else None
                if not bb:
                    continue
                for kk in range(min(p.pu32(bb[0], bb[1] + 4), 32)):
                    rr = p.resolve(bb[0], bb[1] + 0x50 + 0x10 * kk)
                    if rr and rr[0] == th[0] and th[1] <= rr[1] < th[1] + 4 * ntex:
                        j = (rr[1] - th[1]) // 4
                        if j not in exact:
                            exact.append(j)
        except Exception:
            exact = []
        out.append(dict(name=name, textures=exact if exact else idx + extra))
    return out, texs


FOLIAGE = re.compile(r'plant|leaf|leaves|vine|card(?!board)|grass|moss|brom|bush|fern|ivy|palm-frond|frond|shrub|weed|reed|'
                     r'flower|hedge|branch|twig|canopy|willow|banyan')


def ps3_alpha_textures(p):
    """hashes of PS3 DXT1 textures whose top mip has > 5% punch-through (transparent) blocks"""
    import numpy as np
    import texcodec as tc
    out = set()
    for t in p.textures():
        f = tc.ps3_format(t)
        if not f or not f[2]:
            continue
        bw, bh = (t['width'] + 3) // 4, (t['height'] + 3) // 4
        d = p.vram_bytes(t['vramOffset'], bw * bh * f[1])
        if f[0] == 'DXT1':
            a = np.frombuffer(d, '<u2').reshape(-1, 4)
            idx = np.frombuffer(d, '<u4').reshape(-1, 2)[:, 1]
            pt = a[:, 0] <= a[:, 1]
            has3 = np.zeros(len(idx), bool)
            for k in range(16):
                has3 |= ((idx >> (2 * k)) & 3) == 3
            if (pt & has3).mean() > 0.05:
                out.add(t['hash'])
        # DXT3/DXT5 alpha is mostly a blend/spec mask in these levels, not a cutout
    return out


GROUNDISH = re.compile(r'floor|tile|ground|terrain|road|path|stone|rock|brick|wall|concrete|dirt|sand|mud|plaster|'
                       r'pavement|cobble|street|roof|step|stair|trim|wood-plank|planks|metal|marble|tarmac|gravel')
WIND_FEATS = ('GLOBAL_PIVOT_WIND', 'BP_TREE_ANIM', 'ND_TREE_ANIM')
_PRULE = None


def param_rule():
    """retail prime-shader feature set -> material param names it carries (learned from bundles)"""
    global _PRULE
    if _PRULE is None:
        f = os.path.join(os.path.dirname(HERE), 'data', 'fx_param_feats.json')
        _PRULE = {k: frozenset(v) for k, v in (json.load(open(f)) if os.path.exists(f) else {}).items()}
    return _PRULE


_PP = {}


def predicted_params(feats):
    key = frozenset(feats)
    r = _PP.get(key)
    if r is None:
        r = _PP[key] = frozenset(pn for pn, req in param_rule().items() if req and req <= key)
    return r


# PS3 params that say nothing about the shader variant (present on nearly every material)
GENERIC_P = {'g_specularBrightness', 'g_specularExponentMax'}


def material_class(name, m, texs3, alpha_tex):
    """'cut' (alpha tested), 'blend' (alpha blended), 'leaf' (foliage) for a PS3 material, from
    its name and whether its colour texture carries alpha"""
    c = set()
    low = name.lower()
    col_alpha = False
    if m:
        for i in m['textures']:
            t = texs3[i]
            r = tex_role(short_of(t['name']))
            if r == 'trans' or (r == 'color' and t['hash'] in alpha_tex):
                col_alpha = True
    if FOLIAGE.search(low) and not GROUNDISH.search(low.rsplit('/', 1)[-1] if '/' in low else low):
        c.add('leaf')
    if re.search(r'glass|window|-trans\b', low):
        c.add('blend')
    elif col_alpha:
        c.add('cut')
    return c



# ---- v2 fallback scoring (U3_MKPLAN_V2, default on): donor features that misrender a PS3 material
V2 = os.environ.get('U3_MKPLAN_V2', '1') != '0'
BAD_FEATS = [(re.compile(r'_BLACK$'), 25), (re.compile(r'MASK_COLOR_MAP'), 20), (re.compile(r'FACING_TINT'), 12),
             (re.compile(r'BORDER_TINT'), 12), (re.compile(r'_DSR_|DIFFUSE_SUBSURFACE'), 10),
             (re.compile(r'DISTORTION'), 15), (re.compile(r'CAUSTICS'), 15), (re.compile(r'VERTEX_OVERLAY|VERTEX_MASK|SLOPE'), 12),
             (re.compile(r'INSTANCE_TINT|INSTANCE_OFFSET'), 12), (re.compile(r'TINT_WITH_MASK'), 12),
             (re.compile(r'APPLY_OVERLAY_TEXTURE|OVERLAY_CONTROL|OVERLAY_MASK'), 12), (re.compile(r'TIME_SCROLL|TILT_SCROLL|UV_FLOW'), 15),
             (re.compile(r'OFFSET_MAPPING'), 6), (re.compile(r'PICKUP_SHIMMER'), 30), (re.compile(r'SHADOW_PROXY'), 30),
             (re.compile(r'CLOTH|SUBSURF'), 10), (re.compile(r'RIPPLES'), 15), (re.compile(r'PARTIALLY_CONVERTED'), 3),
             (re.compile(r'GLASS_FRESNEL'), 10), (re.compile(r'TRANSLUCEN'), 6), (re.compile(r'EARTH_CURVATURE|SPHERIZE'), 20)]
USES_UV2 = re.compile(r'USE_UV2|USE_SET_2|OFFSET_MAPPING_USE_UV2')
USES_UV34 = re.compile(r'USE_UV3|USE_UV4')
USES_VCOL = re.compile(r'VERTEX_COLOR|VERTEX_AMBIENT|VERTEX_OVERLAY|VERTEX_MASK|_VERTEX_')


def ps3_material_attrs(p):
    """PS3 material name -> set of vertex attribute ids its blocks carry (0x11 uv0, 0x12 uv1, 0x15 colour)"""
    from bggeom import read_format
    out = collections.defaultdict(set)
    try:
        C = collection(p)
    except Exception:
        return out
    for c in C:
        for k, blk in enumerate(c['blocks']):
            m = c['mats'][k]
            try:
                fpg, fmt = p.resolve(blk[0], blk[1] + 0x28)
                out[m] |= {a[0] for a in read_format(p, fpg, fmt)}
            except Exception:
                pass
    return out


_V2C = {}


V21 = os.environ.get('MKPLAN_V21', '1') != '0'


def ps3_needs(params3):
    """v2.1 (agent A): shader properties the PS3 material's own constants prove: incandescence, uv scroll/flow (as the
    set of PS4 features that would carry it: g_timeScrollU<n>/V<n> -> UV_<n>_APPLY_TIME_SCROLL, flow constants -> UV_FLOW),
    distortion"""
    ks = ' '.join(params3 or ()).lower()
    scr = set()
    for k in params3 or ():
        m = re.match(r'g_timescroll[uv](\d)$', k.lower())
        if m:
            scr.add('UV_%s_APPLY_TIME_SCROLL' % m.group(1))
        elif re.search(r'flowtime|flowuv|uvflow', k.lower()):
            scr.add('UV_FLOW')
    return (bool(re.search(r'incand', ks)), frozenset(scr), bool(re.search(r'distort', ks)))


V22 = os.environ.get('MKPLAN_V22', '1') != '0'


def ps3_two_layers(params3):
    """v2.2 (agent A): the PS3 material's constants carry a second colour layer (vertex-colour or blend-map blend)"""
    return any(k in (params3 or {}) for k in ('g_colorTint2', 'g_colorBrightness2', 'g_blendSoftness_blendOffset_linearBlendOffset'))


V23 = os.environ.get('MKPLAN_V23', '1') != '0'
_DEBUG = None
_COLP = re.compile(r'g_color(Tint|Brightness)(\d)$')
_UVP = re.compile(r'g_uvScale(\d)$')


def ps3_strong(params3):
    """v2.3 (agent Q): the PS3 constants the template must carry because they change the picture: non-neutral colour
    tint / brightness per layer, non-1 uv scales, the border tint.  Installed v2.2 paks dropped a non-neutral colour
    constant on 1395 of 4857 fallback materials and a uv scale on 1930 (Sanctuary red walls: colour layer 3 x2.5 red
    lost -> half brightness, grey hue; textures tiled 4x too large)."""
    out = set()
    for k, v in (params3 or {}).items():
        v = list(v) if isinstance(v, (list, tuple)) else [v]
        if _COLP.match(k):
            if (k.startswith('g_colorTint') and any(abs(x - 1) > 0.02 for x in v[:3])) or \
               (k.startswith('g_colorBrightness') and abs(v[0] - 1) > 0.02):
                out.add(k)
        elif _UVP.match(k) and abs(v[0] - 1) > 0.02:
            out.add(k)
        elif k == 'g_diffuseColorBorderTint':
            out.add(k)
    return frozenset(out)


def v23_need(k):
    """PS4 feature that carries the strong PS3 constant k"""
    m = _COLP.match(k)
    if m:
        return 'COLOR_%s_APPLY_COLOR_TINT' % m.group(2)
    m = _UVP.match(k)
    if m:
        return 'UV_%s_APPLY_SCALE' % m.group(1)
    return 'BORDER_TINT'


def v23_lost(f4, params3):
    """strong PS3 constants (ps3_strong) the template feature set f4 cannot carry"""
    return {k for k in ps3_strong(params3) if not any(v23_need(k) in x for x in f4)}


def v23_score(f4, params3):
    """v2.3 score change: -12 per strong colour constant or uv scale the template cannot carry, -6 for the border
    tint; the v2 layer and BORDER_TINT penalties are refunded when the PS3 constants prove that layer / the border tint."""
    st = ps3_strong(params3)
    if not st:
        return 0
    sc = 0
    for k in v23_lost(f4, params3):
        sc -= 6 if k == 'g_diffuseColorBorderTint' else 12
    proven = {int(_COLP.match(k).group(2)) for k in st if _COLP.match(k)}
    layers = {int(x.split('_')[1]) for x in f4 if re.match(r'COLOR_\d_', x)}
    sc += 10 * len((layers - {1, 2}) & proven)
    if 'g_diffuseColorBorderTint' in st and any('BORDER_TINT' in x for x in f4):
        sc += 12
    return sc


def v2_penalty(f4, r3, c3, attrs3, n_color3, name, params3=None):
    """score change for a donor feature set f4 on a PS3 material (roles r3, class c3, vertex attrs attrs3).
    v2.1: a feature the PS3 material's constants prove (incandescence / scroll / distortion) is not penalised, and a
    donor that lacks it loses 15."""
    low = name.lower()
    inc3, scr3, dis3 = ps3_needs(params3) if V21 else (False, frozenset(), False)
    emissive = inc3 or bool(re.search(r'lamp|light|fire|glow|neon|screen|incand|emissive|sky', low))
    two3 = V22 and ps3_two_layers(params3)
    nrm3 = V22 and 'normal' in r3
    key = (frozenset(f4), 'blend' in r3, 'env' in r3, emissive, frozenset(attrs3 or ()), scr3, dis3, inc3, two3, nrm3)
    v = _V2C.get(key)
    if v is None:
        v = _V2C[key] = _v2_penalty(f4, r3, attrs3, emissive, scr3, dis3, inc3, two3, nrm3)
    return v


def _v2_penalty(f4, r3, attrs3, emissive, scr3=frozenset(), dis3=False, inc3=False, two3=False, nrm3=False):
    pen = 0
    blend3 = 'blend' in r3 or two3
    if two3 and not any(x.startswith('COLOR_1_BLEND_COLOR_2') for x in f4):
        pen -= 45                  # v2.2: PS3 second colour layer (vertex-colour blend) needs a 2-layer template
    if nrm3 and not any('NORMAL_1_USE_NORMAL_MAP' in x or 'NORMAL_MAP_USE_SHARED' in x for x in f4):
        pen -= 15                  # v2.2: PS3 normal map needs a normal-mapped template
    for need in scr3:
        if not any(x.startswith(need) for x in f4):
            pen -= 15
    if dis3 and not any('DISTORTION' in x for x in f4):
        pen -= 15
    if inc3 and not any('INCANDESCENCE' in x for x in f4):
        pen -= 15
    layers = {int(x.split('_')[1]) for x in f4 if re.match(r'COLOR_\d_', x)}
    allowed = {1, 2} if blend3 else {1}
    pen -= 10 * len(layers - allowed)
    if blend3 and 2 not in layers:
        pen -= 10
    if not blend3 and any(x.startswith('BLEND_FACTOR') for x in f4):
        pen -= 10
    for ff in f4:
        for rx, w in BAD_FEATS:
            if rx.search(ff):
                if any(ff.startswith(need) for need in scr3) or (dis3 and 'DISTORTION' in ff):
                    break
                pen -= w
                break
        if 'INCANDESCENCE' in ff and not emissive:
            pen -= 15
        if 'ADD_ENVIRONMENT' in ff and 'env' not in r3:
            pen -= 4
    if attrs3:
        if any(USES_UV2.search(x) for x in f4) and 0x12 not in attrs3:
            pen -= 40
        if any(USES_UV34.search(x) for x in f4):
            pen -= 40
        if any(USES_VCOL.search(x) for x in f4) and 0x15 not in attrs3:
            pen -= 40
    return pen


def load_survey(res):
    S = collections.defaultdict(list)       # key -> [(pak, size, rec)]
    for l in gzip.open(os.path.join(res, 'ps4mats.jsonl.gz'), 'rt'):
        d = json.loads(l)
        for m in d.get('mats', []):
            S[m['n'].split(',')[0]].append((d['pak'], d.get('size', 0), m))
    return S


def load_texindex(res):
    T = collections.defaultdict(list)       # short -> [(pak, fullpath, fields)]
    for l in open(os.path.join(res, 'ps4tex.tsv')):
        f = l.rstrip('\n').split('\t')
        if len(f) < 11:
            continue
        T[short_of(f[1])].append((f[0], f[1], f))
    return T


def plan(ps3path, exclude=(), res=RES, verbose=True):
    p = Pak(ps3path)
    stage = os.path.basename(ps3path)[:-4]
    exclude = set(exclude) | {stage + '.pak'}
    S = load_survey(res)
    T = load_texindex(res)
    paksize = {}
    for v in S.values():
        for pk, sz, _ in v:
            paksize[pk] = sz
    C = collection(p)
    names = []
    for c in C:
        for g in c['groups']:
            if g[0] not in names:
                names.append(g[0])
    # water surfaces (WATER_DATA) reference materials no prototype uses: plan them too (water.fx)
    import waterdata
    water = set(waterdata.materials(p))
    for n in waterdata.materials(p):
        if n not in names:
            names.append(n)
    mats3, texs3 = read_materials(p)
    m3 = {m['name']: m for m in mats3}
    import ps3params
    P3 = ps3params.all_material_params(p)
    A3 = ps3_material_attrs(p) if V2 else {}

    def roles3(name):
        m = m3.get(name)
        if not m:
            return set()
        return {tex_role(texs3[i]['name']) for i in m['textures'] if i is not None} - {None}

    used = collections.Counter()
    out = {}
    # exact first (greedy: prefer paks that cover many materials)
    cand = {}
    for n in names:
        k = ps4key(n)
        cand[n] = [(pk, sz, m) for pk, sz, m in S.get(k, []) if pk not in exclude and m['s'] == 'prime-shader.fx'] or \
                  [(pk, sz, m) for pk, sz, m in S.get(k, []) if pk not in exclude]
    cover = collections.Counter()
    for n, cs in cand.items():
        for pk in {c[0] for c in cs}:
            cover[pk] += 1
    for n in names:
        cs = cand[n]
        if cs:
            pk, sz, m = max(cs, key=lambda c: (cover[c[0]], -c[1]))
            out[n] = dict(key=ps4key(n), pak=pk, exact=True, name=m['n'], binds=m['b'], ps3params=P3.get(n, {}))
            used[pk] += 1
    # fallbacks
    alpha_tex = ps3_alpha_textures(p)
    for n in names:
        if n in out:
            continue
        if n in water:
            # water material without a PS4 twin: the closest PS4 water.fx material (same directory first)
            wbest = None
            # fixer L (n005 / batch98-water): the old rule matched the directory, then took the SMALLEST pak, so every PS3
            # water without a directory match got levels/cruise-ship/water/world-water-displacement (no water program, no
            # textures: Oasis, Village, Warzone, co-op Dig) and Old Quarter's algae water got the clear Yemen market fountain.
            # Now: score every retail water.fx template (water_templates.json: params / features / binds, extracted from the
            # retail paks) against the PS3 material: PS3 constant names (packed names split) vs template constant names,
            # texture roles (diffuse colour, diffuse mask = algae / murk, foam, env cube, shadow map), and a water program;
            # ties: shared textures, directory, smaller pak. MKPLAN_WATER_TEX=0 = old rule.
            wtpl = None
            if os.environ.get('MKPLAN_WATER_TEX', '1') != '0':
                wp = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'water_templates.json')
                if os.path.exists(wp):
                    wtpl = json.load(open(wp))
            if wtpl is not None:
                import waterscore
                tx3 = []
                for i_ in (m3[n]['textures'] if m3.get(n) else []):
                    if i_ is None or i_ >= len(texs3):
                        continue
                    tx3.append(re.sub(r'/[0-9A-F]{16}\.ndb.*$', '', texs3[i_]['name']).rsplit('/', 1)[-1])
                cands = {}
                for k, lst in S.items():
                    for pk, sz, m in lst:
                        if pk in exclude or m['s'] != 'water.fx' or k not in wtpl:
                            continue
                        if k not in cands or sz < cands[k][1]:
                            cands[k] = (pk, sz, m)
                psz = {c[0]: c[1] for c in cands.values()}
                W_ = {k: dict(wtpl[k], pak=cands[k][0]) for k in cands}
                if W_:
                    _, k = waterscore.pick(n, (P3.get(n) or {}).keys(), tx3, W_, psz)
                    pk, sz, m = cands[k]
                    wbest = (None, k, pk, m)
            if wbest is None:
                for k, lst in S.items():
                    for pk, sz, m in lst:
                        if pk in exclude or m['s'] != 'water.fx':
                            continue
                        kc = k.split(':')[0].split('/'); nc = n.split('/')[:-1]
                        common = 0
                        for a_, b_ in zip(nc, kc):
                            if a_ != b_:
                                break
                            common += 1
                        key = (common, n.rsplit('/', 1)[1] == k.rsplit(':', 1)[-1], -sz)
                        if wbest is None or key > wbest[0]:
                            wbest = (key, k, pk, m)
            if wbest:
                _, k, pk, m = wbest
                out[n] = dict(key=k, pak=pk, exact=False, name=m['n'], binds=m['b'], roles=[], cls=['water'],
                              ps3params=P3.get(n, {}))
                used[pk] += 1
                continue
        r3 = roles3(n)
        c3 = material_class(n, m3.get(n), texs3, alpha_tex)
        fdir = n.rsplit('/', 1)[0]
        top = '/'.join(n.split('/')[:2])
        ncomp = n.split('/')[:-1]
        best = None
        for k, lst in S.items():
            for pk, sz, m in lst:
                if pk in exclude or m['s'] != 'prime-shader.fx':
                    continue
                if 'shadowblocker' in k and 'shadowblocker' not in n:
                    continue
                r4 = {param_role(b[0]) for b in m['b']} - {None, 'lightmap', 'env'}
                score = len(r3 & r4) * 2 - len(r4 - r3) - len(r3 - r4)
                f4 = set(m.get('f', []))
                c4 = {'cut'} if 'TRANSPARENCY_CUTOFF' in f4 else set()
                if 'TRANSPARENCY_ALPHA_BLEND' in f4:
                    c4.add('blend')
                if 'GLOBAL_LEAF' in f4:
                    c4.add('leaf')
                # render class (alpha test / alpha blend / foliage) matters more than the path
                score += 12 if ('cut' in c3) == ('cut' in c4) else -12
                score += 8 if ('blend' in c3) == ('blend' in c4) else -8
                score += 4 if ('leaf' in c3) == ('leaf' in c4) else -2
                if 'cut' in c3 and 'trans' not in r3 and 'TRANSPARENCY_MAP_USE_SHARED_TEXTURE_1_A' in f4:
                    score += 6            # alpha taken from the colour map, like the PS3 texture
                kc = k.split(':')[0].split('/')
                common = 0
                for a_, b_ in zip(ncomp, kc):
                    if a_ != b_:
                        break
                    common += 1
                if k.split(':')[0] == fdir:
                    score += 20 if not V2 else 8
                elif k.startswith(top):
                    score += (3 + 2 * max(0, common - 2)) if not V2 else 1 + max(0, common - 2)
                if ('trans' in r3) != ('trans' in r4):
                    score -= 5
                # shader-variant signature: the PS3 material's constants vs the params the template's feature set carries
                s3 = set(P3.get(n, {})) - GENERIC_P
                if s3:
                    p4 = predicted_params(f4) - GENERIC_P
                    score += 4 * len(s3 & p4) - 3 * len(s3 - p4) - 2 * len(p4 - s3)
                # PS3 level materials carry no wind/leaf-bend constants: never pick a vertex-animated (wind) variant,
                # and a leaf variant only for real foliage (else floors and walls sway)
                if any(w in ff for ff in f4 for w in WIND_FEATS):
                    score -= 40
                if 'GLOBAL_LEAF' in f4 and 'leaf' not in c3:
                    score -= 30
                if V2:
                    score += v2_penalty(f4, r3, c3, A3.get(n, set()), None, n, P3.get(n, {}))
                    if V23:
                        score += v23_score(f4, P3.get(n, {}))
                    score -= 2 * len(r4 - r3)          # template-only texture roles get flat fillers
                score += 0.5 * (used[pk] > 0)
                if _DEBUG is not None and n == _DEBUG.get('mat'):
                    _DEBUG.setdefault('cands', []).append((score, k, pk, sorted(f4)))
                key = (score, -sz)
                if best is None or key > best[0]:
                    best = (key, k, pk, m)
        if best is None:
            raise SystemExit('no fallback material for %s' % n)
        _, k, pk, m = best
        out[n] = dict(key=k, pak=pk, exact=False, name=m['n'], binds=m['b'], roles=sorted(r3), cls=sorted(c3),
                      ps3params=P3.get(n, {}))
        used[pk] += 1
    # textures: every PS3 texture and every texture bound by a chosen PS4 material
    want = {}
    shorts = [short_of(t['path']) for t in p.textures()]
    for e in out.values():
        shorts += [b[1] for b in e['binds']]
    for s in shorts:
        if s in want or s not in T:
            continue
        lst = T[s]
        pk, full, f = max(lst, key=lambda x: (used[x[0]] > 0, -paksize.get(x[0], 1 << 40)))
        want[s] = (pk, full)
    req = dict(mats=sorted({(e['pak'], e['key']) for e in out.values()}),
               tex=sorted(set(want.values())))
    if verbose:
        ne = sum(e['exact'] for e in out.values())
        print('%s: %d materials, %d exact, %d fallback; %d source paks; %d textures requested'
              % (stage, len(out), ne, len(out) - ne, len({e['pak'] for e in out.values()}), len(req['tex'])))
        for n, e in out.items():
            if not e['exact']:
                print('  fallback %s -> %s (%s) roles %s class %s' % (n, e['key'], e['pak'], e['roles'], e.get('cls')))
    return dict(stage=stage, mats=out, requests=req)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ps3', nargs='+')
    ap.add_argument('--exclude', nargs='*', default=[])
    ap.add_argument('--res', default=RES)
    ap.add_argument('-o', '--out', required=True, help='plan.json (one stage) or directory')
    a = ap.parse_args()
    for f in a.ps3:
        pl = plan(f, a.exclude, a.res)
        o = a.out if not os.path.isdir(a.out) else os.path.join(a.out, pl['stage'] + '.plan.json')
        json.dump(pl, open(o, 'w'), indent=1)


if __name__ == '__main__':
    main()
