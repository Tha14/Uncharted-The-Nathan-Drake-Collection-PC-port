import sys
sys.path.insert(0,'gh2/u3mp/paktools')
from dc import DC
for p in [r"{{PROJECT}}/work/overlay-r3/u3data/build/main/dc1/ps4/net.bin"]:
  c=DC(p);s=c.sym(0x75e3b9a9);print(s and (hex(s[0]),hex(s[1])))
  if s:
    o=s[1];n=c.u32(o);a=c.ptr(o+8);print(n,hex(a))
    for i in list(range(min(n,8)))+[n-1]:
      e=a+i*0x20;print(i,c.cstr(c.ptr(e)),c.u32(e+8),c.u32(e+12),c.u32(e+16),c.cstr(c.ptr(e+24)))
