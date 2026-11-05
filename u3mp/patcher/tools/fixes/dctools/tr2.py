import sys
sys.path.insert(0,'.')
from common import DC
import collections
f="{{PROJECT}}/work/overlay-r3/u3data/build/main/dc1/ps4/net.bin"
a=DC(f)
q=a.bysid[0xf1f296cb][1];ar=a.ptr(q+8);sets={a.u32(ar+i*12) for i in range(a.u32(q))}
q=a.bysid[0xb114fe00][1];ar=a.ptr(q+8);n=a.u32(q)
for o in range(0,0x68,4):
  c=sum(a.u32(ar+i*0x68+o) in sets for i in range(n))
  if c:print(hex(o),c,n)
for o in(0,0x10,0x18,0x20,0x24):print(hex(o),len({a.u32(ar+i*0x68+o) for i in range(n)}))
for i in range(4):print(a.cstr(a.ptr(ar+i*0x68+8)))
