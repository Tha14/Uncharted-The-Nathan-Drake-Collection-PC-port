#!/usr/bin/env python3
"""Rebuild the converted level paks (pak23/ps4) of overlay-port / overlay-r2 / overlay-r3 and the Lab levels from the
user's own PS3 / PS4 files, file by file as recorded in plan.tsv (converter version, options, PS3 source, sha256).

usage: levels.py <family> [--apply] [--only REGEX] [--strict] [--jobs N] [--keep-work]
  family    port | r2 | r3 | lab | maps-batch   (plan.tsv column 'family')
  --apply   write into {{PROJECT}}/work/<overlay>/u3data/build/main/pak23/ps4 (default: dry run, build + verify only)
  --strict  install only byte-identical results; without it a row of status 'equivalent' is installed when its
            reference file is missing (its sha256 differs from the reference, see plan.tsv / README.md)
Idempotent: a file already equal to the plan's sha256 is skipped ("already applied"). A different existing file is
backed up to {{PROJECT}}/work/backup-pak23-levels/<overlay>/ (never overwriting an existing backup) before install.
Converters (see README.md): f525859 = {{PAKTOOLS}} + paktools-f525859/ overlay files, git snapshots in paktools-<ver>/.
Methods: stage (mkplan -> fxextract -> ps4stage [+ paramfix/cubefix: opts mb]), ingame (ps4ingame), vischeat,
empty (the 248-byte empty PS4 level), copy (another plan output), slice (maps-batch Lab 34 pieces), havok (maps-batch).
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = "{{PROJECT}}"
WORK = os.path.join(PROJECT, 'work')
PAKTOOLS = "{{PAKTOOLS}}"
PS4 = "{{PS4}}"
PS3_DISC = "{{PS3_DISC}}"
PS3_UPDATE = "{{PS3_UPDATE}}"
PS3_DATA2 = "{{PS3_DATA2}}"
PS3_DLCMAPS = "{{PS3_DLCMAPS}}"
PY = sys.executable
TMP = os.path.join(PROJECT, 'scratch', 'pak23-levels-work')
PS4MAIN = os.path.join(PS4, 'u3data', 'build', 'main')
TEXDICT = os.path.join(PS4MAIN, 'texturedict1', 'ps4', 'TextureDictionaries.csv')


def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def run(cmd, env=None, cwd=None):
    p = subprocess.run(cmd, env=env, cwd=cwd, capture_output=True, text=True, errors='replace')
    if p.returncode:
        raise RuntimeError('%s failed (%d): %s %s' % (os.path.basename(cmd[1]), p.returncode, p.stdout[-800:], p.stderr[-1500:]))
    return p.stdout


def plan_rows():
    rows = []
    for l in open(os.path.join(HERE, 'plan.tsv'), encoding='utf-8'):
        if l.startswith('#') or not l.strip():
            continue
        f = l.rstrip('\n').split('\t')
        if f[0] == 'family':
            hdr = f
            continue
        rows.append(dict(zip(hdr, f)))
    return rows


# ------------------------------------------------------------------ converters
_PT_LOCK = threading.Lock()


def _pt_sources(ver):
    """[(relative path in the tree, source file)] of converter version `ver`, later entries overriding earlier ones.
    f525859 = tools/paktools + paktools-f525859/; an OVERLAY_ON version (tt1026 = the 10-26 _claude_transfer/tools
    ps4ingame / ps4writer / ndpak) = the f525859 tree with that version's files on top; others = paktools-<ver>/."""
    if ver == 'f525859' or os.path.exists(os.path.join(HERE, 'paktools-' + ver, 'OVERLAY_ON')):
        roots = [PAKTOOLS, os.path.join(HERE, 'paktools-f525859')] + ([] if ver == 'f525859' else [os.path.join(HERE, 'paktools-' + ver)])
    else:
        roots = [os.path.join(HERE, 'paktools-' + ver)]
    m = {}
    for r in roots:
        for dp, dn, fn in os.walk(r):
            dn[:] = [d for d in dn if d != '__pycache__']
            for f in fn:
                if f == 'OVERLAY_ON' or f.endswith('.pyc'):
                    continue
                s = os.path.join(dp, f)
                m[os.path.relpath(s, r).replace(os.sep, '/')] = s
    m['data/conv_anim_actors.json'] = os.path.join(HERE, 'data', 'conv_anim_actors.json')
    m['data/ps4_anim_actors.json'] = anim_list()
    ps4stage = m.get('stage/ps4stage.py')
    if ps4stage and 'default_lightmaps.json' in open(ps4stage, encoding='utf-8', errors='replace').read():
        m['stage/default_lightmaps.json'] = default_lightmaps()     # made from the player's PS4 copy, never shipped
    return sorted(m.items())


