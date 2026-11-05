"""Compare two PS4 paks item by item, ignoring block order and padding."""
import sys,collections
from ndpak import Pak
from gdump import regions
def blocks(p,it):
  out=[]
  b=p.pbase(it.page)
  for lo,hi in regions(p,it):
    bs=bytearray(p.d[b+lo:b+hi])
    for o in range(lo,hi,8):
      if p.is_ptr(it.page,o): bs[o-lo:o-lo+8]=b'PPPPPPPP'
    s=bytes(bs).rstrip(b'\xaa').rstrip(b'\x00')
    out.append(s)
  return out
def cmp(a,b):
  pa,pb=Pak(a),Pak(b)
  ok=True
  for t in sorted({i.type for i in pa.items}|{i.type for i in pb.items}):
    ia=pa.items_of(t); ib=pb.items_of(t)
    if len(ia)!=len(ib): print(t,'count',len(ia),len(ib)); ok=False; continue
    for x,y in zip(ia,ib):
      ba=collections.Counter(blocks(pa,x)); bb=collections.Counter(blocks(pb,y))
      if ba==bb: print(t,'MATCH',len(ba)); continue
      ok=False
      only_a=ba-bb; only_b=bb-ba
      print(t,'DIFF',sum(only_a.values()),'only-converted',sum(only_b.values()),'only-retail of',sum(ba.values()))
      for s in list(only_a)[:3]: print('   conv ',s[:64].hex())
      for s in list(only_b)[:3]: print('   real ',s[:64].hex())
  return ok
if __name__=='__main__': cmp(sys.argv[1],sys.argv[2])
