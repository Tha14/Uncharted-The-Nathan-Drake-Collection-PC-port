from lib import *
import texcodec as tc
from PIL import Image
q=Pak(OV+'air-multi.pak');p=Pak(S+'emblem-batch/p3/air-multi.pak')
def stat(P,lab,sub):
  for t in P.textures():
    if sub in t['path']:
        w,h=t['width'],t['height'];data=P.vram_bytes(t['vramOffset'],t['vramSize'])
        if lab=='ps3':
            kind,bpb,_=tc.ps3_format(t);lin=data[:tc.ps3_linear_size(w,h,1,bpb)]
        else:
            f=t['format'];kind={0x47:'BC1',0x48:'BC1',0x4d:'BC3',0x4e:'BC3'}.get(f);bpb=16 if kind=='BC3' else 8
            if not kind:print(lab,sub,'fmt',hex(f),w,h);continue
            lin=tc.ps4_mip0_linear(data,w,h,bpb)
        im=tc.decode(bytes(lin),w,h,kind);print(lab,t['path'].rsplit('/',2)[1][:44],w,h,hex(t.get('format',t.get('gcmRemapCtl'))),'mean',im.reshape(-1,4).mean(0).round(0),'p5',np.percentile(im.reshape(-1,4),5,axis=0),'p95',np.percentile(im.reshape(-1,4),95,axis=0))
        Image.fromarray(im[...,:3]).resize((256,256)).save('tex/%s-%s.png'%(lab,t['path'].rsplit('/',2)[1][:40]))
for s in sys.argv[1:]:
    stat(q,'ps4',s);stat(p,'ps3',s)
