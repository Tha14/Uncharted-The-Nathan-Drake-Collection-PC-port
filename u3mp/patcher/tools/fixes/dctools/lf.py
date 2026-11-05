import sys,collections
sys.path.insert(0,'.')
from common import DC
b=DC("{{PS3_DATA2}}/net18.bin")
o=b.bysid[0x3b9a067d][1];n=b.u32(o);ar=b.ptr(o+4);c=collections.Counter();x=[]
for i in range(n):
  e=ar+i*0xe4;v=b.u32(e+0x68);c[v]+=1
  if v:x.append(hex(b.u32(e)))
print(c,x[:24])
