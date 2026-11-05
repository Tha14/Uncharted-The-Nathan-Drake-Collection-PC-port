import sys,os,re,struct,json,shutil
sys.path.insert(0,'.')
from common import DC,sid
from dcedit import DCEdit
import dcsyms
sys.path.insert(0,os.environ['PKT'])
from psarc_lib import Psarc
ROOT="{{PROJECT}}/work/"
MAIN=ROOT+"overlay-r3/u3data/build/main/";D=MAIN+"dc1/ps4/";BK=ROOT+"backup-lab/"
os.makedirs(BK,exist_ok=True)
for f in("dc1/ps4/net.bin","dc1/ps4/net-always-loaded.bin","dc1/ps4/modules.bin","pak23_ps4.txt"):
  t=BK+os.path.basename(f)
  if not os.path.exists(t):shutil.copy2(MAIN+f,t)
NET18="{{PS3_DATA2}}/net18.bin"
b=DC(NET18)
# idempotent (recipe 1.1): Lab already registered in the current net.bin -> nothing to do
_c=DC(D+"net.bin");_o=_c.bysid[0xdec97a4][1];_ids={_c.u32(_c.ptr(_o+8)+i*0x98) for i in range(_c.u32(_o))}
_o3=b.bysid[0xdec97a4][1]
if b.u32(b.ptr(_o3+4)+15*0x58) in _ids:print('already applied: Lab maps are in net.bin');sys.exit(0)
a=DC(BK+"net.bin");e=DCEdit(BK+"net.bin")
MAPS,NG,PL=0xdec97a4,0x3b9a067d,0xc1b0be0d;LIST_T=0xd14c6ffc;NG_OBJ=0xf0670de8;NG_SUB=0x8b617b51
strs={}
def string(s):
  if s in strs:return strs[s]
  enc=s.encode('latin1')+b'\0';d=a.d;pos=d.find(enc)
  while 0<=pos<a.reloc:
    if pos==0 or d[pos-1]==0:break
    pos=d.find(enc,pos+1)
  if pos<0 or pos>=a.reloc:pos=e.add_string(s)
  strs[s]=pos;return pos
def obj(t,size):
  e.align(8);e.d+=struct.pack('<II',0,t);o=len(e.d);e.d+=bytes(size);return o
def obj4(t,w):
  e.align(4);e.d+=struct.pack('<I',t);o=len(e.d);e.d+=struct.pack('<%dI'%len(w),*w);return o
def table(c,S):
  o=c.bysid[S][1];return o,c.u32(o),c.ptr(o+c.ps)
DROP={'cargo-plane-static'}
LABMAPS=[15,16,17,18,19,20,21,22,23,24,25,26,27,82]
READY={15,16,17,18,20,21,22,23,24,25,27}     # 19, 26, 82 need the lt-box / mixer-map / ballroom pieces first
PAKS=set(os.listdir(MAIN+'pak23/ps4'))
o4,n4,ar4=table(a,MAPS);o3,n3,ar3=table(b,MAPS)
ids4={a.u32(ar4+i*0x98) for i in range(n4)};na=e.alloc(0x98*(n4+len(LABMAPS)),16);e.copy_words(ar4,na,0x98*n4)
log=dict(maps=[],netgames=[]);levels=set(['lab-stage']);mains={}
for j,i3 in enumerate(LABMAPS):
  src=ar3+i3*0x58;mid=b.u32(src);assert mid not in ids4
  l3=b.ptr(src+0x30);assert b.u32(l3-4)==LIST_T;ln=b.u32(l3);li=b.ptr(l3+4)
  sub=[b.cstr(b.ptr(li+8*k)) for k in range(ln)];sub=[s for s in sub if s not in DROP]
  main=b.cstr(b.ptr(src+0x18));g=main[:-4]
  if main.startswith('lab-multi') and g not in sub and g+'.pak' in PAKS:
    sub=[g,main,'sky-village-multi','multi-all']+[s for s in sub if s not in('sky-village-multi','multi-all',main)]
  items=e.alloc(16*len(sub),8)
  for k,s in enumerate(sub):
    e.putptr(items+16*k,string(s));struct.pack_into('<I',e.d,items+16*k+8,sid(s))
  lst=obj(LIST_T,16);struct.pack_into('<I',e.d,lst,len(sub));e.putptr(lst+8,items)
  m=na+0x98*(n4+j)
  def u(k3,k4):struct.pack_into('<I',e.d,m+k4,b.u32(src+k3))
  def p(k3,k4):
    if b.isptr(src+k3):e.putptr(m+k4,string(b.cstr(b.ptr(src+k3))))
    else:assert b.u32(src+k3)==0
  u(0,0);u(4,4);u(8,8);p(0xc,0x10);p(0x10,0x18);p(0x18,0x28);u(0x1c,0x30);p(0x20,0x38);u(0x24,0x40);p(0x28,0x48);u(0x2c,0x50)
  e.putptr(m+0x58,lst);u(0x38,0x68);u(0x3c,0x6c);p(0x40,0x70);u(0x44,0x78);p(0x48,0x80);u(0x4c,0x88);u(0x50,0x90);u(0x54,0x94)
  mains[i3]=(mid,main);levels|=set(s for s in sub if s.startswith('lab-'));levels.add(main)
  log['maps'].append(('%08x'%mid,main,b.cstr(b.ptr(src+0xc)),sub))
