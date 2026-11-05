import sys,collections
sys.path.insert(0,'.')
from common import DC
R="{{PROJECT}}/work/"
a=DC(R+'backup-classic/net.bin');b=DC(R+'overlay-r3/u3data/build/main/dc1/ps4/net.bin')
q=a.bysid[0x3b9a067d][1];ar=a.ptr(q+8);q2=b.bysid[0x3b9a067d][1];br=b.ptr(q2+8)
C=collections.Counter()
for i in range(a.u32(q)):
  C[(a.u32(ar+i*0xd0+0xc),a.u32(ar+i*0xd0+0x68),a.u32(ar+i*0xd0+0x6c),b.u32(br+i*0xd0+0x68),b.u32(br+i*0xd0+0x6c))]+=1
print('type old68 old6c new68 new6c count');[print(k,v) for k,v in sorted(C.items())]
