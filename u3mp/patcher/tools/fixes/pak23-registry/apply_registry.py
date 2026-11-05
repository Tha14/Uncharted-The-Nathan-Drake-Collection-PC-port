#!/usr/bin/env python3
"""Write the final pak23_ps4.txt of overlay-mp / -port / -r2 / -r3 from the user's own game texts.

Each registry is the retail PS4 pak23_ps4.txt plus PS3 1.19 level definitions (pak23.txt of dlc-patch18, DLC / Lab
level .txt files), as mkregistry.py, lab_reg.py, apply_maps.py, apply_air_emblem.py, build_map_levels.py and
apply_shade_npc_paks.py left it on 2026-11-03.  recipes/<overlay>.json records, line by line, which source line range
each part copies (regprov.py); the only literal lines are our own additions (emblem actors, lab34-stage /
lab34-spawners, the san-multi-hunter-pak header).  No game text is stored here.

usage: apply_registry.py [--apply] [overlay ...]      (default: all four; dry run without --apply)
Runs last in the recipe (after every step that edits a registry), checks the sha256 of recipes/sha256.tsv and writes
nothing on a mismatch.  Idempotent; the replaced file is backed up to work/backup-pak23-registry/<overlay>/."""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = "{{WORK}}"
SRC = os.path.join("{{SCRATCH}}", 'pak23-registry-sources')
PY = sys.executable


def main():
    apply = '--apply' in sys.argv
    want = [a for a in sys.argv[1:] if not a.startswith('--')] or ['overlay-mp', 'overlay-port', 'overlay-r2', 'overlay-r3']
    ref = {}
    for l in open(os.path.join(HERE, 'recipes', 'sha256.tsv')):
        o, n, h = l.split()
        ref[o] = h
    if not os.path.exists(os.path.join(SRC, 'retail.txt')):
        env = dict(os.environ, PYTHONPATH="{{TRANSFER_TOOLS}}")
        p = subprocess.run([PY, os.path.join(HERE, 'regsources.py'), "{{PS4}}", "{{PS3_UPDATE}}", "{{PS3_DLCMAPS}}",
                            "{{PS3_DISC}}", "{{PS3_DATA2}}", SRC], env=env, capture_output=True, text=True)
        print(p.stdout.strip(), p.stderr[-1500:])
        if p.returncode:
            return 1
    rc = 0
    for o in want:
        dst = os.path.join(WORK, o, 'u3data', 'build', 'main', 'pak23_ps4.txt')
        tmp = os.path.join(SRC, o + '.pak23_ps4.txt')
        p = subprocess.run([PY, os.path.join(HERE, 'regbuild.py'), os.path.join(HERE, 'recipes', o + '.json'), SRC, tmp,
                            '--sha256', ref[o]], capture_output=True, text=True)
        print(p.stdout.strip())
        if p.returncode:
            print(p.stderr[-1500:])
            rc = 1
            continue
        data = open(tmp, 'rb').read()
        if os.path.exists(dst) and open(dst, 'rb').read() == data:
            print('already applied', o)
            continue
        if not apply:
            print('would write', dst)
            continue
        if os.path.exists(dst):
            bk = os.path.join(WORK, 'backup-pak23-registry', o, 'pak23_ps4.txt')
            if not os.path.exists(bk):
                os.makedirs(os.path.dirname(bk), exist_ok=True)
                shutil.copy2(dst, bk)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(tmp, dst + '.tmp')
        os.replace(dst + '.tmp', dst)
        print('installed', o)
    return rc


if __name__ == '__main__':
    sys.exit(main())
