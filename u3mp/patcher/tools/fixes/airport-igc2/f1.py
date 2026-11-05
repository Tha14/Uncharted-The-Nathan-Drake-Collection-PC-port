import sys,io,contextlib,struct,os,hashlib,json
import build_hat as B
from build_hat import *
with contextlib.redirect_stdout(io.StringIO()):
    d,m=B.build('mp-hero-flynn-coop','hero-london',128,verbose=False)
bad,info=B.gate('mp-hero-flynn-coop',d,verbose=False)
print('gate',hex(m['vram']),info['size'],'check',info['check'],'crash',info['crash'],'verify',info['verify'],bad)
print(json.dumps(m['roles'])[:60])
def rgb565(c): return ((c>>11)*255//31,((c>>5)&63)*255//63,(c&31)*255//31)
def stats(b,w,h,step):
    tot=[0,0,0];n=0;hi=0
    for by in range(h//4):
        for bx in range(w//4):
            o=(by*(w//4)+bx)*step+(step-8)
            c0,c1=struct.unpack_from('<HH',b,o)
            a,bb=rgb565(c0),rgb565(c1)
            for i in range(3):tot[i]+=a[i]+bb[i]
            n+=2
            hi+=(sum(a)+sum(bb))/6>127
    return [round(x/n) for x in tot],round(100*hi/(n/2),1)
for name in ['mp-hero-flynn-coop','mp-hero-lazarevic-coop']:
    p=Pak(f'../live/airport-igc/ps3/{name}.pak')
    for t in p.textures():
        k=t['path'].split('/')[-2] if '/' in t['path'] else t['path']
        if t['width']<16:continue
        for step in (8,):
            try:
                b=p.vram_bytes(t['vramOffset'],t['width']*t['height']//2)
                print(name[8:15],k[:46].ljust(46),t['width'],t['height'],t.get('format'),stats(b,t['width'],t['height'],8))
            except Exception as e: print(k,e)
