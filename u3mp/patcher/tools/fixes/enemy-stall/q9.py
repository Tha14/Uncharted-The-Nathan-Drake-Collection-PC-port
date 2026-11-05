import sys,pickle,struct,glob
sys.path.insert(0,'{{PAKTOOLS}}')
from psarc_lib import Psarc
P='{{PROJECT}}'
D=dict(pickle.load(open(P+'/scratch/live/coop-tmp/dict.pkl','rb')))
G="{{PS3_DISC}}/"
pf=glob.glob(glob.escape(G)+'**/bin.psarc',recursive=True)[0];p=Psarc(pf)
for n in p.names:
  if 'coop-adv-syria' in n or 'coop-adv-airport' in n:print(n)
i=p.names.index('dc1/ss-coop-adv-syria.bin');open('p3-ss-coop-adv-syria.bin','wb').write(p.read(i+1))
def ctx(f,e):
  d=open(f,'rb').read();print('##',f.rsplit('/',1)[-1],len(d),d[:4])
  j=d.find(b'reg-turret1-door-guards-1');print(' str at',hex(j))
  s=struct.pack(e+'I',0x51916f0);k=-1
  while True:
    k=d.find(s,k+1)
    if k<0:break
    print(' sid at',hex(k))
    for a in range(k-0x40,k+0x60,4):
      v=struct.unpack_from(e+'I',d,a)[0];print('  %s%x: %08x %s'%('*' if a==k else ' ',a,v,D.get(v,'')))
ctx("{{PS4}}/u3data/build/main/dc1/ps4/ss-coop-adv-syria.bin",'<')
ctx('p3-ss-coop-adv-syria.bin','>')
