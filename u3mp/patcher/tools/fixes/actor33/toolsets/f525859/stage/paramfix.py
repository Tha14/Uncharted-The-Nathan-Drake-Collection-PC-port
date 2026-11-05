#!/usr/bin/env python3
"""paramfix.py: neutralise the foreign constants a fallback template brings into a converted BG pak (in place, same
size).  Every MP material is a FALLBACK (U3 PS4 retail has no MP levels): ps4stage copies an unrelated PS4 material's
EFFECT and params and only rebinds the textures, then overwrites the params the PS3 material itself carries
(ps3params).  The rest keep the template's values: colour tints / brightness (cyan or washed-out walls), facing
tints, blend-border tints (light "snow" seams), rim light, UV scale / offset (wrong tiling), UV scroll and distortion
(crawling / shimmering floors) and incandescence (glowing non-emissive surfaces).
Rule, per material, only for params the PS3 material does not set itself and only on non-exact materials:
  g_colorTint* = 1,1,1   g_colorBrightness* = 1   g_uvScale* = 1   g_uvOffset* = 0   g_timeScroll* = 0
  g_distortionIntensity* / g_distortionStrength = 0   g_incandescenceStrength = 0 (unless emissive by name)
  g_facingTintIn* = g_facingTintOut* = 1,1,1   g_diffuseColorBorderTint = 1,1,1   g_rim* = 0,0,0
water.fx / sky materials are left alone.  A value array shared with a material that must keep it is not touched.
usage: paramfix.py <pak> <plan.json|-> [out]      (plan gives exact flags + PS3 param names; '-' = treat all as fallback)"""
import json, os, re, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.dirname(HERE)]
from ndpak import Pak

EMISSIVE = re.compile(r'lamp|light|fire|glow|neon|lava|candle|torch|screen|incand|emissive|bulb|sign|flame|ember|sun')
RULES = [(re.compile(r'^g_colorTint\d$'), 1.0), (re.compile(r'^g_colorBrightness\d$'), 1.0),
         (re.compile(r'^g_uvScale\d$'), 1.0), (re.compile(r'^g_uvOffset[UV]\d$'), 0.0),
         (re.compile(r'^g_timeScroll[UV]\d$'), 0.0), (re.compile(r'^g_distortionIntensity\d$|^g_distortionStrength$'), 0.0),
         (re.compile(r'^g_facingTint(In|Out)\d$'), 1.0), (re.compile(r'^g_diffuseColorBorderTint$'), 1.0),
         (re.compile(r'^g_rim\d$'), 0.0), (re.compile(r'^g_incandescenceStrength$'), 0.0)]


def ps3name(n):
    k = n.split(',')[0]
    return k.replace(':', '/') if ':' in k else k


def materials(q):
    g = q.items_of('GEOMETRY_1')
    if not g:
        return []
    g = g[0]; pg, b = g.page, g.data_off
    S = lambda a, o: q.pstr(*q.resolve(a, o)) if q.resolve(a, o) else ''
    nmat = q.u64(q.pbase(pg) + b + 0x48); mr = q.resolve(pg, b + 0x50)
    out = []
    for i in range(nmat if mr else 0):
        mp, mo = mr[0], mr[1] + 0x48 * i
        name, shader = S(mp, mo), S(mp, mo + 8)
        nf, npar, nbind = struct.unpack_from('<3I', q.d, q.pbase(mp) + mo + 0x30)
        pr = q.resolve(mp, mo + 0x20); params = []
        for k in range(npar if pr else 0):
            po = pr[1] + 0x18 * k
            pn = S(pr[0], po); vr = q.resolve(pr[0], po + 8)
            cnt, = struct.unpack_from('<I', q.d, q.pbase(pr[0]) + po + 0x10)
            if vr and 0 < cnt < 64:
                params.append((pn, q.pbase(vr[0]) + vr[1], cnt))
        out.append(dict(name=name, shader=shader, params=params))
    return out


def fix(path, plan=None, out=None, dry=False):
    q = Pak(path); d = bytearray(q.d)
    pm = (plan or {}).get('mats', {})
    ms = materials(q)
    keep_off = set()          # value arrays some material must keep
    todo = []
    for m in ms:
        n3 = ps3name(m['name']); e = pm.get(n3)
        # exact twins and same-file templates (same PS3 file, other variant) keep their values
        samefile = bool(e) and e.get('key', '').split(':')[0] == n3.rsplit('/', 1)[0]
        frozen = m['shader'] != 'prime-shader.fx' or 'sky' in m['name'].split(',')[0].lower() or (e and e.get('exact')) or samefile
        own = set((e or {}).get('ps3params') or {})
        emissive = bool(EMISSIVE.search(n3.rsplit('/', 2)[-2] if '/' in n3 else n3))
        for pn, off, cnt in m['params']:
            val = None
            if not frozen and pn not in own:
                for rx, v in RULES:
                    if rx.match(pn):
                        val = v
                        break
                if pn == 'g_incandescenceStrength' and emissive:
                    val = None
            if val is None:
                keep_off.add(off)
            else:
                todo.append((m['name'].split(',')[0], pn, off, cnt, val))
    changes = []
    for nm, pn, off, cnt, val in todo:
        if off in keep_off:
            continue
        old = struct.unpack_from('<%df' % cnt, d, off)
        new = tuple([val] * min(cnt, 3) + list(old[3:]))      # rgb(a): keep a 4th component
        if any(abs(a - b) > 1e-6 for a, b in zip(old, new)):
            struct.pack_into('<%df' % cnt, d, off, *new)
            changes.append((nm, pn, tuple(round(x, 3) for x in old), new[:3]))
    if changes and not dry:
        open(out or path, 'wb').write(bytes(d))
    return changes


if __name__ == '__main__':
    pl = None if sys.argv[2] == '-' else json.load(open(sys.argv[2]))
    ch = fix(sys.argv[1], pl, sys.argv[3] if len(sys.argv) > 3 else None)
    print(len(ch), 'params neutralised')
    for c in ch[:40]:
        print('  ', c)
