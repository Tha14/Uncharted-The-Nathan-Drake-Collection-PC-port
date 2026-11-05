import sys,os,struct,zlib
sys.dont_write_bytecode=True
sys.path.insert(0,'dctools');sys.path.insert(0,'gh2/u3mp/paktools')
from common import sid
R="{{PROJECT}}/work/overlay-r3/u3data/build/main/actor33/ps4/"
G="{{PS4}}/u3data/build/main/actor33/ps4/"
gl=os.listdir(G);rl=os.listdir(R)
print(len(gl),len(rl),[x for x in gl if 'hat' in x or 'chloe' in x][:60])
for n in sys.argv[1:]:
  f=R+n+'.pak' if n+'.pak' in rl else G+n+'.pak'
  if not os.path.exists(f):print(n,'NONE');continue
  d=open(f,'rb').read();s=sid(n)
  hits=[i for i in range(0,len(d)-3) if d[i:i+4]==struct.pack('<I',s)]
  print(n,'ov' if f.startswith(R) else 'ret','sid %08x'%s,'hits',len(hits),[hex(h) for h in hits[:6]],'names:',d.count(n.encode()))
