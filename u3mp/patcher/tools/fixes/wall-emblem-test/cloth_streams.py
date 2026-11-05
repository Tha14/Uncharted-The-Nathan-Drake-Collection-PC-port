import sys,struct
sys.path.insert(0,"{{PAKTOOLS}}");sys.dont_write_bytecode=True
from ndpak import Pak
p=Pak("{{PS4}}/u3data/build/main/actor33/ps4/loading-menu-cloth.pak");raw=p.d
for it in p.items:
    if it.type in ('STREAMING_BUFFER','GEOMETRY_1','FOREGROUND_SECTION_2','ARTGROUP_DATA','LEVEL_PATS_1'):
        b=p.pbase(it.page)+it.data_off;print(it.type,it.name,'page',it.page,'off %#x size %#x file %#x'%(it.data_off,it.size,b));print('  ',raw[b:b+0x60].hex())
print('pages',[(i,hex(p.pbase(i))) for i in range(len(p.pages))] if hasattr(p,'pages') else '')
g=p.items_of('GEOMETRY_1')[0];pg,b=g.page,g.data_off;bb=p.pbase(pg)
nsub=p.u64(bb+b+8);sr=p.resolve(pg,b+0x10)
for i in range(nsub):
    o=sr[1]+0x88*i;mb=p.pbase(sr[0])+o
    nv,ni,ns=struct.unpack_from('<III',raw,mb+0x20);st=p.resolve(sr[0],o+0x30)
    print('sub',i,'nv',nv,'ni',ni,'streams at file %#x'%(p.pbase(st[0])+st[1]))
    for k in range(ns):
        e=p.pbase(st[0])+st[1]+16*k;print('   ',raw[e:e+16].hex(), 'isptr data', p.is_ptr(st[0],st[1]+16*k+8))
    ib=p.resolve(sr[0],o+0x40);print('   ib',ib, raw[mb+0x38:mb+0x48].hex())
# anim items summary
an=p.items_of('ANIM');print('ANIM',len(an),an[:3],an[-2:])
