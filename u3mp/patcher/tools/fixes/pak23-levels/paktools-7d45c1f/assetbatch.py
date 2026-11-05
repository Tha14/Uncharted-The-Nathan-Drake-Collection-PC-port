#!/usr/bin/env python3
"""Convert one batch of missing PS3 actor33 paks to PS4, verify them, and publish the batch.

usage: assetbatch.py <batchNN> <jobs.tsv> [--out-root /mnt/user-data/outputs/assets] [--dry]

jobs.tsv columns: name  source(disc|dlc-patch13|...)  ps3-path  mode  template-path  [opts]
  mode   kit    ps4actor.py (shared hero/chloe skeleton; template supplies skeleton+materials)
         graft  ps4actor.py --graft (prop with own skeleton: template skeleton/anim/collision)
         own    ps4actor.py --own-skel (prop with own skeleton converted from PS3: skel.py, anim.py,
                fgcoll.py; the template only supplies materials/effects)
         rename ps4rename.py of the template (PS3 data identical to a PS4 actor)
  opts   comma list: skel=<pak> (kit: --skel-template), lod0 (--lod0-only), sf=N (stream flags),
         nomatch (no --match-streams), budget=N (VRAM budget bytes),
         spread (PS4ACTOR_SPREAD_MATS=1: distinct template slot per PS3 material, hair->hair shader),
         notmpl (PS4ACTOR_NO_TMPL_TEX=1: never copy template textures; flat ones fill the gaps)
  PS4ACTOR_DONORS (env, ':'-separated retail paks) is passed through to ps4actor.py.
Each pak is converted with textures capped at 256 and retried at 128 / 64 until its VRAM fits the
budget (default 128 KB, the budget that made kit parts load; ASSET_VRAM_BUDGET overrides).
Verification: ps4verify (errors), ps4check (problems other than 'unknown item type'), and an item
type diff against the template (types present in one but not the other, ignoring EFFECT/RAW_DATA/
VRAM_DESC counts).  Only paks that pass are copied to <out-root>/<batch>/u3data/build/main/actor33/ps4
and get a READY.tsv line: batch, name, source, template, verify result.
"""
import argparse, os, re, shutil, subprocess, sys
from collections import Counter
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ndpak import Pak
from ps4verify import verify
from ps4check import check

IGNORE_TYPES = {'RAW_DATA', 'VRAM_DESC', 'EFFECT'}


def type_diff(out, tmpl):
    a = {i.type for i in Pak(out).items} - IGNORE_TYPES
    b = {i.type for i in Pak(tmpl).items} - IGNORE_TYPES
    d = []
    if a - b:
        d.append('+' + '+'.join(sorted(a - b)))
    if b - a:
        d.append('-' + '-'.join(sorted(b - a)))
    return ' '.join(d)


