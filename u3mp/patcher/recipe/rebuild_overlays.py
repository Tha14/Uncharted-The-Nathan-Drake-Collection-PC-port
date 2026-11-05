#!/usr/bin/env python3
"""rebuild_overlays.py - regenerate the four game-data overlays (overlay-r3, overlay-r2, overlay-port, overlay-mp)
from the user's own game copies, step by step, in the order the fixes were applied.

usage:
  python rebuild_overlays.py --ps4 <PS4 game folder> --ps3-u3 <folder>... --ps3-u2 <folder> --out <output folder> [options]

  --ps4      the PS4 Nathan Drake Collection game folder (eboot.bin, sce_sys/param.sfo, u3data/, u2data/)
  --ps3-u3   one or more folders holding the PS3 Uncharted 3 data (repeatable).  Each is scanned (4 levels deep) for:
               the disc / full game data (PS3_GAME/USRDIR with build/main/pak23.psarc),
               the 1.19 title update (USRDIR with dlc-patch13.psarc + dlc-patch18.psarc),
               the game-data install the game wrote (...DATA2/USRDIR with net18.bin, build/main/*-7.psarc),
               the DLC map packs (USRDIR with dlc-map1..4.psarc).
             An RPCS3 dev_hdd0/game folder plus the disc folder covers all four.
  --ps3-u2   folder holding the PS3 Uncharted 2 data (PS3_GAME/USRDIR or RPCS3 USRDIR with build/main/pak22.psarc).
             Required (the patcher is for owners of all three games); no current step reads it.
  --out      output folder.  Overlays land in <out>/work/overlay-*; re-running on the same folder only redoes
             steps whose outputs changed (see --force).
options:
  --list                 print the steps and exit
  --only ID[,ID...]      run only these steps        --from ID   start at this step
  --skip ID[,ID...]      skip these steps            --force     rerun steps already recorded in <out>/_recipe/state.json
  --speech-langs LIST    speech languages to copy from the PS3 disc (default: english; 'all' = every <lang>-mp.psarc found)
  --dry-run              show what would run, write nothing
  --continue-on-error    keep going after a failing step (default: stop)

Every step is a file in steps/ (see ADDING-A-FIX.md).  Progress goes to stdout and <out>/_recipe/log.txt.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

import recipe_lib as R

sys.dont_write_bytecode = True


def main():
    ap = argparse.ArgumentParser(add_help=True, formatter_class=argparse.RawDescriptionHelpFormatter, description=__doc__)
    ap.add_argument('--ps4')
    ap.add_argument('--ps3-u3', action='append', default=[])
    ap.add_argument('--ps3-u2', action='append', default=[])
    ap.add_argument('--out')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--only')
    ap.add_argument('--from', dest='from_')
    ap.add_argument('--skip')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--speech-langs', default='english')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--continue-on-error', action='store_true')
    ap.add_argument('--allow-missing-u2', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args()

    steps = R.load_steps()
    if a.list:
        for s in steps:
            print('%-34s %-10s %-24s %s' % (s['id'], s['kind'], s.get('status', ''), s['title']))
        return 0
    if not (a.ps4 and a.ps3_u3 and a.out):
        ap.error('--ps4, --ps3-u3 and --out are required (--ps3-u2 too; see --help)')

    inp = R.Inputs()
    inp.set_ps4(a.ps4)
    inp.add_ps3_u3(a.ps3_u3)
    if a.ps3_u2:
        inp.set_ps3_u2(a.ps3_u2)
    elif not a.allow_missing_u2:
        inp.problems.append('--ps3-u2 is required: point it at your PS3 Uncharted 2 game data')
    print(inp.describe())
    if inp.problems:
        print('\nCannot start: fix the problems above.')
        return 2

    out = os.path.abspath(a.out)
    os.makedirs(os.path.join(out, '_recipe'), exist_ok=True)
    logf = open(os.path.join(out, '_recipe', 'log.txt'), 'a', encoding='utf-8')

    def log(msg):
        print(msg, flush=True)
        logf.write(msg + '\n'); logf.flush()

    log('\n=== rebuild_overlays recipe %s  %s' % (R.VERSION, time.strftime('%Y-%m-%d %H:%M:%S')))
    log(inp.describe())
    values = inp.placeholders(out)
    langs = inp.speech_langs() if a.speech_langs == 'all' else [l.strip() for l in a.speech_langs.split(',') if l.strip()]
    values['SPEECH_LANGS'] = ','.join(langs)
    state = R.State(out)
    manifest = {('work/%s/%s' % (m['overlay'], m['path'])): m['sha256'] for m in R.read_tsv(os.path.join(R.HERE, 'manifest.tsv'))}
    hcache = {}
    only = set(a.only.split(',')) if a.only else None
    skip = set(a.skip.split(',')) if a.skip else set()
    started = a.from_ is None
    counts = {'ok': 0, 'skipped': 0, 'already': 0, 'failed': 0, 'unavailable': 0}
    t_all = time.time()
    for i, s in enumerate(steps, 1):
        sid = s['id']
        if not started:
            if sid == a.from_:
                started = True
            else:
                continue
        head = '[%2d/%d] %s - %s' % (i, len(steps), sid, s['title'])
        if (only and sid not in only) or sid in skip:
            log(head + ': skipped (selection)'); counts['skipped'] += 1
            continue
        # inputs this step needs
        missing = [n for n in s.get('needs', []) if not _have(inp, n)]
        if missing:
            log(head + ': SKIP, input not available: %s' % ', '.join(missing)); counts['unavailable'] += 1
            continue
        if s['kind'] == 'unavailable':
            log(head + ': NOT REPRODUCIBLE HERE - %s' % s.get('reason', '')); counts['unavailable'] += 1
            continue
        rec = state.done(sid)
        if rec and not a.force and rec.get('status') == 'ok' and R.outputs_unchanged(out, rec):
            log(head + ': already applied (%s)' % rec['when']); counts['already'] += 1
            continue
        if s['kind'] == 'script' and not a.force and at_reference_state(s, out, manifest, hcache):
            # every file this step may write already equals the reference build: nothing to do
            state.mark(sid, {}, 'ok', 'files already at the reference state')
            log(head + ': already at the reference state (manifest hashes match)'); counts['already'] += 1
            continue
        if a.dry_run:
            log(head + ': would run'); continue
        t0 = time.time()
        log(head + ' ...')
        try:
            outputs = run_step(s, inp, out, values, langs, log)
            state.mark(sid, outputs, 'ok')
            log('        ok  %d file(s)  %.1f s' % (len(outputs), time.time() - t0)); counts['ok'] += 1
        except Exception as e:
            state.mark(sid, {}, 'failed', str(e)[:500])
            log('        FAILED after %.1f s: %s' % (time.time() - t0, e)); counts['failed'] += 1
            if not a.continue_on_error:
                log('stopping (use --continue-on-error to go on)')
                break
    log('done in %.0f s: %s' % (time.time() - t_all, ', '.join('%s %d' % kv for kv in counts.items())))
    return 1 if counts['failed'] else 0


def at_reference_state(s, out, manifest, hcache):
    """True when every manifest file covered by the step's 'writes' globs exists and has the reference hash."""
    import fnmatch
    pats = s.get('writes', [])
    if not pats:
        return False
    covered = [rel for rel in manifest if any(fnmatch.fnmatch(rel, pat) for pat in pats)]
    if not covered:
        return False
    for rel in covered:
        p = os.path.join(out, rel)
        if not os.path.exists(p):
            return False
        st = os.stat(p)
        key = (p, st.st_size, int(st.st_mtime))
        if key not in hcache:
            hcache[key] = R.sha256_file(p)
        if hcache[key] != manifest[rel]:
            return False
    return True


