from lib import *
import psarc_lib
RET="{{PS4}}/u3data/build/main/pak23/ps4/"
PS3="{{PS3_DISC}}/build/main/pak23.psarc"
pk=psarc_lib.Psarc(PS3)
def p3(n):
  c=[x for x in pk.names if x.endswith('/'+n+'.pak')]
  return Pak(pk.read(pk.names.index(c[0])+1)) if c else None
def ps3tex(p,t):
    w,h=t['width'],t['height'];data=p.vram_bytes(t['vramOffset'],t['vramSize']);kind,bpb,_=tc.ps3_format(t);lin=data[:tc.ps3_linear_size(w,h,1,bpb)];return tc.decode(bytes(lin),w,h,kind),t
for n in sys.argv[1:]:
    q=Pak(RET+n+'.pak');p=p3(n)
    if p is None:print(n,'no ps3');continue
    for sub in('indirect.rgbmax.','indirect.rgbmaxdelta.'):
        A=[t for t in q.textures() if sub in t['path']];B=[t for t in p.textures() if sub in t['path']]
        if not A or not B:print(n,sub,'missing');continue
        a=A[0];b=B[0]
        print(n,sub,'ps4',a['width'],a['height'],hex(a['format']),a['mips'],a['unk34'],'ps3',b['width'],b['height'],hex(b['gcmRemapCtl']),b['mips'],'same-hash',a['hash']==b['hash'],a['path'][-20:],b['path'][-20:])
        if sub=='indirect.rgbmaxdelta.':continue
        ia=load_tex(q,a).astype(float)/255;ib,_=ps3tex(p,b);ib=ib.astype(float)/255
        if ia.shape!=ib.shape:
            s=ib.shape[0]//ia.shape[0] if ib.shape[0]>ia.shape[0] else 1
            if s>1:ib=ib[::s,::s]
            elif ia.shape[0]>ib.shape[0]:ia=ia[::ia.shape[0]//ib.shape[0],::ia.shape[0]//ib.shape[0]]
        print('  ps4 rgba mean',ia.reshape(-1,4).mean(0).round(3),'ps3',ib.reshape(-1,4).mean(0).round(3))
        x=ib[...,3].ravel();y=ia[...,3].ravel();bins=np.linspace(0,1,17)
        rows=[(round(float(lo),3),int(k.sum()),round(float(y[k].mean()),3)) for lo,hi in zip(bins[:-1],bins[1:]) for k in[(x>=lo)&(x<hi)] if k.sum()>200]
        print('  ps3a->ps4a',rows)
        ca=ia[...,:3].mean(2).ravel();cb=ib[...,:3].mean(2).ravel();print('  rgb corr',np.corrcoef(ca,cb)[0,1].round(3),'a corr',np.corrcoef(x,y)[0,1].round(3),'cb mean',cb.mean().round(3),'ca',ca.mean().round(3))
        np.savez('pair-%s.npz'%n,ia=ia.astype('f4'),ib=ib.astype('f4'))
