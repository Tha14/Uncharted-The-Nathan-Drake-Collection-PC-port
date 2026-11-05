import sys,os,re,collections
import numpy as np
M2=os.path.abspath('map-colors2');sys.path.insert(0,M2);sys.path.insert(0,os.path.abspath('recovered/orange-wall'))
from lib import *
import math
S="{{SCRATCH}}"
def load(f):
  d=open(f,'rb').read();P=Pak(d);it=[i for i in P.items if i.type=='GEOMETRY_1'][0];b=P.pbase(it.page)+it.data_off
  ns=struct.unpack_from('<I',d,b+8)[0];tp,to=P.resolve(it.page,it.data_off+0x10);pb=P.pbase(tp);SM={}
  for i in range(ns):
    o=to+i*0x88;nm=P.pstr(*P.resolve(tp,o+8)) or '';w=struct.unpack_from('<34I',d,pb+o)
    mat=(P.pstr(*P.resolve(tp,o+0x48)) if P.is_ptr(tp,o+0x48) else '') or ''
    st=P.resolve(tp,o+0x30);sq=P.pbase(st[0])+st[1];stride=struct.unpack_from('<H',d,sq+2)[0]
    da=P.resolve(st[0],st[1]+16);vb=P.pbase(da[0])+da[1];ia=P.resolve(tp,o+0x40);ib=P.pbase(ia[0])+ia[1]
    SM[nm]=dict(name=nm,mat=mat.split(',')[0],nv=w[8],ni=w[9],v=vb,s=stride,i=ib)
  npr=struct.unpack_from('<I',d,b+0x18)[0];pp,po=P.resolve(it.page,it.data_off+0x20);PR=[]
  for i in range(npr):
    t=P.resolve(pp,po+i*8);q=P.pbase(t[0])+t[1];c=struct.unpack_from('<I',d,q+4)[0];L=[]
    if c:
      l=P.resolve(t[0],t[1]+0x18)
      for j in range(c):L.append(P.pstr(*P.resolve(l[0],l[1]+j*0x10)) or '')
    PR.append(L)
  ni=struct.unpack_from('<I',d,b+0x28)[0];pp,po=P.resolve(it.page,it.data_off+0x30);IN=[]
  for i in range(ni):
    t=P.resolve(pp,po+i*8);q=P.pbase(t[0])+t[1];nm=P.pstr(*P.resolve(t[0],t[1]+8)) or '';mm=re.match(r'Prototype(\d+)',nm)
    if mm:IN.append((int(mm.group(1)),np.array(struct.unpack_from('<16f',d,q+0x80)).reshape(4,4)))
  return d,SM,PR,IN
OVD=OV
for pak in sys.argv[1:]:
  d,SM,PR,IN=load(OV+pak);A=collections.defaultdict(float);BB=collections.defaultdict(list)
  for pi,M in IN:
    for sn in PR[pi]:
      s=SM.get(sn)
      if not s or not re.search('corrug|metal-sheet|container',s['mat']):continue
      V=np.frombuffer(d,dtype=np.uint8,count=s['nv']*s['s'],offset=s['v']).reshape(s['nv'],s['s'])[:,:12].tobytes();V=np.frombuffer(V,dtype='<f4').reshape(-1,3)
      Vw=np.c_[V,np.ones(len(V))]@M;Vw=Vw[:,:3]
      I=np.frombuffer(d,dtype='<u2',count=s['ni'],offset=s['i']).reshape(-1,3)
      I=I[(I[:,0]!=I[:,1])]
      if len(I)==0:continue
      a=np.linalg.norm(np.cross(Vw[I[:,1]]-Vw[I[:,0]],Vw[I[:,2]]-Vw[I[:,0]]),axis=1).sum()/2
      k=s['mat'].split('/')[-1];A[k]+=a;BB[k].append((Vw.min(0).round(1).tolist(),Vw.max(0).round(1).tolist()))
  print('==',pak)
  for k,v in sorted(A.items(),key=lambda x:-x[1]):
    print(round(v),k,len(BB[k]),BB[k][:3])
wj=json.load(open('excess-wall/air-wall.json'))
d0=open(W+'backup-excess/air-multi-strip.pak','rb').read()
open(os.environ.get('TMPP',S+'/wb/strip0.pak'),'wb').write(d0)
d,SM,PR,IN=load(S+'/wb/strip0.pak')
off={t[0]:t for t in wj['tris']}
for n,s in SM.items():
  ib=s['i']
  h=[o for o in off if ib<=o<ib+s['ni']*2]
  if h:
    insts=[(pi,M) for pi,M in IN if n in PR[pi]]
    V=np.frombuffer(np.frombuffer(d,dtype=np.uint8,count=s['nv']*s['s'],offset=s['v']).reshape(s['nv'],s['s'])[:,:12].tobytes(),dtype='<f4').reshape(-1,3)
    print('STRIP excess',n,s['mat'],len(h),'insts',len(insts))
    for pi,M in insts:
      vi=sorted({x for o in h for x in off[o][1:]});Vw=(np.c_[V[vi],np.ones(len(vi))]@M)[:,:3];print('  bbox',Vw.min(0).round(1).tolist(),Vw.max(0).round(1).tolist())
