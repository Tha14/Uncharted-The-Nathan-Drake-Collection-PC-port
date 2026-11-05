import sys,struct,re,os,collections
sys.path.insert(0,'.')
from common import DC,sid
ROOT="{{PROJECT}}/work/"
W=ROOT+"overlay-r3/u3data/build/main/dc1/ps4/"
b=DC("{{PS3_DATA2}}/net18.bin");a=DC(W+"net.bin")
print('sym sample a',a.syms[:2],'count',len(a.syms),'b',len(b.syms));print([k for k in dir(a) if not k.startswith('_')])
def dumpb(c,o,n=24,e=None):
  r=[]
  for k in range(0,n*c.ps,c.ps):
    if c.isptr(o+k):
      p=c.ptr(o+k);s=c.cstr(p);r.append('->%s'%(s[:30] if s else hex(p)))
    else:r.append(hex(c.u32(o+k)))
  return r
for s in(0x3719ce57,0x4ea65a02):
  t,o=b.bysid[s];print(hex(s),'type',hex(t),'off',hex(o),dumpb(b,o,12))
  p=b.ptr(o+4) if b.isptr(o+4) else None
  print(' n',b.u32(o));
  if p:print(' arr',dumpb(b,p,min(b.u32(o)*2,40)))
# weapons used by PS4 netgames
o=a.bysid[0x3b9a067d][1];n=a.u32(o);ar=a.ptr(o+8);c=collections.Counter(a.u32(ar+i*0xd0+0x2c) for i in range(n))
print('ps4 wpn ids',[(hex(k),v,k in a.bysid) for k,v in c.most_common(12)])
for k,_ in c.most_common(2):
  if k in a.bysid:t,oo=a.bysid[k];print(hex(k),hex(t),'n',a.u32(oo),dumpb(a,oo,6));pp=a.ptr(oo+8) if a.isptr(oo+8) else None;print(' arr',dumpb(a,pp,8) if pp else '')
