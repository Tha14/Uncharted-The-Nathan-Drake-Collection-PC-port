import sys,struct
from ndpak import Pak
from sg import spawners
import gdump
def blk(p,pg,o,n=64):
  # dump words from pg:o for n bytes with ptr annotation
  ps=p.ptrsize;b=p.pbase(pg);out=[];x=o
  while x<o+n:
    if p.is_ptr(pg,x):
      tp,t=p.resolve(pg,x);s=p.pstr(tp,t) or '';ok=len(s)>1 and all(32<=ord(c)<127 for c in s[:12])
      out.append('->%x%s'%(t,('"%s"'%s[:20]) if ok else ''));x+=ps
    else: out.append(p.d[b+x:b+x+4].hex());x+=4
  return ' '.join(out)
def byname(p):
  m={}
  for it,(pg,o) in spawners(p):
    r=p.resolve(pg,o+32); 
    if r: m[p.pstr(*r)]=(pg,o)
  return m
