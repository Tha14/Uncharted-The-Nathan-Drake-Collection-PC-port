import sys,struct
sys.path.insert(0,"{{PAKTOOLS}}");sys.dont_write_bytecode=True
from ndpak import Pak
def bounds(path):
    p=Pak(path);raw=p.d;print('==',path.split('/')[-1])
    g=p.items_of('GEOMETRY_1')[0];pg,b=g.page,g.data_off;bb=p.pbase(pg)
    nsub=p.u64(bb+b+8);sr=p.resolve(pg,b+0x10)
    allp=[]
    for i in range(nsub):
        o=sr[1]+0x88*i;mb=p.pbase(sr[0])+o
        nv,ni,ns=struct.unpack_from('<III',raw,mb+0x20);st=p.resolve(sr[0],o+0x30)
        print(' sub',i,p.pstr(*p.resolve(sr[0],o+8)),'nv',nv,'ni',ni)
        for k in range(ns):
            e=st[1]+24*k;sd=p.pu32(st[0],e);ln=p.pu32(st[0],e+4);fm=p.resolve(st[0],e+8);dt=p.resolve(st[0],e+16)
            fb=raw[p.pbase(fm[0])+fm[1]:p.pbase(fm[0])+fm[1]+4]
            print('   stream',k,'hdr %08x'%sd,'len',ln,'fmt',fb.hex())
            if fb[0]==0 and (sd>>16)==12:
                d=p.pbase(dt[0])+dt[1];xs=struct.unpack_from('<%df'%(3*nv),raw,d);pts=list(zip(xs[0::3],xs[1::3],xs[2::3]));allp+=pts
                print('     pos min',[round(min(a[j] for a in pts),3) for j in range(3)],'max',[round(max(a[j] for a in pts),3) for j in range(3)])
    print(' ALL min',[round(min(a[j] for a in allp),3) for j in range(3)],'max',[round(max(a[j] for a in allp),3) for j in range(3)],'maxdist',round(max((a[0]**2+a[1]**2+a[2]**2)**.5 for a in allp),3))
    fg=p.items_of('FOREGROUND_SECTION_2')[0];fb_=p.pbase(fg.page)+fg.data_off
    print(' FG skel/hier',hex(p.u32(fb_+8)),hex(p.u32(fb_+12)))
    jh=p.items_of('JOINT_HIERARCHY')[0];jb=p.pbase(jh.page)+jh.data_off;print(' JH',jh.name,hex(jh.size),raw[jb:jb+0x40].hex())
for a in sys.argv[1:]:bounds(a)
