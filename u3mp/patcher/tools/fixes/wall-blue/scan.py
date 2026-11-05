"""Scan: every overlay map material whose tint/brightness differs from the PS3 material, and every saturated tint. Read-only."""
from ilib import *
import gzip,json
os.chdir(M2)
O=[json.loads(l) for l in gzip.open('ov.jsonl.gz','rt')]
R={}
for l in open('rows.jsonl'):
  r=json.loads(l);R[(r['pak'],r['mat'])]=r
tot=sat=0;mis=[];sats=collections.Counter();hs=[]
for x in O:
  for m in x.get('mats',[]) or []:
    k=m['n'].split(',')[0];r=R.get((x['pak'],k))
    if not r:continue
    P4={p[0]:p[2] for p in m['p']}
    tot+=1
    for nm,v in r['ps3p'].items():
      if not re.match(r'g_color(Tint|Brightness)',nm) or nm not in P4:continue
      n=len(v);a=P4[nm][:n]
      if any(abs(p-q)>2e-3 for p,q in zip(a,v)):mis.append((x['pak'],k,nm,[round(q,3) for q in v],[round(q,3) for q in a]))
      if n>=3 and 'Tint' in nm and min(v[:3])>0 and max(v[:3])/min(v[:3])>3 and v[2]>v[0]*2:
        sat+=1;sats[(k.split('/')[-1],nm,tuple(round(q,3) for q in v[:3]))]+=1
print('materials compared',tot,'tint/brightness mismatches overlay vs PS3',len(mis))
pk=collections.Counter(a[0] for a in mis);print(pk.most_common(12))
for a in mis[:25]:print(' ',a)
print('saturated blue tints (b>2r, max/min>3) identical to PS3: unique',len(sats))
for k,v in sats.most_common(40):print(' ',v,k)
