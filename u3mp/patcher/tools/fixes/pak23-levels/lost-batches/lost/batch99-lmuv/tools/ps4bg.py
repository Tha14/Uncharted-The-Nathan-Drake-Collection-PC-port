"""read PS4 background GEOMETRY_1 (retail) into python structures"""
import sys,struct
import numpy as np
sys.path.insert(0,'.')
TYPES={6:('<f4',3),0x22:('<f2',2),0x0a:('<f2',4)}
def streams(q,ref,n):
    out=[]
    for k in range(n):
        pg,o=ref[0],ref[1]+0x18*k
        b=q.pbase(pg)
        one,stride,size=struct.unpack_from('<HHI',q.d,b+o)
        f=q.resolve(pg,o+8); d=q.resolve(pg,o+0x10)
        fb=q.pbase(f[0]); slot,z,sz,ty=q.d[fb+f[1]:fb+f[1]+4]
        arr=None
        if d:
            db=q.pbase(d[0]); dt,nc=TYPES[ty]
            arr=np.frombuffer(q.d[db+d[1]:db+d[1]+size],dt).reshape(-1,nc).astype(float)
        out.append(dict(slot=slot,stride=stride,size=size,type=ty,data=arr,fmtref=f,dataref=d))
    return out
def geometry(q):
    g=q.items_of('GEOMETRY_1')[0]; pg,b=g.page,g.data_off
    cnt=lambda o:q.u64(q.pbase(pg)+b+o)
    G=dict(ver=q.pu32(pg,b),flags=q.pu32(pg,b+4))
    nsub=cnt(8); sref=q.resolve(pg,b+0x10)
    subs=[]
    for k in range(nsub):
        o=sref[1]+0x88*k; sp=sref[0]; sb=q.pbase(sp)
        nm=q.pstr(*q.resolve(sp,o+8))
        w=struct.unpack_from('<16I',q.d,sb+o+0x10)
        nv,ni,ns,ns2=struct.unpack_from('<4I',q.d,sb+o+0x20)
        st=streams(q,q.resolve(sp,o+0x30),ns)
        st2=streams(q,q.resolve(sp,o+0x38),ns2) if ns2 else []
        ib=q.resolve(sp,o+0x40); ibb=q.pbase(ib[0])
        idx=np.frombuffer(q.d[ibb+ib[1]:ibb+ib[1]+2*ni],'<u2')
        mat=q.pstr(*q.resolve(sp,o+0x48))
        subs.append(dict(name=nm,nv=nv,ni=ni,streams=st,streams2=st2,idx=idx,mat=mat,raw=q.d[sb+o:sb+o+0x88],ref=(sp,o)))
    G['subs']=subs
    return G
