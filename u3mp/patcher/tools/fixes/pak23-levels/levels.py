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
def paktools(ver):
    """A ready paktools tree for converter version `ver` (with data/ anim actor lists)."""
    dst = os.path.join(TMP, 'pt-' + ver)
    stamp = os.path.join(dst, '.ready')
    if os.path.exists(stamp):
        return dst
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    if ver == 'f525859':
        shutil.copytree(PAKTOOLS, dst, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        ov = os.path.join(HERE, 'paktools-f525859')
        for dp, dn, fn in os.walk(ov):
            for f in fn:
                s = os.path.join(dp, f)
                d = os.path.join(dst, os.path.relpath(s, ov))
                os.makedirs(os.path.dirname(d), exist_ok=True)
                shutil.copyfile(s, d)
    else:
        shutil.copytree(os.path.join(HERE, 'paktools-' + ver), dst, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    data = os.path.join(dst, 'data')
    os.makedirs(data, exist_ok=True)
    for f in ('hk2010_ps3.json', 'hk2011_ps4.json'):      # FG collision tables: empty, as in every recorded run
        open(os.path.join(data, f), 'w').write('{}\n')
    shutil.copyfile(os.path.join(HERE, 'data', 'conv_anim_actors.json'), os.path.join(data, 'conv_anim_actors.json'))
    shutil.copyfile(anim_list(), os.path.join(data, 'ps4_anim_actors.json'))
    open(stamp, 'w').write(ver)
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('family')
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--only')
    ap.add_argument('--strict', action='store_true')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument('--keep-work', action='store_true')
    a = ap.parse_args()
    rows = [r for r in plan_rows() if r['family'] == a.family and (not a.only or re.search(a.only, r['name']))]
    if not rows:
        print('no plan rows for', a.family)
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
        if r['method'] in ('stage', 'ingame', 'vischeat', 'hud'):
            ps3_source(r, idx)
    if any(r['method'] == 'stage' for r in todo):
        res_dir()
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
