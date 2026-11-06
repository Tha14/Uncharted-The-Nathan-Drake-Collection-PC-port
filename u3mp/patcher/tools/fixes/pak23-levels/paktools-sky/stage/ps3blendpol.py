"""ps3blendpol (fixer W batch103): blend-map polarity of a PS3 material from its NV40 fragment program (main pass).
PS3 and PS4 (BLEND_FACTOR_INVERT_BLEND_MAP) both decide per material whether the blend map enters as map or (1 - map):
'inv' = the program computes (1 - map) * K (MAD x, -map.y, K, K).  Checked against 2188 retail exact twins: never 'inv' where
retail lacks INVERT (no false positives); misses ~20% of inverted ones (they read as 'non'), so only 'inv' is acted on."""
import re, struct
import ps3params, ps3fp_nv40 as fp
_cache = {}
def _ops(l):
    m = re.match(r'(\w+) (\S+?),? (.*)$', l)
    if not m: return None, None, []
    return m.group(1), m.group(2).rstrip(','), [x.strip() for x in m.group(3).replace('  (end)', '').split(', ')]
def _base(x): return x.lstrip('-|').split('.')[0].rstrip('|')
def polarity(L):
    tex = set()
    for l in L:
        op, d, a = _ops(l)
        if not op: continue
        if op.startswith('TEX') and d and d.endswith('.y'): tex.add(_base(d))
        if op.startswith('MAD') and len(a) >= 3 and a[0].startswith('-') and a[1] == a[2] and _base(a[0]) in tex \
                and '.' in a[0] and a[0].lstrip('-').split('.')[1].startswith('y'):
            return 'inv'
    return 'non'
def material_polarity(p, name):
    key = id(p)
    if key not in _cache:
        mt = p.items_of('MATERIAL_TABLE_1')
        tab = {}
        if mt:
            mt = mt[0]; pg, b = mt.page, mt.data_off
            nmat = p.pu32(pg, b + 0xc); ma = p.resolve(pg, b + 0x20); sr = p.resolve(pg, b + 0x1c)
            for i in range(nmat):
                m = p.resolve(ma[0], ma[1] + 4 * i); nm = p.pstr(*p.resolve(*m))
                try:
                    W = ps3params._walk(p, m)
                    if not W: continue
                    node = W[0][2]
                    pid = struct.unpack_from('>H', p.d, p.pbase(node[0]) + node[1])[0]
                    t = p.resolve(sr[0], sr[1] + 0x10 * pid + 12)
                    if not t: continue
                    B = p.pbase(t[0]) + t[1]; hd = struct.unpack_from('>8I', p.d, B)
                    tab[nm] = polarity(fp.dis(p.d[B + hd[4]:B + hd[4] + hd[0]]))
                except Exception:
                    pass
        _cache[key] = tab
    return _cache[key].get(name)
