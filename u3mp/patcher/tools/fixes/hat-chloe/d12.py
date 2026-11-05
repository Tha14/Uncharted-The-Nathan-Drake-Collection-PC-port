import sys,os,json
sys.dont_write_bytecode=True
sys.path.insert(0,'hat-chloe');sys.path.insert(0,'dctools');sys.path.insert(0,'gh2/u3mp/paktools')
import d4
from d4 import V,a,NONE
from matsurvey import ps4_materials
from ndpak import Pak
R="{{PROJECT}}/work/overlay-r3/u3data/build/main/actor33/ps4/"
G="{{PS4}}/u3data/build/main/actor33/ps4/"
A=V(a);seen={}
miss=set();allarts={}
for i in range(A.n):
  for si in range(6):
    sl=A.slot(i,si)
    if not sl or a.u32(sl-4)!=0x88e3fe54:continue
    cnt=a.u32(sl);q=a.ptr(sl+8)
    for j in range(cnt):
      for e in A.ents(a.u32(q+4*j)):
        if e['sid']==NONE or not e['art'] or e['art']=='none':continue
        allarts.setdefault(e['art'],[si,e['pal'],A.name(i)])
res={'ok':0,'ret':0}
rows=[]
for n,(si,pal,h) in sorted(allarts.items()):
  ov=os.path.exists(R+n+'.pak');ret=os.path.exists(G+n+'.pak')
  if not ov and not ret:miss.add((n,si,h));continue
  if si!=2 or not pal:continue
  q=Pak(R+n+'.pak' if ov else G+n+'.pak')
  try:ms=ps4_materials(q)
  except Exception as ex:rows.append((n,'ERR',str(ex)));continue
  tint=[]
  for m in ms:
    b=dict(m['binds']);fs=m['feats'] or []
    nm=(m['name'] or '').split(',')[0].split('/')[-1]
    if 'COLOR_1_APPLY_INSTANCE_TINT' in fs:
      tint.append((nm,(b.get('g_tNdFetchColor1Map') or '').split('/')[-1],(b.get('g_tNdFetchColorTintMask1Map') or '').split('/')[-1],'mask' if 'COLOR_1_APPLY_COLOR_TINT_WITH_MASK' in fs else 'nomask'))
  mats=[(m['name'] or '').split(',')[0].split('/')[-1] for m in ms if 'shadowblocker' not in (m['name'] or '')]
  rows.append((n,'ov' if ov else 'ret',mats,tint))
json.dump(rows,open('hat-chloe/hats.json','w'),indent=0)
print('missing paks',sorted(miss))
no=[r for r in rows if r[1]!='ERR' and not r[3]];bad=[]
for r in rows:
  if r[1]=='ERR':print(r);continue
  for t in r[3]:
    stem=t[1].replace('-color.tga','').replace('-color-00.tga','')
    if 'hair' in t[0] or 'hair' in t[1]:bad.append((r[0],r[1],t))
print('accessories w/ palette',len(rows),'no instance-tint material:',len(no))
for r in no:print('  NOTINT',r[0],r[1],r[2])
print('tint on hair:',len(bad))
for r in bad:print('  HAIRTINT',r)
print('--sample ok');[print(' ',r[0],r[1],r[3]) for r in rows if r[1]!='ERR' and r[3] and r[0] not in[x[0] for x in bad]][:0]
c=0
for r in rows:
  if r[1]!='ERR' and r[3] and r[0] not in [x[0] for x in bad]:
    c+=1
    if c<30:print(' ',r[0],r[1],r[3][:2])
print(c)
