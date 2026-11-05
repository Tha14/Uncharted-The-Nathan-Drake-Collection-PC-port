"""ASSETS agent PC job (read-only on the game folders; writes only _claude_transfer/assets-pc/).
Run:  cd ~/mnt/Uncharted-windows/_claude_transfer && python3 fetch_assets.py
1) TOC (index, size, name) of every PS4 *.psarc in the PS4 game folder -> assets-pc/toc/ps4-*.txt
2) TOC of PS3 animstream4 / text1 / sound1 banks psarcs -> assets-pc/toc/ps3-*.txt
3) extracts PS3 actor33 paks named in want-actors.txt (actor33.psarc, goty-patch, goty-dlc) -> assets-pc/actor33/
4) extracts all of PS3 text1.psarc and every goty-patch.psarc entry that is not pak23/actor33 -> assets-pc/ps3-text1/, assets-pc/goty-patch/
5) extracts PS4 text files (any psarc entry under a text dir) -> assets-pc/ps4-text/
Resumable: existing outputs are skipped.  Then: tar cf assets-pc.tar assets-pc"""
import os, sys, glob
T = os.path.expanduser('~/mnt/Uncharted-windows/_claude_transfer')
sys.path.insert(0, T + '/maps/tools')
from psarc import Psarc
U = os.path.expanduser('~/mnt/PS3_GAME/USRDIR')
G4 = os.path.expanduser('~/mnt/Uncharted - The Nathan Drake Collection [CUSA02320]')
O = T + '/assets-pc'
for d in ('toc', 'actor33', 'ps3-text1', 'goty-patch', 'ps4-text'):
    os.makedirs(O + '/' + d, exist_ok=True)
log = open(O + '/fetch.log', 'a')
def L(*a):
    s = ' '.join(str(x) for x in a); print(s, flush=True); log.write(s + '\n'); log.flush()
def toc(p, out):
    if os.path.exists(out): return
    a = Psarc(p)
    with open(out + '.tmp', 'w') as f:
        for i, n in enumerate(a.names): f.write('%d %d %s\n' % (i + 1, a.entries[i + 1][2], n))
    os.replace(out + '.tmp', out); L('toc', p, len(a.names))
def save(a, i, dst):
    if os.path.exists(dst): return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst + '.tmp', 'wb').write(a.read(i + 1)); os.replace(dst + '.tmp', dst)
# 1
for p in sorted(glob.glob(G4 + '/**/*.psarc', recursive=True)):
    toc(p, O + '/toc/ps4-' + os.path.relpath(p, G4).replace('/', '_') + '.txt')
# 2
for rel in ('build/main/animstream4.psarc', 'build/main/text1.psarc', 'build/main/sound1/banks.psarc', 'build/main/actor33.psarc'):
    if os.path.exists(U + '/' + rel): toc(U + '/' + rel, O + '/toc/ps3-' + os.path.basename(rel) + '.txt')
for rel in ('goty-dlc.psarc', 'goty-patch.psarc'):
    if os.path.exists(U + '/' + rel): toc(U + '/' + rel, O + '/toc/ps3-' + rel + '.txt')
# 3
want = set(l.strip() for l in open(T + '/want-actors.txt') if l.strip())
got = set()
for rel in ('goty-patch.psarc', 'goty-dlc.psarc', 'build/main/actor33.psarc'):
    p = U + '/' + rel
    if not os.path.exists(p): L('missing archive', p); continue
    a = Psarc(p)
    for i, n in enumerate(a.names):
        b = os.path.basename(n)
        if b.endswith('.pak') and b[:-4] in want and b[:-4] not in got and ('actor33' in n or '/' not in n.strip('/')):
            save(a, i, O + '/actor33/' + b); got.add(b[:-4]); L('actor', rel, n)
L('actors got', len(got), 'of', len(want), 'missing:', ' '.join(sorted(want - got)))
# 4
p = U + '/build/main/text1.psarc'
if os.path.exists(p):
    a = Psarc(p)
    for i, n in enumerate(a.names): save(a, i, O + '/ps3-text1/' + n.lstrip('/'))
    L('ps3 text1', len(a.names))
p = U + '/goty-patch.psarc'
if os.path.exists(p):
    a = Psarc(p); k = 0
    for i, n in enumerate(a.names):
        if '/pak23/' in n or '/actor33/' in n or n.endswith('pak23.txt'): continue
        save(a, i, O + '/goty-patch/' + n.lstrip('/')); k += 1
    L('goty-patch extra', k)
# 5
for p in sorted(glob.glob(G4 + '/**/*.psarc', recursive=True)):
    a = Psarc(p); k = 0
    for i, n in enumerate(a.names):
        if 'text' in n.lower().split('/')[0] or '/text' in n.lower():
            save(a, i, O + '/ps4-text/' + os.path.basename(p) + '/' + n.lstrip('/')); k += 1
    L('ps4 text', p, k)
L('DONE')