def default_lightmaps():
    """stage/default_lightmaps.json (the two retail default lightmap textures ps4stage binds to unlit props), extracted
    from the user's retail PS4 pak23 by paktools/stage/make_default_lightmaps.py."""
    out = os.path.join(TMP, 'default_lightmaps.json')
    if not os.path.exists(out):
        os.makedirs(TMP, exist_ok=True)
        run([PY, os.path.join(PAKTOOLS, 'stage', 'make_default_lightmaps.py'), os.path.join(PS4MAIN, 'pak23', 'ps4'), out])
    return out


def _pt_manifest(files):
    """Stamp text: format line, then 'size<TAB>relpath' per file (the tree is reused only if this matches exactly)."""
    return 'paktools-tree v2\n' + ''.join('%d\t%s\n' % (os.path.getsize(s), r) for r, s in files)


def paktools(ver):
    """A ready paktools tree for converter version `ver` (with data/ anim actor lists).
    The .ready stamp holds a manifest of the copied files (sizes + paths); a tree whose stamp is old-format or differs
    from the current sources, or whose files no longer match it, is rebuilt.  Built in a temp dir, renamed into place."""
    dst = os.path.join(TMP, 'pt-' + ver)
    with _PT_LOCK:
        files = _pt_sources(ver)
        want = _pt_manifest(files)
        stamp = os.path.join(dst, '.ready')
        if os.path.exists(stamp):
            ok = open(stamp, encoding='utf-8', errors='replace').read() == want
            if ok:
                for r, s in files:
                    p = os.path.join(dst, r)
                    if not os.path.isfile(p) or os.path.getsize(p) != os.path.getsize(s):
                        ok = False
                        break
            if ok:
                return dst
        tmp = '%s.tmp-%d' % (dst, os.getpid())
        if os.path.isdir(tmp):
            shutil.rmtree(tmp)
        for r, s in files:
            d = os.path.join(tmp, r)
            os.makedirs(os.path.dirname(d), exist_ok=True)
            shutil.copyfile(s, d)
        data = os.path.join(tmp, 'data')
        os.makedirs(data, exist_ok=True)
        for f in ('hk2010_ps3.json', 'hk2011_ps4.json'):      # FG collision tables: empty, as in every recorded run
            open(os.path.join(data, f), 'w').write('{}\n')
        open(os.path.join(tmp, '.ready'), 'w', encoding='utf-8').write(want)
        if os.path.isdir(dst):
            old = '%s.old-%d' % (dst, os.getpid())
            os.replace(dst, old)
            shutil.rmtree(old, ignore_errors=True)
        os.replace(tmp, dst)
        return dst


def anim_list():
    """data/ps4_anim_actors.json: retail actor33 paks with an ANIM item (names only), built from the user's PS4 files."""
    out = os.path.join(TMP, 'ps4_anim_actors.json')
    if not os.path.exists(out):
        os.makedirs(TMP, exist_ok=True)
        run([PY, os.path.join(HERE, 'lib', 'anim_actors.py'), PAKTOOLS, os.path.join(PS4MAIN, 'actor33', 'ps4'), out])
    return out


def res_dir():
    """mkplan survey (ps4mats.jsonl.gz, ps4tex.tsv) of the retail PS4 pak23, made with the f525859 reader."""
    d = os.path.join(TMP, 'res')
    if not os.path.exists(os.path.join(d, 'ps4tex.tsv')):
        pt = paktools('f525859')
        run([PY, os.path.join(HERE, 'lib', 'mkres.py'), d, os.path.join(PS4MAIN, 'pak23', 'ps4')], env=dict(os.environ, PYTHONPATH=pt))
    return d


