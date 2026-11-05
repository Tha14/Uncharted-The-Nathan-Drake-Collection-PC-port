from lib import *
import ps4actor,sys
sys.path.insert(0,T+'/stage')
import ps4bg
R=sys.argv[1]+'/work/overlay-r3/u3data/build/main/pak23/ps4/'
q=Pak(R+'air-multi.pak')
g=q.items_of('GEOMETRY_1')[0];pg,gb=g.page,g.data_off
base=q.pbase(pg)
ns=q.u64(base+gb+8);sr=q.resolve(pg,gb+0x10)
tmats={m['name']:m for m in ps4actor.tmpl_materials(q)}
tx={t['path']:t for t in q.textures()}
print('nsub',ns,'nmat',len(tmats),'tex',len(tx))
for i in range(ns):
    o=sr[1]+0x88*i
    mat=q.pstr(*q.resolve(sr[0],o+0x48))
    if 'emblem' not in mat:continue
    nm=q.pstr(*q.resolve(sr[0],o+8));nv=q.pu32(sr[0],o+0x20);ni=q.pu32(sr[0],o+0x24);ib=q.resolve(sr[0],o+0x40)
    print(i,nm,'nv',nv,'ni',ni,'ib',ib,'mat',mat.split(',')[0][-55:],'ibfile',hex(q.pbase(ib[0])+ib[1]),'bytes/idx',2)
for n,m in tmats.items():
    if 'emblem' in n:
        print('MAT',n.split(',')[0].split('/')[-2],[f for f in m['feats'] if f.startswith(('COLOR','UV','NORMAL_1'))][:18])
        for a,b,c in m['binds']:
            t=tx.get(c) or tx.get(c+'.ndb');print('   ',a,b.split('/')[-1],(t['width'],t['height'],t['mips'],hex(t['format']),t['unk34'],t['vramSize']) if t else None)
        print('   P',[(a,round(struct.unpack('<f',d[:4])[0],3)) for a,cc,d in m['params'] if 'Tint' in a or 'Bright' in a or 'uvScale' in a][:10])
