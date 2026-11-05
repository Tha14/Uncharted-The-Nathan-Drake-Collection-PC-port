import sys,struct,re,collections,os,json
S="{{SCRATCH}}/"
# paktools: the recipe's copy ({{PAKTOOLS}}); the session used scratchpad gh2/u3mp/paktools (same API)
PT="{{PAKTOOLS}}"
for _x in (S+'gh2/u3mp/paktools/stage',S+'gh2/u3mp/paktools',PT+'/stage',PT):
    if os.path.isdir(_x) and _x not in sys.path:sys.path.insert(0,_x)
from ndpak import Pak
R="{{PROJECT}}/"
OV=R+'work/overlay-r3/u3data/build/main/pak23/ps4/'
def mats(q):
    out=[]
    g=q.items_of('GEOMETRY_1')[0];pg,b=g.page,g.data_off
    n=q.pu32(pg,b+0x48);r=q.resolve(pg,b+0x50)
    if not r:return out
    mp,mo=r
    for i in range(n):
        o=mo+i*0x48
        S_=lambda off:(q.pstr(*q.resolve(mp,o+off)) if q.resolve(mp,o+off) else None)
        nf,npar,nb,x3=struct.unpack_from('<4I',q.d,q.pbase(mp)+o+0x30)
        feats=[];fr=q.resolve(mp,o+0x10)
        for k in range(nf if fr else 0):
            rr=q.resolve(fr[0],fr[1]+8*k);feats.append(q.pstr(*rr) if rr else None)
        params=[];pr=q.resolve(mp,o+0x20)
        for k in range(npar if pr else 0):
            po=pr[1]+0x18*k;nm=q.pstr(*q.resolve(pr[0],po));vr=q.resolve(pr[0],po+8);cnt=q.pu32(pr[0],po+0x10)
            vals=list(struct.unpack_from('<4f',q.d,q.pbase(vr[0])+vr[1])) if vr else None
            params.append((nm,cnt,vals))
        binds=[];br=q.resolve(mp,o+0x28)
        for k in range(nb if br else 0):
            bo=br[1]+0x28*k
            a=q.resolve(br[0],bo);c=q.resolve(br[0],bo+8)
            binds.append((q.pstr(*a) if a else '',q.pstr(*c) if c else ''))
        out.append(dict(i=i,name=S_(0),shader=S_(8),hash=S_(0x18),feats=feats,params=params,binds=binds,x3=x3))
    return out
def subs(q):
    g=q.items_of('GEOMETRY_1')[0];d=q.d;b=q.pbase(g.page)+g.data_off
    ns=struct.unpack_from('<I',d,b+8)[0];tp,to=q.resolve(g.page,g.data_off+0x10);pb=q.pbase(tp);out=[]
    for i in range(ns):
        o=to+i*0x88
        nm=q.pstr(*q.resolve(tp,o+8)) or '';w=struct.unpack_from('<34I',d,pb+o)
        mat=(q.pstr(*q.resolve(tp,o+0x48)) if q.is_ptr(tp,o+0x48) else '') or ''
        st=q.resolve(tp,o+0x30);sq=q.pbase(st[0])+st[1];stride=struct.unpack_from('<H',d,sq+2)[0]
        da=q.resolve(st[0],st[1]+16);vb=q.pbase(da[0])+da[1]
        ia=q.resolve(tp,o+0x40);ib=q.pbase(ia[0])+ia[1]
        out.append(dict(i=i,o=(tp,o),name=nm,mat=mat,nv=w[8],ni=w[9],v=vb,s=stride,ib=ib,w=w,st=st))
    return out

