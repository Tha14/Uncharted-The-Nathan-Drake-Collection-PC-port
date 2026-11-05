import sys,os
sys.dont_write_bytecode=True
sys.path.insert(0,'gh2/u3mp/paktools')
from matsurvey import ps4_materials
from ndpak import Pak
R="{{PROJECT}}/work/overlay-r3/u3data/build/main/actor33/ps4/"
for n in sys.argv[1:]:
  q=Pak(R+n+'.pak')
  print('==',n)
  for m in ps4_materials(q):
    print(m['name'].split(',')[0]);print('  F',m['feats']);print('  B',[(a,(b or '').split('/')[-1]) for a,b in m['binds']])
