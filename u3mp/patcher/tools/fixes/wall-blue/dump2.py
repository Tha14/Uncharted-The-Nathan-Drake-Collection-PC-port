import sys,os,re,json,gzip
M2=os.path.abspath('../map-colors2');sys.path.insert(0,M2);os.chdir(M2)
from lib import *
import p3prog,fp,ps3params
p=Pak('p3/air-multi.pak');F=p3prog.facts(p,{})
print(len(F));want=sys.argv[1:] or ['corrugated-main-blue/','corrugated-main/','corrugated-main-blue-striped','corrugated-main-orange/']
for k,g in F.items():
  if any(w in k for w in want):
    print('==',k,'pid',hex(g['pid']));print(' units',g['units']);print(' src',g['src'])
    print(' params',{a:[round(x,3) for x in b] for a,b in g['params'].items() if re.search('olor|lend',a)})
for w in ['corrugated-main-blue/']:
  for k,g in F.items():
    if w in k:
      import ps3fp
      n,res=ps3fp.material_programs(p,k)
      code=[r for r in res if r[2]==g['pid']][0][4]
      print('\n'.join(fp.dis(code)))
R=[json.loads(l) for l in gzip.open('rt.jsonl.gz','rt')]
for x in R:
  for m in x.get('mats',[]):
    if re.search('corrugated-main-blue:|corrugated-main:default|hangar-blue:',m['n']):
      print('RT',x['pak'],m['n'].split(',')[0],m['h'],[f for f in m['f'] if re.match('COLOR|BLEND',f)]);print('  ',[(b[0].replace('g_tNdFetch',''),b[1].split('/')[-1]) for b in m['b']]);print('  ',[(q[0],[round(v,3) for v in q[2]][:3]) for q in m['p'] if 'olor' in q[0]] if m.get('p') and len(m['p'][0])>2 else m.get('p'))
