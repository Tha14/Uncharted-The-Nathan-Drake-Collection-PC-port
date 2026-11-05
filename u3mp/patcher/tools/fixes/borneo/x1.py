import sys,os,re,struct,hashlib
sys.path.insert(0,"{{TRANSFER_TOOLS}}")
from psarc_lib import Psarc
PS3="{{PS3_DISC}}"
b=Psarc(PS3+'/build/main/bin.psarc');pa=Psarc(PS3+'/goty-patch.psarc')
R="{{PS4}}/u3data/build/main/dc1/ps4/"
W="{{PROJECT}}/work/"
out=sys.argv[1]
def rd(a,n):
  for x in a.names:
    if x.endswith('dc1/'+n): return a.read(a.names.index(x)+1)
names=['ss-coop-adv-%s.bin'%m for m in 'airport dig fort fort-b ldn mon mon-2 shambhala syria'.split()]+['part-adv-dig.bin','ss-multiplayer/ss-coop-arena-npcs.bin','ss-multiplayer/ss-npc-coop-general.bin']
for n in names:
  p=rd(pa,n);q=rd(b,n);r=open(R+n,'rb').read() if os.path.exists(R+n) else None
  ov=[o for o in('overlay-r3','overlay-r2','overlay-port','overlay-mp') if os.path.exists(W+o+'/u3data/build/main/dc1/ps4/'+n)]
  print(n,'patch',p and len(p),'base',q and len(q),'ps4 retail',r and len(r),'ov',ov)
  bn=n.replace('/','_')
  for t,d in(('p3p',p),('p3b',q),('r4',r)):
    if d:open(f'{out}/{t}-{bn}','wb').write(d)
  if ov: open(f'{out}/ov-{bn}','wb').write(open(W+ov[0]+'/u3data/build/main/dc1/ps4/'+n,'rb').read())