def _have(inp, need):
    if need == 'ps4':
        return bool(inp.ps4)
    if need == 'ps3_u2':
        return bool(inp.u2)
    if need.startswith('ps3_'):
        return need[4:] in inp.ps3
    if need.startswith('tool:'):
        return os.path.exists(os.path.join(R.TOOLS, need[5:]))
    if need.startswith('python:'):
        try:
            __import__(need[7:]); return True
        except ImportError:
            return False
    return False


# ----------------------------------------------------------------------------- step kinds
def run_step(s, inp, out, values, langs, log):
    k = s['kind']
    if k == 'copy-list':
        return step_copy_list(s, inp, out, langs, log)
    if k == 'script':
        return step_script(s, inp, out, values, log)
    raise ValueError('unknown step kind %r' % k)


def _dest_hashes(out, rels):
    return {rel: (R.sha256_file(os.path.join(out, rel)) if os.path.exists(os.path.join(out, rel)) else None) for rel in rels}


def step_copy_list(s, inp, out, langs, log):
    """Copy the files of lists/<list>.tsv from the PS3/PS4 inputs: loose files or PSARC entries, sha256-checked."""
    from psarc_lib import Psarc
    rows = list(R.read_tsv(os.path.join(R.LISTS, s['list'])))
    sources = {
        'ps3-u3-disc-loose': lambda: inp.ps3.get('disc'),
        'ps4-retail': lambda: inp.ps4,
    }
    # where an archive may be found, by basename
    arch_dirs = []
    for kind in ('disc', 'update', 'data2', 'dlcmaps'):
        d = inp.ps3.get(kind)
        if d:
            arch_dirs += [d, os.path.join(d, 'build', 'main'), os.path.join(d, 'build', 'main', 'pak23'),
                          os.path.join(d, 'build', 'main', 'sound1', 'streams'), os.path.join(d, 'build', 'main', 'sound1', 'streams', 'speech')]
    arch_cache = {}

    def archive(name):
        if name not in arch_cache:
            for d in arch_dirs:
                p = os.path.join(d, name)
                if os.path.isfile(p):
                    arch_cache[name] = (p, Psarc(p)); break
            else:
                raise FileNotFoundError('archive %s not found in any PS3 input' % name)
        return arch_cache[name]

    outputs = {}
    n = 0; t0 = time.time(); bad = []
    # speech: the list names english files; other languages are mapped onto <lang>-mp.psarc and speech/<lang>/
    lang_variants = [''] + ['%s' % l for l in langs if l != 'english'] if s.get('speech') else ['']
    for r in rows:
        for lv in lang_variants:
            dest = r['dest']; arc = r['archive']; ent = r['entry']
            if lv:
                dest = dest.replace('/speech/english/', '/speech/%s/' % lv)
                arc = arc.replace('english-mp.psarc', '%s-mp.psarc' % lv).replace('goty-speech-english', 'goty-speech-%s' % lv)
                ent = ent.replace('/speech/english/', '/speech/%s/' % lv)
            dp = os.path.join(out, 'work', r['overlay'], dest)
            if os.path.exists(dp) and not lv and R.sha256_file(dp) == r['sha256']:
                outputs[os.path.relpath(dp, out).replace('\\', '/')] = r['sha256']; continue
            try:
                if arc == 'loose':
                    base = sources[r['source_kind']]()
                    src = os.path.join(base, ent)
                    data = open(src, 'rb').read()
                else:
                    pth, ps = archive(arc)
                    names = ps.names
                    try:
                        idx = names.index(ent)
                    except ValueError:
                        try:
                            idx = names.index('/' + ent)
                        except ValueError:
                            raise FileNotFoundError('%s has no entry %s' % (arc, ent))
                    data = ps.read(idx + 1)
            except FileNotFoundError as e:
                if lv:
                    continue        # a language the disc does not have
                bad.append(str(e)); continue
            h = R.sha256_bytes(data)
            if not lv and h != r['sha256']:
                bad.append('%s: content differs from the reference (%s)' % (dest, h[:12])); continue
            os.makedirs(os.path.dirname(dp), exist_ok=True)
            with open(dp + '.tmp', 'wb') as f:
                f.write(data)
            os.replace(dp + '.tmp', dp)
            outputs[os.path.relpath(dp, out).replace('\\', '/')] = h
            n += 1
            if n % 1000 == 0:
                log('        %d files, %.0f s' % (n, time.time() - t0))
    if bad:
        log('        %d file(s) could not be copied:' % len(bad))
        for b in bad[:20]:
            log('          ' + b)
        if len(bad) > len(rows) * s.get('tolerate_missing', 0.0):
            raise RuntimeError('%d of %d files missing or different' % (len(bad), len(rows)))
    return outputs


