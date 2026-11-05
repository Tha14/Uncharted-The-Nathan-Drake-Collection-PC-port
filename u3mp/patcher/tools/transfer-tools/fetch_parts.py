#!/usr/bin/env python3
"""v2: glob.escape (the game folder name has [CUSA02320], which glob read as a character class). ASSETS PC job (read-only on the game; writes _claude_transfer/parts4/). Copies the PS4 particle modules needed to learn
the PS3->PS4 dc2 particle layout and to merge the 127 missing PS3 1.19 effects: all 33 MP part modules plus
dc2/particle-gameparts.bin and particle-settings.bin (types/defaults), ~3 MB.
Run: cd ~/mnt/Uncharted-windows/_claude_transfer && python3 fetch_parts.py && tar cf parts4.tar parts4
Stage parts4.tar to /mnt/user-data/uploads/Uncharted-windows/_claude_transfer/"""
import os, shutil, glob
G = os.path.join(os.path.expanduser('~/mnt'), 'Uncharted - The Nathan Drake Collection [CUSA02320]', 'u3data', 'build', 'main', 'dc2')
O = os.path.expanduser('~/mnt/Uncharted-windows/_claude_transfer/parts4'); os.makedirs(O, exist_ok=True)
n = 0
for p in glob.glob(os.path.join(glob.escape(G), 'part-module', '*multi*.bin')) + [os.path.join(G, f) for f in ('particle-gameparts.bin', 'particle-settings.bin', 'particle-override.bin')]:
    if os.path.exists(p): shutil.copyfile(p, os.path.join(O, os.path.basename(p))); n += 1
for p in glob.glob(os.path.join(glob.escape(G), 'part-module', '*.dci')) + glob.glob(os.path.join(glob.escape(G), '*.dci')):
    shutil.copyfile(p, os.path.join(O, os.path.basename(p)))
print('copied', n, 'modules; listing of dc2:'); print('\n'.join(sorted(os.listdir(G))[:50]))
