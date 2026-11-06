"""ps3specmask (fixer H h028): the PS3 spec mask of a BG material, for templates without a spec-map slot.
mask_scale(p, name) -> mean of the mask channel the PS3 main-pass NV40 program reads from the material's '-spec*' texture
(gamma-decoded when the sampler ref has the gamma bit; falls back to the RGB mean, linear, when the sampler-unit mapping is
not certain), or None when the material has no '-spec' texture.  The exact fix is a template with COLOR_4_APPLY_MASK_TEXTURE;
this keeps the average spec / env strength at PS3's level (white reflections otherwise: Facility ice floor x15, London trims x10)."""
import re
import numpy as np
import ps3blendsrc as BS
import texcodec as tc
from mkplan import read_materials

_c = {}


def _lin(v): return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def mask_scale(p, name):
    key = (id(p), name)
    if key in _c and _c[key][0] is p: return _c[key][1]
    out = None
    try:
        mm, texs = read_materials(p)
        m = [x for x in mm if x['name'] == name]
        specs = [texs[i] for i in m[0]['textures'] if re.search(r'-spec', texs[i]['name'].lower())] if m else []
        if specs:
            full = specs[0]['name']; short = full.split('/')[-2] if '/' in full else full
            chan, gam = 'rgb', False
            e = BS._table(p).get(name)
            if e:
                L, nref = e
                um, _w = BS.unit_map(nref, L)
                ri = [k for k, r in enumerate(nref) if r[0] == short]
                if um and ri:
                    inv = {v: k for k, v in um.items()}
                    u = inv.get(ri[0])
                    ch = [mm_.group(2) for l in L for mm_ in [re.search(r'^TEX\w* (\w+)\.(\w+),.*tex(\d+)$', l)] if mm_ and int(mm_.group(3)) == u]
                    if ch and len(ch[0]) == 1:
                        chan = ch[0]; gam = bool(((nref[ri[0]][1][2] >> 20) >> 'xyzw'.index(chan)) & 1)
            t = [x for x in p.textures() if '/' + short + '/' in x['path']]
            if t:
                t = t[0]; kind, bpb, comp = tc.ps3_format(t)
                if comp:
                    w, h = t['width'], t['height']
                    img = tc.decode(p.vram_bytes(t['vramOffset'], t['vramSize'])[:tc.ps3_linear_size(w, h, 1, bpb)], w, h, kind).astype(np.float64)
                    v = (img[..., 'xyzw'.index(chan)] if chan != 'rgb' else img[..., :3].mean(-1)) / 255.0
                    out = float((_lin(v) if gam else v).mean())
    except Exception:
        out = None
    _c[key] = (p, out)
    return out
