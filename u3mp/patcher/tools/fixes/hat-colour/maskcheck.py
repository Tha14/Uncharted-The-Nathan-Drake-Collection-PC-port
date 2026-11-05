"""mean BC1 endpoint brightness of every tint mask / colour map in a pak (0 = black mask: tint would never show)"""
import sys,os,struct
sys.dont_write_bytecode=True
AM='{{PROJECT}}/scratch/actor-materials'
sys.path.insert(0,AM)
from lib import *
def bc1_mean(b):
    n=len(b)//8; s=0
    for i in range(n):
        c0,c1=struct.unpack_from('<HH',b,i*8)
        for c in (c0,c1): s+=((c>>11)*255//31+((c>>5)&63)*255//63+(c&31)*255//31)/3
    return s/(2*n) if n else -1
for f in sys.argv[1:]:
    q=Pak(f if os.path.exists(f) else find('actor33',f)[0]); print('==',f.split('/')[-1])
    for t in q.textures():
        if 'tint-mask' in t['path'] or 'color' in t['path']:
            print('  %-60s %dx%d mean %.1f'%(t['path'].split('/')[-2][-60:],t['width'],t['height'],bc1_mean(q.vram_bytes(t['vramOffset'],min(t['vramSize'],t['width']*t['height']//2)))))