def ps3_index():
    f = os.path.join(TMP, 'ps3idx.tsv')
    if not os.path.exists(f):
        run([PY, os.path.join(HERE, 'lib', 'ps3index.py'), PS3_UPDATE, PS3_DLCMAPS, PS3_DISC, PS3_DATA2, f],
            env=dict(os.environ, PYTHONPATH=paktools('f525859')))
    idx = {}
    for l in open(f):
        x = l.rstrip('\n').split('\t')
        if x[0] != 'name':
            idx.setdefault((x[0], x[2]), x)
    return idx


def labsrc():
    d = os.path.join(TMP, 'labsrc')
    if not os.path.exists(os.path.join(d, '.ready')):
        run([PY, os.path.join(HERE, 'lib', 'xl.py'), PS3_UPDATE, PS3_DATA2, d], env=dict(os.environ, PYTHONPATH=paktools('f525859')))
        open(os.path.join(d, '.ready'), 'w').write('ok')
    return os.path.join(d, 'pak')


def ps3_source(row, idx):
    """Extract the row's PS3 source pak (archive key from plan.tsv) to TMP/ps3/<archive>/<name>.pak"""
    if row['archive'] == 'labsrc':
        return os.path.join(labsrc(), row['src'] + '.pak')
    d = os.path.join(TMP, 'ps3', row['archive'])
    p = os.path.join(d, row['src'] + '.pak')
    if not os.path.exists(p):
        e = idx.get((row['src'], row['archive']))
        if e is None:
            raise RuntimeError('PS3 source %s not in %s' % (row['src'], row['archive']))
        os.makedirs(d, exist_ok=True)
        run([PY, os.path.join(HERE, 'lib', 'extract_one.py'), e[5], e[3], p], env=dict(os.environ, PYTHONPATH=paktools('f525859')))
    return p


def build(row, idx, built):
    """Build one plan row into TMP/out/<overlay>/<name>.pak; returns the path."""
    od = os.path.join(TMP, 'out', row['overlay'])
    os.makedirs(od, exist_ok=True)
    op = os.path.join(od, row['name'] + '.pak')
    m = row['method']
    opts = dict(kv.split('=', 1) for kv in row['opts'].split(',') if '=' in kv) if row['opts'] not in ('', '-') else {}
    flags = set(x for x in row['opts'].split(',') if x and '=' not in x and x != '-')
    if m == 'empty':
        run([PY, os.path.join(HERE, 'lib', 'empty_level.py'), op], env=dict(os.environ, PYTHONPATH=paktools(row['version'])))
        return op
    if m == 'copy':
        src = built.get((row['overlay'], row['src']))
        if not src:                                   # already installed (skipped as applied): copy the overlay file
            src = os.path.join(WORK, row['overlay'], 'u3data', 'build', 'main', 'pak23', 'ps4', row['src'] + '.pak')
        shutil.copyfile(src, op)
        return op
    if m == 'hud':                                  # retail hud.pak + 2 PS3 HUD textures (lib/hud_textures.py)
        run([PY, os.path.join(HERE, 'lib', 'hud_textures.py'), paktools(row['version']),
             os.path.join(PS4MAIN, 'pak23', 'ps4', 'hud.pak'), ps3_source(row, idx), op])
        return op
    if m in ('slice', 'havok'):
        return built[(row['overlay'], row['name'])]          # produced by the maps-batch builder (see main)
    if m == 'sky':                                  # the cloud's batch57 sky converter (lib/build_skies.py, paktools-sky)
        ps3_source(row, idx)
        sd = os.path.join(TMP, 'sky', row['overlay'], row['name'])
        shutil.rmtree(sd, ignore_errors=True)
        run([PY, os.path.join(HERE, 'lib', 'build_skies.py'), paktools(row['version']), os.path.join(TMP, 'ps3'),
             os.path.join(PS4MAIN, 'pak23', 'ps4'), res_dir(), sd, row['name']], env=dict(os.environ, U3_TEXDICT=TEXDICT))
        shutil.copyfile(os.path.join(sd, row['name'] + '.pak'), op)
        return op
    pt = paktools(row['version'])
    env = dict(os.environ, PYTHONPATH=pt + os.pathsep + os.path.join(pt, 'stage'), U3_TEXDICT=TEXDICT)
    env.update(opts)
    ovf = os.path.join(HERE, 'data', 'overrides', row['overlay'], row['name'] + '.json')
    if os.path.exists(ovf):                 # template choices / constants / caps recovered from the installed pak
        o = json.load(open(ovf))
        od2 = os.path.join(TMP, 'overrides', row['overlay']); os.makedirs(od2, exist_ok=True)
        for k, e in (('templates', 'U3_MKPLAN_OVERRIDE'), ('params', 'U3_MKPLAN_PARAMS'), ('texsrc', 'U3_MKPLAN_TEXSRC')):
            if o.get(k):
                f = os.path.join(od2, '%s.%s.json' % (row['name'], k)); json.dump(o[k], open(f, 'w')); env[e] = f
        env.update(o.get('env', {}))
    sp = ps3_source(row, idx)
    cmd = [PY, os.path.join(HERE, 'lib', 'convert_one.py'), pt, m, sp, op, res_dir() if m == 'stage' else '-',
           os.path.join(PS4MAIN, 'pak23', 'ps4'), os.path.join(TMP, 'plans', row['overlay'], row['version'] + row['opts'])]
    if 'mb' in flags:
        cmd.append('--mapbuild')
    run(cmd, env=env)
    if os.path.exists(ovf):
        bts = json.load(open(ovf)).get('bytes')
        if bts:                                     # residual converter-decision bytes (flags), checked before writing
            d = bytearray(open(op, 'rb').read())
            for k, (old, new) in bts.items():
                if d[int(k)] != old:
                    raise RuntimeError('byte %s is %d, expected %d' % (k, d[int(k)], old))
                d[int(k)] = new
            open(op, 'wb').write(bytes(d))
    return op


