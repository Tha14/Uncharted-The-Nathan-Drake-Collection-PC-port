import sys,struct
sys.dont_write_bytecode=True
sys.path.insert(0,'dctools')
from common import DC,sid
from heroes_common import hero_rows
R="{{PROJECT}}/work/overlay-r3/u3data/build/main/dc1/ps4/net.bin"
a=DC(R);b=DC('_ps3net.bin')
PT=0x2a8027cf
def ents(c,ps):
  if ps not in c.bysid:return None
  t,o=c.bysid[ps];n=c.u32(o);q=c.ptr(o+c.ps)
  es=0x38 if c.ps==8 else 0x2c;art=8 if c.ps==8 else 4
  r=[]
  for j in range(n):
    e=q+j*es
    fs=[c.u32(e+k) for k in ((0x10,0x18,0x1c,0x20,0x24,0x28,0x2c,0x30,0x34) if c.ps==8 else range(8,0x2c,4))]
    r.append((c.cstr(c.ptr(e+art)),fs))
  return r
want=sys.argv[1:]
for c,tag in((a,'PS4'),(b,'PS3')):
  for r in hero_rows(c):
    if r['name'] not in want:continue
    print('==',tag,r['i'],r['name'])
    for si,s in enumerate(r['slots']):
      if not isinstance(s,dict):print(' slot',si,s);continue
      for p in s['parts']:
        print(' slot',si,'list %08x'%p)
        for j,x in enumerate(ents(c,p) or []):
          print('    %d %-44s'%(j,x[0]),' '.join('%x'%v for v in x[1]))
