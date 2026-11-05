#!/usr/bin/env python3
"""Batch of 38: mesh and texture comparison with the best retail paks. Writes rbatch.json."""
import sys, os, pickle, json
HERE = os.path.dirname(os.path.abspath(__file__)); os.chdir(HERE); sys.path.insert(0, HERE); sys.dont_write_bytecode = True
import numpy as np
from lib import *
import ps4actor as A, ps3geom
from ps3mat_am import read_materials
from rsig import sig
Rr = pickle.load(open('sig_retail.pkl', 'rb'))
un = [(r, u, a, m) for r in Rr if 'err' not in r for u, (a, m) in r['units'].items()]
s3 = S3()


def tex_of_ps3(n):
    src, d = s3.get(n); p = Pak(d); M, t = read_materials(p); used = set()
    for v in ps3geom.prototypes(p).values():
        used |= set(A.ps3_block_materials(p, v['proto']))
    return {m['name']: sorted({t[i]['name'].split('/')[-2] for i in m['textures']}) for m in M if m['name'] in used}, src


def tex_of_ps4(f):
    q = Pak(f); out = {}
    for m in A.tmpl_materials(q):
        out[m['name'].split(',')[0]] = sorted({b[1].split('/')[-1] if not b[1].endswith('.ndb') else b[1].split('/')[-2] for b in m['binds']})
    return out


res = {}
for fn in sorted(json.load(open('manifest.json'))):
    n = fn[:-4]; t = sig(f'{HERE}/out/{fn}'); a = np.unique(np.concatenate([v[0] for v in t['units'].values()]))
    rows = []
    for r, u, ua, um in un:
        c = len(np.intersect1d(a, ua, assume_unique=True))
        if c:
            rows.append((c / len(a), c / len(ua), r['name'], u, (r['skel'], r['hier'], r['jh']) == (t['skel'], t['hier'], t['jh']), r['nfg'], ua))
    rows.sort(key=lambda x: -(x[0] * x[1]))
    same = [x for x in rows if x[4]]
    # greedy union of same-skeleton retail units
    left = a; parts = []
    for _ in range(4):
        best = max(same, key=lambda x: (len(np.intersect1d(left, x[6], assume_unique=True)), x[1]), default=None)
        if not best: break
        g = len(np.intersect1d(left, best[6], assume_unique=True))
        if g / len(a) < 0.03: break
        parts.append((best[2], round(g / len(a), 3), round(best[1], 3))); left = np.setdiff1d(left, best[6], assume_unique=True)
    pm, src = tex_of_ps3(n)
    e = dict(src=src, nverts=len(a), top=[(round(x[0], 3), round(x[1], 3), x[2], x[3], x[4], x[5]) for x in rows[:3]], union=parts, union_cov=round(1 - len(left) / len(a), 3), mats=pm)
    b = same[0] if same else None
    if b:
        rt = tex_of_ps4(RET + '/' + b[2] + '.pak')
        keyp = {k.rsplit('/', 1)[0] + ':' + k.rsplit('/', 1)[1]: v for k, v in pm.items()}
        e['best'] = b[2]; e['mat_only_ps3'] = sorted(k for k in keyp if k not in rt and 'shadowblocker' not in k); e['mat_only_ret'] = sorted(k for k in rt if k not in keyp and 'shadowblocker' not in k)
        allp = {x for v in pm.values() for x in v}; allr = {x for v in rt.values() for x in v}
        e['tex_only_ps3'] = sorted(allp - allr); e['tex_only_ret'] = sorted(allr - allp)
    res[n] = e
    print(n, e['src'], len(a), 'union', e['union_cov'], parts)
    print('   top', e['top'][:2])
    if b:
        print('   mat-only-ps3', [x.split('/')[-1] for x in e['mat_only_ps3']], 'mat-only-ret', [x.split('/')[-1] for x in e['mat_only_ret']])
        print('   tex-only-ps3', e['tex_only_ps3'][:8], 'tex-only-ret', e['tex_only_ret'][:8])
json.dump(res, open('rbatch.json', 'w'), indent=0)
