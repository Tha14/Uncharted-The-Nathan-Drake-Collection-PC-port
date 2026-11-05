import sys,struct
sys.path.insert(0,'../live/airport-igc')
from lib import *
import ps4actor
def mats(path):
    p=Pak(path);raw=p.d;g=p.items_of('GEOMETRY_1')[0];pg,b=g.page,g.data_off
    nmat=p.pu32(pg,b+0x48);mp=p.resolve(pg,b+0x50);out=[]
    nsub=p.pu32(pg,b+8);sr=p.resolve(pg,b+0x10);use={}
    for i in range(nsub):
        o=sr[1]+0x88*i;mm=p.resolve(sr[0],o+0x48)
        k=p.pstr(*mm) if mm else None
        n=p.pstr(*p.resolve(sr[0],o+8)) if p.resolve(sr[0],o+8) else ''
        nv=struct.unpack_from('<I',raw,p.pbase(sr[0])+o+0x20)[0]
        use.setdefault(k,[]).append(nv)
    for i in range(nmat):
        m=mp[1]+0x48*i;nm=p.pstr(*p.resolve(mp[0],m))
        sh=p.resolve(mp[0],m+8);sh=p.pstr(*sh) if sh else ''
        nf,npr,nb,x3=[p.pu32(mp[0],m+0x30+4*k) for k in range(4)]
        fr=p.resolve(mp[0],m+0x10);fe=[p.pstr(*p.resolve(fr[0],fr[1]+8*k)) for k in range(nf)] if fr else []
        pr=p.resolve(mp[0],m+0x20);par={}
        for k in range(npr):
            e=pr[1]+0x18*k;n=p.pstr(*p.resolve(pr[0],e));v=p.resolve(pr[0],e+8);c=p.pu32(pr[0],e+0x10)
            par[n]=tuple(round(x,3) for x in struct.unpack_from('<%df'%c,raw,p.pbase(v[0])+v[1]))
        br=p.resolve(mp[0],m+0x28);bi=[]
        for k in range(nb):
            e=br[1]+0x28*k;a=p.resolve(br[0],e);bi.append((p.pstr(*a) if a else '?',p.pstr(*p.resolve(br[0],e+0x10)).split('/')[-1][:50]))
        ef=p.resolve(mp[0],m+0x18);ef=p.pstr(*ef) if ef else ''
        out.append((nm,sh,ef,fe,par,bi,use.get(nm,[])))
    return out
for n in sys.argv[1:]:
    f,l=find('actor33',n);print('=====',n,l,f[-60:])
    for nm,sh,ef,fe,par,bi,u in mats(f):
        print('--',nm[:70],'|sh',sh[:40],'|ef',ef[:12],'|subs',len(u),sum(u))
        print('   feat',fe)
        print('   par',{k:v for k,v in par.items()})
        print('   bind',bi)