def step_script(s, inp, out, values, log):
    """Render the fix job folder into <out>/scratch/<job> and run its commands there."""
    job = s['job']
    jd = R.render_job(job, out, values, log)
    # helper jobs a step imports from (e.g. the dctools DC reader/editor): rendered next to it, on PYTHONPATH
    extra = [R.render_job(j, out, values, log) for j in s.get('with_jobs', [])]
    before = {}
    writes = s.get('writes', [])
    import glob
    for pat in writes:
        for p in glob.glob(os.path.join(out, pat), recursive=True):
            if os.path.isfile(p):
                before[p] = R.sha256_file(p)
    env = dict(os.environ)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env['PKT'] = values['PAKTOOLS']
    env['PAKTOOLS'] = values['PAKTOOLS']
    env['PYTHONPATH'] = os.pathsep.join([values['PAKTOOLS'], values['PAKTOOLS'] + '/stage', values['TRANSFER_TOOLS'], jd] + extra + [values['RECOVERED']]
                                        + ([env['PYTHONPATH']] if env.get('PYTHONPATH') else []))
    for k, v in values.items():
        env['RECIPE_' + k] = v
    for cmd in s['commands']:
        argv = [sys.executable if c == '{{PYTHON}}' else c.replace('{{OUT}}', out) for c in cmd]
        argv = [c.replace('{{JOB}}', jd) for c in argv]
        for k, v in values.items():
            argv = [c.replace('{{%s}}' % k, v) for c in argv]
        # '@file' arguments are replaced by the file's content (one-line lists such as --only A,B,C)
        argv = [open(c[1:], encoding='utf-8').read().strip() if c.startswith('@') and os.path.isfile(c[1:]) else c for c in argv]
        log('        $ ' + ' '.join(argv if len(' '.join(argv)) < 300 else argv[:4] + ['...']))
        cwd = jd if not s.get('cwd') else s['cwd'].replace('{{OUT}}', out).replace('{{JOB}}', jd)
        p = subprocess.run(argv, cwd=cwd, env=env, capture_output=True, text=True, errors='replace')
        tail = (p.stdout + p.stderr).strip().splitlines()
        for line in tail[-int(s.get('log_lines', 8)):]:
            log('          | ' + line[:220])
        if p.returncode not in s.get('ok_codes', [0]):
            raise RuntimeError('%s exited %d' % (os.path.basename(argv[1] if len(argv) > 1 else argv[0]), p.returncode))
        if s.get('expect') and not any(s['expect'] in line for line in tail):
            raise RuntimeError('expected %r in the output of %s' % (s['expect'], argv[1:3]))
    outputs = {}
    for pat in writes:
        for p in glob.glob(os.path.join(out, pat), recursive=True):
            if os.path.isfile(p):
                h = R.sha256_file(p)
                if before.get(p) != h:
                    outputs[os.path.relpath(p, out).replace('\\', '/')] = h
    if s.get('must_write', False) and not outputs:
        raise RuntimeError('the step changed no file under %s' % writes)
    if not outputs:
        # exit 0 without a change: the fix was already present (or the script's dry-run guard found nothing to do);
        # remember the current hashes of the files the step owns so a later change is noticed
        for pat in writes:
            for p in glob.glob(os.path.join(out, pat), recursive=True):
                if os.path.isfile(p) and p in before:
                    outputs[os.path.relpath(p, out).replace('\\', '/')] = before[p]
        log('        no change (already applied)')
    return outputs


if __name__ == '__main__':
    sys.exit(main())
