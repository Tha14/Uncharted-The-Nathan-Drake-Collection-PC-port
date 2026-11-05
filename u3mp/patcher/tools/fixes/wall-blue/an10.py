from ilib import *
sys.path.insert(0,'{{PAKTOOLS}}')
import texcodec as tc
q=Pak(OV+'air-multi.pak')
tm={}
for t in q.textures():
  if 'lightmaps/' in t['path']:
    kind={0x47:'DXT1',0x4d:'DXT5'}[t['format']]
    tm['max' if 'rgbmax.' in t['path'] else 'delta']=tc.decode(tc.ps4_mip0_linear(q.vram_bytes(t['vramOffset'],t['vramSize']),t['width'],t['height'],8 if kind=='DXT1' else 16),t['width'],t['height'],kind).astype(float)
g=q.items_of('GEOMETRY_1')[0];pg,b=g.page,g.data_off
nsub=q.u64(q.pbase(pg)+b+8);sr=q.resolve(pg,b+0x10);SM={}
for si in range(nsub):
  so=sr[1]+0x88*si;mr=q.resolve(sr[0],so+0x48);nmr=q.resolve(sr[0],so+8)
  if mr and nmr:SM[q.pstr(*nmr)]=(q.pstr(*mr).split(',')[0],struct.unpack_from('<I',q.d,q.pbase(sr[0])+so+0x20)[0])
ni=q.pu32(pg,b+0x28);ip=q.resolve(pg,b+0x30);R=collections.defaultdict(list)
for i in range(ni):
  t=q.resolve(ip[0],ip[1]+8*i)
  if not t:continue
  ns=q.pu32(t[0],t[1]+0x20);sp=q.resolve(t[0],t[1]+0x30)
  if not sp:continue
  for k in range(ns):
    e=sp[1]+0x50*k;nm=q.pstr(*q.resolve(sp[0],e))
    if nm not in SM:continue
    mat,nv=SM[nm];n=q.pu32(sp[0],e+0x10);st=q.resolve(sp[0],e+0x18)
    for j in range(n if st else 0):
      ee=st[1]+0x18*j;f=q.resolve(st[0],ee+8);fb=q.pbase(f[0])+f[1];da=q.resolve(st[0],ee+0x10)
      if q.d[fb]==5 and da:
        stride=q.pu32(st[0],ee)>>16;uv=np.frombuffer(q.d,dtype='<f2',count=nv*2,offset=q.pbase(da[0])+da[1]).reshape(nv,2).astype(float)
        H,W_=tm['max'].shape[:2];x=(uv[:,0]%1*W_).astype(int);y=(uv[:,1]%1*H).astype(int)
        R[mat].append((tm['max'][y,x],tm['delta'][y,x]))
for k,v in sorted(R.items()):
  if re.search('corrugated|roof-cement|sheet-wall|concrete-multi:',k):
    M=np.concatenate([a for a,_ in v]);D=np.concatenate([b for _,b in v]);print(k.split('/')[-1],M.shape[0],'rgbmax',M.mean(0).round(0).tolist(),'delta',D[:,:3].mean(0).round(0).tolist(),'b/r %.2f'%(M[:,2].mean()/M[:,0].mean()))
