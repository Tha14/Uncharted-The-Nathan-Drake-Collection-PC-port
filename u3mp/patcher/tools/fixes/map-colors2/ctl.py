# false-positive control: retail exact twins (PS3 pak with same name + same material name in retail)
from lib import *
import gzip,pickle,p3prog,collections
from psarc_lib import Psarc
idx=pickle.load(open('p3idx.pkl','rb'))
R=[json.loads(l) for l in gzip.open('rt.jsonl.gz','rt')]
c=collections.Counter();arcs={};ex=[]
paks=[x for x in R if x.get('mats') and x['pak'] in idx and 'multi' not in x['pak']][:0]+[x for x in R if x.get('mats') and x['pak'] in idx and len(x['mats'])>30][:25]
for x in paks:
    a,i,n=idx[x['pak']][-1];A=arcs.setdefault(a,Psarc(a))
    try:F=p3prog.facts(Pak(A.read(i)),{})
    except Exception as e: continue
    for m in x['mats']:
        k=m['n'].split(',')[0];d,_,v=k.rpartition(':');g=F.get(d+'/'+v)
        if not g or not g['src'] or g['src'][0]!='tex': continue
        z=g['src'][2]=='z' and g.get('zproof')
        sh=any(f.startswith('BLEND_FACTOR_USE_SHARED') for f in m['f'])
        fb=[b[1].split('/')[-1] for b in m['b'] if 'BlendMap' in b[0]]
        c[(('z' if z else 'notz'),('shared' if sh else 'map'))]+=1
        if (z and not sh) and len(ex)<6: ex.append((k,fb,g['units'].get(g['src'][1])))
    print(x['pak'],dict(c),flush=True)
print(c,ex)
