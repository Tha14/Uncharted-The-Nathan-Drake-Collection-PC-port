import sys,os,glob
sys.dont_write_bytecode=True
sys.path.insert(0,'gh2/u3mp/paktools')
from matsurvey import ps4_materials
from ndpak import Pak
R="{{PROJECT}}/work/overlay-r3/u3data/build/main/actor33/ps4/"
G="{{PS4}}/u3data/build/main/actor33/ps4/"
print(len(glob.glob(G+'*.pak')),[os.path.basename(x) for x in glob.glob(G+'*hat*')][:40])
for n in sys.argv[1:]:
  f=R+n+'.pak'
  if not os.path.exists(f):f=G+n+'.pak'
  if not os.path.exists(f):print(n,'NONE');continue
  print('==',n,'overlay' if f.startswith(R) else 'retail')
  q=Pak(open(f,'rb').read()) if 0 else Pak(f)
  for m in ps4_materials(q):
    fs=[x for x in m['feats'] if x and('TINT' in x or 'HAIR' in x)]
    ps=[p[0] for p in m['params'] if p[0] and ('int' in p[0].lower() or 'color' in p[0].lower())]
    print('  ',m['name'],m['shader'],fs,ps,[b[1] for b in m['binds'] if 'mask' in (b[1] or '').lower() or 'tint' in (b[0] or '').lower()])
