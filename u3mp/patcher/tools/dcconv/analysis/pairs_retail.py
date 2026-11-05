import sys, os, collections
T = sys.argv[1]
sys.path.insert(0, T + '/dcconv'); sys.path.insert(0, T + '/fixes/dctools')
from ps3src import Index
from common import DC
DISC = "D:/2. PS Games/PS3/Uncharted 3 - Drake's Deception - Game of the Year Edition [BCES01670]/PS3_GAME/USRDIR"
UPD = "D:/2. PS Games/PS3/RPCS3/dev_hdd0/game/BCES01670/USRDIR"
D2 = "D:/2. PS Games/PS3/RPCS3/dev_hdd0/game/BCES01670DATA2/USRDIR"
RET = "D:/2. PS Games/PS4/Uncharted - The Nathan Drake Collection [CUSA02320]/u3data/build/main/"
idx = Index(DISC, UPD, D2)
rel = []
for dp, dn, fn in os.walk(RET + 'dc1'):
    for f in fn:
        if f.endswith('.bin'): rel.append(os.path.relpath(os.path.join(dp, f), RET).replace(os.sep, '/'))
print('retail dc1 bins', len(rel))
same = 0; rows = []
for r in sorted(rel):
    L = idx.layers(r)
    if not L: rows.append((r, 'no ps3')); continue
    b = DC(RET + r)
    best = None
    for ly, p, i in L:
        d = idx.read(r, ly)
        open('_t.bin', 'wb').write(d); a = DC('_t.bin')
        k = (len(a.ptrs) == len(b.ptrs), len(a.syms) == len(b.syms))
        rows.append((r, ly, len(a.d), len(b.d), len(a.ptrs), len(b.ptrs), len(a.syms), len(b.syms)))
for x in rows: print(*x)
