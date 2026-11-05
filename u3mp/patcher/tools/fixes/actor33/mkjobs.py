"""Turn the search results (found-*.tsv) + the hand-recovered jobs (weapons, net.pak, Lab boxes) into actor33/jobs.tsv.
usage: mkjobs.py <test root> <recipe manifest.tsv> <work dir> <out jobs.tsv> <out gaps.tsv>
A manifest actor pak with a byte-identical build gets status identical. An overlay-r3 pak that a session step
(actor-materials / hat-colour / airport-igc2 / wall-emblem) replaced gets its pre-session state (work/backup-*) as
status base when that state was reproduced. Everything else goes to gaps.tsv."""
import glob, json, os, sys
root, manifest, work, out, gaps = sys.argv[1:6]
found = {}
for f in glob.glob(os.path.join(root, 'found-*.tsv')):
    for l in open(f):
        x = l.rstrip('\n').split('\t')
        if len(x) > 4 and x[3] == 'MATCH':
            found[(x[1], x[2])] = x[4:11]
man = [l.rstrip('\n').split('\t') for l in open(manifest)][1:]
man = [m for m in man if '/actor33/' in m[1]]
SESSION = {}
for job, bk in (('actor-materials', 'backup-actor-materials'), ('hat-colour', 'backup-hat-colour'), ('airport-igc2', 'backup-igc2')):
    d = os.path.join(work, bk)
    st = json.load(open(os.path.join(d, 'state.json'))) if os.path.exists(os.path.join(d, 'state.json')) else None
    for n in os.listdir(d):
        if n.endswith('.pak'):
            SESSION.setdefault(n, []).append((job, os.path.join(d, n)))
    if st:
        for n, had in st.items():
            SESSION.setdefault(n, [])
    if job == 'airport-igc2':
        SESSION.setdefault('mp-hero-flynn-coop.pak', [])
import hashlib
sha = lambda f: hashlib.sha256(open(f, 'rb').read()).hexdigest()
# session outputs (final r3 state comes from those steps)
outs = {}
for j in ('actor-materials', 'hat-colour'):
    for k, v in json.load(open(os.path.join(work, '..', 'PKG Converter', 'tools', 'fixes', j, 'manifest.json'))).items():
        outs.setdefault(k, set()).add(v)
outs.setdefault('mp-hero-flynn-coop.pak', set()).add('686e426036295d94699f8566f44fdd423bcbe9a1d14ce48b7ddc1757692bc0f6')
emb = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'emblem-actors.json')))
for k, v in emb.items():
    outs.setdefault(k, set()).add(v['reference'])
EXTRA = {}     # (overlay, name) -> job row tail, hand-recovered (scratch/actor-rebuild/search_wpn.py, labtest2.py)
WPN = {('overlay-r3', 'ak74-mp.pak'): ('dlc-patch18', 'cutter', 'hello-doggy,jacko,snakeyes,trumpet', 'cb3fix'),
       ('overlay-r3', 'dragunov-mp.pak'): ('dlc-patch18', 'draza', 'hello-doggy,jacko,snakeyes,trumpet', 'cb3fix'),
       ('overlay-r3', 'fn-fal-mp.pak'): ('dlc-patch18', 'lod0', 'hello-doggy,jacko,snakeyes,trumpet', 'cb3fix'),
       ('overlay-r3', 'galil-mp.pak'): ('dlc-patch18', 'flynn', 'jacko,snakeyes', 'cb3fix'),
       ('overlay-r3', 'hk416-mp.pak'): ('dlc-patch18', 'flynn', 'jeremy,jacko,snakeyes', 'cb3fix'),
       ('overlay-r3', 'machinegun-mp.pak'): ('dlc-patch18', 'navarro', 'snake,nd,jacko,snakeyes', 'cb3fix'),
       ('overlay-r3', 'p99-mp.pak'): ('disc', 'pax', 'snake,nd', 'cb3fix'),
       ('overlay-port', 'hk416-mp.pak'): ('dlc-patch18', 'flynn', 'jeremy', ''),
       ('overlay-port', 'machinegun-mp.pak'): ('dlc-patch18', 'navarro', 'snake,nd', ''),
       ('overlay-port', 'p99-mp.pak'): ('disc', 'pax', 'snake,nd', '')}
LAB = ['lt-box%02d.pak' % i for i in list(range(21)) + [31, 32, 33, 36, 37]]
rows, gap = [], []
for ov, path, size, h, mt in man:
    n = os.path.basename(path)
    key = (ov, n)
    if key in WPN:
        src, sib, skins, post = WPN[key]
        rows.append([ov, n, h, 'identical', 'wpnmods', '7d45c1f', src, '%s|%s|%s' % (n, sib, skins), '-', '256', '{}', post])
        continue
    if ov == 'overlay-r3' and (n in LAB or n.startswith('sd')) and n not in found:
        rows.append([ov, n, h, 'identical', 'lab', 'f525859', 'labsrc', 'crate-wood-1x1x1-a', 'own', '256', '{}', 'cubefix+cb3fix'])
        continue
    f = found.get((n, h))
    status = 'identical'
    if f is None and ov == 'overlay-r3' and n in outs and h in outs[n]:
        # final state from a session step: build the pre-session state when it was reproduced
        for job, bp in SESSION.get(n, []):
            pass
        bk = [p for j, p in SESSION.get(n, [])]
        f = None
        for p in sorted(bk):
            g = found.get((n, sha(p)))
            if g:
                f = g; h = sha(p); status = 'base'; break
        if f is None:
            continue            # new file of a session step, or base not reproduced: the step's output is the file
    if f is None:
        gap.append([ov, n, h]); continue
    ts, src, tm, mode, cap, env, post = f
    ts = {'6db6b8f': '0a20361', 'cb545d9': '7d45c1f', '4c36373': '7d45c1f'}.get(ts, ts)   # same converter files
    env = json.loads(env) if env else {}
    if '+donor:' in mode:
        mode, d = mode.split('+donor:')
        env['PS4ACTOR_DONORS'] = d
    elif 'PS4ACTOR_DONORS' in env:
        env['PS4ACTOR_DONORS'] = ':'.join(os.path.basename(x)[:-4] for x in env['PS4ACTOR_DONORS'].split(':'))
    method = 'anim' if mode.startswith('anim') else 'rename' if mode == 'rename' else 'ps4actor'
    rows.append([ov, n, h, status, method, ts, src, tm, mode, cap if cap != '-' else '0', json.dumps(env, sort_keys=True), post])
rows.append(['overlay-mp', 'net.pak', '15cf61b7fecc60b743f1bcd2f54c232d5233f2b1f3a8520c3aff0db5fac72a66', 'identical', 'convert_net',
             '7d45c1f', 'pak23:', 'menu', '-', '0', '{}', ''])
with open(out, 'w', newline='\n') as fo:
    fo.write('#overlay\tname\tsha256\tstatus\tmethod\ttoolset\tsource\ttemplate\tmode\tcap\tenv\tpost\n')
    for r in rows:
        fo.write('\t'.join(r) + '\n')
with open(gaps, 'w', newline='\n') as fo:
    for g in gap:
        fo.write('\t'.join(g) + '\n')
import collections
print('jobs', len(rows), collections.Counter((r[0], r[3]) for r in rows))
print('gaps', len(gap), collections.Counter(g[0] for g in gap))
