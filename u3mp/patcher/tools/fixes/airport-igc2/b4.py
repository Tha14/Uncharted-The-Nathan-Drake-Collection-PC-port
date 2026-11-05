from lib import *
from dc import DC,sid
from psarc_lib import Psarc
import re,struct,pickle
R=G+'/u3data/build/main/dc1/ps4/'
names=open(R+'reticule-settings.dci').read().split()
M={}
for n in names: M[sid(n)]=n
for n in open(R+'hud-settings.dci').read().split(): M[sid(n)]=n
for v in (0x26a27cd1,0x46c3ecb,0x2e709b8a,0xf46daf5b,0x4a7d02c5,0xd4c7035c): print(hex(v),M.get(v))
c=DC(R+'reticule-settings.bin')
c3=DC('rsdc/ps3-ret.bin')
print(len(c.syms),len(c3.syms),set(s for s,t,o in c.syms)^set(s for s,t,o in c3.syms))
print([n for n in names if 'scope' in n or 'circle-full' in n or 'vign' in n][:40])
def walk(c,o,depth=0,seen=None,maxn=64,ps=8):
    seen=seen or set()
    for k in range(maxn):
        q=o+k*ps
        if q in c.ptrs:
            v=c.ptr(q);s=c.cstr(v)
            if s and len(s)>2 and s.isprintable(): print(' '*depth,hex(q),'str',s)
            else:
                print(' '*depth,hex(q),'ptr',hex(v))
                if depth<4 and v not in seen:
                    seen.add(v);walk(c,v,depth+2,seen,24,ps)
for v in (0x26a27cd1,0x46c3ecb):
    print('=====',M.get(v));t,o=c.bysid[v];print(hex(t),hex(o))
    walk(c,o,0,None,28)
for m in re.finditer(rb'[ -~]{4,}',c.d):
    pass
print(sorted(set(x.decode() for x in re.findall(rb'[a-z][-a-z0-9_/]{5,}',c.d) if b'/' in x or b'hud' in x))[:60])