e.put32(o4,n4+len(LABMAPS));e.putptr(o4+8,na)
# netgames
o4,n4,ar4=table(a,NG);o3,n3,ar3=table(b,NG);idx3={b.u32(ar3+i*0xe4):i for i in range(n3)}
PS3NG=[0x0d18cb6f,0x2653000e,0x73bddaa6,0x2b1026d7,0x22921db9,0x3cd54dbc]
CL=[(sid('lab-custom-'+mains[i][1]),mains[i][0],0xcaffc253) for i in sorted(READY) if i!=18]
ALL=[(s,None,s) for s in PS3NG]+CL
na=e.alloc(0xd0*(n4+len(ALL)),16);e.copy_words(ar4,na,0xd0*n4)
for j,(s,mp,srcs) in enumerate(ALL):
  src=ar3+idx3[srcs]*0xe4;g=na+0xd0*(n4+j)
  for k3 in range(0,0xe4,4):
    if b.isptr(src+k3) and k3!=0x74:raise SystemExit('ptr %x'%k3)
  for k in range(0,0x48,4):struct.pack_into('<I',e.d,g+k,b.u32(src+k))
  struct.pack_into('<I',e.d,g,s)
  if mp:struct.pack_into('<I',e.d,g+4,mp)
  for k3,k4 in((0x50,0x58),(0x54,0x5c),(0x58,0x60),(0x5c,0x64),(0x60,0x68),(0x64,0x6c),(0x78,0x78),(0x7c,0x7c))+tuple((k,k+4) for k in range(0x84,0xbc,4)):
    struct.pack_into('<I',e.d,g+k4,b.u32(src+k3))
  dropped={hex(k):b.u32(src+k) for k in(0x6c,0x70)+tuple(range(0xc4,0xe4,4)) if b.u32(src+k)};assert not dropped,dropped
  t3=b.ptr(src+0x74);assert b.u32(t3-4)==NG_OBJ;subs=[]
  for k in range(3):
    r=b.ptr(t3+4*k);assert b.u32(r-4)==NG_SUB;subs.append(obj4(NG_SUB,[b.u32(r+4*w) for w in range(9)]))
  t4=obj(NG_OBJ,24)
  for k in range(3):e.putptr(t4+8*k,subs[k])
  e.putptr(g+0x70,t4);log['netgames'].append('%08x'%s)
