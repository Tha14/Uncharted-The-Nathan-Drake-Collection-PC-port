#!/usr/bin/env python3
"""Recipe helper (2026-11-06): extract the PS3 air-multi.pak and air-multi-ingame12.pak that mkemblem.py reads as
./p3/<name> from the user's PS3 disc archive build/main/pak23.psarc (the source the cloud conversion of air-multi used).
usage: prep_p3.py   (writes <this folder>/p3/, creates ./out/actor33/ps4 and ./out/pak23/ps4)"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, "{{PAKTOOLS}}")
from psarc_lib import Psarc
A = Psarc(os.path.join("{{PS3_DISC}}", 'build', 'main', 'pak23.psarc'))
os.makedirs(os.path.join(HERE, 'p3'), exist_ok=True)
for d in ('out/actor33/ps4', 'out/pak23/ps4'):          # mkemblem.py writes below ./out
    os.makedirs(os.path.join(HERE, d), exist_ok=True)
for n in ('air-multi.pak', 'air-multi-ingame12.pak'):
    i = [k for k, x in enumerate(A.names) if ('/' + x.replace(chr(92), '/')).endswith('/' + n)]
    if not i:
        raise SystemExit('%s not in pak23.psarc' % n)
    d = A.read(i[0] + 1)
    open(os.path.join(HERE, 'p3', n), 'wb').write(d)
    print('p3/%s %d bytes' % (n, len(d)))
