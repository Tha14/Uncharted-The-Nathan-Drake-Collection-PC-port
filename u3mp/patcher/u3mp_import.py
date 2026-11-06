"""u3mp_import.py - prepare Uncharted 3 multiplayer on this PC from the player's own game copies.

Run by the launcher (MULTIPLAYER tab -> Import) in the background; can also be run by hand:

  python u3mp_import.py --ps4 <PS4 Collection folder> --ps3-u3 <folder> [--ps3-u3 <folder> ...] [--ps3-u2 <folder>]
                        --data <multiplayer data folder> --port <port root> [--ps3-elf <EBOOT.elf>] [--image-only]

  1. multiplayer image: mpimage.py (the port's prepare scripts on the retail executable, the menu shell, the helper link;
     every stage checked by sha256)                                  -> <data>/image/boot-linked.bin
  2. overlays: the overlay recipe (recipe/rebuild_overlays.py, steps in recipe/steps/ORDER.txt) on the PS3 / PS4 copies,
     continuing after a failing step                                 -> <data>/recipe/work/overlay-{r3,r2,port,mp}
  3. <data>/state.json: what was made, which steps failed; the launcher starts Uncharted 3 with it.

Progress for the launcher: lines starting with "@U3MP " followed by a JSON object.  Nothing is written outside --data.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.dont_write_bytecode = True
import mpimage  # noqa: E402
import u3mp_version  # noqa: E402

RECIPE = HERE / 'recipe'
OVERLAYS = ('overlay-r3', 'overlay-r2', 'overlay-port', 'overlay-mp')   # first wins (as uncharted3_mp.cmd passes them)
# The decrypted PS3 1.19 EBOOT the reference tables were made from (RPCS3: Utilities -> Decrypt PS3 Binaries).
PS3_ELF_SHA256 = 'aac4faa63ab1387e8aaa0b8e9e80ee87ac4ae61e0b40292f7aa7401df7b3f50a'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def event(**kw):
    print('@U3MP ' + json.dumps(kw), flush=True)


def log(text):
    print(text, flush=True)


def bin_dirs(port):
    """Folders with the import's helper programs (ffmpeg, objdump): the launcher package's u3mp/bin first."""
    dirs = []
    for d in (port / 'u3mp' / 'bin', HERE / 'bin'):
        if d.is_dir():
            dirs += [d] + sorted(p for p in d.iterdir() if p.is_dir())   # bin/binutils: GNU objdump and its DLLs
    return dirs


def find_program(port, name, extra=()):
    for d in bin_dirs(port):
        if (d / (name + '.exe')).is_file():
            return str(d / (name + '.exe'))
    found = shutil.which(name)
    if found:
        return found
    for path in extra:
        if os.path.isfile(path):
            return path
    return None


def find_ps3_elf(folders, explicit):
    """The decrypted PS3 1.19 EBOOT.elf: the one given, else an EBOOT.elf in the PS3 folders (RPCS3 writes it next to
    the EBOOT.BIN it decrypts), 4 levels deep like the recipe's own scan."""
    if explicit:
        return explicit if os.path.isfile(explicit) else None
    best = None
    for root in folders:
        root = Path(root)
        if not root.is_dir():
            continue
        for depth in range(5):
            for p in root.glob('/'.join(['*'] * depth + ['EBOOT.elf'])):
                try:
                    with open(p, 'rb') as f:
                        if f.read(4) != b'\x7fELF':
                            continue
                except OSError:
                    continue
                if mpimage.sha256_file(p) == PS3_ELF_SHA256:
                    return str(p)
                best = best or str(p)
    return best


