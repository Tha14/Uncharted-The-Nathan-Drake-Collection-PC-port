import sys,os,json
sys.dont_write_bytecode=True
AM='{{PROJECT}}/scratch/actor-materials'
sys.path.insert(0,AM)
from lib import *
import ps3geom
b=json.load(open('batch.json')); am={f[:-4]:json.load(open(AM+'/out/'+f[:-4]+'.json')) for f in json.load(open(AM+'/manifest.json'))}
ok={n:v for n,v in b.items() if v.get('ok')}
print('ok',len(ok),'skip',[n for n,v in b.items() if not v.get('ok')])
import collections; print('caps',collections.Counter(v['cap'] for v in ok.values()),'layers',collections.Counter(v['layer'] for v in ok.values()))
top=sorted(ok.items(),key=lambda x:-x[1]['vram'][1])[:5]; print('largest vram',[(n,hex(v['vram'][1]),v['template'],v['cap']) for n,v in top])
for n in sorted(set(ok)&set(am)): print('  overlap',n,'am template',am[n]['template'],'cap',am[n]['cap'],'-> new',ok[n]['template'],ok[n]['cap'],'vram %#x'%ok[n]['vram'][1])
s=S3()
for src,d in s.all('mp-hero-sullivan-part-glasses-a'):
    p=Pak(d); nv=sum(sum(ps3geom.decode_block(p,pg,bo)['nverts'] for pg,bo in v['blocks']) for v in ps3geom.prototypes(p).values()); print('glasses ps3',src,'verts',nv)
