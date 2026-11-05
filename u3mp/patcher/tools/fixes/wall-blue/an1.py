import sys,os,re,json
import numpy as np
M2=os.path.abspath('../map-colors2');sys.path.insert(0,M2);os.chdir(M2)
from lib import *
PT='{{PAKTOOLS}}'
sys.path.insert(0,PT+'/stage')
import bggeom
def p4streams(q,rx):
  g=q.items_of('GEOMETRY_1')[0];pg,b=g.page,g.data_off
  n=q.u64(q.pbase(pg)+b+8);sr=q.resolve(pg,b+0x10);out=collections.defaultdict(lambda:[0,0,[]])
  for si in range(n):
    so=sr[1]+0x88*si;mr=q.resolve(sr[0],so+0x48)
    if not mr: continue
    nm=q.pstr(*mr).split(',')[0]
    if not re.search(rx,nm): continue
    nv,ni,ns=struct.unpack_from('<III',q.d,q.pbase(sr[0])+so+0x20)
    st=q.resolve(sr[0],so+0x30)
    out[nm][0]+=1;out[nm][1]+=nv
    for k in range(ns):
      e=st[1]+0x18*k;f=q.resolve(st[0],e+8);fb=q.pbase(f[0])+f[1];aid=q.d[fb]
      stride=q.pu32(st[0],e)>>16;da=q.resolve(st[0],e+0x10)
      if aid==4:
        c=np.frombuffer(q.d,dtype=np.uint8,count=nv*stride,offset=q.pbase(da[0])+da[1]).reshape(nv,stride)[:,:4]
        out[nm][2].append(c)
  return out
RX='corrugated-main-blue|corrugated-main:|corrugated-main-orange:|hangar-blue|corrugated-main-white|corrugated-wall:'
def show(tag,u):
  print('==',tag)
  for k,(a,nv,c) in sorted(u.items()):
    if c:
      x=np.concatenate(c);pc=np.percentile(x,[5,50,95],axis=0).astype(int).tolist();print(k.split('/')[-1],a,nv,'mean',x.mean(0).round(1).tolist(),'p5/50/95',pc)
    else: print(k.split('/')[-1],a,nv,'no colour stream')
show('overlay air-multi',p4streams(Pak(OV+'air-multi.pak'),RX))
show('retail airport-1',p4streams(Pak(RT+'airport-1.pak'),RX))
show('retail airport-rest',p4streams(Pak(RT+'airport-rest.pak'),RX))
p=Pak('p3/air-multi.pak')
C=collections.defaultdict(list);cnt=collections.Counter();ids=collections.Counter()
for pr in bggeom.collection(p):
  mats=pr['mats'] if 'mats' in pr else None
  blocks=pr['blocks']
  for bk,m in zip(blocks,mats):
    if m and re.search(RX.replace(':','/'),m):
      r=bggeom.decode_block(p,bk[0],bk[1]);cnt[m]+=r['nverts'];ids[(m,tuple(r['order']))]+=1
      if 'color' in r:C[m].append(r['color'])
print('== PS3')
for m,n in cnt.items():
  if C[m]:
    x=np.concatenate(C[m])*255;print(m.split('/')[-2:],n,'comps',x.shape[1],'mean',x.mean(0).round(1).tolist(),'p5/50/95',np.percentile(x,[5,50,95],axis=0).astype(int).tolist())
  else: print(m,n,'no color')
print(ids.most_common(8))
print('== textures')
import texcodec as tc
def st(q,tag,ps3=False):
  for t in q.textures():
    s=t['path'].split('/')[-2]
    if re.search('corrugated-wall(-blend|-painted-blend|-color)?\.tga',s):
      if ps3:
        kind,bpb,comp=tc.ps3_format(t);d=q.vram_bytes(t['vramOffset'],t['vramSize']);img=tc.decode(d[:tc.ps3_linear_size(t['width'],t['height'],1,bpb)],t['width'],t['height'],kind);print(tag,s,kind,hex(t['gcmRemapCtl']),t['width'],t['height'],t['mips'],'mean',img.reshape(-1,img.shape[-1]).mean(0).round(1).tolist())
      else:
        print(tag,s,{k:(hex(v) if isinstance(v,int) else v) for k,v in t.items() if k not in('path',)})
st(p,'ps3',True);st(Pak(OV+'air-multi.pak'),'ov');st(Pak(RT+'airport-1.pak'),'rt1')
