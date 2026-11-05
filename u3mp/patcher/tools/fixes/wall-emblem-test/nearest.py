import sys,struct
sys.path.insert(0,"{{PAKTOOLS}}");sys.dont_write_bytecode=True
from ndpak import Pak
p=Pak("{{PROJECT}}/work/overlay-r3/u3data/build/main/pak23/ps4/air-multi-ingame12.pak");raw=p.d
it=p.items_of('SPAWNER_GROUP')[0];arr=p.resolve(it.page,it.data_off+8);n=p.pu32(it.page,it.data_off+0x10)
T=(-8.979,0.136,-4.77);L=[]
for k in range(n):
    s=p.resolve(arr[0],arr[1]+8*k);sb=p.pbase(s[0])+s[1]
    nm=p.resolve(s[0],s[1]+0x20);nm=p.pstr(*nm) if nm else None
    art=p.resolve(s[0],s[1]+0x48);artn=p.pstr(*art) if art else None
    pos=struct.unpack_from('<3f',raw,sb);d=sum((a-b)**2 for a,b in zip(pos,T))**.5
    L.append((d,nm,artn,'%08x'%p.u32(sb+0x40),[round(v,2) for v in pos]))
L.sort()
for e in L[:25]:print(round(e[0],2),e[1:])
# count refs to the art string offset 0xdf980
import re
print('refs to artstr', [hex(m.start()) for m in re.finditer(struct.pack('<Q',0),b'')][:0])
