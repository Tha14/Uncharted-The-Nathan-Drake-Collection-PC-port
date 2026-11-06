#!/usr/bin/env python3
"""Recipe helper (2026-11-06): build p3idx.pkl, the PS3 archive index apply_mapcolor2.py reads ({entry basename:
[(archive, entry index, name), ...]}, later archives last = preferred), as the session's x3.py made it, from the user's
PS3 disc (build/main/pak23.psarc, goty-dlc.psarc) and 1.19 update (dlc-patch13.psarc, dlc-patch18.psarc).
usage: mk_p3idx.py   (writes <this folder>/p3idx.pkl)"""
import os, sys, pickle
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, "{{PAKTOOLS}}")
from psarc_lib import Psarc
D, U = "{{PS3_DISC}}", "{{PS3_UPDATE}}"
ARCS = [D + '/build/main/pak23.psarc', D + '/goty-dlc.psarc', U + '/dlc-patch13.psarc', U + '/dlc-patch18.psarc']
idx = {}
for a in ARCS:
    A = Psarc(a)
    for i, n in enumerate(A.names):
        idx.setdefault(os.path.basename(n), []).append((a, i + 1, n))
pickle.dump(idx, open(os.path.join(HERE, 'p3idx.pkl'), 'wb'))
print('p3idx.pkl: %d names from %d archives' % (len(idx), len(ARCS)))
