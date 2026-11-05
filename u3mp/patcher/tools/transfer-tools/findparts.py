#!/usr/bin/env python3
"""ASSETS PC job findparts.py (read-only on the game; resumable; ~20 s). Searches EVERY PS4 particle module (dc2/part-module/*.bin
and dc2/*.bin) for the 22 PS3 1.19 MP effect sids that are absent from the 33 MP part modules and particle-gameparts. It also
searches for the 4 gompa effects whose only PS4 copies differ from 1.19. It writes findparts.txt and copies every module that
holds a hit to _claude_transfer/parts5/ (skipped when already copied).
Run:   cd ~/mnt/Uncharted-windows/_claude_transfer && python3 findparts.py && tar cf parts5.tar parts5 findparts.txt
Stage: parts5.tar to /mnt/user-data/uploads/Uncharted-windows/_claude_transfer/"""
import os, glob, struct, shutil
G = os.path.join(os.path.expanduser('~/mnt'), 'Uncharted - The Nathan Drake Collection [CUSA02320]', 'u3data', 'build', 'main', 'dc2')
T = os.path.expanduser('~/mnt/Uncharted-windows/_claude_transfer'); O = os.path.join(T, 'parts5'); os.makedirs(O, exist_ok=True)
WANT = {int(x, 16) for x in '''83c51051 8e863688 c351ec10 ce12cac9 047888d5 1722e625 1eae0e13 2414875b 36d46446 38b3eae8 4396706b
45d58b2f 4a9a4091 585aa38c 5f321941 a3415fc0 c4d4ce20 cd33f5d5 ecaea475 fe6e4768 32780aaa cd77997f
8f61485c 911c16ec d3a8aae0 dbb06503'''.split()}
files = sorted(glob.glob(os.path.join(glob.escape(G), 'part-module', '*.bin')) + glob.glob(os.path.join(glob.escape(G), '*.bin')))
hits = {}; lines = []
for p in files:
    try:
        with open(p, 'rb') as f: h = f.read(0x18); n = struct.unpack_from('<I', h, 0x14)[0] if h[:4] == b'00CD' else 0; t = f.read(16 * n)
    except Exception as e: lines.append('ERR %s %s' % (p, e)); continue
    got = []
    for i in range(n):
        off, s, ty = struct.unpack_from('<QII', t, 16 * i)
        if s in WANT: got.append('%08x:%08x' % (s, ty)); hits.setdefault(s, []).append(os.path.basename(p))
    if got:
        lines.append('%s %d syms: %s' % (os.path.relpath(p, G), n, ' '.join(got)))
        dst = os.path.join(O, os.path.basename(p))
        if not os.path.exists(dst) or os.path.getsize(dst) != os.path.getsize(p): shutil.copyfile(p, dst)
lines.append('scanned %d modules; found %d of %d sids' % (len(files), len(hits), len(WANT)))
lines.append('NOT FOUND: ' + ' '.join('%08x' % s for s in sorted(WANT - set(hits))))
open(os.path.join(T, 'findparts.txt'), 'w').write('\n'.join(lines) + '\n'); print('\n'.join(lines[-2:]))
