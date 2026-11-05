"""PS3 fragment-program facts for one material: unit->texture, blend numerator source, per-unit uv/alpha use."""
from lib import *
import ps3params,fp
from mkplan import tex_role_final
def base(x): return x.lstrip('-|').split('.')[0].rstrip('|')
def sw(x):
    x=x.lstrip('-|').rstrip('|'); return x.split('.')[1] if '.' in x else 'xyzw'
def parse(l):
    l=l.replace('  (end)','')
    m=re.match(r'(\w+) (\S+), (.*)$',l)
    if not m: return None
    return m.group(1),m.group(2),[a.strip() for a in re.split(r', (?![^{]*})',m.group(3))]
def wr(d):
    return (d.split('.')[0], d.split('.')[1] if '.' in d else 'xyzw')
def trace(P,i,reg,c,depth=0):
    """walk back from instr i for the writer of reg.c; returns ('tex',unit,chan,coord) or other"""
    for j in range(i-1,-1,-1):
        t=P[j]
        if not t: continue
        op,d,a=t
        rn,mk=wr(d)
        if rn!=reg or c not in mk: continue
        if op.startswith('TEX'):
            k=mk.index(c); return ('tex',int(a[-1][3:]),c,a[0])
        if op.startswith('MOV') and depth<6 and a and not a[0].startswith('{') and not a[0].startswith('f['):
            k=mk.index(c); s=sw(a[0]); s2=s[k] if len(s)==len(mk) or len(s)==4 and len(mk)==4 else ('xyzw'['xyzw'.index(c)] if s=='xyzw' else s[min('xyzw'.index(c),len(s)-1)])
            return trace(P,j,base(a[0]),s2,depth+1)
        return (op.split('_')[0],None,None,None)
    return None
def blend_source(L):
    P=[parse(l) for l in L]
    k2=None;rc=None
    for i,t in enumerate(P):
        if not t: continue
        op,d,a=t
        if op.startswith('MAD') and '0.0100021' in L[i]: k2=(wr(d)[0],wr(d)[1][0]);rc=None;continue
        if k2 and op.startswith('DIV') and len(a)>=2 and base(a[1])==k2[0]:
            s=sw(a[0]); return trace(P,i,base(a[0]),s[0]) or ('?',)
        if k2 and op.startswith('RCP') and base(a[0])==k2[0]: rc=(wr(d)[0],wr(d)[1][0]);continue
        if rc and op.startswith(('MUL','MAD')) and any(base(x)==rc[0] for x in a[:2]):
            o=[x for x in a[:2] if base(x)!=rc[0]]
            if op.startswith('MAD') and a[0].startswith('-') and a[1]==a[2]:  # inverted form t - R*map... 
                o=[a[0]]
            if o:
                s=sw(o[0]); return trace(P,i,base(o[0]),s[0]) or ('?',)
    return None
def tex_use(L):
    out={}
    for l in L:
        t=parse(l)
        if t and t[0].startswith(('TEX','TXP')):
            u=int(t[2][-1][3:]); out.setdefault(u,[]).append((t[2][0],wr(t[1])[1]))
    return out
def facts(p,cache):
    """{material: dict(units={u:short},src=...,use=..,params=..)}"""
    mt=p.items_of('MATERIAL_TABLE_1')
    if not mt: return {}
    mt=mt[0];pg,b=mt.page,mt.data_off
    nmat=p.pu32(pg,b+0xc);ma=p.resolve(pg,b+0x20);th=p.resolve(pg,b+0x18);nm=p.resolve(pg,b+0x30);ntex=p.pu32(pg,b+4);sr=p.resolve(pg,b+0x1c)
    PAR=ps3params.all_material_params(p)
    out={}
    def tn(rr):
        if rr and rr[0]==th[0] and th[1]<=rr[1]<th[1]+4*ntex:
            return p.pstr(*p.resolve(nm[0],nm[1]+4*((rr[1]-th[1])//4))).split('/')[-2]
    for i in range(nmat):
        m=p.resolve(ma[0],ma[1]+4*i);name=p.pstr(*p.resolve(*m))
        best=None
        W=ps3params._walk(p,m)
        for k,e,node in W:
            if k>2: continue
            pid=struct.unpack_from('>H',p.d,p.pbase(node[0])+node[1])[0]
            t=p.resolve(sr[0],sr[1]+0x10*pid+12)
            if not t: continue
            B=p.pbase(t[0])+t[1];h=struct.unpack_from('>8I',p.d,B)
            if pid not in cache:
                try: cache[pid]=fp.dis(p.d[B+h[4]:B+h[4]+h[0]])
                except Exception: cache[pid]=[]
            if e==0 and (best is None or len(cache[pid])>len(best[0])): best=(cache[pid],node,pid)
        if not best: continue
        L,node,pid=best
        units={}
        bb=p.resolve(node[0],node[1]+0x10)
        if bb:
            x=tn(p.resolve(bb[0],bb[1]+0x10))
            if x: units[0]=x
            for u in range(1,12):
                x=tn(p.resolve(bb[0],bb[1]+0x40+0x10*u))
                if x: units[u]=x
        zp=False
        U=tex_use(L)
        sc=blend_source(L)
        if sc and sc[0]=='tex' and sc[2]=='z':
            for c,mk in U.get(sc[1],[]):
                if 'x' in mk and 'y' in mk and 'w' not in mk: zp=True
            zp=zp and any(l.startswith(('DP2','DP3')) for l in L)
        out[name]=dict(zproof=zp,units=units,src=blend_source(L),use=tex_use(L),params=PAR.get(name,{}),pid=pid)
    return out
