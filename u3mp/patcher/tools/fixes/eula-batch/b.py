import sys,struct,re
sys.path.insert(0,'gh2/u3mp/paktools')
from dc import DC,sid
c=DC('_ps3menu.bin')
# name table
names={}
for m in re.finditer(rb'[A-Za-z0-9_\-\.\(\) ]{3,}',c.d):
  names[sid(m.group().decode())]=m.group().decode()
def w(o,n):
  for k in range(0,n*4,4):
    p=o+k;v=c.u32(p);a=''
    if p in c.ptrs:
      s=c.cstr(v);a='-> '+repr(s[:40]) if s and len(s)>1 and all(31<ord(x)<127 for x in s) else '->'
    elif v in names:a=names[v]
    else:
      f=c.f32(p)
      if v and 1e-3<abs(f)<1e5:a='%.4g'%f
    print('%05x %08x %s'%(p,v,a))
print('SYMS',[(hex(s),hex(t),hex(o),names.get(s),names.get(t)) for s,t,o in c.syms])
for a in sys.argv[1:]:
  o,n=a.split(':');print('==',o);w(int(o,16),int(n))
