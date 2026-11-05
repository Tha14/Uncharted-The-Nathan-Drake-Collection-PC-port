import sys,struct,os,collections
sys.path.insert(0,"{{PAKTOOLS}}");sys.dont_write_bytecode=True
from ndpak import Pak
G="{{PS4}}/u3data/build/main/actor33/ps4/"
O="{{PROJECT}}/work/overlay-r3/u3data/build/main/actor33/ps4/"
c=collections.Counter();ex={}
fs=sorted(f for f in os.listdir(G) if f.endswith('.pak'))
import random;random.seed(1);fs=random.sample(fs,min(120,len(fs)))+['loading-menu-cloth.pak','cargo-plane-int-moving-multi.pak','air-multi-wind-socks.pak']
for f in fs:
    try:p=Pak(G+f)
    except Exception as e:continue
    raw=p.d
    for g in p.items_of('GEOMETRY_1'):
        pg,b=g.page,g.data_off;bb=p.pbase(pg)
        nsub=p.u64(bb+b+8);sr=p.resolve(pg,b+0x10);npr=p.u64(bb+b+0x18);pr=p.resolve(pg,b+0x20)
        maxst=0;skin=0
        for i in range(nsub):
            o=sr[1]+0x88*i;mb=p.pbase(sr[0])+o;ns=p.u32(mb+0x28);maxst=max(maxst,ns)
            if p.resolve(sr[0],o+0x60):skin+=1
        for i in range(npr):
            r=p.resolve(pr[0],pr[1]+8*i);mb=p.pbase(r[0])+r[1]
            v30,v34=struct.unpack_from('<II',raw,mb+0x30);fl=p.resolve(r[0],r[1]+0x20);flb=raw[p.pbase(fl[0])+fl[1]:p.pbase(fl[0])+fl[1]+8].hex()
            nsn=p.u32(mb+4)
            key=(v30,v34);c[key]+=1;ex.setdefault(key,[]).append((f[:-4],'nsub',nsn,'maxst',maxst,'skin',skin,'fl',flb,'has_anim',bool(p.items_of('ANIM'))))
for k,v in c.most_common():print(k,v,ex[k][:4])
print('--- overlay converted')
for f in sorted(os.listdir(O)):
    if 'emblem' not in f:continue
    p=Pak(O+f);raw=p.d;g=p.items_of('GEOMETRY_1')[0];pg,b=g.page,g.data_off;bb=p.pbase(pg);pr=p.resolve(pg,b+0x20);r=p.resolve(pr[0],pr[1]);mb=p.pbase(r[0])+r[1]
    print(f,struct.unpack_from('<II',raw,mb+0x30),'proto file off %#x'%mb)
