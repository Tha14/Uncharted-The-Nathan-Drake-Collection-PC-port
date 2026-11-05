"""Seven Airstrip emblem paks: per-instance cbuffer capacity +0x30 to 3, the crashcheck G6 minimum (1 feature set + 2).
On the overlay it was the emblem-test value 2; a fresh wall-emblem build (mkemblem.py) has 8 there. Originals in work/backup-emblem-test."""
import struct,subprocess,os
B='work/overlay-r3/u3data/build/main/actor33/ps4/air-multi-emblem-%s.pak'
for n,o in (('diamond',0x6d220),('corr',0x6cd80),('corr-blue',0x6c740),('tower',0x6c740),('stucco',0x6c7d0),('board-0',0x6c780),('board-1',0x6c780)):
    if not os.path.exists(B%n):print(n,'missing (needs the wall-emblem step first)');continue
    d=bytearray(open(B%n,'rb').read());v=struct.unpack_from('<I',d,o)[0]
    if v==3:print(n,'already 3');continue
    assert v in (2,8),(n,v)
    struct.pack_into('<I',d,o,3);open(B%n,'wb').write(d);print(n,v,'-> 3')
