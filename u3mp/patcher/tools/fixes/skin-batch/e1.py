import sys,os,csv,re,collections
G="{{PS4}}/u3data/build/main/"
sys.path.insert(0,os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','gh2','u3mp','paktools'))
from ndpak import Pak
d=collections.defaultdict(list)
for l in open(G+'texturedict1/ps4/TextureDictionaries.csv',errors='replace'):
    f=[x.strip() for x in l.split(',')]
    if f[0]=='dictionary':continue
    p=f[1].split('texture2/')[-1];h=p.rsplit('/',1)[-1]
    d[int(h,16) if re.fullmatch('[0-9A-Fa-f]{16}',h) else p].append((f[0],f[5],f[6],f[9],f[10]))
print('csv',len(d),collections.Counter(x[0][0] for x in d.values()))
for f in ['hk416-mp','hk416']:
    p=Pak(G+'actor33/ps4/%s.pak'%f)
    print(f,p.platform,len(p.d),len(p.pages),'vrambase',hex(p.vram_base))
    for t in p.textures():
        print(' ',t['path'].split('texture2/')[-1][:80],t['width'],t['height'],t['mips'],hex(t['format']),t['unk34'],hex(t['vramSize']),d.get(t['hash'],'NOCSV'))
