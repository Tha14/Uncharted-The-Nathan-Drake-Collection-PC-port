"""PS3 background (PM_COLLECTION_2) geometry decoder, built on ps3geom."""
import sys,struct
import numpy as np
sys.path.insert(0,'/home/claude/u3/paktools')
import ps3geom

NAMES={1:'pos',2:'normal',3:'tangent',0x11:'uv0',0x12:'uv1',0x13:'uv2',0x14:'uv3',0x15:'color',0x4c:'ilight',0x4e:'iconst'}

def read_format(p,pg,fmt):
    b=p.pbase(pg)
    attrs=[]; o=fmt+8; first=None
    while first is None or o<fmt+first:
        aid,af,aoff=struct.unpack_from('>BBH',p.d,b+o)
        widths=list(p.d[b+o+4:b+o+8])
        attrs.append((aid,af,aoff,widths))
        if af&0xf0 in (0x40,0x60):
            first=aoff if first is None else min(first,aoff)
        o+=8
        if o>=fmt+8+8*16: break
    return attrs

def decode_attr(p,pg,fmt,af,aoff,widths,data_pg,data_off,nv,prec=None):
    b=p.pbase(pg)
    n=af&0xf; w=widths[:n]
    if sum(w):
        db=p.pbase(data_pg)
        V=ps3geom._bits_be(p.d[db+data_off:db+data_off+(nv*sum(w)+7)//8],w,nv).astype(np.float64)
    else:
        V=np.zeros((nv,n))
    if af&0xf0==0x40:
        pa=struct.unpack_from('>%df'%(2*n),p.d,b+fmt+aoff); a=np.array(pa[:n]); bb=np.array(pa[n:])
        return a*(1.0+V*2.0**-21)+bb
    if af&0xf0==0x60:
        pa=struct.unpack_from('>%df'%(2*n),p.d,b+fmt+aoff)
        off=np.array(struct.unpack_from('>%dI'%n,p.d,b+fmt+aoff+8*n),dtype=np.float64)
        a=np.array(pa[:n]); bb=np.array(pa[n:])
        return a*(1.0+(4.0*V+off)*2.0**-23)+bb
    if af&0xf0==0x00:
        # n-bit integers are zero-extended into a byte and read as v*2/255-1 (verified against
        # PS4: 5/6/7-bit fields cover only part of [-1,1]); 1-bit fields are signs (+-1)
        # prec (background blocks, see block_prec): nominal bit depth of the attribute; narrower
        # fields have their high zero bits dropped
        if prec is not None:
            return np.stack([V[:,c]*2.0-1.0 if (w[c]==1 and c==3) else V[:,c]*2.0/((1<<prec)-1)-1.0 for c in range(n)],1)
        return np.stack([V[:,c]*2.0-1.0 if w[c]==1 else V[:,c]*2.0/(255.0 if w[c]<8 else (1<<w[c])-1)-1.0 for c in range(n)],1)
    raise ValueError('attr fmt %#x'%af)

TYPE_BITS={2:10,3:10,0x15:8}
def block_prec(attrs):
    """background blocks: nominal bit depth of each signed-normalised (0x0N) attribute =
    min(type depth (normal/tangent 10, colour 8), widest 0x0N normal/tangent field of the block,
    8 if there is none).  Verified
    against retail PS4 ldn-train-stage / san-upper-multi-stage: 7-bit colours of a block whose
    normals are 10-bit decode /255, those of an all-7-bit block /127."""
    P=max([max([x for x in a[3][:a[1]&0xf] if x>1] or [0]) for a in attrs
           if a[1]&0xf0==0 and a[0] not in (0x15,0x4c,0x4e)] or [0]) or 8   # colour-only block: bytes
    out={a[0]:max(1,min(TYPE_BITS.get(a[0],8),P)) for a in attrs if a[1]&0xf0==0}
    for a in attrs:      # instance constant (iconst, 0x4e): its own widest field (values k/3 for 2 bits)
        if a[0]==0x4e and a[1]&0xf0==0:
            out[0x4e]=max(1,max(a[3][:a[1]&0xf] or [1]))
    return out

def decode_block(p,pg,blk,inst=None):
    """inst = (fmt (pg,off), stream table (pg,off)) of the instance's per-block data, or None"""
    cnt=p.resolve(pg,blk+0x24); nv=p.pu32(*cnt)
    fpg,fmt=p.resolve(pg,blk+0x28)
    spg,st=p.resolve(pg,blk)
    attrs=read_format(p,fpg,fmt)
    res={'nverts':nv,'raw':{}}
    for ai,(aid,af,aoff,w) in enumerate(attrs):
        n=af&0xf
        if aid in (0x4c,0x4e): continue                  # instance attributes (per-instance data)
        r=p.resolve(spg,st+8*ai)
        if sum(w[:n]) and r is None: raise ValueError('missing stream')
        val=decode_attr(p,fpg,fmt,af,aoff,w,r[0] if r else 0,r[1] if r else 0,nv)
        if aid==0x15:
            val=(val+1.0)/2.0 if af&0xf0==0 else val     # vertex colour: unorm
        res[NAMES.get(aid,'a%x'%aid)]=val
        res['raw'][aid]=(af,w)
    res['order']=[a[0] for a in attrs]
    ii=p.resolve(pg,blk+4); b=p.pbase(ii[0])
    ntris,_,isz,_=struct.unpack_from('>4H',p.d,b+ii[1])
    idat=p.resolve(ii[0],ii[1]+8); bi=p.pbase(idat[0])
    res['tris']=ps3geom.decode_indices(p.d[bi+idat[1]:bi+idat[1]+isz],ntris)
    if inst is not None:
        (ipg,ifmt),(tpg,tst)=inst
        iattrs=read_format(p,ipg,ifmt)
        battr={a[0]:a for a in attrs}
        k=0
        for (aid,af,aoff,w) in iattrs:
            ba=battr.get(aid)
            if aid not in (0x4c,0x4e):
                continue
            r=p.resolve(tpg,tst+4*k); k+=1
            n=af&0xf
            val=decode_attr(p,ipg,ifmt,af,aoff,w,r[0] if r else 0,r[1] if r else 0,nv)
            res[NAMES.get(aid,'a%x'%aid)]=val
            res['raw'][aid]=(af,w)
    return res

def material_groups(p,proto):
    """[(material name, first block, block count)] in material-table order"""
    pg,po=proto
    nmat=p.u16(p.pbase(pg)+po+4)
    t=p.resolve(pg,po+0x10)
    out=[]
    for s in range(nmat):
        e=t[1]+0x10*s
        sd=p.resolve(t[0],e)
        first,cnt=struct.unpack_from('>HH',p.d,p.pbase(t[0])+e+4)
        m=p.resolve(sd[0],sd[1])
        out.append((p.pstr(*p.resolve(m[0],m[1])),first,cnt))
    return out

def block_materials(p,proto):
    pg,po=proto
    nmat=p.u16(p.pbase(pg)+po+4); nblk=p.u16(p.pbase(pg)+po+6)
    t=p.resolve(pg,po+0x10)
    out=[None]*nblk
    for s in range(nmat):
        e=t[1]+0x10*s
        sd=p.resolve(t[0],e)
        first,cnt=struct.unpack_from('>HH',p.d,p.pbase(t[0])+e+4)
        m=p.resolve(sd[0],sd[1])
        nm=p.pstr(*p.resolve(m[0],m[1]))
        for k in range(first,first+cnt): out[k]=nm
    return out

def collection(p):
    """-> list of prototypes: dict(hdr, blocks[(pg,off)], mats[], instances[(pg,off)])"""
    pms=p.items_of('PM_COLLECTION_2')
    if not pms: return []                               # water-only / empty BG level
    pm=pms[0]
    n=p.pu32(pm.page,pm.data_off)
    out=[]
    for k in range(n):
        e=p.resolve(pm.page,pm.data_off+4+4*k)
        hdr=p.resolve(*e)
        il=p.resolve(e[0],e[1]+4); ni=p.pu32(e[0],e[1]+8)
        insts=[p.resolve(il[0],il[1]+4*i) for i in range(ni)]
        nblk=p.u16(p.pbase(hdr[0])+hdr[1]+6)
        tb=p.resolve(hdr[0],hdr[1]+0x14)
        blocks=[p.resolve(tb[0],tb[1]+4*i) for i in range(nblk)]
        out.append(dict(entry=e,hdr=hdr,blocks=blocks,mats=block_materials(p,hdr),groups=material_groups(p,hdr),insts=insts))
    return out

def instance(p,ia):
    pg,o=ia; b=p.pbase(pg)
    f=lambda x: struct.unpack_from('>f',p.d,b+o+x)[0]
    r=dict(addr=ia,x8=p.pu32(pg,o+8),bsphere=[f(0x30+4*i) for i in range(4)],
           mat=np.array([f(0x40+4*i) for i in range(16)]).reshape(4,4))
    r['p0c']=p.resolve(pg,o+0xc); r['spheres']=p.resolve(pg,o+0x14)
    r['blockdata']=p.resolve(pg,o+0x18); r['blockmats']=p.resolve(pg,o+0x1c)
    r['raw']=p.d[b+o:b+o+0x100]
    return r

def gen_tangents(pos,nrm,uv,tris):
    """per-vertex tangents (xyz + w sign) from UV derivatives, Gram-Schmidt against the normal"""
    nv=len(pos)
    t=np.zeros((nv,3)); bt=np.zeros((nv,3))
    if uv is not None and len(tris):
        T=np.asarray(tris)
        p0,p1,p2=pos[T[:,0]],pos[T[:,1]],pos[T[:,2]]
        u0,u1,u2=uv[T[:,0]],uv[T[:,1]],uv[T[:,2]]
        e1,e2=p1-p0,p2-p0; d1,d2=u1-u0,u2-u0
        r=d1[:,0]*d2[:,1]-d2[:,0]*d1[:,1]
        r=np.where(np.abs(r)<1e-12,0,1.0/np.where(np.abs(r)<1e-12,1,r))[:,None]
        sd=(e1*d2[:,1:2]-e2*d1[:,1:2])*r
        td=(e2*d1[:,0:1]-e1*d2[:,0:1])*r
        for c in range(3):
            np.add.at(t,T[:,c],sd); np.add.at(bt,T[:,c],td)
    n=nrm/np.maximum(np.linalg.norm(nrm,axis=1,keepdims=True),1e-9)
    t=t-n*(n*t).sum(1,keepdims=True)
    ln=np.linalg.norm(t,axis=1)
    bad=ln<1e-9
    if bad.any():   # any vector perpendicular to the normal
        a=np.where(np.abs(n[:,0:1])<0.9,np.array([[1,0,0]]),np.array([[0,1,0]]))
        alt=np.cross(n,a); alt/=np.linalg.norm(alt,axis=1,keepdims=True)
        t[bad]=alt[bad]; ln[bad]=1
    t=t/ln[:,None]
    w=np.where((np.cross(n,t)*bt).sum(1)<0,-1.0,1.0)
    return np.c_[t,w]
