import sys, os, struct, hashlib, collections
T = sys.argv[1]
sys.path.insert(0, T + '/dcconv'); sys.path.insert(0, T + '/fixes/dctools')
from ps3src import Index
DISC = "D:/2. PS Games/PS3/Uncharted 3 - Drake's Deception - Game of the Year Edition [BCES01670]/PS3_GAME/USRDIR"
UPD = "D:/2. PS Games/PS3/RPCS3/dev_hdd0/game/BCES01670/USRDIR"
D2 = "D:/2. PS Games/PS3/RPCS3/dev_hdd0/game/BCES01670DATA2/USRDIR"
RET = "D:/2. PS Games/PS4/Uncharted - The Nathan Drake Collection [CUSA02320]/u3data/build/main/"
P = sys.argv[2]
idx = Index(DISC, UPD, D2)
print('ps3 dc entries', len(idx.names()), collections.Counter(l[0][0] for l in idx.map.values()))
rows = []
for ov in ('overlay-r3', 'overlay-r2', 'overlay-port'):
    for sub in ('dc1', 'dc2'):
        b = P + '/work/' + ov + '/u3data/build/main/' + sub
        if not os.path.isdir(b): continue
        for dp, dn, fn in os.walk(b):
            for f in fn:
                rel = os.path.relpath(os.path.join(dp, f), P + '/work/' + ov + '/u3data/build/main').replace(os.sep, '/')
                d = open(os.path.join(dp, f), 'rb').read()
                L = idx.layers(rel)
                ret = os.path.exists(RET + rel)
                kind = d[:4]
                ps3 = [ly for ly, _, _ in L]
                rows.append((ov, rel, len(d), kind, ps3, ret))
for r in rows:
    if not r[1].endswith('.dci'):
        print('%-12s %-55s %7d %s ps3=%s retail=%s' % r)
print(collections.Counter((r[0], r[3], bool(r[4]), r[5]) for r in rows))
