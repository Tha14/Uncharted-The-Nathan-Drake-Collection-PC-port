#!/usr/bin/env python3
"""Match each target pak's LOD0 mesh and materials against every retail FG section. Writes match.pkl; prints the batch."""
import sys, os, pickle, json, re
HERE = os.path.dirname(os.path.abspath(__file__)); os.chdir(HERE); sys.path.insert(0, HERE); sys.dont_write_bytecode = True
import numpy as np
R = pickle.load(open('sig_retail.pkl', 'rb')); Tg = pickle.load(open('sig_targets.pkl', 'rb'))
un = [(r, u, a, m) for r in R if 'err' not in r for u, (a, m) in r['units'].items()]
H = np.concatenate([x[2] for x in un]); I = np.concatenate([np.full(len(x[2]), i) for i, x in enumerate(un)]); ord_ = np.argsort(H); H, I = H[ord_], I[ord_]
sz = np.array([len(x[2]) for x in un])
names = {r['name'] for r in R}


def hits(a):
    lo, hi = np.searchsorted(H, a), np.searchsorted(H, a, 'right')
    c = np.zeros(len(un), dtype=np.int64)
    for l, h in zip(lo, hi):
        if h > l:
            c[I[l:h]] += 1
    return c


def key(m):
    return m


out = {}
for t in Tg:
    if 'err' in t or not t['units']:
        continue
    a = np.unique(np.concatenate([v[0] for v in t['units'].values()])); mats = sorted({x for v in t['units'].values() for x in v[1]})
    c = hits(a); order = np.argsort(-c)[:40]; cand = []
    for i in order:
        if c[i] == 0:
            break
        r, u, ua, um = un[i]
        cand.append(dict(pak=r['name'], unit=u, cov=round(c[i] / len(a), 3), rev=round(c[i] / sz[i], 3),
                         same=(r['skel'], r['hier'], r['jh']) == (t['skel'], t['hier'], t['jh']), nfg=r['nfg'], own=r['own_skel'], anim=r['anim']))
    cand.sort(key=lambda x: -(x['cov'] * x['rev']))
    out[t['name']] = dict(file=t['file'], skel=hex(t['skel']), nj=t['jh'], nverts=len(a), mats=mats, same_name_retail=t['name'] in names, cand=cand[:8], own=t['own_skel'], anim=t['anim'])
pickle.dump(out, open('match.pkl', 'wb'))
b = json.load(open('manifest.json'))
for n in sorted(x[:-4] for x in b):
    o = out.get(n)
    if not o:
        print(n, 'NOT IN TARGETS'); continue
    print(n, o['nverts'], 'same-name-retail' if o['same_name_retail'] else '')
    for c in o['cand'][:4]:
        print('    %-34s %-34s cov %.3f rev %.3f same=%s nfg=%d' % (c['pak'][:34], c['unit'][:34], c['cov'], c['rev'], c['same'], c['nfg']))
