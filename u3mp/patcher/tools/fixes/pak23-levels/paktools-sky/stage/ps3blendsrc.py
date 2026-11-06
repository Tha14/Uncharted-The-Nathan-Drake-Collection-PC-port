"""ps3blendsrc (fixer W batch104): where a PS3 material's blend factor comes from, read from its NV40 fragment program.

The PS3 main pass computes f = sat(t + M/(2s+0.01)) (t = vertex term from the VP, same maths as the PS4 prime shader).
For materials without a '-blend' texture M is still sampled, from ONE CHANNEL of the texture on sampler unit 0:
the material's normal map (.y or .z holds a height mask) or a colour map (.x/.y/.z).  Others blend by vertex colour
only (f = sat(1 - v)).  blend_source(p, name) returns
    {'kind': 'tex', 'full': <PS3 texture path after texture1/>, 'chan': 'x'|'y'|'z'|'w', 'pol': 'straight'|'inv',
     'gamma': bool}        M = channel (pol 'inv': 1 - channel), gamma-decoded when the PS3 sampler had the gamma bit
    {'kind': 'vc-only'}     f = sat(1 - v)
    None                    no blend factor of these forms / mapping not certain

Sampler units -> node texture refs (node +0x18, 0x10-byte entries {ref, addr word, 0, gamma word}): the three lighting
buffers are three consecutive 'missingtexture' refs (units 12-14/15); refs after them take the used units below 12 in
ascending order without unit 0, then a ref before them (at most one, usually the normal map) or else the last ref takes
unit 0.  Checked on all 49 resolved rows: every unit sampled as .xyz maps to a colour/detail/env texture, and the gamma
word (bits 20-23 = R,G,B,A) is set exactly on colour refs (shared colour unit: set; separate blend-slot ref: clear)."""
import re, struct
import numpy as np
import ps3params, ps3fp_nv40 as fp
from mkplan import read_materials
import texcodec as tc

_KF = re.compile(r'^\{([^}]*)\}\.(\w+)$')


def _ops(l):
    m = re.match(r'(\w+) (\S+?),? (.*)$', l)
    if not m: return None, None, []
    return m.group(1), m.group(2).rstrip(','), [x.strip() for x in m.group(3).replace('  (end)', '').split(', ')]


def _base(x): return x.lstrip('-|').split('.')[0].rstrip('|')


def _kval(x):
    m = _KF.match(x.lstrip('-'))
    if not m: return None
    v = [float(t) for t in m.group(1).split(',')]
    return v['xyzw'.index(m.group(2)[0])] * (-1 if x.startswith('-') else 1)


def _comp(x, c):
    x2 = x.lstrip('-|').rstrip('|')
    if '.' not in x2: return c
    s = x2.split('.')[1]
    if len(s) == 1: return s
    k = 'xyzw'.index(c)
    return s[k] if len(s) > k else s[-1]


def _scal(x):
    x = x.lstrip('-|').rstrip('|')
    return x.split('.')[1][0] if '.' in x else 'x'


def _writer(P, reg, c, before):
    """index of the last write of reg.c before `before`; -1 when an aliasing R/H write intervenes"""
    for j in range(before - 1, -1, -1):
        op, d, a, l = P[j]
        if not d: continue
        b = _base(d); m = d.split('.')[1] if '.' in d else 'xyzw'
        if b == reg and c in m: return j
        if b[:1] == 'R' and reg[:1] == 'H' and b[1:].isdigit() and reg[1:].isdigit() and int(reg[1:]) // 2 == int(b[1:]):
            k = int(reg[1:]); hit = {'x': 'xy', 'y': 'zw'} if k % 2 == 0 else {'z': 'xy', 'w': 'zw'}
            if any(rc in m and c in hc for rc, hc in hit.items()): return -1
        if b[:1] == 'H' and reg[:1] == 'R' and b[1:].isdigit() and reg[1:].isdigit() and int(b[1:]) // 2 == int(reg[1:]):
            k = int(b[1:]); hc = {'x': 'xy', 'y': 'zw', 'z': 'xy', 'w': 'zw'}[c]
            if (k % 2 == 0) == (c in 'xy') and any(z in m for z in hc): return -1
    return None


