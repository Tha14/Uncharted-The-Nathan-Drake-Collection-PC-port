import sys,os
T=r'{{SCRATCH}}/gh2/u3mp/paktools'
sys.path.insert(0,T);sys.path.insert(0,T+'/stage')
from ndpak import Pak
import bggeom as G, struct, collections
PS4="{{PS4}}/u3data/build/main/actor33/ps4/"
print(len(os.listdir(PS4)),[f for f in os.listdir(PS4) if 'loading' in f])
b=open(PS4+'loading-menu-cloth.pak','rb').read();open('tmpl/ps4-loading-menu-cloth.pak','wb').write(b)
q=Pak('tmpl/ps4-loading-menu-cloth.pak')
print(len(b),collections.Counter(i.type for i in q.items))
for i in q.items:print(i.type,i.name,i.size)
sys.path.insert(0,T)
import ps4actor
for m in ps4actor.tmpl_materials(q):print(m['name'],m['shader'],m['feats'],m['binds'],[(a,c) for a,c,_ in m['params']])
g=q.items_of('GEOMETRY_1')[0];pg,gb=g.page,g.data_off
print('flags',hex(q.pu32(pg,gb+4)),'nsub',q.pu32(pg,gb+8),'nmat',q.pu32(pg,gb+0x48))
for nm,s in ps4actor.template_streams(q).items():print('streams',nm,s)
