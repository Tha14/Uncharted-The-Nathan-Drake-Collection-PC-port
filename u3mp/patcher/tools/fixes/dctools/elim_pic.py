import sys,os,struct,shutil
sys.path.insert(0,'.')
from common import DC
from dcedit import DCEdit
R="{{PROJECT}}/work/"
D=R+"overlay-r3/u3data/build/main/dc1/ps4/";BK=R+"backup-elim-pic/";os.makedirs(BK,exist_ok=True)
for f in('net.bin','modules.bin'):
  if not os.path.exists(BK+f):shutil.copy2(D+f,BK+f)
b=DC("{{PS3_DATA2}}/net18.bin")
_c=DC(D+'net.bin');_t,_o=_c.bysid[0xf985c7b6];_e=_c.ptr(_o+8)+10*0x58
if any(_c.isptr(_e+k) and _c.cstr(_c.ptr(_e+k))=='net-playlists/gametype-elimination' for k in range(0,0x58,8)):
  print('already applied: elimination picture is set');sys.exit(0)
t,o=b.bysid[0xf985c7b6];n=b.u32(o);ar=b.ptr(o+4)
es=(b.ptr(o+4)+0) and None
for st in range(0x40,0x60,4):
  ok=[k for k in range(0,st,4) if b.isptr(ar+10*st+k) and (b.cstr(b.ptr(ar+10*st+k)) or '').startswith('net-playlists')]
  ok0=[k for k in range(0,st,4) if b.isptr(ar+k) and (b.cstr(b.ptr(ar+k)) or '').startswith('net-playlists')]
  if ok and ok==ok0:print('ps3 stride',hex(st),'field',[hex(k) for k in ok],b.cstr(b.ptr(ar+10*st+ok[0])))
a=DC(BK+'net.bin');t,o=a.bysid[0xf985c7b6];ar=a.ptr(o+8);e=ar+10*0x58
fields=[k for k in range(0,0x58,8) if a.isptr(e+k)];print('ps4 ptr fields',[hex(k) for k in fields],hex(a.u32(e)))
k=[x for x in fields if a.isptr(ar+x) and (a.cstr(a.ptr(ar+x)) or '').startswith('net-playlists')][0]
assert (a.cstr(a.ptr(e+k)) or '')==''
E=DCEdit(BK+'net.bin');s=E.add_string('net-playlists/gametype-elimination');E.putptr(e+k,s)
net=E.build();open(D+'net.bin','wb').write(net)
m=bytearray(open(BK+'modules.bin','rb').read());old=struct.unpack_from('<Q',m,192352)[0];assert old==os.path.getsize(BK+'net.bin')
struct.pack_into('<Q',m,192352,len(net));open(D+'modules.bin','wb').write(m)
c=DC(D+'net.bin');t,o=c.bysid[0xf985c7b6];print(c.cstr(c.ptr(c.ptr(o+8)+10*0x58+k)),len(net),struct.unpack_from('<Q',m,192352)[0])
