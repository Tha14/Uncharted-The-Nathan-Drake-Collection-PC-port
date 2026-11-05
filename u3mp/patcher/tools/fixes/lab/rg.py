import sys,re,os,struct,collections
sys.path.insert(0,"{{TRANSFER_TOOLS}}")
from psarc_lib import Psarc
U="{{PS3_UPDATE}}/"
W="{{PROJECT}}/work/overlay-r3/u3data/build/main/"
p=Psarc(U+"dlc-patch18.psarc");print([n for n in p.names if n.endswith(('pak23.txt','modules.bin','net.bin','modules.dci','menu.bin','banks.txt'))])
t=p.read(p.names.index('build/main/pak23.txt')+1).decode('latin-1')
B=re.split(r'(?m)^(?=level-name )',t);L=[b for b in B if b.startswith('level-name lab')]
print('ps3 lab blocks',len(L),[b.split('\n')[0][11:] for b in L][:50]);print(L[0][:360] if L else '')
c=open(W+'pak23_ps4.txt',encoding='latin-1').read();print('ps4 levels',c.count('level-name '),'lab in ps4',c.count('level-name lab'))
net=open(W+'dc1/ps4/net.bin','rb').read();print('overlay net',len(net),sorted(set(x.decode() for x in re.findall(rb'lab[a-z0-9-]{3,}',net)))[:30])
print('modules',os.path.exists(W+'dc1/ps4/modules.bin'),'ss-labs',os.path.exists(W+'dc1/ps4/ss-multiplayer/ss-labs.bin'))
d=open("{{PS3_DATA2}}/net18.bin",'rb').read();print('net18',d[:32].hex())
for k in (b'lab-trench-v1',b'lab-stadium-v1',b'lab-multi-03-pak'):
  i=d.find(k);print(k,i,d[max(0,i-90):i+70])
