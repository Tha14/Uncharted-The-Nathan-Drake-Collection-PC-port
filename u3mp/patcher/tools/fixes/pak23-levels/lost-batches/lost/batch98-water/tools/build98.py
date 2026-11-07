#!/usr/bin/env python3
"""build98.py (fixer L, n005): rebuild a level pak exactly like build91 (batch91-v23 chain: ps4stage -> paramfix -> cubefix ->
windfix -> sbflag), from the fx15 plan (optionally with the water materials re-planned) and the fx15 bundle re-extracted on
the PC (+ the retail water templates bundle).  usage: build98.py OUTDIR PLANDIR BUNDLEDIR level..."""
import sys, os, json, traceback, hashlib
SCN = '/tmp/claude-0/-home-claude/528884b1-f615-54ea-bcbe-b8ead4764d56/scratchpad/nL2'
sys.path[:0] = ['/home/claude/u3/paktools', '/home/claude/u3/paktools/stage', '/home/claude/u3/aga/maps/b57', '/home/claude/u3/aga/maps/b88',
                '/home/claude/u3/agm', '/home/claude/u3/aga/maps/b82', '/home/claude/u3/aga/maps/b92', SCN + '/py']
import ps4stage, paramfix, cubefix, crashcheck, holescan, sbflag, windfix
from bindcheck import check as bindcheck
from ps3src import ps3path
def md5(p): return hashlib.md5(open(p, 'rb').read()).hexdigest()
def merge(B, extra):
    have = {(m['pak'], m['key']) for m in B['mats']}
    for m in extra['mats']:
        if (m['pak'], m['key']) not in have: B['mats'].append(m)
    for k, v in extra['techs'].items(): B['techs'].setdefault(k, v)
    B['tex'] += extra.get('tex', [])
    return B
def run(out, plans, buns, lvs):
    os.makedirs(out, exist_ok=True); rp = out + '/report.json'
    rep = json.load(open(rp)) if os.path.exists(rp) else {}
    W = json.load(open(SCN + '/wbundle.json'))
    for lv in lvs:
        f = lv + '.pak'; r = rep[lv] = {}
        try:
            P = json.load(open(plans + '/' + lv + '.plan.json')); B = merge(json.load(open(buns + '/' + lv + '.bundle.json')), W)
            bad = [m for m in B['mats'] if 'err' in m or not m.get('rec')]
            if bad: r['error'] = 'bundle errors %s' % bad[:2]; continue
            assert not lv.startswith("coop-syria"), "gobo levels need selfbuild.gobo_tex"
            sp = ps3path(lv); op = out + '/' + f; r['src'] = sp
            data, srep = ps4stage.convert(sp, P, B, op, verbose=False)
            r['paramfix'] = len(paramfix.fix(op, P)); r['cubefix'] = cubefix.fix(op, sp)
            wt = windfix.targets(op, plans + '/' + lv + '.plan.json')
            if wt:
                import windoff; tmp = op + '.wf'; r['windfix'] = list(windoff.patch(op, tmp, [x + ',' for x in wt])); os.replace(tmp, op)
            r['sbflag'] = len(sbflag.patch(op, op + '.sb', sp)); os.replace(op + '.sb', op)
            e, _ = crashcheck.check(op); fi = crashcheck.fidelity(sp, op)
            h = holescan.scan(op); r['holes'] = None if h is None else len(h); r['bind'] = sum(len(v) for v in bindcheck(op).values())
            r.update(crash=e, fidelity=fi, md5=md5(op), size=os.path.getsize(op), water=srep.get('water'), water_objects=srep.get('water_objects'),
                     approx_water=[a for a in srep['approx'] if 'water' in a.lower()])
            print(lv, r['md5'], 'crash', e[:2] if e else 'clean', 'fid', fi[:2] if fi else 'clean', 'holes', r['holes'], 'bind', r['bind'], flush=True)
        except Exception as ex:
            r['error'] = repr(ex); traceback.print_exc()
        json.dump(rep, open(rp, 'w'), indent=1, default=str)
if __name__ == '__main__':
    run(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:])
