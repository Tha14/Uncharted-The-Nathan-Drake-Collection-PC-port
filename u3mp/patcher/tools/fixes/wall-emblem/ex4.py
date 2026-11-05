from lib import *
from sg import spawners,spawner_info
import sys
R=sys.argv[1]+'/work/overlay-r3/u3data/build/main/pak23/ps4/'
for f in ['air-multi-ingame12.pak','air-multi-strip-ingame12.pak','air-multi-stage-ingame12.pak']:
    q=Pak(R+f)
    c=collections.Counter();ex={}
    for it,(pg,o) in spawners(q):
        b=q.pbase(pg);typ=q.u32(b+o+0x40);ar=q.resolve(pg,o+0x48);art=q.pstr(*ar) if ar else None
        c[(hex(typ),art)]+=1
        i=spawner_info(q,(pg,o));ex.setdefault((hex(typ),art),(i['name'],list(np.round(i['pos'],1)),[hex(k) for k in i['tags']][:8],it.name))
    print(f,q.platform,sum(c.values()),[i.type for i in q.items][:0])
    print(collections.Counter(i.type for i in q.items))
    for k,v in sorted(c.items(),key=lambda x:str(x))[:60]:print(k,v,ex[k])
