#!/usr/bin/env python3
"""Cluster det/*.json hits (connected within 1.5 m, per pak) and split them into tiers. Writes hits.tsv and plan.json.
default: wall-like cluster from a wide/strip/other non-stage companion, single-use submeshes, no hole risk, area 0.5..150 m2,
         every triangle under 40 m2, and the cluster partly overlaps main/collision (some triangle with fex<0.95): a leftover attached
         to the real structure, not a free-standing far-field object.
review:  everything else with at least 0.5 m2 of excess. Never applied unless named on the command line."""
import json, glob, os, re
import numpy as np


def clusters(rows):
    p = list(range(len(rows)))

    def f(a):
        while p[a] != a:
            p[a] = p[p[a]]; a = p[a]
        return a
    lo = np.array([r['lo'] for r in rows]); hi = np.array([r['hi'] for r in rows])
    for i in range(len(rows)):
        g = np.where(((lo[i + 1:] <= hi[i] + 1.5) & (hi[i + 1:] >= lo[i] - 1.5)).all(1))[0] + i + 1
        for j in g:
            p[f(i)] = f(int(j))
    out = {}
    for i, r in enumerate(rows):
        out.setdefault(f(i), []).append(r)
    return list(out.values())


def main():
    plan = {}; tsv = []; n = {}
    for fn in sorted(glob.glob('det/*.json')):
        if '.control' in fn:
            continue
        m = os.path.basename(fn)[:-5]
        for h in json.load(open(fn)):
            if not h['rows']:
                continue
            for k, cl in enumerate(clusters(h['rows'])):
                ex = sum(r['area'] * r['fex'] for r in cl)
                if ex < .5:
                    continue
                lo = np.min([r['lo'] for r in cl], 0).round(1).tolist(); hi = np.max([r['hi'] for r in cl], 0).round(1).tolist()
                mats = sorted({r['mat'].split(':')[0] for r in cl})
                comp = h['pak'][len(m) + 1:-4] if h['pak'].startswith(m) else h['pak']
                ok = ('stage' not in h['pak'] and 'moving' not in h['pak'] and lo[1] < 4 and all(r['uses'] == 1 for r in cl) and max(r['fhole'] for r in cl) < .05 and ex <= 150
                      and max(r['area'] for r in cl) < 40 and min(r['fex'] for r in cl) < .95 and not re.search('wide|far|mountain|sand|rock|cliff|ground', ' '.join(mats)))
                tier = 'default' if ok else 'review'
                cid = '%s/%s#%d' % (m, comp, k)
                tsv.append([tier, cid, h['pak'], round(ex, 1), len(cl), lo, hi, ','.join(mats)[:70]]); n[(m, tier)] = n.get((m, tier), 0) + 1
                e = plan.setdefault(h['pak'], dict(md5=h['md5'], bytes=h['bytes'], clusters={}))
                e['clusters'][cid] = dict(tier=tier, lo=lo, hi=hi, excess=round(ex, 1), tris=[[r['off'], *r['idx']] for r in cl])
    tsv.sort(key=lambda r: (r[0], -r[3]))
    open('hits.tsv', 'w').write('\n'.join('\t'.join(map(str, r)) for r in tsv) + '\n')
    json.dump(plan, open('plan.json', 'w'))
    for r in tsv[:60]:
        print(*r)
    print(n)


if __name__ == '__main__':
    main()
