from ilib import *
sys.path.insert(0,'{{PAKTOOLS}}/stage')
import bggeom
os.chdir(M2)
RX='corrugated-main-blue'
q=Pak(OV+'air-multi.pak');g=q.items_of('GEOMETRY_1')[0];pg,b=g.page,g.data_off;n=q.u64(q.pbase(pg)+b+8);sr=q.resolve(pg,b+0x10);V4={}
for si in range(n):
  so=sr[1]+0x88*si;mr=q.resolve(sr[0],so+0x48)
  if not mr:continue
  nm=q.pstr(*mr).split(',')[0]
  if RX not in nm:continue
  nv,ni,ns=struct.unpack_from('<III',q.d,q.pbase(sr[0])+so+0x20);st=q.resolve(sr[0],so+0x30)
  for k in range(ns):
    e=st[1]+0x18*k;f=q.resolve(st[0],e+8);fb=q.pbase(f[0])+f[1];da=q.resolve(st[0],e+0x10)
    if q.d[fb]==4:V4[nm.split('/')[-1].split(':')[0]]=np.frombuffer(q.d,dtype='<f2',count=nv*4,offset=q.pbase(da[0])+da[1]).reshape(nv,4).astype(float)
p=Pak('p3/air-multi.pak')
for pr in bggeom.collection(p):
  for bk,m in zip(pr['blocks'],pr['mats']):
    if m and RX in m:
      r=bggeom.decode_block(p,bk[0],bk[1]);k=m.split('/')[-2];c=r['color'];a=V4.get(k)
      print(k,c.shape,a.shape if a is not None else None,'max abs diff',float(np.abs(a[:,:3]-c).max()) if a is not None and len(a)==len(c) else 'n/a','overlay alpha',set(np.round(a[:,3],2).tolist()) if a is not None else '')