# ---- functions appended to the session lib.py on 2026-11-01 20:07 / 20:14 / 20:15 (agent a346383c, map-colors);
# ---- recovered verbatim from the transcript (they were missing from the recovered copy: NameError insts/isubs)
import numpy as np
def streams(q,s):
    out={}
    for k in range(s['w'][10]):
        so=s['st'][1]+0x18*k;f=q.resolve(s['st'][0],so+8);slot,_,sz,ty=q.d[q.pbase(f[0])+f[1]:q.pbase(f[0])+f[1]+4]
        da=q.resolve(s['st'][0],so+16);base=q.pbase(da[0])+da[1];L=q.pu32(s['st'][0],so+4)
        dt={6:('<f4',3),0x22:('<f2',2),0x0a:('<f2',4)}[ty]
        out[slot]=np.frombuffer(q.d,dt[0],L//sz*dt[1],base).reshape(-1,dt[1])
    return out
def indices(q,s):
    return np.frombuffer(q.d,'<u2',s['ni'],s['ib']).reshape(-1,3)
def protos(q):
    g=q.items_of('GEOMETRY_1')[0];d=q.d;b=q.pbase(g.page)+g.data_off
    npr=struct.unpack_from('<I',d,b+0x18)[0];pp,po=q.resolve(g.page,g.data_off+0x20);PR=[]
    for i in range(npr):
        t=q.resolve(pp,po+i*8);c=struct.unpack_from('<I',d,q.pbase(t[0])+t[1]+4)[0];L=[]
        if c:
            l=q.resolve(t[0],t[1]+0x18)
            for j in range(c):L.append(q.pstr(*q.resolve(l[0],l[1]+j*0x10)) or '')
        PR.append(L)
    return PR
def insts(q):
    g=q.items_of('GEOMETRY_1')[0];d=q.d;b=q.pbase(g.page)+g.data_off
    ni=struct.unpack_from('<I',d,b+0x28)[0];pp,po=q.resolve(g.page,g.data_off+0x30);out=[]
    for i in range(ni):
        t=q.resolve(pp,po+i*8);a=q.pbase(t[0])+t[1];nm=q.pstr(*q.resolve(t[0],t[1]+8)) or ''
        mm=re.match(r'Prototype(\d+)',nm)
        out.append(dict(i=i,proto=int(mm.group(1)) if mm else None,M=np.array(struct.unpack_from('<16f',d,a+0x80)).reshape(4,4),t=t))
    return out
def isubs(q,ins):
    t=ins['t'];a=q.pbase(t[0])+t[1];ns=struct.unpack_from('<I',q.d,a+0x20)[0];r=q.resolve(t[0],t[1]+0x30);out=[]
    for k in range(ns):
        o=r[1]+0x50*k;b=q.pbase(r[0])+o
        name=q.pstr(*q.resolve(r[0],o));n=struct.unpack_from('<I',q.d,b+0x10)[0];fl,fl2=struct.unpack_from('<II',q.d,b+0x20)
        st={}
        sr=q.resolve(r[0],o+0x18)
        for j in range(n if sr else 0):
            so=sr[1]+0x18*j;f=q.resolve(sr[0],so+8);slot,_,sz,ty=q.d[q.pbase(f[0])+f[1]:q.pbase(f[0])+f[1]+4]
            da=q.resolve(sr[0],so+16);base=q.pbase(da[0])+da[1];L=q.pu32(sr[0],so+4)
            dt={6:('<f4',3),0x22:('<f2',2),0x0a:('<f2',4)}[ty]
            st[slot]=np.frombuffer(q.d,dt[0],L//sz*dt[1],base).reshape(-1,dt[1])
        out.append(dict(name=name,flags=fl,flags2=fl2,st=st))
    return out
def _tc():
    import texcodec as tc
    return tc
def load_tex(q,t):
    tc=_tc()
    w,h=t['width'],t['height'];data=q.vram_bytes(t['vramOffset'],t['vramSize']);f=t['format'];kind={0x47:'BC1',0x48:'BC1',0x4d:'BC3',0x4e:'BC3',0x4a:'BC2',0x4b:'BC2'}.get(f);bpb=16 if kind in('BC3','BC2') else 8
    lin=tc.ps4_mip0_linear(data,w,h,bpb);return tc.decode(bytes(lin),w,h,kind)
def s2l(v):
    v=np.asarray(v,float);return np.where(v<=0.04045,v/12.92,((v+0.055)/1.055)**2.4)
def bc5(q,t):
    tc=_tc()
    w,h=t['width'],t['height'];data=q.vram_bytes(t['vramOffset'],t['vramSize']);lin=tc.ps4_mip0_linear(data,w,h,16)
    a=np.frombuffer(bytes(lin),'u1').reshape(-1,16)
    def ep(blk):
        a0,a1=blk[:,0].astype(float),blk[:,1].astype(float);return a0.mean(),a1.mean()
    # mean of endpoints approximate
    return dict(r=(a[:,0].astype(float).mean(),a[:,1].astype(float).mean()),g=(a[:,8].astype(float).mean(),a[:,9].astype(float).mean()))