e.put32(o4,n4+len(ALL));e.putptr(o4+8,na)
# playlist: Lab, cloned from the first PS4 playlist layout
o4,n4,ar4=table(a,PL);o3,n3,ar3=table(b,PL);src=[ar3+i*0x5c for i in range(n3) if b.u32(ar3+i*0x5c)==0x5242014b][0]
na=e.alloc(0x68*(n4+1),16);e.copy_words(ar4,na,0x68*n4);m=na+0x68*n4;e.copy_words(ar4,m,0x68)
for k in(0,4,8,0xc):struct.pack_into('<I',e.d,m+k,b.u32(src+k))
e.putptr(m+0x10,string(b.cstr(b.ptr(src+0x10))))
for k3,k4 in((0x14,0x18),(0x18,0x1c),(0x1c,0x20),(0x20,0x24),(0x24,0x28),(0x28,0x2c)):struct.pack_into('<I',e.d,m+k4,b.u32(src+k3))
pl=[(0x0d18cb6f,5),(0x2653000e,5)]+[(s,10) for s,_,_ in CL]
it=e.alloc(8*len(pl),8)
for k,(s,w) in enumerate(pl):struct.pack_into('<II',e.d,it+8*k,s,w)
ll=obj(LIST_T,16);struct.pack_into('<I',e.d,ll,len(pl));e.putptr(ll+8,it);e.putptr(m+0x30,ll)
e.put32(o4,n4+1);e.putptr(o4+8,na)
net=e.build();open(D+'net.bin','wb').write(net)
# loadouts
e2=DCEdit(BK+'net-always-loaded.bin');T=0x1755b0c7;new=[]
for s in(0x3719ce57,):
  o=b.bysid[s][1];w=[b.u32(o+k) for k in range(0,24,4)]
  e2.align(8);e2.d+=struct.pack('<II',0,T);v=len(e2.d);e2.d+=struct.pack('<8I',w[0],w[1],0,w[2],w[3],0,w[4],w[5]);new.append((s,T,v))
nal,_,_=dcsyms.add_symbols(e2.build(),new);open(D+'net-always-loaded.bin','wb').write(nal)
# modules sizes
m=bytearray(open(BK+'modules.bin','rb').read())
for off,old,newv in((192352,699697,len(net)),(192312,34487,len(nal))):
  assert struct.unpack_from('<Q',m,off)[0]==old;struct.pack_into('<Q',m,off,newv)
open(D+'modules.bin','wb').write(m)
# level list
p=Psarc(os.environ.get('P18') or "{{PS3_UPDATE}}/dlc-patch18.psarc");t3=p.read(p.names.index('build/main/pak23.txt')+1).decode('latin1').replace('\r','')
B={re.search(r'^level-id (\S+)',x,re.M).group(1):x.rstrip('\n').split('\n') for x in re.split(r'\n(?=level-name )',t3.rstrip('\n'))}
raw=open(BK+'pak23_ps4.txt','rb').read().decode('latin1');assert raw.count('\r\n')==raw.count('\n')
TS=['texturedict1/chunk-41-tstrm','texturedict1/chunk-41-tstrm-1','texturedict1/chunk-42-tstrm','texturedict1/chunk-43-tstrm','texturedict1/common-tstrm','texturedict1/common-tstrm-1','texturedict1/common-tstrm-2','texturedict1/common-tstrm-3','texturedict1/common-tstrm-4']
md=bytes(m);add=[];drops={};nodef=[]
for lv in sorted(levels):
  if lv not in B:nodef.append(lv);continue
  blk=[]
  for x in B[lv]:
    ty,_,nm=x.partition(' ')
    if ty=='symbol' and struct.pack('<I',sid(nm.strip())) not in md:drops.setdefault(lv,[]).append(nm.strip());continue
    blk.append(x.rstrip())
  add.append('\r\n'.join(blk+['package-ts '+s for s in TS]))
assert not any(('level-name %s\r\n'%lv) in raw for lv in levels)
sep='' if raw.endswith('\r\n') else '\r\n'
open(MAIN+'pak23_ps4.txt','wb').write((raw+sep+'\r\n'.join(add)+'\r\n').encode('latin1'))
log['levels']=sorted(levels);log['dropped']=drops;log['nodef']=nodef;log['sizes']=dict(net=len(net),nal=len(nal))
json.dump(log,open(BK+'lab_reg.log.json','w'),indent=1)
print('net',len(net),'nal',len(nal),'levels',len(levels),'added',len(add),'nodef',nodef,'dropped',drops,'netgames',len(ALL))
