from lib import *
import texcodec as tc, numpy as np
from PIL import Image
def p4tex(q,sub):
    r=[]
    for t in q.textures():
        if sub in t['path']:r.append(t)
    return r
q=Pak(OV+'air-multi.pak');p=Pak(S+'emblem-batch/p3/air-multi.pak')
print(list(q.textures()[0].keys()))
os.makedirs('tex',exist_ok=True)
for sub in ['ground-tarmac-color','road-broken-color','road-broken-blend','tarmac-normal','road-broken-normal','ground-sand-color']:
  for lab,P in (('ps4',q),('ps3',p)):
    for t in p4tex(P,sub):
        print(lab,sub,t['path'][-70:],t['width'],t['height'],t['mips'],hex(t.get('format',t.get('gcmRemapCtl',0))),t['vramSize'],{k:v for k,v in t.items() if k not in('path',)})
        w,h=t['width'],t['height']
        data=P.vram_bytes(t['vramOffset'],t['vramSize'])
        if lab=='ps3':
            kind,bpb,_=tc.ps3_format(t)
            lin=data[:tc.ps3_linear_size(w,h,1,bpb)]
        else:
            f=t.get('format');kind={0x47:'BC1',0x48:'BC1',0x4d:'BC3',0x4e:'BC3',0x4a:'BC2',0x4b:'BC2'}.get(f);bpb=16 if kind in('BC3','BC2') else 8
            if not kind:print('skip',f);continue
            lin=tc.ps4_mip0_linear(data,w,h,bpb)
        im=tc.decode(bytes(lin),w,h,kind)
        print('   mean rgba',im.reshape(-1,4).mean(0).round(1),'min',im.reshape(-1,4).min(0),'max',im.reshape(-1,4).max(0))
        Image.fromarray(im).save('tex/%s-%s.png'%(lab,t['path'].rsplit('/',2)[1].replace('.tga','')[:40]))
