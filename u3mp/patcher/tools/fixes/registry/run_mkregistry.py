#!/usr/bin/env python3
"""Build <overlay>/u3data/build/main/pak23_ps4.txt with transfer-tools/mkregistry.py: the retail PS4 registry plus a
PS4-format definition of every ported PS3 level named in recipe/lists/levels-<overlay>.txt (level names only; no game data).
PS3 level definitions come from the newest pak23.txt available: dlc-patch18.psarc > dlc-patch13.psarc > disc build/main/pak23.txt.
usage: run_mkregistry.py <overlay name> [<overlay name>...]
"""
import os
import subprocess
import sys

sys.path.insert(0, '{{TRANSFER_TOOLS}}')
from psarc_lib import Psarc  # noqa: E402

WORK = '{{PROJECT}}/work/'
PS4MAIN = '{{PS4}}/u3data/build/main'
UPDATE = '{{PS3_UPDATE}}'
DISC = '{{PS3_DISC}}'
LISTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'lists')
HERE = os.path.dirname(os.path.abspath(__file__))


def ps3_pak23_txt():
    """Extract the PS3 1.19 pak23.txt (patched level list) next to this script."""
    out = os.path.join(HERE, 'ps3-pak23.txt')
    for arc in ('dlc-patch18.psarc', 'dlc-patch13.psarc'):
        p = os.path.join(UPDATE, arc)
        if os.path.exists(p):
            ps = Psarc(p)
            for i, n in enumerate(ps.names):
                if n.lstrip('/') == 'build/main/pak23.txt':
                    open(out, 'wb').write(ps.read(i + 1)); return out
    src = os.path.join(DISC, 'build', 'main', 'pak23.txt')
    if os.path.exists(src):
        open(out, 'wb').write(open(src, 'rb').read()); return out
    raise FileNotFoundError('no PS3 pak23.txt (dlc-patch18/13.psarc or the disc build/main/pak23.txt)')


def main():
    txt = ps3_pak23_txt()
    rc = 0
    for ov in sys.argv[1:]:
        lst = os.path.join(LISTS, 'levels-%s.txt' % ov)
        levels = [l.strip() for l in open(lst, encoding='utf-8') if l.strip()]
        main_dir = os.path.join(WORK, ov, 'u3data', 'build', 'main')
        os.makedirs(main_dir, exist_ok=True)
        # the lower overlays are also roots for dependency checks (same order the launcher loads them)
        extra = [os.path.join(WORK, o, 'u3data', 'build', 'main') for o in ('overlay-r3', 'overlay-r2', 'overlay-port', 'overlay-mp') if o != ov]
        env = dict(os.environ, MKREG_EXTRA=os.pathsep.join(e for e in extra if os.path.isdir(e)))
        cmd = [sys.executable, os.path.join('{{TRANSFER_TOOLS}}', 'mkregistry.py'), PS4MAIN, txt, main_dir] + levels
        print('==', ov, len(levels), 'levels')
        p = subprocess.run(cmd, env=env, capture_output=True, text=True, errors='replace')
        print(p.stdout[-3000:], p.stderr[-2000:])
        rc |= p.returncode
    return rc


if __name__ == '__main__':
    sys.exit(main())