def convert(job, dst):
    name, src, ps3, mode, tmpl, opts = job
    if mode == 'rename':
        old = os.path.basename(tmpl)[:-4]
        r = subprocess.run([sys.executable, os.path.join(HERE, 'ps4rename.py'), tmpl, dst, old, name[:-4]],
                           capture_output=True, text=True)
        return r.returncode == 0, (r.stdout.strip().splitlines() or [''])[-1] if r.returncode == 0 else r.stderr.strip()[-300:], 'rename'
    cmd = [sys.executable, os.path.join(HERE, 'ps4actor.py'), ps3, tmpl, dst, '--mat-by-file']
    if 'nomatch' not in opts:
        cmd.append('--match-streams')
    if mode == 'graft':
        cmd += ['--graft', '--stream-flags', opts.get('sf', '3')]
    elif mode == 'own':
        cmd += ['--own-skel', '--stream-flags', opts.get('sf', '3')]
    else:
        cmd += ['--stream-flags', opts.get('sf', '2')]
    if opts.get('skel'):
        cmd += ['--skel-template', opts['skel']]
    if 'lod0' in opts:
        cmd.append('--lod0-only')
    budget = int(opts.get('budget', os.environ.get('ASSET_VRAM_BUDGET', '131072')))
    env = dict(os.environ)
    if 'spread' in opts:
        env['PS4ACTOR_SPREAD_MATS'] = '1'
    if opts.get('skel3'):           # own mode: PS3 pak whose skeleton a skeleton-less LOD/part uses
        env['PS4ACTOR_SKEL3'] = opts['skel3']
    if 'notmpl' in opts:            # unused/unmatched template slots get flat textures, not the template's
        env['PS4ACTOR_NO_TMPL_TEX'] = '1'
    r = None
    for mt in ('256', '128', '64'):
        r = subprocess.run(cmd + ['--max-tex', mt], capture_output=True, text=True, env=env)
        last = (r.stdout.strip().splitlines() or [''])[-1]
        m = re.search(r'vram 0x([0-9a-f]+)', last)
        if r.returncode or not m or int(m.group(1), 16) <= budget:
            break
    if r.returncode:
        return False, (r.stderr.strip().splitlines() or ['?'])[-1][:300], 'tex<=' + mt
    return True, last.split(': ', 1)[-1], 'tex<=' + mt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('batch')
    ap.add_argument('jobs')
    ap.add_argument('--out-root', default='/mnt/user-data/outputs/assets')
    ap.add_argument('--work', default=None)
    ap.add_argument('--dry', action='store_true', help='convert+verify only, do not publish')
    a = ap.parse_args()
    work = a.work or os.path.join(os.path.dirname(os.path.abspath(a.jobs)), a.batch)
    os.makedirs(work, exist_ok=True)
    pub = os.path.join(a.out_root, a.batch, 'u3data/build/main/actor33/ps4')
    jobs = []
    for l in open(a.jobs):
        if not l.strip() or l.startswith('#'):
            continue
        f = l.rstrip('\n').split('\t')
        opts = {}
        for o in (f[5].split(',') if len(f) > 5 and f[5] else []):
            k, _, v = o.partition('=')
            opts[k] = v
        jobs.append((f[0], f[1], f[2], f[3], f[4], opts))
    ready = []
    for job in jobs:
        name, src, ps3, mode, tmpl, opts = job
        dst = os.path.join(work, name)
        ok, msg, cap = convert(job, dst)
        if not ok:
            print('FAIL', name, '|', msg)
            continue
        errs, notes = verify(dst, tmpl if mode != 'rename' else None, name[:-4], quiet=True)
        cerrs, _, _ = check(dst, quiet=True)
        # ignored: item types ps4check does not model, and the cube/env-reflection tiled-size
        # mismatch that retail paks (and the accepted mpk9 kits) report as well
        cerrs = [e for e in cerrs if not e.startswith('unknown item type')
                 and not (e.startswith('tiled size') and 'env-reflection' in e)]
        td = type_diff(dst, tmpl)
        res = 'verify:%s check:%s' % ('OK' if not errs else 'ERR(%s)' % '; '.join(errs)[:200],
                                      'OK' if not cerrs else 'ERR(%s)' % '; '.join(cerrs)[:200])
        if td:
            res += ' types:' + td
        good = not errs and not cerrs
        print(('OK  ' if good else 'BAD ') + name, '<-', mode, os.path.basename(tmpl), cap, '|', res, '|', msg[:90])
        if good:
            ready.append((name, src, os.path.basename(tmpl)[:-4] + ('' if mode == 'kit' else ' (' + mode + ')'),
                          res + ' ' + cap))
    if a.dry or not ready:
        return
    os.makedirs(pub, exist_ok=True)
    with open(os.path.join(a.out_root, 'READY.tsv'), 'a') as f:
        for name, src, t, res in ready:
            shutil.copy2(os.path.join(work, name), os.path.join(pub, name))
            f.write('%s\t%s\t%s\t%s\t%s\n' % (a.batch, name, src, t, res))
    print('published %d paks to %s' % (len(ready), pub))


if __name__ == '__main__':
    main()
