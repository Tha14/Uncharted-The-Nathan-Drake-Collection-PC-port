from lib import *
import numpy as np, texcodec as tc
p3=Pak('p3/air-multi.pak');q=Pak(OV+'air-multi.pak')
T3={t['path'].split('/')[-2]:t for t in p3.textures()};T4={t['path'].split('/')[-2]:t for t in q.textures()}
def ps3img(n):
    t=T3[n];k=tc.ps3_format(t);print(n,k,t['width'],t['height'],t['mips'],hex(t['gcmRemapCtl']))
    d=p3.vram_bytes(t['vramOffset'],t['vramSize']);return tc.decode(d[:tc.ps3_linear_size(t['width'],t['height'],1,k[1])],t['width'],t['height'],k[0])
def ps4img(n):
    t=T4[n];print(n,hex(t['format']),t['width'],t['height'],t['mips'],t['vramSize'])
    kind={0x47:'DXT1',0x48:'DXT1',0x4d:'DXT5',0x4e:'DXT5'}[t['format']];bpb={'DXT1':8,'DXT5':16}[kind]
    lin=tc.ps4_mip0_linear(q.vram_bytes(t['vramOffset'],t['vramSize']),t['width'],t['height'],bpb)
    return tc.decode(lin,t['width'],t['height'],kind)
for a,b in [('air-yem-m-wall-brick-construction-normal.tga','air-yem-m-wall-brick-construction-blend.tga'),('air-yem-m-ground-concrete-normal.tga','air-yem-m-ground-concrete-dirty-blend.tga'),('air-yem-m-metal-beams-normal.tga','air-wall-panel-metal-01-blend.tga')]:
    if a not in T3 or b not in T4: print('skip',a,b);continue
    A=ps3img(a);B=ps4img(b)
    h=min(A.shape[0],B.shape[0]);sa=A.shape[0]//h;sb=B.shape[0]//h
    x=A[::sa,::sa].astype(float);y=B[::sb,::sb].astype(float)
    print(' A mean rgb',x.mean((0,1))[:4],'std',x.std((0,1))[:4],'B',y.mean((0,1))[:3],y.std((0,1))[:3])
    for ch in range(4 if x.shape[2]>3 else 3):
        print('  ch',ch,'corr',round(np.corrcoef(x[...,ch].ravel(),y[...,1].ravel())[0,1],3) if x[...,ch].std()>0 else None)
