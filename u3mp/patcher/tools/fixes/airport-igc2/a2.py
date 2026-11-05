import sys,os,glob,pickle,re
from lib import *
from psarc_lib import Psarc
from dc import DC,sid
D=dict(pickle.load(open('../live/coop-tmp/dict.pkl','rb')))
for l in LAY[:4]:
    d=l+'/u3data/build/main/dc1/ps4/render-settings/game'
    if os.path.isdir(d): print(l[-12:],sorted(os.listdir(d))[:80])
    d=l+'/u3data/build/main/dc1/ps4/render-settings'
    if os.path.isdir(d): print(l[-12:],sorted(os.listdir(d))[:30])
B=Psarc(PSA+'bin.psarc'); N=B.names
G4=G+'/u3data/build/main/'
Q=None
for a in glob.glob(G4+'*.psarc'):
    q=Psarc(a)
    if any('render-settings/game/airport.bin' in n for n in q.names): Q=q;print('ps4 in',a); 
print([n for n in Q.names if 'render-settings' in n and 'game/' not in n][:20])
for n in ['air','air-multi','airport','airport-const','coop','coop-all','yem-drugged-alleys']:
    for side,X in (('ps3',B),('ps4',Q)):
        c=[k for k in X.names if k.endswith('render-settings/game/%s.bin'%n)]
        if not c: print(side,n,'none');continue
        d=X.read(X.names.index(c[0])+1); open(f'dc/{side}-{n}.bin','wb').write(d)
        print(side,n,len(d),c[0])
