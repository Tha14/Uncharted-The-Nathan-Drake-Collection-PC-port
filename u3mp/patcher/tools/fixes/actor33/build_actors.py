#!/usr/bin/env python3
"""Rebuild the converted actor paks (actor33/ps4) of overlay-port / -r2 / -r3 / -mp, and overlay-mp's net.pak,
from the user's PS3 + PS4 files, with the converter version, PS3 source, retail template and options that
reproduce each reference file (jobs.tsv, recovered 2026-11-05 by rebuilding and comparing sha256).

  build_actors.py [--apply] [--overlays port,r2,r3,mp] [--only name,...] [--allow-equivalent] [--jobs N]

jobs.tsv columns: overlay, name, sha256, status, method, toolset, ps3 source, template, mode, cap, env, post
  status   identical  = the build reproduces the reference file byte for byte (installed only on a sha256 match)
           base       = reproduces the pre-session state (work/backup-*); a later recipe step (actor-materials,
                        hat-colour, airport-igc2) replaces it, so it is only written when the file is missing
  env      json: extra PS4ACTOR_* variables (PS4ACTOR_DONORS = ':'-separated retail pak names)
  method   ps4actor | anim | rename | wpnmods | convert_net | lab
  toolset  toolsets/<name>/ (paktools snapshots: git commits of fidalgojoao/Uncharted2-Multiplayer-Dev and the
           recovered cloud variants, see toolsets/SOURCES.txt)
  post     cubefix, cb3fix (stage/ post passes), '|solo' = template directory holding only the template
Writes {{WORK}}/overlay-<o>/u3data/build/main/actor33/ps4/<name> (backup of a replaced file in
{{WORK}}/backup-actor33/). Idempotent: a file already at its sha256 is left alone. Dry run without --apply.
Needs numpy; own-skeleton props need the Havok reflection tables (step actors-hk-tables)."""
import argparse, hashlib, json, os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = "{{WORK}}"
PS4 = "{{PS4}}"
PS3_DISC = "{{PS3_DISC}}"
PS3_UPDATE = "{{PS3_UPDATE}}"
PS3_DATA2 = "{{PS3_DATA2}}"
SCRATCH = "{{SCRATCH}}"
TT = "{{TRANSFER_TOOLS}}"
RET = PS4 + '/u3data/build/main/actor33/ps4'
TMP = SCRATCH + '/actor33-work'
OVL = {'port': 'overlay-port', 'r2': 'overlay-r2', 'r3': 'overlay-r3', 'mp': 'overlay-mp'}


