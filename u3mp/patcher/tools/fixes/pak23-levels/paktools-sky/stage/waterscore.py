"""waterscore.py (fixer L, n005): score retail water.fx templates against a PS3 water material (used by mkplan)."""
import re
def split_names(names):
    out = set()
    for k in names:
        parts = k[2:].split('_') if k.startswith('g_') else []
        if len(parts) > 1 and all(re.match(r'^[a-z][A-Z0-9]', s) for s in parts):
            out |= {'g_' + s for s in parts}
        else:
            out.add(k)
    return out
def ps3_texinfo(texnames):
    t = [n.lower() for n in texnames if 'missingtexture' not in n]
    foam = any('foam' in n for n in t)
    diff = any(re.search(r'-(color|diffuse|col)\.', n) and 'foam' not in n for n in t)
    dmask = any(re.search(r'-(mask|trans\w*|alpha)\.', n) and 'foam' not in n and 'default-mask' not in n and 'shadow' not in n for n in t)
    env = any(n.endswith('.dds') for n in t)
    shadow = any('shadow' in n for n in t)
    return dict(diff=diff, dmask=dmask, foam=foam, env=env, shadow=shadow)
def score(p3names, texnames, tpl):
    P = split_names(p3names); T = split_names(tpl['params'])
    s = 2 * len(P & T) - len(T - P) - len(P - T)
    ti = ps3_texinfo(texnames); b = {x[0] for x in tpl['binds']}; f = set(tpl['feats'])
    tt = dict(diff='g_tDiffuseColorMap' in b, dmask='g_tNdFetchDiffuseMaskMap' in b, foam='ND_FOAM' in f,
              env='ND_SPECULAR_ENVMAP' in f, shadow='ND_WATER_TEXTURE_SHADOWS' in f)
    for k, w in (('diff', 8), ('dmask', 5), ('foam', 4), ('env', 3), ('shadow', 2)):
        s += w if ti[k] == tt[k] else -w
    if 'ND_WATER_REFRACTIONS' not in f:
        s -= 100                          # not a water surface program (world-water-displacement)
    return s
def pick(name, p3names, texnames, W, paksize=None):
    """best retail water template key for a PS3 water material: score, then shared textures, then directory, then pak size"""
    tb = {t.rsplit('/', 1)[-1] for t in texnames}
    best = None
    for k, t in W.items():
        s = score(p3names, texnames, t)
        tov = len(tb & {b[1].rsplit('/', 1)[-1] for b in t['binds']})
        kc = k.split(':')[0].split('/'); nc = name.split('/')[:-1]; common = 0
        for a, b in zip(nc, kc):
            if a != b: break
            common += 1
        key = (s, tov, common, -(paksize or {}).get(t['pak'], 0), k)
        if best is None or key > best[0]:
            best = (key, k)
    return best
