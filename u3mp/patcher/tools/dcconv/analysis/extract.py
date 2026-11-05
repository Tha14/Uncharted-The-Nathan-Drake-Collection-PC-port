import sys, os
T = sys.argv[1]; OUT = sys.argv[2]
sys.path.insert(0, T + '/dcconv')
from ps3src import Index
DISC = "D:/2. PS Games/PS3/Uncharted 3 - Drake's Deception - Game of the Year Edition [BCES01670]/PS3_GAME/USRDIR"
UPD = "D:/2. PS Games/PS3/RPCS3/dev_hdd0/game/BCES01670/USRDIR"
D2 = "D:/2. PS Games/PS3/RPCS3/dev_hdd0/game/BCES01670DATA2/USRDIR"
idx = Index(DISC, UPD, D2)
n = 0
for rel in idx.names():
    for ly, p, i in idx.layers(rel):
        d = idx.read(rel, ly)
        f = os.path.join(OUT, ly, rel); os.makedirs(os.path.dirname(f), exist_ok=True)
        open(f, 'wb').write(d); n += 1
print('extracted', n)
