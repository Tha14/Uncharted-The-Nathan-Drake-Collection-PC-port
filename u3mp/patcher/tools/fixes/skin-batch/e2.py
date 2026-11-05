import sys,os,collections,pickle,struct
G="{{PS4}}/u3data/build/main/"
sys.path.insert(0,os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','gh2','u3mp','paktools'))
from ndpak import Pak
T=G+'texturedict1/ps4/'
idx={}
for f in sorted(os.listdir(T)):
    if not f.endswith('.pak'):continue
    p=Pak(T+f)
    tx=p.textures()
    print(f,len(p.d),p.hdr,len(p.pages),len(tx),collections.Counter(i.type for i in p.items).most_common(5),flush=True)
    if tx: print('  ',tx[0])
    print('  errs',p.validate()[:3])
    for t in tx:
        idx[t['path'].split('texture2/')[-1]]=dict(pak=f,off=p.vram_base+t['vramOffset'],size=t['vramSize'],w=t['width'],h=t['height'],m=t['mips'],fmt=t['format'],flag=t['unk34'],hash=t['hash'])
    del p
pickle.dump(idx,open('tstrm-idx.pkl','wb'))
print(len(idx))
