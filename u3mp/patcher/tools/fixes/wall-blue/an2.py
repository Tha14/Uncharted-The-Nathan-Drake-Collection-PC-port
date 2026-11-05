import sys,os,re
import numpy as np
M2=os.path.abspath('../map-colors2');sys.path.insert(0,M2);os.chdir(M2)
from lib import *
PT='{{PAKTOOLS}}'
sys.path.insert(0,PT+'/stage')
import bggeom
q=Pak(OV+'air-multi.pak');g=q.items_of('GEOMETRY_1')[0];pg,b=g.page,g.data_off
n=q.u64(q.pbase(pg)+b+8);sr=q.resolve(pg,b+0x10)
for si in range(n):
  so=sr[1]+0x88*si;mr=q.resolve(sr[0],so+0x48)
  if not mr: continue
  nm=q.pstr(*mr).split(',')[0]
  if 'corrugated-main-blue:' not in nm: continue
  nv,ni,ns=struct.unpack_from('<III',q.d,q.pbase(sr[0])+so+0x20);st=q.resolve(sr[0],so+0x30)
  print(nm,nv,ni,ns)
  for k in range(ns):
    e=st[1]+0x18*k;f=q.resolve(st[0],e+8);fb=q.pbase(f[0])+f[1];stride=q.pu32(st[0],e)>>16;da=q.resolve(st[0],e+0x10)
    print(k,'fmt',q.d[fb:fb+8].hex(),'word',hex(q.pu32(st[0],e)),'stride',stride)
    if q.d[fb]==4:
      c4=np.frombuffer(q.d,dtype=np.uint8,count=nv*stride,offset=q.pbase(da[0])+da[1]).reshape(nv,stride);print(c4[:12].tolist())
    if q.d[fb]==1:
      pos4=np.frombuffer(q.d,dtype='<f4',count=nv*stride//4,offset=q.pbase(da[0])+da[1]).reshape(nv,stride//4)
p=Pak('p3/air-multi.pak')
for pr in bggeom.collection(p):
  for bk,m in zip(pr['blocks'],pr['mats']):
    if m and 'corrugated-main-blue/' in m:
      pgb,blk=bk;fpg,fmt=p.resolve(pgb,blk+0x28);A=bggeom.read_format(p,fpg,fmt)
      print(m,[(hex(a[0]),hex(a[1]),a[3]) for a in A])
      r=bggeom.decode_block(p,pgb,blk);print(r['nverts'],(r['color'][:12]*255).round(1).tolist())
      print('prec',bggeom.block_prec(A));print('pos',r['pos'][:3],'pos4',pos4[:3])
