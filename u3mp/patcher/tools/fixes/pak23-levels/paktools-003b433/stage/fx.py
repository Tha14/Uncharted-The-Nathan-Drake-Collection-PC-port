"""PS4 EFFECT closure extraction / canonical comparison"""
import sys,struct,hashlib
sys.path.insert(0,'/home/claude/u3/paktools')
from ndpak import Pak

def item_index(p):
    idx={}
    for it in p.items:
        idx.setdefault(it.page,[]).append(it)
    for k in idx: idx[k].sort(key=lambda i:i.off)
    return idx

def container(p,idx,pg,off):
    import bisect
    L=idx[pg]; offs=[i.off for i in L]
    k=bisect.bisect_right(offs,off)-1
    if k>=0 and L[k].off<=off<L[k].data_off+L[k].size: return L[k]
    return None

def effects(p):
    """-> dict hash -> (effect item, closure items list)"""
    idx=item_index(p)
    et=p.items_of('EFFECT_TABLE')[0]
    n=p.pu32(et.page,et.data_off)
    arr=p.resolve(et.page,et.data_off+8)
    out={}
    for k in range(n):
        o=arr[1]+0x18*k
        ep=p.resolve(arr[0],o)
        h=p.u64(p.pbase(arr[0])+o+0x10)
        eit=container(p,idx,*ep)
        seen={}; todo=[eit]
        while todo:
            it=todo.pop()
            if (it.page,it.off) in seen: continue
            seen[(it.page,it.off)]=it
            for oo in range(it.data_off,it.data_off+it.size,8):
                r=p.resolve(it.page,oo)
                if r:
                    c=container(p,idx,*r)
                    assert c is not None
                    todo.append(c)
        out[h]=(eit,sorted(seen.values(),key=lambda i:(i.page,i.off)),ep)
    return out,et

def canon(p,eit,clos):
    """canonical bytes of an effect closure: pointer -> (item order index, offset in item)"""
    order={(i.page,i.off):n for n,i in enumerate(sorted(clos,key=lambda i:(i.page,i.off)))}
    # order by graph traversal from eit instead for cross-pak stability
    ids={}; seq=[eit]; ids[(eit.page,eit.off)]=0
    import bisect
    idx=item_index(p)
    k=0
    while k<len(seq):
        it=seq[k]; k+=1
        for oo in range(it.data_off,it.data_off+it.size,8):
            r=p.resolve(it.page,oo)
            if r:
                c=container(p,idx,*r)
                if (c.page,c.off) not in ids:
                    ids[(c.page,c.off)]=len(seq); seq.append(c)
    h=hashlib.sha1()
    for it in seq:
        b=bytearray(p.d[p.pbase(it.page)+it.data_off:p.pbase(it.page)+it.data_off+it.size])
        for oo in range(it.data_off,it.data_off+it.size,8):
            r=p.resolve(it.page,oo)
            if r:
                c=container(p,idx,*r)
                struct.pack_into('<II',b,oo-it.data_off,ids[(c.page,c.off)],r[1]-c.off)
        h.update(struct.pack('<I',len(b))+bytes(b))
    return h.hexdigest(),seq
