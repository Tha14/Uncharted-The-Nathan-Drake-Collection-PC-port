import sys,os,json,struct
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from ndpak import Pak
def mats(q):
    out=[]
    for g in q.items_of('GEOMETRY_1'):
        pg,b=g.page,g.data_off
        flags=q.pu32(pg,b+4)
        n=q.u64(q.pbase(pg)+b+0x48)
        r=q.resolve(pg,b+0x50)
        if not r: continue
        mp,mo=r
        for i in range(n):
            o=mo+i*0x48
            S=lambda off: q.pstr(*q.resolve(mp,o+off)) if q.resolve(mp,o+off) else None
            nf,npar,nb,x3=struct.unpack_from('<4I',q.d,q.pbase(mp)+o+0x30)
            feats=[]
            fr=q.resolve(mp,o+0x10)
            for k in range(nf if fr else 0):
                feats.append(q.pstr(*q.resolve(fr[0],fr[1]+8*k)))
            binds=[]
            br=q.resolve(mp,o+0x28)
            for k in range(nb if br else 0):
                bo=br[1]+0x28*k
                binds.append((q.pstr(*q.resolve(br[0],bo)),q.pstr(*q.resolve(br[0],bo+8))))
            out.append(dict(n=S(0),s=S(8),h=S(0x18),f=feats,b=binds,fl=flags))
    return out
res=open(sys.argv[2],'w')
d=sys.argv[1]
for fn in sorted(os.listdir(d)):
    if not fn.endswith('.pak') or fn.endswith('-ingame12.pak'): continue
    try:
        q=Pak(os.path.join(d,fn))
        m=mats(q)
        nfx=len(q.items_of('EFFECT'))
        res.write(json.dumps(dict(pak=fn,size=len(q.d),nfx=nfx,mats=m))+'\n')
    except Exception as e:
        res.write(json.dumps(dict(pak=fn,err=str(e)))+'\n')
    res.flush()
