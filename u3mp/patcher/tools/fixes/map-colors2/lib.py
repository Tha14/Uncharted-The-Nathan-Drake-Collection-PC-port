import sys,os,struct,json,re,collections
PT='{{PAKTOOLS}}'
HERE=os.path.dirname(os.path.abspath(__file__))
for x in (PT+'/stage',PT,HERE+'/tools'):
    if x not in sys.path: sys.path.insert(0,x)
from ndpak import Pak
W='{{PROJECT}}/work/'
OV=W+'overlay-r3/u3data/build/main/pak23/ps4/'
RT="{{PS4}}/u3data/build/main/pak23/ps4/"
PS3D="{{PS3_DISC}}/"
def mats4(q):
    out=[]
    for g in q.items_of('GEOMETRY_1'):
        pg,b=g.page,g.data_off
        n=q.u64(q.pbase(pg)+b+0x48)
        r=q.resolve(pg,b+0x50)
        if not r: continue
        mp,mo=r
        for i in range(n):
            o=mo+i*0x48
            S=lambda off:(q.pstr(*q.resolve(mp,o+off)) if q.resolve(mp,o+off) else None)
            nf,npar,nb,x3=struct.unpack_from('<4I',q.d,q.pbase(mp)+o+0x30)
            feats=[];fr=q.resolve(mp,o+0x10)
            for k in range(nf if fr else 0): feats.append(q.pstr(*q.resolve(fr[0],fr[1]+8*k)))
            pars=[];pr=q.resolve(mp,o+0x20)
            for k in range(npar if pr else 0):
                po=pr[1]+0x18*k
                nm=q.pstr(*q.resolve(pr[0],po));vr=q.resolve(pr[0],po+8);cnt=q.pu32(pr[0],po+0x10)
                vals=list(struct.unpack_from('<4f',q.d,q.pbase(vr[0])+vr[1])) if vr else None
                pars.append((nm,cnt,vals))
            binds=[];br=q.resolve(mp,o+0x28)
            for k in range(nb if br else 0):
                bo=br[1]+0x28*k
                binds.append((q.pstr(*q.resolve(br[0],bo)),q.pstr(*q.resolve(br[0],bo+8))))
            out.append(dict(i=i,n=S(0),s=S(8),h=S(0x18),f=feats,p=pars,b=binds,x3=x3,off=q.pbase(mp)+o))
    return out
