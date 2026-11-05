from lib import *
from sg import spawners,spawner_info
D="{{PS4}}/u3data/build/main/"
A='cargo-plane-int-moving-multi'
q=Pak(D+'actor33/ps4/'+A+'.pak')
print(collections.Counter(i.type for i in q.items));print([q.item_at(*e).type for e in q.login['entries']])
for t in q.items:
    if t.type.startswith('TAG'):print(t.type,t.name,q.d[q.pbase(t.page)+t.data_off:q.pbase(t.page)+t.data_off+t.size].hex(), )
ag=q.items_of('ARTGROUP_DATA')[0];print('ag',ag.size,ag.name,q.d[q.pbase(ag.page)+ag.data_off:q.pbase(ag.page)+ag.data_off+ag.size].hex())
j=q.items_of('JOINT_HIERARCHY')[0];print(j.name,j.size)
print('fg',[(f.name,f.size,struct.unpack('<f',q.d[q.pbase(f.page)+f.data_off+0x18:q.pbase(f.page)+f.data_off+0x1c])) for f in q.items_of('FOREGROUND_SECTION_2')])
g=q.items_of('GEOMETRY_1')[0];pg,gb=g.page,g.data_off;print('sub',q.u64(q.pbase(pg)+gb+8))
sb=q.resolve(pg,gb+0x10);print('nv',q.pu32(sb[0],sb[1]+0x20));sk=q.resolve(sb[0],sb[1]+0x60);print('skinhdr',q.d[q.pbase(sk[0])+sk[1]:q.pbase(sk[0])+sk[1]+40].hex())
c=Pak('tmpl/ps4-loading-menu-cloth.pak')
g=c.items_of('GEOMETRY_1')[0];pg,gb=g.page,g.data_off;sb=c.resolve(pg,gb+0x10)
n=c.pu32(sb[0],sb[1]+0x28);st=c.resolve(sb[0],sb[1]+0x30);nv=c.pu32(sb[0],sb[1]+0x20)
for k in range(n):
    o=st[1]+0x18*k;f=c.resolve(st[0],o+8);fb=c.d[c.pbase(f[0])+f[1]:c.pbase(f[0])+f[1]+4];d=c.resolve(st[0],o+0x10);sz=c.pu32(st[0],o+4)
    if fb[3]==0x22:
        a=np.frombuffer(c.d[c.pbase(d[0])+d[1]:c.pbase(d[0])+d[1]+sz],'<f2').reshape(-1,2);print('cloth slot',fb[0],a.min(0),a.max(0))
    if fb[3]==0x0a and fb[0]==4:
        a=np.frombuffer(c.d[c.pbase(d[0])+d[1]:c.pbase(d[0])+d[1]+sz],'<f2').reshape(-1,4);print('cloth color',a.min(0),a.max(0))
import glob
for f in os.listdir(D+'pak23/ps4'):
    if 'yemen-airport-multi-pak-ingame12' in f:
        q=Pak(D+'pak23/ps4/'+f)
        for it,(pg,o) in spawners(q):
            ar=q.resolve(pg,o+0x48)
            if ar and q.pstr(*ar)==A:
                i=spawner_info(q,(pg,o));print(i['name'],[(hex(k),v[0],v[1],q.d[q.pbase(v[2][0])+v[2][1]:q.pbase(v[2][0])+v[2][1]+v[1]].hex() if v[2] else None) for k,v in i['tags'].items()]);print(q.d[q.pbase(pg)+o:q.pbase(pg)+o+0x90].hex())
p=Pak('p3/air-multi.pak')
for pi,c2 in enumerate(G.collection(p)):
  for m3,fi,cnt in c2['groups']:
    if 'emblem' in m3:
      d=G.decode_block(p,*c2['blocks'][fi]);print(pi,m3.split('/')[-2],'color mean',np.round(d['color'].mean(0),3).tolist() if 'color' in d else None,d['raw'].get(0x15))
