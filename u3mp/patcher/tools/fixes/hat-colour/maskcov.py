"""decode mip 0 of a PS3 (linear) BC1 tint mask and report the share of texels above 50% (the area the tint reaches)"""
import sys,os,struct
sys.dont_write_bytecode=True
AM='{{PROJECT}}/scratch/actor-materials'
sys.path.insert(0,AM)
from lib import *
def lum(c): return ((c>>11)*255//31+((c>>5)&63)*255//63+(c&31)*255//31)/3
def cov(b,w,h):
    hi=tot=0
    for by in range(h//4):
        for bx in range(w//4):
            o=(by*(w//4)+bx)*8; c0,c1,idx=struct.unpack_from('<HHI',b,o); l0,l1=lum(c0),lum(c1)
            pal=[l0,l1,(2*l0+l1)/3,(l0+2*l1)/3] if c0>c1 else [l0,l1,(l0+l1)/2,0]
            for k in range(16):
                v=pal[(idx>>(2*k))&3]; tot+=1; hi+=v>127
    return hi/tot
for f in sys.argv[1:]:
    q=Pak(f); print('==',f.split('/')[-1])
    for t in q.textures():
        if 'tint-mask' in t['path']:
            b=q.vram_bytes(t['vramOffset'],t['width']*t['height']//2); print('  %s %dx%d tinted share %.1f%%'%(t['path'].split('/')[-2],t['width'],t['height'],100*cov(b,t['width'],t['height'])))
