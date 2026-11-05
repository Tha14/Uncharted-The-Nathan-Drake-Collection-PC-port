#!/usr/bin/env python3
"""install_streams.py [overlay u3data root]   -- batch44-streams installed from the PC's own PS4 game folders.
Copies the 209 picked PS4 streams (XVAG v6/ATRAC9 from u2data / u3data) to <overlay>/build/main/sound1/streams/sfx/<name>
and verifies each against batch44's md5 (streams_install.tsv). Candidate sources come from streamfind's found.tsv
(streams_found.tsv); the first PS4-folder candidate whose md5 matches is used. Read-only on the game; resumable.
Default overlay: ~/mnt/Uncharted The Nathan Drake Collection - U3 Multiplayer/work/overlay-r3/u3data"""
import os, sys, hashlib, shutil
H = os.path.dirname(os.path.abspath(__file__))
OV = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/mnt/Uncharted The Nathan Drake Collection - U3 Multiplayer/work/overlay-r3/u3data')
G = 'Uncharted - The Nathan Drake Collection [CUSA02320]'
dst_dir = os.path.join(OV, 'build', 'main', 'sound1', 'streams', 'sfx'); os.makedirs(dst_dir, exist_ok=True)
want = dict(l.rstrip('\n').split('\t') for l in open(os.path.join(H, 'streams_install.tsv')))   # name -> md5
cands = {}
for l in open(os.path.join(H, 'streams_found.tsv')):
    name, src, inner, size = l.rstrip('\n').split('\t')
    if G in src and not inner:
        cands.setdefault(name, []).append(os.path.expanduser(src))
def md5(p):
    h = hashlib.md5()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()
ok = skip = bad = 0; missing = []
for name, m in sorted(want.items()):
    dst = os.path.join(dst_dir, name)
    if os.path.exists(dst) and md5(dst) == m:
        skip += 1; continue
    for src in cands.get(name, []):
        if os.path.exists(src) and md5(src) == m:
            shutil.copyfile(src, dst); ok += 1; break
    else:
        missing.append(name)
bad = sum(1 for n, m in want.items() if not (os.path.exists(os.path.join(dst_dir, n)) and md5(os.path.join(dst_dir, n)) == m))
print('copied %d, already ok %d, unresolved %d, final md5 mismatches/missing %d' % (ok, skip, len(missing), bad))
for n in missing: print('  no matching source:', n)
print('OK' if bad == 0 else 'FAIL')
