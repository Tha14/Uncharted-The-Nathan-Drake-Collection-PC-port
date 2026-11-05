import sys,os,pickle,numpy as np
H=os.path.dirname(os.path.abspath(__file__))
G="{{PS4}}/u3data/build/main/"
R="{{PROJECT}}/"
sys.path.insert(0,H+'/../gh2/u3mp/paktools');sys.path.insert(0,"{{TRANSFER_TOOLS}}")
from ndpak import Pak
import texcodec as tc
from psarc_lib import Psarc
from PIL import Image
idx=pickle.load(open(H+'/tstrm-idx.pkl','rb'))
p3=Psarc("{{PS3_UPDATE}}/dlc-patch18.psarc");b3=p3.read(p3.names.index('build/main/actor33/hk416-mp.pak')+1)
P3=Pak(b3);P4=Pak(R+'work/overlay-r3/u3data/build/main/actor33/ps4/hk416-mp.pak');PR=Pak(G+'actor33/ps4/hk416-mp.pak')
print('overlay==retail',P4.d==PR.d,len(P4.d),len(PR.d))
def p4img(pk,t):
    return tc.decode(tc.ps4_mip0_linear(pk.vram_bytes(t['vramOffset'],t['vramSize']),t['width'],t['height'],8),t['width'],t['height'],'BC1')
def strm(k):
    e=idx.get(k) 
    if not e:return None
    f=open(G+'texturedict1/ps4/'+e['pak'],'rb');f.seek(e['off']);d=f.read(e['size']);return tc.decode(tc.ps4_mip0_linear(d,e['w'],e['h'],8),e['w'],e['h'],'BC1'),e
def p3img(t,lvl=0):
    o=0;w,h=t['width'],t['height']
    for i in range(lvl):
        bw,bh=tc.blocks(w,h,True);o+=bw*bh*8;w//=2;h//=2
    bw,bh=tc.blocks(w,h,True)
    return tc.decode(P3.vram_bytes(t['vramOffset']+o,bw*bh*8),w,h,'DXT1')
T3={t['path'].split('/')[-2]:t for t in P3.textures()}
rows=[]
for nm in ('hk416-d-flynn-color.tga','hk416-d-color.tga'):
    t4=[t for t in P4.textures() if '/'+nm+'/' in t['path']][0];k=t4['path'].split('texture2/')[-1]
    a=p4img(P4,t4);t3=T3[nm];c=p3img(t3,4) # 1024>>4=64
    s=strm(k)
    print(nm,'p3',t3['width'],t3['mips'],'p4 mean',a[:,:,:3].mean((0,1)).round(1),'p3 mean',c[:,:,:3].mean((0,1)).round(1),'rms p4 vs p3@64',float(np.sqrt(((a[:,:,:3].astype(float)-c[:,:,:3])**2).mean())))
    if s:
        im,e=s;print('  stream',e,'mean',im[:,:,:3].mean((0,1)).round(1))
        small=np.array(Image.fromarray(im).resize((64,64),Image.BOX))
        print('  rms stream@64 vs p4',float(np.sqrt(((small[:,:,:3].astype(float)-a[:,:,:3])**2).mean())),' vs p3@64',float(np.sqrt(((small[:,:,:3].astype(float)-c[:,:,:3])**2).mean())))
    rows.append((nm,a,c,s and s[0]))
# cross
print('rms p4 flynn vs p4 default',float(np.sqrt(((rows[0][1][:,:,:3].astype(float)-rows[1][1][:,:,:3])**2).mean())))
print('rms p3 flynn vs p3 default',float(np.sqrt(((rows[0][2][:,:,:3].astype(float)-rows[1][2][:,:,:3])**2).mean())))
if rows[0][3] is not None and rows[1][3] is not None:
    print('rms stream flynn vs stream default',float(np.sqrt(((rows[0][3][:,:,:3].astype(float)-rows[1][3][:,:,:3])**2).mean())))
W=400;sheet=Image.new('RGB',(W*3,W*2),(40,40,40))
for y,(nm,a,c,s) in enumerate(rows):
    sheet.paste(Image.fromarray(a).convert('RGB').resize((W,W),Image.NEAREST),(0,y*W))
    sheet.paste(Image.fromarray(c).convert('RGB').resize((W,W),Image.NEAREST),(W,y*W))
    if s is not None:sheet.paste(Image.fromarray(s).convert('RGB').resize((W,W),Image.LANCZOS),(2*W,y*W))
    Image.fromarray(a).save(H+'/ps4-64-%s.png'%nm[:-4]);Image.fromarray(c).save(H+'/ps3-mip64-%s.png'%nm[:-4])
    if s is not None:Image.fromarray(s).convert('RGB').resize((1024,1024),Image.BOX).save(H+'/ps4-stream-%s.png'%nm[:-4])
sheet.save(H+'/compare-hk416.png')