def maps_batch(rows, idx, built):
    """lab-multi-34-pak(-ingame12), lab34-spawners-ingame12, lab-multi-37-pak: maps-batch/tools/build_paks.py level part."""
    need = [r for r in rows if r['method'] in ('slice', 'havok')]
    if not need:
        return
    pt = paktools('f525859')
    od = os.path.join(TMP, 'out', 'maps-batch')
    run([PY, os.path.join(HERE, 'lib', 'build_level_paks.py'), pt, labsrc(), os.path.join(TMP, 'mb-tmp'), od],
        env=dict(os.environ, PYTHONPATH=pt + os.pathsep + os.path.join(pt, 'stage') + os.pathsep + os.path.join(HERE, 'lib')))
    for r in need:
        built[(r['overlay'], r['name'])] = os.path.join(od, r['name'] + '.pak')



def shadowed():
    """(overlay, path) pairs a higher overlay hides (the game never loads them): rebuild_overlays writes them to
    $RECIPE_SHADOWED unless it runs with --all-files.  Empty set = build everything."""
    f = os.environ.get('RECIPE_SHADOWED')
    if not f or not os.path.exists(f):
        return set()
    return {tuple(l.rstrip('\r\n').split('\t', 1)) for l in open(f, encoding='utf-8') if '\t' in l}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('family')
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--only')
    ap.add_argument('--strict', action='store_true')
    ap.add_argument('--jobs', type=int, default=int(os.environ.get('RECIPE_JOBS') or max(1, (os.cpu_count() or 2) // 2)))
    ap.add_argument('--keep-work', action='store_true')
    a = ap.parse_args()
    rows = [r for r in plan_rows() if r['family'] == a.family and (not a.only or re.search(a.only, r['name']))]
    if not rows:
        print('no plan rows for', a.family)
        return 0
    hidden = shadowed()
    if hidden:
        def rel(r):
            return (r['overlay'], 'u3data/build/main/pak23/ps4/%s.pak' % r['name'])
        need = {(r['overlay'], r['src']) for r in rows if r['method'] == 'copy' and rel(r) not in hidden}
        keep = [r for r in rows if rel(r) not in hidden or (r['overlay'], r['name']) in need]
        if len(keep) != len(rows):
            print('%s: %d rows skipped (hidden by a higher overlay, the game never loads them)' % (a.family, len(rows) - len(keep)))
        rows = keep
        if not rows:
            print('already applied')
            return 0
    os.makedirs(TMP, exist_ok=True)
    t0 = time.time()
    todo, done = [], 0
    for r in rows:
        dst = os.path.join(WORK, r['overlay'], 'u3data', 'build', 'main', 'pak23', 'ps4', r['name'] + '.pak')
        r['dst'] = dst
        if os.path.exists(dst) and sha(dst) == r['sha256']:
            done += 1
            continue
        if r['status'] != 'identical' and (a.strict or os.path.exists(dst)):
            continue                                  # never replace an existing file by a non-identical rebuild
        todo.append(r)
    print('%s: %d rows, %d already applied, %d to build' % (a.family, len(rows), done, len(todo)))
    if not todo:
        print('already applied')
        return 0
    idx = ps3_index()
    built = {}
    maps_batch(todo, idx, built)
    for r in todo:                                   # PS3 sources first (serial: one reader per archive)
        if r['method'] in ('stage', 'ingame', 'vischeat', 'hud', 'sky'):
            ps3_source(r, idx)
    if any(r['method'] in ('stage', 'sky') for r in todo):
        res_dir()
    for v in sorted(set(r['version'] for r in todo if r['version'] not in ('', '-'))):
        paktools(v)                         # set up every converter tree before the worker threads (copytree race)
    res = {}
    order = [r for r in todo if r['method'] != 'copy'] + [r for r in todo if r['method'] == 'copy']
    first = [r for r in order if r['method'] != 'copy']
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        fut = {ex.submit(build, r, idx, built): r for r in first}
        for f in cf.as_completed(fut):
            r = fut[f]
            try:
                res[id(r)] = f.result()
                built[(r['overlay'], r['name'])] = res[id(r)]
            except Exception as e:
                res[id(r)] = e
    for r in order:
        if r['method'] == 'copy':
            try:
                res[id(r)] = build(r, idx, built)
            except Exception as e:
                res[id(r)] = e
    bad = 0
    report = []
    for r in todo:
        p = res.get(id(r))
        if isinstance(p, Exception) or p is None:
            print('FAIL %s/%s: %s' % (r['overlay'], r['name'], str(p)[:600])); bad += 1
            report.append(dict(overlay=r['overlay'], name=r['name'], result='fail', error=str(p)[:2000]))
            continue
        h = sha(p)
        same = h == r['sha256']
        if not same and r['status'] == 'identical':
            print('MISMATCH %s/%s: built %s, plan %s (not installed)' % (r['overlay'], r['name'], h[:12], r['sha256'][:12]))
            bad += 1
            report.append(dict(overlay=r['overlay'], name=r['name'], result='mismatch', sha256=h))
            continue
        tag = 'identical' if same else 'equivalent (sha %s, reference %s)' % (h[:12], r['sha256'][:12])
        report.append(dict(overlay=r['overlay'], name=r['name'], result='identical' if same else 'equivalent', sha256=h))
        if a.apply:
            dst = r['dst']
            if os.path.exists(dst):
                bk = os.path.join(WORK, 'backup-pak23-levels', r['overlay'], r['name'] + '.pak')
                if not os.path.exists(bk):
                    os.makedirs(os.path.dirname(bk), exist_ok=True)
                    shutil.copy2(dst, bk)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(p, dst + '.tmp')
            os.replace(dst + '.tmp', dst)
            print('installed %s/%s: %s' % (r['overlay'], r['name'], tag))
        else:
            print('ok %s/%s: %s (dry run)' % (r['overlay'], r['name'], tag))
    json.dump(report, open(os.path.join(TMP, 'report-%s.json' % a.family), 'w'), indent=1)
    print('%s: %d built, %d failed/mismatched, %.0f s' % (a.family, len(todo) - bad, bad, time.time() - t0))
    if not a.keep_work:
        shutil.rmtree(os.path.join(TMP, 'out'), ignore_errors=True)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
