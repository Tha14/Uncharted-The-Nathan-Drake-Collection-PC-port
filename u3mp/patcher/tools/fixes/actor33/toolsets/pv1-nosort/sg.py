"""PS3 SPAWNER_GROUP reader."""
import struct,collections
from ndpak import Pak
def rd(p,pg,o,f):
  return struct.unpack_from(p.e+f,p.d,p.pbase(pg)+o)
def spawners(p):
  out=[]
  for it in p.items_of('SPAWNER_GROUP'):
    pg,base=it.page,it.data_off
    ps=p.ptrsize
    if ps==4:
      arr=p.resolve(pg,base+4); n=rd(p,pg,base+8,'I')[0]
    else:
      arr=p.resolve(pg,base+8); n=rd(p,pg,base+16,'I')[0]
    for k in range(n):
      sp=p.resolve(arr[0],arr[1]+ps*k)
      out.append((it,sp))
  return out
def spawner_info(p,sp):
  pg,o=sp; ps=p.ptrsize
  pos=rd(p,pg,o,'4f'); q=rd(p,pg,o+16,'4f')
  name=p.pstr(*p.resolve(pg,o+32)) if p.resolve(pg,o+32) else None
  sid=rd(p,pg,o+32+ps,'I')[0]
  tagoff= 68 if ps==4 else 104
  tags={}
  t=p.resolve(pg,o+tagoff)
  if t:
    tb=p.resolve(t[0],t[1]); n=rd(p,t[0],t[1]+ps,'I')[0]
    for k in range(n):
      eo=tb[1]+k*(8 if ps==4 else 16)
      ksid=rd(p,tb[0],eo,'I')[0]; v=p.resolve(tb[0],eo+(4 if ps==4 else 8))
      ty,sz=rd(p,v[0],v[1],'HH'); dp=p.resolve(v[0],v[1]+(4 if ps==4 else 8))
      tags[ksid]=(ty,sz,dp)
  return dict(pos=pos,q=q,name=name,sid=sid,tags=tags)
if __name__=='__main__':
  import sys,glob
  tc=collections.Counter()
  for f in sys.argv[1:]:
    p=Pak(f)
    for it,sp in spawners(p):
      i=spawner_info(p,sp)
      for k,(ty,sz,dp) in i['tags'].items(): tc[(ty,sz)]+=1
  print(tc)
