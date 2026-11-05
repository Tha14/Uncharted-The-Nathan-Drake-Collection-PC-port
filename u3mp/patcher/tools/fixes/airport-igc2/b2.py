from lib import *
from psarc_lib import Psarc
from dc import DC,sid
import pickle,struct,re,os
D=dict(pickle.load(open('../live/coop-tmp/dict.pkl','rb')))
nm=lambda v:D.get(v,hex(v))
B=Psarc(PSA+'bin.psarc')
os.makedirs('rsdc',exist_ok=True)
d=B.read(B.names.index('dc1/reticule-settings.bin')+1);open('rsdc/ps3-ret.bin','wb').write(d)
c3=DC('rsdc/ps3-ret.bin');c4=DC(G+'/u3data/build/main/dc1/ps4/reticule-settings.bin')
print('ps3',len(d),[(nm(s),nm(t)) for s,t,o in c3.syms])
print('ps4',os.path.getsize(c4.path),[(nm(s),nm(t)) for s,t,o in c4.syms])
for l in LAY[:4]:
    f=l+'/u3data/build/main/dc1/ps4/reticule-settings.bin'
    print(l[-10:],os.path.exists(f))
print(hex(sid('binoculars')), nm(sid('binoculars')))
print(open(G+'/u3data/build/main/dc1/ps4/reticule-settings.dci').read()[:500])
s=open('../live/coop-tmp/p3-ss-coop-adv-airport.bin','rb').read()
print('scr has sid',s.count(struct.pack('>I',sid('binoculars'))))
