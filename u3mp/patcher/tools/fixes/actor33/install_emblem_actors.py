#!/usr/bin/env python3
"""Build and install the 8 Airstrip wall-emblem actor paks (air-multi-emblem-*.pak) into overlay-r3.

The paks the game loads were built on 2026-11-01 22:01 by mkemblem.py with the second-version patch (flat shared map
B=166, per-wall tone map for Overlay4, '--actors-only'; recovered from the transcript: wall-emblem/mkemblem_v2.py).
The first version (17:41) is wall-emblem/mkemblem.py. emblem-cap3 (set_cap3.py) then sets the cbuffer capacity of 7 of
them to 3; air-multi-emblem-ground keeps the build value. The actor build does not depend on the state of
air-multi.pak (pre-emblem and post-mapcolor builds are identical), so this step can run right after the level paks.

  install_emblem_actors.py [--apply]     run from the rendered actor33 job; wall-emblem must be rendered next to it
Dry run without --apply. Idempotent: a pak already at its v2 hash, or at its reference (cap 3) hash, is left alone."""
import hashlib, json, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
WE = os.path.join(os.path.dirname(HERE), 'wall-emblem')
MAIN = "{{WORK}}/overlay-r3/u3data/build/main/"
RET = "{{PS4}}/u3data/build/main/actor33/ps4"
J = json.load(open(os.path.join(HERE, 'emblem-actors.json')))


def sha(f):
    return hashlib.sha256(open(f, 'rb').read()).hexdigest() if os.path.exists(f) else None


def main():
    apply_ = '--apply' in sys.argv
    dst = MAIN + 'actor33/ps4/'
    todo = [n for n, e in J.items() if sha(dst + n) not in (e['v2'], e['reference'])]
    if not todo:
        print('emblem actors already installed (%d)' % len(J)); return 0
    out = os.path.join(WE, 'out', 'actor33', 'ps4')
    if any(sha(os.path.join(out, n)) != J[n]['v2'] for n in todo):
        os.makedirs(os.path.join(WE, 'tmpl'), exist_ok=True)
        os.makedirs(out, exist_ok=True); os.makedirs(os.path.join(WE, 'out', 'pak23', 'ps4'), exist_ok=True)
        if not os.path.exists(os.path.join(WE, 'p3', 'air-multi.pak')):
            subprocess.run([sys.executable, 'ex1.py'], cwd=WE, check=True, capture_output=True)
        if not os.path.exists(MAIN + 'pak23/ps4/air-multi.pak'):
            print('missing ' + MAIN + 'pak23/ps4/air-multi.pak (the converted level; convert-levels-r3 first)'); return 2
        r = subprocess.run([sys.executable, 'mkemblem_v2.py', MAIN, RET, '--actors-only'], cwd=WE, capture_output=True, text=True)
        if r.returncode:
            print((r.stdout + r.stderr)[-800:]); return 1
    bad = 0
    for n in todo:
        s = os.path.join(out, n); h = sha(s)
        ok = h == J[n]['v2']
        print(('install ' if ok else 'DIFF    ') + n, (h or 'missing')[:16])
        if not ok:
            bad += 1; continue
        if apply_:
            os.makedirs(dst, exist_ok=True)
            shutil.copyfile(s, dst + n + '.tmp'); os.replace(dst + n + '.tmp', dst + n)
    print('done' if apply_ else 'dry run: nothing written (use --apply)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
