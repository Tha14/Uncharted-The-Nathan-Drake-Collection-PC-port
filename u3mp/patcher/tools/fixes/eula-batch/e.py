import sys,re,struct
sys.path.insert(0,'gh2/u3mp/paktools')
from dc import DC,sid
c=DC('eula-batch/x/net.bin');b=c.d
S={}
for m in re.finditer(rb'[ -~]*(?i:eula)[ -~]*',b):
  S[m.start()]=m.group().decode();print(hex(m.start()),m.group()[:90])
print('syms',[(hex(s),hex(t),hex(o)) for s,t,o in c.syms if s==sid('eula') or s==sid('eula-list')])
rev={}
for q in c.ptrs:
  v=c.ptr(q)
  if v in S:rev.setdefault(v,[]).append(q)
qs=sorted(q for l in rev.values() for q in l);print([hex(q) for q in qs])
a=min(qs)-0x40
for o in range(a,max(qs)+0x60,4):
  v=c.u32(o);print('%05x %08x %s %s'%(o,v,'P' if o in c.ptrs else '',S.get(v,'') if o in c.ptrs else ''))
for q in sorted(c.ptrs):
  v=c.ptr(q)
  if a<=v<=max(qs)+0x60:print('ref to table',hex(q),hex(v))
for s,t,o in c.syms:
  if a-0x100<=o<=max(qs)+0x100:print('sym',hex(s),hex(t),hex(o))
