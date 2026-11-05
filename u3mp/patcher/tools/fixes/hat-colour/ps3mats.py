import sys,os
sys.dont_write_bytecode=True
AM='{{PROJECT}}/scratch/actor-materials'
sys.path.insert(0,AM)
from lib import *
from ps3mat_am import read_materials
import ps4actor as A, ps3geom
s=S3()
for n in sys.argv[1:]:
    for src,d in s.all(n):
        p=Pak(d); print('=====',n,src,len(d))
        used=set()
        for v in ps3geom.prototypes(p).values(): used|=set(A.ps3_block_materials(p,v['proto']))
        mats,texs=read_materials(p)
        for m in mats:
            print(' MAT',m['name'],'used' if m['name'] in used else 'unused','sid %08x'%m['sid'])
            for i in m['textures']: print('    tex',texs[i]['name'].split('art/')[-1])
        for t in p.textures(): print(' TEX %08x'%t['hash'],t['path'].split('art/')[-1],t['width'],t['height'])