def chain(L):
    """(dict(unit, chan, pol, s), None) or (None, reason)"""
    P = [_ops(l) + (l,) for l in L]
    for i, (op, d, a, l) in enumerate(P):
        if op == 'ADD_SAT' and d and '.' in d and len(d.split('.')[1]) == 1:
            vt = [x for x in a if re.match(r'-?f\[TEX\d\]', x)]; hr = [x for x in a if x.lstrip('-|')[:1] in 'HR']
            if len(vt) == 1 and len(hr) == 1: break
    else:
        return None, 'no ADD_SAT(vterm, H)'
    dm = d.split('.')[1]; x = hr[0]
    if x.startswith('-'): return None, 'neg H term'
    j = _writer(P, _base(x), _comp(x, dm), i)
    if j is None or j < 0: return None, 'no writer of H term'
    op, d2, a2, l2 = P[j]
    dm2 = d2.split('.')[1]

    def two_s(reg, c, before):
        jd = _writer(P, reg, c, before)
        if jd is None or jd < 0: return None
        o3, d3, a3, _ = P[jd]
        if o3 == 'MAD' and _kval(a3[1]) == 2 and abs((_kval(a3[2]) or 0) - 0.01) < 1e-4:
            js = _writer(P, _base(a3[0]), _comp(a3[0], d3.split('.')[1]), jd)
            if js is not None and js >= 0 and P[js][0] == 'MOV': return _kval(P[js][2][0])
        return None
    if op == 'DIV':
        num, den = a2[0], a2[1]; s = two_s(_base(den), _scal(den), j); inv_mad = False
    elif op == 'MAD' and a2[0].startswith('-') and a2[1] == a2[2] and a2[1][:1] in 'HR':
        num = a2[0][1:]; jr = _writer(P, _base(a2[1]), _comp(a2[1], dm2), j); s = None
        if jr is not None and jr >= 0 and P[jr][0] == 'RCP':
            x = P[jr][2][0]; s = two_s(_base(x), _scal(x), jr)
        inv_mad = True
    else:
        return None, 'H term not DIV / MAD-inv'
    if s is None: return None, 'denominator not 2s+0.01'
    pol = 'inv' if inv_mad else 'straight'
    if num.startswith('-'): return None, 'neg num'
    c = _comp(num, dm2)
    jn = _writer(P, _base(num), c, j)
    if jn is None or jn < 0: return None, 'no num writer'
    o4, d4, a4, l4 = P[jn]
    if o4 == 'ADD' and a4[0].startswith('-') and _kval(a4[1]) == 1:
        pol = 'straight' if inv_mad else 'inv'
        inner = a4[0]; c = _comp(inner, c)
        jn = _writer(P, _base(inner), c, jn)
        if jn is None or jn < 0: return None, 'no inv writer'
        o4, d4, a4, l4 = P[jn]
    m = re.search(r'tex(\d+)$', l4)
    if not (o4 == 'TEX' and m): return None, 'num not TEX'
    u = int(m.group(1))
    if u >= 12: return None, 'lighting unit'
    return dict(unit=u, chan=c, pol=pol, s=s), None


def vc_only(L):
    for l in L:
        op, d, a = _ops(l)
        if op == 'ADD_SAT' and len(a) == 2 and re.match(r'-f\[TEX\d\]', a[0]) and _kval(a[1]) == 1:
            return True
    return False


def unit_map(nref, L):
    """unit -> ref index ({}, reason) per the rule in the module doc"""
    used = sorted({int(m.group(1)) for l in L for m in [re.search(r'tex(\d+)$', l)] if m})
    U = [x for x in used if x < 12]
    names = [n for n, _f, _full in nref]
    idx = [k for k, r in enumerate(names) if r == 'missingtexture'][:3]
    if len(idx) < 3 or idx != list(range(idx[0], idx[0] + 3)): return None, 'lighting refs not contiguous'
    if idx[0] > 1: return None, 'more than one ref before the lighting refs'
    S = list(range(idx[2] + 1, len(names))) + list(range(0, idx[0]))
    if len(S) != len(U): return None, 'ref/unit count %d/%d' % (len(S), len(U))
    order = [x for x in U if x] + ([0] if 0 in U else [])
    um = dict(zip(order, S))
    for l in L:                                           # sanity: colour-sampled units are colour textures
        m = re.search(r'^TEX\w* \w+\.(\w+),.*tex(\d+)$', l)
        if m and int(m.group(2)) < 12 and m.group(1) == 'xyz':
            if not re.search(r'color|detail|reflection|env', names[um[int(m.group(2))]]): return None, 'xyz unit not colour'
    return um, None


_cache = {}


