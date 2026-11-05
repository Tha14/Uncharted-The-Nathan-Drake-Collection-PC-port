#!/usr/bin/env python3
"""Rebuild every head-wear pak the survey (scan_tint.tsv) marked TINT-LOST, trying retail templates on the same skeleton
in rank order (build2.candidates) and keeping the first build that passes every gate. Writes out/*.pak and batch.json.
Only this folder is written."""
import sys, os, re, json, time, subprocess, pickle, hashlib
HERE = os.path.dirname(os.path.abspath(__file__)); os.chdir(HERE)
AM = os.path.join(HERE, '..', 'actor-materials'); sys.path.insert(0, AM); sys.path.insert(0, HERE); sys.dont_write_bytecode = True
from lib import *
import ps4actor as A, ps3geom
from build import role, SPECIAL
from build_hat import gate, fg, BUDGET
os.chdir(HERE)   # build.py chdirs to actor-materials on import
R = pickle.load(open(AM + '/retail.pkl', 'rb')); s3 = S3()
TINTPAKS = json.load(open('tintpaks.json'))     # retail paks with a tintable, overlay-free material (tintpaks.py)
HEAD = re.compile(r'hat|helmet|mask|goggles|glasses|crown|headband|wrap|monocle|tiara|veil|headdress|wings|bunny|hood|beret|turban|bandana|-cap|ascot|scarf|jester|party|horns|ears|brain|tree|pads', re.I)
lost = {}
for l in open('scan_tint.tsv'):
    n, lay, v, _ = l.rstrip('\n').split('\t')
    if v in ('TINT-LOST', 'TINT-EXTRA'):
        lost.setdefault(n, lay)
AMOUT = os.path.join(AM, 'out')
only = sys.argv[1:]
names = [n for n in sorted(lost) if not only or n in only]
print(len(names), 'paks', flush=True)


def candidates(name, old):
    _, d = s3.get(name); p = Pak(d); ms = set()
    for v in ps3geom.prototypes(p).values():
        ms |= set(A.ps3_block_materials(p, v['proto']))
    keys = {m.rsplit('/', 1)[0] + ':' + m.rsplit('/', 1)[1] for m in ms}
    need = {role(m) for m in ms if 'shadowblocker' not in m} & set(SPECIAL)
    f = fg(old); c = []
    from ps3mat_am import read_materials
    any_mask = any('tint-mask' in t['name'] for t in read_materials(p)[1])   # a part without any PS3 mask may use any template
    for b, r in R.items():
        if (r['skel'], r['hier'], r['jh']) != f[:2] + f[3:4] or (any_mask and b not in TINTPAKS):
            continue
        names_ = [x[0] for x in r['mats']]
        tint = any(x[2] & 0 == 0 for x in r['mats'])      # (feature bits are not in retail.pkl; checked by the gate)
        have = {x for x in need if any(re.search(SPECIAL[x], n.lower()) for n in names_)}
        sb = any('shadowblocker' in n for n in names_)
        c.append((len(have), len(keys & set(names_)), sb, -len(names_), b))
    out = [x[-1] for x in sorted(c, reverse=True)[:8]]
    j = os.path.join(AMOUT, name + '.json')          # a part actor-materials already rebuilt keeps its template first
    if os.path.exists(j):
        t = json.load(open(j)).get('template')
        if t and t in R:
            out = [t] + [x for x in out if x != t]
    return out


res = json.load(open('batch.json')) if os.path.exists('batch.json') else {}
for name in names:
    if name in res and res[name].get('ok'):
        continue
    oldf, lay = find('actor33', name); old = Pak(oldf); dst = f'out/{name}.pak'; why = []; ok = False
    try:
        cands = candidates(name, old)
    except Exception as e:
        res[name] = dict(failed=['candidates: ' + str(e)[:120]]); print('SKIP', name, res[name], flush=True); continue
    for t in cands:
        for cap in (256, 128, 64):
            t0 = time.time()
            try:
                r = subprocess.run([sys.executable, '-I', 'build_hat.py', name, t, str(cap)], capture_output=True, text=True, timeout=900)
            except subprocess.TimeoutExpired:
                why.append((t, cap, 'timeout')); r = None; break
            if r.returncode:
                why.append((t, cap, 'build failed', (r.stdout + r.stderr).strip()[-200:])); break
            if Pak(dst).u32(0x20) <= BUDGET:
                break
        if r is None or r.returncode:
            continue
        bad, info = gate(name, dst, verbose=False)
        gained = [k for k, v in info['tint_new'].items() if v[0] and v[1]]   # the gate itself checks tint against the PS3 masks
        if bad:
            why.append((t, cap, bad)); continue
        ok = True
        res[name] = dict(ok=True, template=t, cap=cap, layer=lay, vram=info['vram'], check=info['check'], crash=info['crash'], size=info['size'],
                         tint_new={k: v for k, v in info['tint_new'].items()}, tint_old={k: v for k, v in info['tint_old'].items()}, tried=why, secs=int(time.time() - t0))
        print('OK  ', name, t, 'cap', cap, 'vram %#x->%#x' % info['vram'], 'check', info['check'], 'gained', [g.split('/')[-1] for g in gained], flush=True)
        break
    if not ok:
        res[name] = dict(ok=False, failed=why, layer=lay); print('SKIP', name, why[:3], flush=True)
        for x in (dst, f'out/{name}.json'):
            if os.path.exists(x): os.remove(x)
    json.dump(res, open('batch.json', 'w'), indent=0, default=str)
print('done', sum(1 for v in res.values() if v.get('ok')), 'ok of', len(res))
