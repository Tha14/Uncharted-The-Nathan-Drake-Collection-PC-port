import sys,os,re,collections,struct
import numpy as np
M2=os.path.abspath(os.path.dirname(os.path.abspath(__file__))+'/../map-colors2');sys.path.insert(0,M2)
from lib import *
def h2f(a): return np.frombuffer(a.tobytes(),dtype='<f2').astype(float)
def instlight(q,rx=None):
  """{material:[N*4 float arrays of ilight]} + flags count"""
  g=q.items_of('GEOMETRY_1')[0];pg,b=g.page,g.data_off
  nsub=q.u64(q.pbase(pg)+b+8);sr=q.resolve(pg,b+0x10);SM={}
  for si in range(nsub):
    so=sr[1]+0x88*si;mr=q.resolve(sr[0],so+0x48);nmr=q.resolve(sr[0],so+8)
    if mr and nmr:SM[q.pstr(*nmr)]=(q.pstr(*mr).split(',')[0],struct.unpack_from('<I',q.d,q.pbase(sr[0])+so+0x20)[0])
  ni=q.pu32(pg,b+0x28);ip=q.resolve(pg,b+0x30);out=collections.defaultdict(list);lm=collections.Counter()
  for i in range(ni):
    t=q.resolve(ip[0],ip[1]+8*i)
    if not t:continue
    ns=q.pu32(t[0],t[1]+0x20);sp=q.resolve(t[0],t[1]+0x30)
    if not sp:continue
    for k in range(ns):
      e=sp[1]+0x50*k;nm=q.pstr(*q.resolve(sp[0],e))
      if nm not in SM:continue
      mat,nv=SM[nm]
      if rx and not re.search(rx,mat):continue
      n=q.pu32(sp[0],e+0x10);st=q.resolve(sp[0],e+0x18);fl=q.pu32(sp[0],e+0x20)
      if fl&0x100:lm[mat]+=1
      for j in range(n if st else 0):
        ee=st[1]+0x18*j;f=q.resolve(st[0],ee+8);fb=q.pbase(f[0])+f[1];da=q.resolve(st[0],ee+0x10)
        if q.d[fb]==8 and da:
          stride=q.pu32(st[0],ee)>>16;a=np.frombuffer(q.d,dtype='<f2',count=nv*stride//2,offset=q.pbase(da[0])+da[1]).reshape(nv,-1)
          out[mat].append(a.astype(float))
  return out,lm
