import sys,struct,re
sys.dont_write_bytecode=True
sys.path.insert(0,'gh2/u3mp/paktools')
R="{{PROJECT}}/work/overlay-r3/u3data/build/main/actor33/ps4/"
G="{{PS4}}/u3data/build/main/actor33/ps4/"
import zlib,os
for n,p in(('mp-chloe-navy-hat',R),('mp-chloe-u2-torso',G),('mp-chloe-wings',G),('mp-hero-hat-a',G),('mp-chloe-u2-hair',G)):
  f=p+n+'.pak'
  if not os.path.exists(f):print(n,'MISSING');continue
  d=open(f,'rb').read()
  ss=set(m.group().decode() for m in re.finditer(rb'[A-Za-z_][A-Za-z0-9_\-]{5,}',d) if b'COLOR' in m.group() or b'TINT' in m.group() or b'tint' in m.group() or b'Tint' in m.group() or b'hair' in m.group().lower() and b'mp-' not in m.group())
  print(n,len(d),sorted(ss)[:12])
