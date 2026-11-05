import sys,os,struct
sys.path.insert(0,'.')
from common import DC,sid
ROOT="{{PROJECT}}/work/"
D=ROOT+"overlay-r3/u3data/build/main/dc1/ps4/";o=ROOT+'backup-lab/'
a=DC(D+'net.bin');k=DC(o+'net.bin')
for S,es in((0xdec97a4,0x98),(0x3b9a067d,0xd0),(0xc1b0be0d,0x68)):
  q=a.bysid[S][1];n=a.u32(q);ar=a.ptr(q+8);q0=k.bysid[S][1];n0=k.u32(q0);ar0=k.ptr(q0+8)
  same=all(a.d[ar+i*es:ar+i*es+es]==k.d[ar0+i*es:ar0+i*es+es] for i in range(n0));print(hex(S),n0,'->',n,'old preserved',same)
q=a.bysid[0xdec97a4][1];ar=a.ptr(q+8);n=a.u32(q)
for i in(n-14,n-1):
  e=ar+i*0x98;l=a.ptr(e+0x58);print(hex(a.u32(e)),a.cstr(a.ptr(e+0x10)),a.cstr(a.ptr(e+0x28)),a.cstr(a.ptr(e+0x80)),[a.cstr(a.ptr(a.ptr(l+8)+16*j)) for j in range(a.u32(l))])
c=DC(D+'net-always-loaded.bin');print(len(c.syms),[(hex(s),hex(t),[hex(c.u32(v+x)) for x in range(0,32,4)]) for s,t,v in c.syms if s in(0x3719ce57,0x4ea65a02)])
print('old nal syms ok',all(s in c.bysid for s,_,_ in DC(o+'net-always-loaded.bin').syms))
m=open(D+'modules.bin','rb').read();print(struct.unpack_from('<Q',m,192352),os.path.getsize(D+'net.bin'),struct.unpack_from('<Q',m,192312),os.path.getsize(D+'net-always-loaded.bin'))
t=open(ROOT+'overlay-r3/u3data/build/main/pak23_ps4.txt','rb').read();print('levels',t.count(b'level-name '),t.count(b'\r\n')==t.count(b'\n'),t[t.find(b'level-name lab-multi-03\r\n'):][:170])
