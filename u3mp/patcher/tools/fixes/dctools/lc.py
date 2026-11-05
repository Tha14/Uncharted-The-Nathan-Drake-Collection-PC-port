import sys,struct,re,os,glob,collections
sys.path.insert(0,'.')
from common import DC,sid
ROOT="{{PROJECT}}/work/"
D=ROOT+"overlay-r3/u3data/build/main/dc1/ps4/"
b=DC("{{PS3_DATA2}}/net18.bin");a=DC(D+"net.bin")
A=0x1755b0c7
b_all=open(D+'modules.bin','rb').read()
def show(s,d=0):
  if s in b.bysid:
    t,o=b.bysid[s]
    v=b.u32(o)
    print(' '*d,hex(s),'type',hex(t),'val',hex(v),'in ps4 net',s in a.bysid,(hex(a.bysid[s][0]),a.u32(a.bysid[s][1])) if s in a.bysid else '')
    if t==A and d<8:show(v,d+2)
    return
  print(' '*d,hex(s),'not in net18; ps4 net',s in a.bysid,'bytes',a.d.count(struct.pack('<I',s)))
for s in(0x3719ce57,0x4ea65a02,0x8cf8e1cc,0x74910f1,0xd009e7c5,0x1ad223b4):show(s)
ng=b.bysid[0x8cf8e1cc];print('list 8cf8e1cc ps3',[hex(b.u32(ng[1]+k)) for k in range(0,16,4)])
miss=[(s,t) for s,t,o in b.syms if s not in a.bysid];print('b-only syms',len(miss),collections.Counter(hex(t) for s,t in miss).most_common(8))
for s,t in miss:
  o=b.bysid[s][1];print(' ',hex(s),hex(t),[hex(b.u32(o+k)) for k in range(0,16,4)],b.isptr(o+4) and (b.cstr(b.ptr(o+4)) or '')[:24])
