"""interactive helpers for reversing: region dump bounded by pointer targets"""
import sys,struct,bisect
sys.path.insert(0,'/home/claude/u3/paktools')
from ndpak import Pak
class RX:
    def __init__(s,path):
        s.p=p=Pak(path)
        pt={}
        for (pg,o),tp in p.ptrmap.items():
            pt.setdefault(tp,set()).add(p.pptr(pg,o))
        for it in p.items:
            pt.setdefault(it.page,set()).update({it.off,it.data_off,it.data_off+it.size})
        s.cuts={k:sorted(v) for k,v in pt.items()}
    def ext(s,pg,o):
        c=s.cuts.get(pg,[]); i=bisect.bisect_right(c,o)
        return (c[i] if i<len(c) else s.p.pages[pg][1])-o
    def show(s,pg,o,n=None,maxw=64):
        p=s.p; ps=p.ptrsize; b=p.pbase(pg)
        if n is None: n=s.ext(pg,o)
        out=[]; x=o
        while x<o+n and len(out)<maxw:
            if p.is_ptr(pg,x):
                tp,t=p.resolve(pg,x); st=p.pstr(tp,t)[:40]
                ok=len(st)>1 and all(32<=ord(c)<127 for c in st)
                out.append('%x:->p%d:%x%s'%(x-o,tp,t,('"%s"'%st) if ok else '')); x+=ps
            else:
                v=p.u32(b+x); f=struct.unpack(p.e+'f',p.d[b+x:b+x+4])[0]
                out.append('%x:%08x%s'%(x-o,v,('(%.4g)'%f) if 1e-4<abs(f)<1e7 else '')); x+=4
        return 'p%d:%x [%#x]\n  '%(pg,o,n)+'\n  '.join(out)
