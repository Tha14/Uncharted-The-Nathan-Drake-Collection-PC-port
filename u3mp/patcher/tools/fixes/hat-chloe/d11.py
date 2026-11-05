import sys,os
sys.dont_write_bytecode=True
sys.path.insert(0,'gh2/u3mp/paktools')
from ndpak import Pak
R="{{PROJECT}}/work/overlay-r3/u3data/build/main/actor33/ps4/"
G="{{PS4}}/u3data/build/main/actor33/ps4/"
for n in sys.argv[1:]:
  f=R+n+'.pak' if os.path.exists(R+n+'.pak') else G+n+'.pak'
  q=Pak(f);print('==',n,f[:6])
  for it in q.items:
    print('  ',it.typ if hasattr(it,'typ') else getattr(it,'type',''),it.name)
