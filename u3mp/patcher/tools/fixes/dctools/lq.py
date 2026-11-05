import sys,struct,os,glob
sys.path.insert(0,'.')
from common import DC,sid
ROOT="{{PROJECT}}/work/"
D=ROOT+"overlay-r3/u3data/build/main/dc1/ps4/"
b=DC("{{PS3_DATA2}}/net18.bin")
for f in('net-always-loaded.bin','net.bin','modules.bin'):
  ok=[(o,os.path.exists(ROOT+o+"/u3data/build/main/dc1/ps4/"+f)) for o in('overlay-r3','overlay-r2','overlay-port','overlay-mp')];print(f,[x[0] for x in ok if x[1]])
c=DC(D+'net-always-loaded.bin');print('nal',len(c.d),len(c.syms))
for s in(0x8cf8e1cc,0x9dbd7b9d,0x5915ddac):
  if s in c.bysid:t,o=c.bysid[s];print(hex(s),'type',hex(t),[hex(c.u32(o+k)) for k in range(0,0x40,4)],'ptrs',[hex(k) for k in range(0,0x40,8) if c.isptr(o+k)]);
for s in(0x8cf8e1cc,0x3719ce57,0x4ea65a02,0x5915ddac):
  t,o=b.bysid[s];print('ps3',hex(s),hex(t),[hex(b.u32(o+k)) for k in range(0,0x40,4)],'ptr',[hex(k) for k in range(0,0x40,4) if b.isptr(o+k)]);
print('type obj size: next sym gaps',sorted(set(b.syms[i+1][2]-b.syms[i][2] for i in range(len(b.syms)-1) if b.syms[i][1]==0x1755b0c7))[:5])
m=open(D+'modules.bin','rb').read()
for nm,fn in(('net','net.bin'),('net-always-loaded','net-always-loaded.bin')):
  sz=os.path.getsize(D+fn);ks=[i for i in range(0,len(m)-8,4) if struct.unpack_from('<Q',m,i)[0]==sz];print(nm,hex(sid(nm)),sz,'size slots',ks,[m[i-8:i+8].hex() for i in ks][:3])
