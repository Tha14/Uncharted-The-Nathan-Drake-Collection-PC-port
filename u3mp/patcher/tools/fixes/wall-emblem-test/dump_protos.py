import sys,struct,collections
sys.path.insert(0,"{{PAKTOOLS}}");sys.dont_write_bytecode=True
from ndpak import Pak
G="{{PS4}}/u3data/build/main/actor33/ps4/"
O="{{PROJECT}}/work/overlay-r3/u3data/build/main/actor33/ps4/"
def d(path,lab):
    p=Pak(path);raw=p.d
    print('==',lab,len(raw),collections.Counter(i.type for i in p.items).most_common(20))
    for g in p.items_of('GEOMETRY_1'):
        pg,b=g.page,g.data_off;bb=p.pbase(pg)
        nsub=p.u64(bb+b+8);sr=p.resolve(pg,b+0x10);npr=p.u64(bb+b+0x18);pr=p.resolve(pg,b+0x20)
        print('hdr',raw[bb+b:bb+b+0x60].hex())
        for i in range(nsub):
            o=sr[1]+0x88*i;mb=p.pbase(sr[0])+o
            nv,ni,ns=struct.unpack_from('<III',raw,mb+0x20)
            print(' sub',i,p.pstr(*p.resolve(sr[0],o+8)),'nv',nv,'ni',ni,'ns',ns, 'mat',p.pstr(*p.resolve(sr[0],o+0x48)).split(',')[0][-50:])
            print('   ',raw[mb:mb+0x88].hex())
        for i in range(npr):
            r=p.resolve(pr[0],pr[1]+8*i);mb=p.pbase(r[0])+r[1]
            print(' proto',i,p.pstr(*p.resolve(r[0],r[1]+0x10)),raw[mb:mb+0x78].hex())
            fl=p.resolve(r[0],r[1]+0x20)
            if fl:print('   flags',raw[p.pbase(fl[0])+fl[1]:p.pbase(fl[0])+fl[1]+16].hex())
    for it in p.items_of('FOREGROUND_SECTION_2'):
        b=p.pbase(it.page)+it.data_off;print(' FG',it.name,raw[b:b+0x40].hex())
    for it in p.items:
        if it.type in('TAG_VEC4','TAG_INT','TAG_FLOAT','TAG_STRING'):
            b=p.pbase(it.page)+it.data_off;print(' ',it.type,it.name,raw[b:b+16].hex(),struct.unpack_from('<4f',raw,b) if it.type=='TAG_VEC4' else '')
for l,pth in (('cloth',G+'loading-menu-cloth.pak'),('emb-ground',O+'air-multi-emblem-ground.pak'),('cargo',G+'cargo-plane-int-moving-multi.pak')):
    d(pth,l)
