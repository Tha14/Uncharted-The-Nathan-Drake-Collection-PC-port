from lib import *
import ps3params
p=Pak('p3/air-multi.pak')
mt=p.items_of('MATERIAL_TABLE_1')[0];pg,b=mt.page,mt.data_off
nmat=p.pu32(pg,b+0xc);ma=p.resolve(pg,b+0x20);th=p.resolve(pg,b+0x18);nm=p.resolve(pg,b+0x30);ntex=p.pu32(pg,b+4)
for i in range(nmat):
    m=p.resolve(ma[0],ma[1]+4*i);name=p.pstr(*p.resolve(*m))
    if name.endswith(sys.argv[1]):
        for k,e,node in ps3params._walk(p,m)[:3]:
            bb=p.resolve(node[0],node[1]+0x10)
            print(k,e,bb)
            if bb:
                B=p.pbase(bb[0])+bb[1];n=struct.unpack_from('>I',p.d,B+4)[0];print(' n',n)
                for t in range(min(n,16)):
                    rr=p.resolve(bb[0],bb[1]+0x50+0x10*t)
                    x=None
                    if rr and rr[0]==th[0] and th[1]<=rr[1]<th[1]+4*ntex:
                        x=p.pstr(*p.resolve(nm[0],nm[1]+4*((rr[1]-th[1])//4))).split('/')[-2]
                    print('  ',t,x,hex(struct.unpack_from('>I',p.d,B+0x54+0x10*t)[0]))
