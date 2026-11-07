#!/usr/bin/env python3
"""replanwater.py (fixer L, n005): copy a plan and re-plan ONLY its water (WATER_DATA) fallback materials with mkplan's new
water rule (waterscore + water_templates.json). Everything else stays byte-identical to the source plan.
usage: replanwater.py SRC_PLANDIR OUT_PLANDIR level..."""
import sys, os, re, json
SCN = '/tmp/claude-0/-home-claude/528884b1-f615-54ea-bcbe-b8ead4764d56/scratchpad/nL2'
sys.path[:0] = ['/home/claude/u3/paktools', '/home/claude/u3/paktools/stage', SCN + '/py']
import mkplan, waterdata, ps3params, waterscore
from ndpak import Pak
from ps3src import ps3path
S = mkplan.load_survey(mkplan.RES)
W = json.load(open('/home/claude/u3/paktools/stage/water_templates.json'))
src, dst = sys.argv[1], sys.argv[2]; os.makedirs(dst, exist_ok=True); log = {}
for lv in sys.argv[3:]:
    P = json.load(open(src + '/' + lv + '.plan.json'))
    p = Pak(ps3path(lv)); mats, texs = mkplan.read_materials(p); m3 = {m['name']: m for m in mats}
    P3 = ps3params.all_material_params(p); exclude = {lv + '.pak'}
    for n in waterdata.materials(p):
        e = P['mats'].get(n)
        if e is None or e.get('exact'): continue
        tx = [re.sub(r'/[0-9A-F]{16}\.ndb.*$', '', texs[i]['name']).rsplit('/', 1)[-1] for i in m3[n]['textures'] if i is not None and i < len(texs)]
        cands = {}
        for k, lst in S.items():
            for pk, sz, m in lst:
                if pk in exclude or m['s'] != 'water.fx' or k not in W: continue
                if k not in cands or sz < cands[k][1]: cands[k] = (pk, sz, m)
        Wc = {k: dict(W[k], pak=cands[k][0]) for k in cands}
        (sc, tov, com, _, _), k = waterscore.pick(n, (P3.get(n) or {}).keys(), tx, Wc, {c[0]: c[1] for c in cands.values()})
        pk, sz, m = cands[k]
        old = e['key']
        P['mats'][n] = dict(key=k, pak=pk, exact=False, name=m['n'], binds=m['b'], roles=[], cls=['water'], ps3params=P3.get(n, {}))
        log.setdefault(lv, []).append(dict(mat=n, old=old, new=k, pak=pk, score=sc, old_score=waterscore.score((P3.get(n) or {}).keys(), tx, W[old]) if old in W else None))
    json.dump(P, open(dst + '/' + lv + '.plan.json', 'w'))
json.dump(log, open(dst + '/replan.json', 'w'), indent=1); print(json.dumps(log, indent=1))