def run_recipe(a, data, ps4_elf, ps3_elf):
    out = data / 'recipe'
    out.mkdir(parents=True, exist_ok=True)
    python = os.environ.get('U3MP_PYTHON') or sys.executable
    cmd = [python, '-u', str(RECIPE / 'rebuild_overlays.py'), '--ps4', a.ps4, '--out', str(out), '--continue-on-error',
           '--speech-langs', a.speech_langs]
    for f in a.ps3_u3:
        cmd += ['--ps3-u3', f]
    if a.only:
        cmd += ['--only', a.only]
    if a.ps3_u2:
        cmd += ['--ps3-u2', a.ps3_u2]
    else:
        cmd += ['--allow-missing-u2']   # no step reads Uncharted 2 (recipe/RECIPE.md): optional here
    env = dict(os.environ)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    env['PYTHONUTF8'] = '1'
    env['PATH'] = os.pathsep.join([str(d) for d in bin_dirs(a.port)] + [env.get('PATH', '')])
    if ps4_elf:
        env['RECIPE_PS4_ELF'] = str(ps4_elf)
    if ps3_elf:
        env['RECIPE_PS3_ELF'] = ps3_elf
        env['U3_PS3_ELF'] = ps3_elf
    ffmpeg = find_program(a.port, 'ffmpeg', (r'C:\msys64\clang64\bin\ffmpeg.exe', r'C:\msys64\ucrt64\bin\ffmpeg.exe'))
    if ffmpeg:
        env['RECIPE_FFMPEG'] = ffmpeg
    log('Python for the recipe: %s' % python)
    log('ffmpeg: %s' % (ffmpeg or 'not found (EULA pages are written without it; banks with MP3 samples are skipped)'))
    log('objdump: %s' % (find_program(a.port, 'objdump') or 'not found (the Havok tables step will report it)'))
    try:
        import numpy  # noqa: F401
        have_numpy = True
    except ImportError:
        have_numpy = python != sys.executable   # another interpreter: the recipe checks it itself
    if not have_numpy:
        log('numpy is not installed in this Python: the steps that need it are skipped by the recipe')
    steps = {}
    # the driver's step lines: "[ n/N] id - title ..." (starts) or "[ n/N] id - title: <result>" (did not run)
    step_re = re.compile(r'^\[\s*(\d+)/(\d+)\] (\S+) - (.*)$')
    result_re = re.compile(r'^(.*): ((?:skipped \(selection\)|SKIP, |NOT REPRODUCIBLE HERE|already ).*)$')
    current = None
    t0 = time.time()
    p = subprocess.Popen(cmd, cwd=str(RECIPE), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, text=True, encoding='utf-8', errors='replace', creationflags=NO_WINDOW)
    for line in p.stdout:
        line = line.rstrip('\n')
        print(line, flush=True)
        m = step_re.match(line)
        if m:
            n, total, sid, rest = int(m.group(1)), int(m.group(2)), m.group(3), m.group(4)
            r = None if rest.endswith(' ...') else result_re.match(rest)
            if r is None:
                title, result = (rest[:-4] if rest.endswith(' ...') else rest), None
            else:
                title, result = r.group(1), r.group(2)
            if result is None:
                current = sid
                steps[sid] = dict(n=n, title=title, status='running')
                event(phase='recipe', step=n, total=total, id=sid, title=title, status='running')
            else:
                current = None
                status = ('already' if result.startswith('already') else 'skipped' if result.startswith('skipped') else
                          'unavailable')
                steps[sid] = dict(n=n, title=title, status=status, note=result[:300])
                event(phase='recipe', step=n, total=total, id=sid, title=title, status=status, note=result[:300])
            continue
        s = line.strip()
        if current and s.startswith('ok '):
            steps[current]['status'] = 'ok'
            event(phase='recipe', id=current, status='ok', note=s)
            current = None
        elif current and s.startswith('FAILED'):
            steps[current]['status'] = 'failed'
            steps[current]['note'] = s[:500]
            event(phase='recipe', id=current, status='failed', note=s[:500])
            current = None
    code = p.wait()
    log('recipe finished in %.0f s (exit %d)' % (time.time() - t0, code))
    overlays = [str(out / 'work' / name) for name in OVERLAYS if (out / 'work' / name).is_dir()]
    return dict(exit=code, steps=steps, overlays=overlays,
                recipe_version=(RECIPE / 'VERSION').read_text(encoding='utf-8').strip() if (RECIPE / 'VERSION').exists() else '?')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ps4', required=True)
    ap.add_argument('--ps4-update', help='the PS4 update 1.02 folder, when it is not merged into --ps4')
    ap.add_argument('--ps3-u3', action='append', default=[])
    ap.add_argument('--ps3-u2')
    ap.add_argument('--ps3-elf')
    ap.add_argument('--data', type=Path, required=True)
    ap.add_argument('--port', type=Path, required=True)
    ap.add_argument('--speech-langs', default='english')
    ap.add_argument('--image-only', action='store_true', help='only (re)build the multiplayer image')
    ap.add_argument('--only', help='run only these recipe steps (tests; passed to rebuild_overlays.py --only)')
    a = ap.parse_args()
    data = a.data.resolve()
    data.mkdir(parents=True, exist_ok=True)
    state_file = data / 'state.json'
    try:
        old = json.loads(state_file.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        old = {}
    state = dict(old, status='running', pid=os.getpid(), started=time.strftime('%Y-%m-%d %H:%M:%S'),
                 inputs=dict(ps4=a.ps4, ps3_u3=a.ps3_u3, ps3_u2=a.ps3_u2 or ''))
    state.pop('error', None)
    state_file.write_text(json.dumps(state, indent=1), encoding='utf-8')

    def finish(status, **kw):
        state.update(kw, status=status, finished=time.strftime('%Y-%m-%d %H:%M:%S'))
        state_file.write_text(json.dumps(state, indent=1), encoding='utf-8')
        event(phase='done', status=status, error=state.get('error', ''))
        return 0 if status in ('imported', 'partial') else 1

    # 1. the multiplayer image (seconds): a mismatching game or port version is reported before the long part
    event(phase='image', status='running')
    ps4_elf = data / 'tools' / 'u3_ps4.elf'
    try:
        image = mpimage.build_image(Path(a.ps4), a.port, data / 'image', log=log, elf_out=ps4_elf,
                                    update=Path(a.ps4_update) if a.ps4_update else None)
    except (mpimage.ImageError, OSError) as e:
        log('ERROR: ' + str(e))
        return finish('failed', error='Multiplayer image: %s' % e, failed_at='image')
    state['image'] = image
    event(phase='image', status='ok', sha256=image['sha256'], version=image['version'])
    if a.image_only:
        return finish(old.get('status') if old.get('status') in ('imported', 'partial') else 'image-only')

    # 2. overlays
    if not a.ps3_u3:
        return finish('failed', error='No PS3 Uncharted 3 folder given', failed_at='inputs')
    ps3_elf = find_ps3_elf(a.ps3_u3, a.ps3_elf)
    if ps3_elf:
        log('PS3 1.19 EBOOT.elf: %s%s' % (ps3_elf, '' if mpimage.sha256_file(ps3_elf) == PS3_ELF_SHA256 else
                                           ' (differs from the tested 1.19 EBOOT.elf)'))
    else:
        log('No decrypted PS3 1.19 EBOOT.elf found: decrypt it with RPCS3 (Utilities -> Decrypt PS3 Binaries, '
            'dev_hdd0/game/BCES01670/USRDIR/EBOOT.BIN); the Havok tables step is skipped without it')
    event(phase='recipe', status='running')
    recipe_id = u3mp_version.shipped_id()   # the recipe this run applies (the launcher updates when it changes)
    r = run_recipe(a, data, ps4_elf, ps3_elf)
    failed = [sid for sid, s in r['steps'].items() if s['status'] == 'failed']
    # steps that could not run here (an input or a Python package missing, or not reproducible yet): their files
    # are missing too
    unavailable = [sid for sid, s in r['steps'].items() if s['status'] == 'unavailable']
    failed += unavailable if not a.only else []
    state.update(recipe_version=r['recipe_version'], steps=r['steps'], overlays=r['overlays'], failed=failed,
                 unavailable=unavailable, ps3_elf=ps3_elf or '')
    if not a.only:
        state['recipe_id'] = recipe_id
    if r['exit'] == 2 or not r['steps']:
        return finish('failed', error='The overlay recipe could not start (see the log: input folders)',
                      failed_at='inputs')
    if not r['overlays']:
        return finish('failed', error='The overlay recipe produced no overlay', failed_at='recipe')
    if failed:
        first = min(failed, key=lambda s: r['steps'][s]['n'])
        return finish('partial', error='%d step(s) failed, first: %s' % (len(failed), first), failed_at=first)
    return finish('imported')


if __name__ == '__main__':
    sys.exit(main())
