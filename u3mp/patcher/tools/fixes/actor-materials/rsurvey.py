#!/usr/bin/env python3
"""List only: other character paks in overlay-r3 and overlay-port against retail. Changes nothing. Writes retail_survey.tsv."""
import sys, os, pickle, json, re, collections
HERE = os.path.dirname(os.path.abspath(__file__)); os.chdir(HERE); sys.path.insert(0, HERE); sys.dont_write_bytecode = True
from lib import *
import ps4actor as A
from ps3mat_am import read_materials
M = pickle.load(open('match.pkl', 'rb')); T_ = {t['name']: t for t in pickle.load(open('sig_targets.pkl', 'rb'))}; s3 = S3()
batch = {f[:-4] for f in json.load(open('manifest.json'))} | {'mp-hero-flynn-hair'}
CH = re.compile(r'^(mp-hero|mp-chloe|mp-kit|mp-coop|chloe|hero-|sully|sullivan|elena|cutter|talbot|marlowe|eddie|flynn|lazarevic|pirate-|sol-|agent-|desoss|eurothug|.*-coop$)')
SK = collections.Counter(t['skel'] for t in T_.values() if 'skel' in t and t['name'].startswith(('mp-hero', 'mp-chloe', 'mp-kit')))
skels = {s for s, c in SK.items() if c >= 5}
rows = []; cnt = collections.Counter()
for n, o in sorted(M.items()):
    t = T_[n]
    if n in batch or not (CH.match(n) or t['skel'] in skels):
        continue
    lay = 'overlay-r3' if 'overlay-r3' in o['file'] else 'overlay-port'
    same = [c for c in o['cand'] if c['same']]; b = same[0] if same else None; x = o['cand'][0] if o['cand'] else None
    col = ''
    if b and b['cov'] >= 0.9 and b['rev'] >= 0.8:
        _, d = s3.get(n)
        if d:
            mm, tt = read_materials(Pak(d)); p3 = {tt[i]['name'].split('/')[-2] for m in mm for i in m['textures']}
            r4 = {y for m in A.tmpl_materials(Pak(RET + '/' + b['pak'] + '.pak')) for y in ((bb[1].split('/')[-2] if bb[1].endswith('.ndb') else bb[1].split('/')[-1]) for bb in m['binds'])}
            col = ','.join(sorted(v for v in p3 - r4 if 'color' in v))
        else:
            col = 'no-ps3-source'
    if o['same_name_retail']:
        v = 'retail-same-name'
    elif b and b['cov'] >= 0.97 and b['rev'] >= 0.97 and not col:
        v = 'retail-copy'
    elif b and b['cov'] >= 0.9 and b['rev'] >= 0.8:
        v = 'same-mesh-other-texture' if col else 'near-mesh-review'
    elif b and b['cov'] >= 0.6:
        v = 'partial-same-skeleton'
    elif x and x['cov'] >= 0.9:
        v = 'mesh-on-other-skeleton'
    else:
        v = 'no-retail'
    cnt[v] += 1
    rows.append([n, lay, v, b['pak'] if b else '', b['cov'] if b else '', b['rev'] if b else '', col, x['pak'] if x else '', x['unit'] if x else '', x['cov'] if x else '', x['same'] if x else ''])
with open('retail_survey.tsv', 'w') as f:
    f.write('actor\tlayer\tverdict\tbest_same_skeleton\tcov\trev\tps3_color_not_in_retail\tbest_any\tunit\tcov\tsame_skeleton\n')
    for r in rows:
        f.write('\t'.join(map(str, r)) + '\n')
print(len(rows), dict(cnt))
for v in ('retail-same-name', 'retail-copy', 'near-mesh-review'):
    print(v, [(r[0], r[3]) for r in rows if r[2] == v][:60])
print('same-mesh-other', [r[0] for r in rows if r[2] == 'same-mesh-other-texture'][:60])
q = Pak(RET + '/cutter.pak'); print('retail cutter pages', [hex(p[1]) for p in q.pages])
