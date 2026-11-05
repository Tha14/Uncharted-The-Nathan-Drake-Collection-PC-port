import sys,os,glob,pickle,re,struct
from lib import *
from psarc_lib import Psarc
from dc import DC,sid
D=dict(pickle.load(open('../live/coop-tmp/dict.pkl','rb')))
nm=lambda v:D.get(v,hex(v))
B=Psarc(PSA+'bin.psarc'); N=B.names
os.makedirs('dc',exist_ok=True)
for n in N:
    if n.endswith('.bin') and 'render-settings/game/' in n and re.search(r'/(air|air-multi|airport|airport-const|coop|coop-all)\.bin',n):
        b=os.path.basename(n); d=B.read(N.index(n)+1); open('rsdc/ps3-'+b,'wb').write(d)
        g=G+'/u3data/build/main/dc1/ps4/render-settings/game/'+b
        print(b,len(d),'ps4',os.path.getsize(g))
        c=DC('rsdc/ps3-'+b)
        print('  ps3 syms',[(nm(s),nm(t)) for s,t,o in c.syms][:30])
        c4=DC(g)
        print('  ps4 syms',[(nm(s),nm(t)) for s,t,o in c4.syms][:30])
# search all ps3 dc for 'coop-dawn' / level-def
hits={}
pats=[b'coop-dawn',b'dawn',b'rs-coop',b'igc-intro',]
for i,n in enumerate(N):
    if not n.endswith('.bin'): continue
    d=B.read(i+1)
    for p in pats:
        if p in d: hits.setdefault(p,[]).append(n)
    if 'coop' in n or 'airport' in n: pass
for k,v in hits.items(): print(k,len(v),v[:30])
# sid search
S=[sid(x) for x in ['rs-coop-dawn','coop-dawn','rr-airport-rs-coop-dawn-igc-intro','render-settings/game/air','render-settings/game/airport','render-settings/game/coop']]
for i,n in enumerate(N):
    if not n.endswith('.bin'): continue
    d=B.read(i+1)
    for s,name in zip(S,['rs-coop-dawn','coop-dawn','rr..igc-intro','rs air','rs airport','rs coop']):
        if struct.pack('>I',s) in d: print(name,n)
