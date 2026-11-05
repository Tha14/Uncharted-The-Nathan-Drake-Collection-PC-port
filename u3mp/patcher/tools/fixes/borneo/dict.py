import sys,os,re,struct,pickle,glob
sys.path.insert(0,"{{TRANSFER_TOOLS}}")
sys.path.insert(0,sys.argv[1]+"/gh2/u3mp/paktools")
from psarc_lib import Psarc
from dc import sid
PS3="{{PS3_DISC}}"
R="{{PS4}}/u3data/build/main/"
out=sys.argv[1]+'/borneo/d/'
pak=Psarc(PS3+'/build/main/pak23.psarc');act=Psarc(PS3+'/build/main/actor33.psarc')
os.makedirs(out+'p3',exist_ok=True)
D={}
def add(b):
  for m in re.findall(rb'[A-Za-z0-9_\-\*\?\.:/ ]{3,}',b):
    s=m.decode();D.setdefault(sid(s),s)
for a,ns in((pak,[n for n in pak.names if re.search(r'(dig-coop|dig-adv|world-coop-adv-dig|sky-dig|coop-borneo)',n)]),(act,[n for n in act.names if re.search(r'(adv-dig-spawners|anim-coop-dig)',n)])):
  for n in ns:
    d=a.read(a.names.index(n)+1); open(out+'p3/'+os.path.basename(n),'wb').write(d); add(d)
# dc debug-info, retail scripts, executable strings
for f in glob.glob(R+'dc1/ps4/*.dci')+glob.glob(R+'dc1/ps4/ss-*.bin')+[R+'dc1/ps4/script-funcs.bin'] if True else []:
  try: add(open(f,'rb').read())
  except: pass
for f in glob.glob(sys.argv[1]+'/strs_*.txt'):
  add(open(f,'rb').read())
for w in open(sys.argv[1]+'/ps4k.json','rb').read().split(b'"')[:0]:pass
for f in glob.glob(out+'p3*.bin'): add(open(f,'rb').read())
pickle.dump(D,open(out+'dict.pkl','wb'));print(len(D))