def _table(p):
    key = id(p)
    if key in _cache and _cache[key][0] is p: return _cache[key][1]
    tab = {}
    mt = p.items_of('MATERIAL_TABLE_1')
    if mt:
        mt = mt[0]; pg, b = mt.page, mt.data_off
        nmat = p.pu32(pg, b + 0xc); ma = p.resolve(pg, b + 0x20); sr = p.resolve(pg, b + 0x1c); th = p.resolve(pg, b + 0x18)
        _m, texs = read_materials(p)
        for i in range(nmat):
            m = p.resolve(ma[0], ma[1] + 4 * i); nm = p.pstr(*p.resolve(*m))
            if nm in tab: continue
            try:
                W = ps3params._walk(p, m)
                if not W: continue
                node = W[0][2]
                pid = struct.unpack_from('>H', p.d, p.pbase(node[0]) + node[1])[0]
                t = p.resolve(sr[0], sr[1] + 0x10 * pid + 12)
                if not t: continue
                B = p.pbase(t[0]) + t[1]; hd = struct.unpack_from('>8I', p.d, B)
                L = fp.dis(p.d[B + hd[4]:B + hd[4] + hd[0]])
                tl = p.resolve(node[0], node[1] + 0x18); nref = []
                for y in range(0, 0x200, 0x10):
                    r = p.resolve(tl[0], tl[1] + y) if tl else None
                    if not (r and r[0] == th[0] and th[1] <= r[1] < th[1] + 4 * len(texs)): break
                    full = texs[(r[1] - th[1]) // 4]['name']
                    fl = struct.unpack_from('>III', p.d, p.pbase(tl[0]) + tl[1] + y + 4)
                    nref.append((full.split('/')[-2] if '/' in full else full, fl, full))
                tab[nm] = (L, nref)
            except Exception:
                pass
    _cache[key] = (p, tab)          # keep p alive: a reused id() must not hit another pak's table
    return tab


def blend_source(p, name):
    e = _table(p).get(name)
    if not e: return None
    L, nref = e
    ch, why = chain(L)
    if ch is None:
        texu = {int(m.group(1)) for l in L for m in [re.search(r'tex(\d+)$', l)] if m and int(m.group(1)) < 12}
        return {'kind': 'vc-only'} if vc_only(L) and len(texu) >= 2 else None
    um, why = unit_map(nref, L)
    if um is None or ch['unit'] not in um: return None
    n_, fl, full = nref[um[ch['unit']]]
    return dict(kind='tex', full=full, chan=ch['chan'], pol=ch['pol'],
                gamma=bool(((fl[2] >> 20) >> 'xyzw'.index(ch['chan'])) & 1))


# ---------------------------------------------------------------- texel derivation (grey BC1 UNORM)
def _srgb2lin(v):
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def bc1_grey(img):
    """(h, w) uint8 -> BC1 blocks (linear order), 4-colour mode, value in G at 6 bits (R/B at 5)"""
    h, w = img.shape
    ph, pw = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    a = np.pad(img, ((0, ph - h), (0, pw - w)), mode='edge').astype(np.int32)
    bl = a.reshape(ph // 4, 4, pw // 4, 4).transpose(0, 2, 1, 3).reshape(ph // 4, pw // 4, 16)
    hi, lo = bl.max(-1), bl.min(-1)
    g0, g1 = (hi * 63 + 127) // 255, (lo * 63 + 127) // 255
    r0, r1 = (hi * 31 + 127) // 255, (lo * 31 + 127) // 255
    c0 = (r0 << 11) | (g0 << 5) | r0
    c1 = (r1 << 11) | (g1 << 5) | r1
    flat = g0 == g1
    c1 = np.where(flat, c0, c1)
    e0, e1 = (g0 << 2) | (g0 >> 4), (g1 << 2) | (g1 >> 4)
    pal = np.stack([e0, e1, (2 * e0 + e1) // 3, (e0 + 2 * e1) // 3], -1)
    idx = np.abs(bl[..., None] - pal[..., None, :]).argmin(-1)
    idx = np.where(flat[..., None], 0, idx)
    bits = (idx.astype(np.uint64) << (2 * np.arange(16, dtype=np.uint64))).sum(-1)
    out = np.zeros(bl.shape[:2] + (8,), np.uint8)
    out[..., 0], out[..., 1] = c0 & 255, c0 >> 8
    out[..., 2], out[..., 3] = c1 & 255, c1 >> 8
    for k in range(4):
        out[..., 4 + k] = (bits >> np.uint64(8 * k)) & np.uint64(255)
    return out.tobytes()


def derive_texels(p, t, chan, gamma, invert):
    """PS3 texture desc t -> (PS4-tiled BC1 texels, w, h, mips): one channel as grey, full mip chain"""
    kind, bpb, comp = tc.ps3_format(t)
    if not comp: return None
    w, h = t['width'], t['height']
    raw = p.vram_bytes(t['vramOffset'], t['vramSize'])
    img = tc.decode(raw[:tc.ps3_linear_size(w, h, 1, bpb)], w, h, kind)
    rm = t['gcmRemapCtl'] & 0xffff
    if rm != 0xaae4:
        img = tc.gcm_remap(np.ascontiguousarray(img[..., [3, 0, 1, 2]]), rm)
    v = img[..., 'xyzw'.index(chan)].astype(np.float64) / 255.0
    if gamma: v = _srgb2lin(v)
    if invert: v = 1.0 - v
    levels = []
    lv = v
    while True:
        levels.append(bc1_grey(np.clip(np.round(lv * 255), 0, 255).astype(np.uint8)))
        if lv.shape[0] == 1 and lv.shape[1] == 1: break
        if lv.shape[0] > 1 and lv.shape[1] > 1:
            lv = (lv[0::2, 0::2] + lv[1::2, 0::2] + lv[0::2, 1::2] + lv[1::2, 1::2]) / 4
        elif lv.shape[0] > 1:
            lv = (lv[0::2] + lv[1::2]) / 2
        else:
            lv = (lv[:, 0::2] + lv[:, 1::2]) / 2
    return tc.ps3_to_ps4_texels(b''.join(levels), w, h, len(levels), 8), w, h, len(levels)