def sha(f):
    h = hashlib.sha256()
    with open(f, 'rb') as fh:
        for b in iter(lambda: fh.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def inputs_signature(folders):
    """Paths + size / mtime of every .psarc (and net18.bin) under the PS3 input folders (two levels deep)."""
    sig = []
    for f in folders:
        sig.append(f)
        for dp, dn, fn in os.walk(f) if f and os.path.isdir(f) else ():
            if os.path.relpath(dp, f).replace('\\', '/').count('/') >= 3:
                dn[:] = []
            for n in sorted(fn):
                if n.endswith('.psarc') or n == 'net18.bin':
                    st = os.stat(os.path.join(dp, n))
                    sig.append('%s %d %d' % (os.path.relpath(os.path.join(dp, n), f), st.st_size, int(st.st_mtime)))
    return hashlib.sha256('\n'.join(sig).encode('utf-8', 'replace')).hexdigest()


def drop_stale_ps3_caches():
    """The extracted PS3 files and the Lab extract are kept between runs; when the PS3 inputs changed (another
    folder, the game data downloaded or replaced), they are rebuilt (an old Lab extract made without the Lab
    psarcs left 27 Lab props failing, 2026-11-07)."""
    stamp = os.path.join(TMP, '.ps3-inputs')
    sig = inputs_signature([PS3_DISC, PS3_UPDATE, PS3_DATA2])
    old = open(stamp).read().strip() if os.path.exists(stamp) else None
    if old != sig:
        for d in ('ps3', 'labsrc'):
            shutil.rmtree(os.path.join(TMP, d), ignore_errors=True)
        os.makedirs(TMP, exist_ok=True)
        open(stamp, 'w').write(sig)


def ps3_sources():
    sys.path.insert(0, TT)
    from psarc_lib import Psarc
    arcs = [('disc', PS3_DISC + '/build/main/actor33.psarc'), ('goty-patch', PS3_DISC + '/goty-patch.psarc'),
            ('goty-dlc', PS3_DISC + '/goty-dlc.psarc'), ('dlc-patch13', PS3_UPDATE + '/dlc-patch13.psarc'),
            ('dlc-patch18', PS3_UPDATE + '/dlc-patch18.psarc'), ('data2-parts', PS3_DATA2 + '/build/main/actor-parts-7.psarc')]
    idx = {}
    for k, p in arcs:
        if os.path.exists(p):
            idx[k] = (p, None)
    return idx, Psarc


def extract(src, name, cache={}):
    """PS3 source pak -> TMP/ps3/<src>/<name>"""
    d = os.path.join(TMP, 'ps3', src.replace(':', '_'))
    f = os.path.join(d, name)
    if os.path.exists(f):
        return f
    if 'idx' not in cache:
        cache['idx'], cache['Psarc'] = ps3_sources()
        cache['open'] = {}
    if src == 'labsrc':
        return lab_source(name)
    if src.startswith('pak23:'):         # PS3 level pak (net.pak)
        p = PS3_DISC + '/build/main/pak23.psarc'; nm = name
    else:
        p = cache['idx'][src][0]; nm = name
    if p not in cache['open']:
        a = cache['Psarc'](p)
        cache['open'][p] = (a, {os.path.basename(x): i for i, x in enumerate(a.names)})
    a, ix = cache['open'][p]
    os.makedirs(d, exist_ok=True)
    tmp = '%s.%d.tmp' % (f, os.getpid())
    open(tmp, 'wb').write(a.read(ix[nm] + 1))
    try:
        os.replace(tmp, f)
    except OSError:                     # another --jobs process installed the same extract first
        if not os.path.exists(f):
            raise
        os.remove(tmp)
    return f


def lab_source(name):
    """the Lab art groups (maps-batch): DATA2 lab-trench-v1-7 / lab-multi-01-7 / actor-parts-7 and dlc-patch13,
    sliced per art group as maps-batch/tools/build_paks.py did"""
    d = os.path.join(TMP, 'labsrc')
    if not os.path.exists(os.path.join(d, '.ready')):
        subprocess.run([sys.executable, os.path.join(HERE, 'lib', 'xl.py'), PS3_UPDATE, PS3_DATA2, d], check=True,
                       env=dict(os.environ, PYTHONPATH=TT))
        open(os.path.join(d, '.ready'), 'w').write('ok')
    f = os.path.join(d, 'pak', name)
    if os.path.exists(f):
        return f
    sys.path.insert(0, os.path.join(HERE, 'toolsets', 'f525859')); sys.path.insert(0, os.path.join(HERE, 'lib'))
    import slicepak
    from ndpak import Pak
    for g in ('anim-col-square-tail', 'anim-col-thrown-out', 'anim-london-pub', 'anim-london-pub-bathroom', 'block-20x20x30',
              'cruise-ship-ballroom-90-lod', 'lab-multi-34-pak-ingame12'):
        q = Pak(os.path.join(d, 'pak', g + '.pak'))
        if name[:-4] in slicepak.arts(q):
            os.makedirs(os.path.join(d, 'sliced'), exist_ok=True)
            out = os.path.join(d, 'sliced', name)
            open(out, 'wb').write(slicepak.slice_bytes(q.path, name[:-4]))
            return out
    raise FileNotFoundError('no Lab source for ' + name)


class Worker:
    def __init__(self, tools):
        env = dict(os.environ)
        env.pop('PYTHONPATH', None)
        self.env = env
        self.p = subprocess.Popen([sys.executable, os.path.join(HERE, 'worker.py'), tools], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1, env=env)

    def run(self, job):
        try:
            self.p.stdin.write(json.dumps(job) + '\n'); self.p.stdin.flush()
            return json.loads(self.p.stdout.readline())
        except Exception as ex:          # the worker died: start a new one for the next job
            try:
                self.p.kill()
            except Exception:
                pass
            self.p = subprocess.Popen(self.p.args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1,
                                      env=self.env)
            return dict(err='worker died: %r' % (ex,))


def template(t, solo):
    tp = os.path.join(RET, t + '.pak')
    if not solo:
        return tp
    d = os.path.join(TMP, 'tsolo', t)        # ps4actor reads the retail material GUIDs of the template's folder
    if not os.path.exists(os.path.join(d, t + '.pak')):
        os.makedirs(d, exist_ok=True); shutil.copyfile(tp, os.path.join(d, t + '.pak'))
    return os.path.join(d, t + '.pak')


MODES = {
    'mp': (dict(stream_flags=2), {}), 'mp-sf3': (dict(stream_flags=3), {}), 'mp-sf0': (dict(stream_flags=0), {}),
    'kit': (dict(stream_flags=2, mat_by_file=True, match_streams=True), {}),
    'kit-nomatch': (dict(stream_flags=2, mat_by_file=True), {}),
    'kit-spread': (dict(stream_flags=2, mat_by_file=True, match_streams=True), {'PS4ACTOR_SPREAD_MATS': '1'}),
    'kit-notmpl': (dict(stream_flags=2, mat_by_file=True, match_streams=True), {'PS4ACTOR_NO_TMPL_TEX': '1'}),
    'kit-spread-notmpl': (dict(stream_flags=2, mat_by_file=True, match_streams=True), {'PS4ACTOR_SPREAD_MATS': '1', 'PS4ACTOR_NO_TMPL_TEX': '1'}),
    'graft': (dict(stream_flags=3, mat_by_file=True, match_streams=True, graft=True), {}),
    'graft-spread': (dict(stream_flags=3, mat_by_file=True, match_streams=True, graft=True), {'PS4ACTOR_SPREAD_MATS': '1'}),
    'graft-notmpl': (dict(stream_flags=3, mat_by_file=True, match_streams=True, graft=True), {'PS4ACTOR_NO_TMPL_TEX': '1'}),
    'graft-spread-notmpl': (dict(stream_flags=3, mat_by_file=True, match_streams=True, graft=True), {'PS4ACTOR_SPREAD_MATS': '1', 'PS4ACTOR_NO_TMPL_TEX': '1'}),
    'own': (dict(stream_flags=3, mat_by_file=True, match_streams=True, own_skel=True), {}),
    'own-spread': (dict(stream_flags=3, mat_by_file=True, match_streams=True, own_skel=True), {'PS4ACTOR_SPREAD_MATS': '1'}),
    'own-notmpl': (dict(stream_flags=3, mat_by_file=True, match_streams=True, own_skel=True), {'PS4ACTOR_NO_TMPL_TEX': '1'}),
    'own-spread-notmpl': (dict(stream_flags=3, mat_by_file=True, match_streams=True, own_skel=True), {'PS4ACTOR_SPREAD_MATS': '1', 'PS4ACTOR_NO_TMPL_TEX': '1'}),
}


def build(r, workers):
    """build one job into TMP/out/<overlay>/<name>; returns the path"""
    out = os.path.join(TMP, 'out', r['overlay'], r['name'])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    tools = os.path.join(HERE, 'toolsets', r['toolset'])
    m = r['method']
    if m in ('wpnmods', 'convert_net', 'lab'):
        env = dict(os.environ, PYTHONPATH=os.pathsep.join([tools, tools + '/stage']), PAKTOOLS=tools)
        if m == 'wpnmods':
            src, skin_src, skins = r['template'].split('|')       # sibling skin | added skins
            cmd = [sys.executable, tools + '/wpnmods.py', os.path.join(RET, r['name']), extract(r['source'], r['name']),
                   out + '.w', '--src', skin_src, '--max-tex', r['cap'], '--stream-flags', '2'] + skins.split(',')
            subprocess.run(cmd, check=True, capture_output=True, env=env)
            if 'cb3fix' in r['post']:
                subprocess.run([sys.executable, tools + '/stage/cb3fix.py', out + '.w', out], check=True, capture_output=True, env=env)
                os.remove(out + '.w')
            else:
                os.replace(out + '.w', out)
        elif m == 'convert_net':
            p3 = extract('pak23:', 'net.pak')
            subprocess.run([sys.executable, tools + '/convert_net.py', p3, out, '--ref', PS4 + '/u3data/build/main/pak23/ps4/menu.pak'],
                           check=True, capture_output=True, env=env)
        else:
            code = ('import sys,os; sys.path[:0]=[%r,%r]; import assetbatch; ok,msg,cap=assetbatch.convert((%r,"lab",%r,"own",%r,{}),%r); '
                    'sys.exit(0 if ok else msg)' % (tools, tools + '/stage', r['name'], extract('labsrc', r['name']),
                                                    os.path.join(RET, r['template'] + '.pak'), out))
            subprocess.run([sys.executable, '-c', code], check=True, capture_output=True, env=env, cwd=TMP)
        return out
    if r['toolset'] not in workers:
        workers[r['toolset']] = Worker(tools)
    w = workers[r['toolset']]
    src, _, other = r['source'].partition('/')      # 'dlc-patch18/mp-hero-part-lion-mask.pak': another PS3 pak
    job = dict(ps3=extract(src, other or r['name']) if r['source'] != '-' else '', out=out)
    if m == 'anim':
        job['anim'] = [os.path.join(TMP, 'stm')] if 'stm' in r['mode'] else []
        if job['anim']:
            stm_dir()
    elif m == 'rename':
        job.update(rename=[r['template'], r['name'][:-4]], tmpl=os.path.join(RET, r['template'] + '.pak'),
                   post=[p for p in r['post'].split('|')[0].split('+') if p])
    else:
        kw, env = MODES[r['mode']]
        kw = dict(kw)
        if int(r['cap']):
            kw['max_tex'] = int(r['cap'])      # (c6b291a has no texture cap)
        env = dict(env)
        for k, v in (json.loads(r['env']) if r['env'] not in ('', '-', '{}') else {}).items():
            if k == 'PS4ACTOR_DONORS':          # retail donor paks by name
                v = ':'.join(os.path.join(RET, x + '.pak') for x in v.split(':'))
            elif k == 'PS4ACTOR_SKEL3':         # '<source>/<name>.pak': the PS3 pak whose skeleton a LOD pak uses
                v = extract(*v.split('/', 1))
            env[k] = v
        if '_rename' in env:                    # ps4actor --rename (art group renamed to the overlay name)
            kw['rename'] = env.pop('_rename')
        if '_skel' in env:                      # ps4actor --skel-template (assetbatch opt skel=): skeleton from that retail pak
            kw['skel_tmpl'] = os.path.join(RET, env.pop('_skel') + '.pak')
        job.update(tmpl=template(r['template'], '|solo' in r['post']), kw=kw, env=env,
                   post=[p for p in r['post'].split('|')[0].split('+') if p])
    res = w.run(job)
    if res.get('err'):
        raise RuntimeError(res['err'])
    return out


def stm_dir():
    d = os.path.join(TMP, 'stm')
    if os.path.exists(os.path.join(d, '.ready')):
        return d
    sys.path.insert(0, TT)
    from psarc_lib import Psarc
    a = Psarc(PS3_DISC + '/build/main/animstream4.psarc')
    os.makedirs(d, exist_ok=True)
    for i, n in enumerate(a.names):
        open(os.path.join(d, os.path.basename(n)), 'wb').write(a.read(i + 1))
    open(os.path.join(d, '.ready'), 'w').write('ok')
    return d


_WORKERS = {}


def _build_job(k_r):
    """process-pool entry (--jobs): one row, with this process's own per-toolset Workers"""
    k, r = k_r
    try:
        return k, build(r, _WORKERS), None
    except Exception as ex:
        return k, None, str(ex)[:200]



def shadowed():
    """(overlay, path) pairs a higher overlay hides (the game never loads them): rebuild_overlays writes them to
    $RECIPE_SHADOWED unless it runs with --all-files.  Empty set = build everything."""
    f = os.environ.get('RECIPE_SHADOWED')
    if not f or not os.path.exists(f):
        return set()
    return {tuple(l.rstrip('\r\n').split('\t', 1)) for l in open(f, encoding='utf-8') if '\t' in l}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--overlays', default='port,r2,r3,mp')
    ap.add_argument('--only', default='')
    ap.add_argument('--allow-equivalent', action='store_true')
    ap.add_argument('--jobs', type=int, default=int(os.environ.get('RECIPE_JOBS') or os.cpu_count() or 1),
                    help='parallel build processes (default: all cores); installs stay in jobs.tsv order')
    a = ap.parse_args()
    drop_stale_ps3_caches()
    ovs = {OVL[x] for x in a.overlays.split(',')}
    only = {x if x.endswith('.pak') else x + '.pak' for x in a.only.split(',') if x}
    cols = 'overlay name sha256 status method toolset source template mode cap env post'.split()
    rows = [dict(zip(cols, l.rstrip('\n').split('\t'))) for l in open(os.path.join(HERE, 'jobs.tsv')) if l.strip() and not l.startswith('#')]
    rows = [r for r in rows if r['overlay'] in ovs and (not only or r['name'] in only)]
    hidden = shadowed()
    if hidden:
        n0 = len(rows)
        rows = [r for r in rows if (r['overlay'], 'u3data/build/main/%s/ps4/%s' % ('pak23' if r['method'] == 'convert_net' else 'actor33', r['name'])) not in hidden]
        if n0 != len(rows):
            print('%d jobs skipped (hidden by a higher overlay, the game never loads them)' % (n0 - len(rows)), flush=True)
    # Havok reflection tables (step actors-hk-tables keeps them in TMP/hk: the job folder is re-rendered per step)
    for t in os.listdir(os.path.join(HERE, 'toolsets')):
        td = os.path.join(HERE, 'toolsets', t)
        if os.path.exists(os.path.join(td, 'hkconvert.py')):
            os.makedirs(os.path.join(td, 'data'), exist_ok=True)
            for k in ('hk2010_ps3.json', 'hk2011_ps4.json'):
                src = os.path.join(TMP, 'hk', k)
                if os.path.exists(src):
                    shutil.copyfile(src, os.path.join(td, 'data', k))
                elif not os.path.exists(os.path.join(td, 'data', k)):
                    open(os.path.join(td, 'data', k), 'w').write('{}')   # stub: own-skeleton props with collision fail
    workers = {}
    t0 = time.time(); c = dict(same=0, installed=0, built_diff=0, fail=0, skipped=0)
    todo = []                       # (index, row, dst) still to build
    for r in rows:
        sub = 'pak23' if r['method'] == 'convert_net' else 'actor33'
        dst = os.path.join(WORK, r['overlay'], 'u3data', 'build', 'main', sub, 'ps4', r['name'])
        if os.path.exists(dst) and sha(dst) == r['sha256']:
            c['same'] += 1; continue
        if r['status'] == 'base' and os.path.exists(dst):
            c['same'] += 1; continue          # a later recipe step owns this file now
        if r['status'] not in ('identical', 'base') and not a.allow_equivalent:
            c['skipped'] += 1; print('skip (%s)' % r['status'], r['overlay'], r['name']); continue
        todo.append((len(todo), r, dst))
    # build: ps4actor / anim / rename rows in a process pool (each process keeps its own Workers; rows grouped by
    # toolset so a process mostly needs one); wpnmods / convert_net / lab rows (own subprocesses, shared temp dirs)
    # and the one-time setups (animstream extract, Lab sources) run here first. Results are installed below in
    # jobs.tsv order, so the output does not depend on --jobs.
    results = {}
    par = [(k, r) for k, r, _d in todo if r['method'] not in ('wpnmods', 'convert_net', 'lab')]
    if any('stm' in r['mode'] for _k, r in par if r['method'] == 'anim'):
        stm_dir()
    for k, r, _d in todo:
        if (k, r) not in par:
            try:
                results[k] = (build(r, workers), None)
            except Exception as ex:
                results[k] = (None, str(ex)[:200])
    if a.jobs > 1 and len(par) > 1:
        from concurrent.futures import ProcessPoolExecutor
        par.sort(key=lambda kr: (kr[1]['toolset'], kr[0]))
        with ProcessPoolExecutor(max_workers=min(a.jobs, len(par))) as ex:
            for k, out, err in ex.map(_build_job, par, chunksize=max(1, min(16, len(par) // (4 * a.jobs) or 1))):
                results[k] = (out, err)
    else:
        for k, r in par:
            try:
                results[k] = (build(r, workers), None)
            except Exception as ex:
                results[k] = (None, str(ex)[:200])
    for k, r, dst in todo:
        out, err = results[k]
        if err is not None:
            c['fail'] += 1; print('FAIL', r['overlay'], r['name'], err, flush=True); continue
        h = sha(out)
        if h != r['sha256'] and r['status'] in ('identical', 'base'):
            c['built_diff'] += 1; print('DIFF', r['overlay'], r['name'], h[:16], '!=', r['sha256'][:16], flush=True); continue
        if not a.apply:
            print('would install', r['overlay'], r['name'], flush=True); continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.exists(dst):
            bk = os.path.join(WORK, 'backup-actor33', r['overlay'], r['name'])
            if not os.path.exists(bk):
                os.makedirs(os.path.dirname(bk), exist_ok=True); shutil.copy2(dst, bk)
        shutil.copyfile(out, dst + '.tmp'); os.replace(dst + '.tmp', dst)
        c['installed'] += 1
    print('actor33: %s, %.0f s%s' % (', '.join('%s %d' % kv for kv in c.items()), time.time() - t0, '' if a.apply else ' (dry run)'))
    sys.exit(1 if c['fail'] or c['built_diff'] else 0)


if __name__ == '__main__':
    main()
