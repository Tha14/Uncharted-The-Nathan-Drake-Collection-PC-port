from lib import *
import bggeom
# PS3: for air-multi floors, which blocks carry lightmap uvs per-instance?
p=Pak(S+'emblem-batch/p3/air-multi.pak')
C=bggeom.collection(p)
print(type(C),len(C))
pr=C[33];print(pr.keys(),len(pr['blocks']),len(pr['insts']),pr['groups'][:3])
from bggeom import instance,read_format
I=instance(p,pr['insts'][0]);bd=I['blockdata'];print('blockdata',bd)
res=collections.Counter()
for gi,(mn,first,cnt) in enumerate(pr['groups']):
    if not re.search('multi-ground-asphalt-broken/default|concrete-multi/polished',mn):continue
    has=[]
    for bi in range(first,first+cnt):
        st=p.resolve(bd[0],bd[1]+8*bi);ifmt=p.resolve(bd[0],bd[1]+8*bi+4)
        ids=[]
        if ifmt:ids=[a[0] for a in read_format(p,*ifmt)]
        k=0;lm=None
        for a in (read_format(p,*ifmt) if ifmt else []):
            if a[0] not in (0x4c,0x13,0x4e):continue
            r=p.resolve(st[0],st[1]+4*k) if st else None
            if a[0]==0x13:lm=r is not None
            k+=1
        has.append((bi,lm))
    print(mn.rsplit('/',2)[-2],first,cnt,'blocks with own LM uv',sum(1 for b,l in has if l),'without',sum(1 for b,l in has if not l),has[:24])
