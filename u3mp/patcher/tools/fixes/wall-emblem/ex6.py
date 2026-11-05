from lib import *
import ps4actor
D="{{PS4}}/u3data/build/main/actor33/ps4/"
for a in ['yem-multi-door-switch','yem-multi-exhaust-fan-sm','air-multi-wind-socks','yem-sm-cone-set-1','acetylene-tank-a']:
    q=Pak(D+a+'.pak');print(a,os.path.getsize(D+a+'.pak'),dict(collections.Counter(i.type for i in q.items)))
    print('  login',[q.item_at(*e).type for e in q.login['entries']][:14])
    g=q.items_of('GEOMETRY_1')[0];pg,gb=g.page,g.data_off;sr=q.resolve(pg,gb+0x10)
    for i in range(min(2,q.u64(q.pbase(pg)+gb+8))):
        o=sr[1]+0x88*i
        sk=q.resolve(sr[0],o+0x60)
        print('  sub',q.pstr(*q.resolve(sr[0],o+8)),'nv',q.pu32(sr[0],o+0x20),'flags',hex(q.pu32(sr[0],o+0x14)),hex(q.pu32(sr[0],o+0x18)),'nj',q.pu32(sr[0],o+0x58),'skin',bool(sk),'ns',q.pu32(sr[0],o+0x28),[x[0] for x in ps4actor.template_streams(q).get(q.pstr(*q.resolve(sr[0],o+0x48)),[])])
    fg=q.items_of('FOREGROUND_SECTION_2')
    print('  fg',[(f.name,hex(q.pu32(f.page,f.data_off+8)),hex(q.pu32(f.page,f.data_off+12))) for f in fg][:3],'jh',[ (j.name,j.size) for j in q.items_of('JOINT_HIERARCHY')],'anims',[x.name for x in q.items_of('ANIM')][:3],'coll',[c.name for c in q.items if 'COLL' in c.type][:2],'tags',[ (t.type,t.name) for t in q.items if t.type.startswith('TAG')][:8])
    ag=q.items_of('ARTGROUP_DATA')[0];print('  ag',q.u32(q.pbase(ag.page)+ag.data_off),ag.size)
from sg import spawners,spawner_info
import sys
q=Pak(sys.argv[1]);
for it,(pg,o) in spawners(q):
    ar=q.resolve(pg,o+0x48);art=q.pstr(*ar) if ar else None
    if art in('yem-multi-door-switch','yem-multi-exhaust-fan-sm'):
        i=spawner_info(q,(pg,o));print(i['name'],hex(i['sid']),hex(sid(i['name'])),'q',i['q'],'x38',hex(q.u32(q.pbase(pg)+o+0x38)),[(hex(k),v[0],v[1],q.d[q.pbase(v[2][0])+v[2][1]:q.pbase(v[2][0])+v[2][1]+4].hex() if v[2] else None) for k,v in i['tags'].items()]);print(q.d[q.pbase(pg)+o:q.pbase(pg)+o+0x90].hex());break
