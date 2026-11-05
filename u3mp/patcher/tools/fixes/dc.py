import sys,struct,re,os
sys.path.insert(0,"{{TRANSFER_TOOLS}}")
from psarc_lib import Psarc
PS3="{{PS3_DISC}}"
W="{{PROJECT}}/work/"
b=Psarc(PS3+'/build/main/bin.psarc');pa=Psarc(PS3+'/goty-patch.psarc')
def r3(n):
  for a in(pa,b):
    for x in a.names:
      if x.endswith('dc1/'+n):return a.read(a.names.index(x)+1)
def r4(n):
  for o in('overlay-r3','overlay-r2','overlay-port','overlay-mp'):
    f=W+o+'/u3data/build/main/dc1/ps4/'+n
    if os.path.exists(f):return open(f,'rb').read(),o
  return None,None
def syms3(d):
  n=struct.unpack_from('>I',d,0x14)[0];return {struct.unpack_from('>II',d,0x18+i*12) for i in range(n)}
def syms4(d):
  n=struct.unpack_from('<I',d,0x14)[0];return {struct.unpack_from('<II',d,0x18+i*16) for i in range(n)}
RET="{{PS4}}/u3data/build/main/dc1/ps4/"
for n in('net.bin','medals.bin','menu.bin','net-always-loaded.bin','weapon-gameplay.bin','player-settings.bin','weapon-art.bin','looks.bin'):
  a=r3(n);c,o=r4(n)
  if c is None and os.path.exists(RET+n):c=open(RET+n,'rb').read();o='retail'
  if not a or not c:print(n,'missing',bool(a),bool(c));continue
  A=syms3(a);C=syms4(c);ia={x for x,_ in A};ic={x for x,_ in C}
  print(n,o,'sym ps3',len(A),'ps4',len(C),'ps3-only',len(ia-ic),[hex(x) for x in sorted(ia-ic)[:25]])
  sa=set(re.findall(rb'[A-Za-z0-9_*-]{4,}',a));sc=set(re.findall(rb'[A-Za-z0-9_*-]{4,}',c))
  print('  strs ps3-only',len(sa-sc),[x.decode() for x in sorted(sa-sc)][:70])
m3=r3('menu.bin');m4=r4('menu.bin')[0]
P=lambda d:set(x.decode() for x in re.findall(rb'(?:PAGE|LIST|BUTTON)_[A-Z0-9_]+',d))
a,c=P(m3),P(m4);print('menu tokens',len(a),len(c),'PS3 only',sorted(a-c));print('PS4 only',sorted(c-a)[:60])
d=r3('net.bin');d4=r4('net.bin')[0]
for lab,x in(('ps3',d),('ps4',d4)):
  print(lab,'gametype-ish',sorted(set(s.decode() for s in re.findall(rb'[a-z-]*(?:elimination|plunder|deathmatch|kill-confirm|team-objective|free-for-all|hardcore|lab|three-team|buddy|turf|survival|arena|hunter|adventure|siege|gold-rush|chain|king)[a-z0-9-]*',x)))[:50])
