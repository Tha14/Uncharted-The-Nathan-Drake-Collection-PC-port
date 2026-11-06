#!/usr/bin/env python3
"""Rebuild a map's PC-built BG levels locally (current mkplan/ps4stage) from the staged PS3 sources + bundle7, into
mapb's WORK/<map>/pc/ (which mapb prefers over build7).  usage: rebuild_pc_bg.py map [map ...]"""
import sys, os, json, glob, time
sys.path[:0] = [os.path.dirname(os.path.abspath(__file__)), os.path.join(os.path.dirname(os.path.abspath(__file__)), 'stage')]
import mkplan, ps4stage
S = '/mnt/user-data/uploads/Uncharted-windows/_claude_transfer/maps'
WORK = '/tmp/claude-0/-home-claude/528884b1-f615-54ea-bcbe-b8ead4764d56/scratchpad/mapsb'
for arg in sys.argv[1:]:
    key, only = (arg.split(":", 1) + [None])[:2]
    od = os.path.join(WORK, key, 'pc'); os.makedirs(od, exist_ok=True)
    for rp in sorted(glob.glob('%s/build7/%s/*.report.json' % (S, key))):
        lv = os.path.basename(rp)[:-len('.report.json')]
        if only and lv != only: continue
        src = '%s/ps3/%s/%s.pak' % (S, key, lv)
        bf = '%s/bundle7/%s/%s.bundle.json' % (S, key, lv)
        if not (os.path.exists(src) and os.path.exists(bf)):
            print(key, lv, 'SKIP (source or bundle missing)', flush=True); continue
        t = time.time()
        pf = '%s/req7/%s/%s.plan.json' % (S, key, lv)
        P = json.load(open(pf)) if os.path.exists(pf) else mkplan.plan(src, (), ps4stage.RES, verbose=False)   # PC plan when staged
        B = json.load(open(bf))
        have = {(m.get('pak'), m.get('key')) for m in B['mats']}
        need = {(e['pak'], e['key']) for e in P['mats'].values()}
        if need - have:
            print(key, lv, 'PLAN MISMATCH vs bundle7 (%d picks missing) - keeping the PC build' % len(need - have), flush=True); continue
        op = os.path.join(od, lv + '.pak')
        d, r = ps4stage.convert(src, P, B, op, verbose=False)
        json.dump(r, open(op[:-4] + '.report.json', 'w'))
        print(key, lv, os.path.getsize(op), 'consts', r.get('ps3params'), 'capped', len(r.get('capped', [])), '%.0fs' % (time.time() - t), flush=True)
