import sys,re,json
sys.dont_write_bytecode=True
sys.path.insert(0,'gh2/u3mp/paktools')
import psarc_lib
U="{{PS3_UPDATE}}/"
idx={}
for f in('dlc-patch13.psarc','dlc-patch18.psarc'):
  a=psarc_lib.Psarc(U+f)
  names=a.names() if hasattr(a,'names') else list(getattr(a,'files',{}))
  for n in names:
    if 'actor33' in n and n.endswith('.pak'):idx[n.split('/')[-1][:-4]]=(a,n)
print(len(idx))
rows=json.load(open('hat-chloe/hats.json'))
no=[r[0] for r in rows if r[1]!='ERR' and not r[3]]+['mp-chloe-navy-hat','mp-chloe-winter-hat','mp-kit-hunter-head-hat-d']
for n in no:
  if n not in idx:print(n,'no ps3');continue
  a,p=idx[n];d=a.read(p) if hasattr(a,'read') else a.extract(p)
  ss=sorted(set(m.group().decode().split('/')[-1] for m in re.finditer(rb'[\w\-/\.]*(?:tint-mask|-color)[\w\-\.]*',d)))
  print(n,ss[:6])
