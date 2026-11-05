"""Dump an item's data region split at pointer targets (both platforms)."""
import struct,sys
from ndpak import Pak
def regions(p,it):
  b=p.pbase(it.page); lo=it.data_off; hi=it.data_off+it.size
  cuts={lo,hi}
  for (pg,o),tp in p.ptrmap.items():
    if pg==it.page and lo<=o<hi:
      t=p.pptr(pg,o)
      if tp==it.page and lo<=t<hi: cuts.add(t)
  cuts=sorted(cuts); return [(cuts[i],cuts[i+1]) for i in range(len(cuts)-1)]
def show(p,it,maxw=48):
  ps=p.ptrsize; b=p.pbase(it.page); out=[]
  for lo,hi in regions(p,it):
    words=[]
    o=lo
    while o<hi and len(words)<maxw:
      if p.is_ptr(it.page,o):
        tp,t=p.resolve(it.page,o); s=p.pstr(tp,t) or ''
        ok=len(s)>1 and all(32<=ord(c)<127 for c in s[:20])
        words.append('->%x%s'%(t,('"%s"'%s[:24]) if ok else '')); o+=ps
      else:
        words.append(p.d[b+o:b+o+4].hex()); o+=4
    out.append('[%05x-%05x %4d] %s'%(lo,hi,hi-lo,' '.join(words)))
  return '\n'.join(out)
if __name__=='__main__':
  p=Pak(sys.argv[1]); t=sys.argv[2]
  for it in p.items_of(t): print(it); print(show(p,it,int(sys.argv[3]) if len(sys.argv)>3 else 48))
