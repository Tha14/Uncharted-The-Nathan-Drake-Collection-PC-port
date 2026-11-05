import sys
sys.path.insert(0,'.')
from common import DC
R="{{PROJECT}}/work/overlay-r3/u3data/build/main/dc1/ps4/net-always-loaded.bin"
a=DC(R)
for s in (0x8cf8e1cc,0xd932420c):
  t,o=a.bysid[s];n=a.u32(o);ar=a.ptr(o+8);print(hex(s),hex(o),n,hex(ar),[hex(a.u32(ar+k)) for k in range(0,0x20,4)])
