from lib import *
import gzip,pickle,time,p3prog
from psarc_lib import Psarc
idx=pickle.load(open('p3idx.pkl','rb'))
O=[json.loads(l) for l in gzip.open('ov.jsonl.gz','rt')]
arcs={}
Z=re.compile(r'missingtexture|indirect|lightmap',re.I)
ROLE=lambda s:p3prog.tex_role_final('x/'+s)
f=open('rows.jsonl','w');t0=time.time()
for x in O:
    if 'mats' not in x or not x['mats'] or x['pak'] not in idx: continue
    a,i,n=idx[x['pak']][-1]
    A=arcs.setdefault(a,Psarc(a))
    try: p=Pak(A.read(i)); F=p3prog.facts(p,{})
    except Exception as e: print('ERR',x['pak'],repr(e)[:100],flush=True); continue
    for m in x['mats']:
        key=m['n'].split(',')[0]
        d,_,v=key.rpartition(':'); pn=d+'/'+v
        g=F.get(pn)
        if not g or m['s']!='prime-shader.fx': continue
        bn={b[1].split('/')[-1] for b in m['b']}
        fl=[b[1].split('/')[-1] for b in m['b'] if 'generated/u3mp/flat' in b[1]]
        un={int(u):s for u,s in g['units'].items()}
        src=None
        if g['src'] and g['src'][0]=='tex':
            s=un.get(g['src'][1]); src=[g['src'][1],s,g['src'][2],g['src'][3]]
        dropped=[]
        for u,s in un.items():
            if Z.search(s) or s in bn: continue
            use=(g['use'].get(u) or [['','']])[0]
            dropped.append([u,s,ROLE(s),use[0],use[1]])
        fs=m['f']
        f.write(json.dumps(dict(pak=x['pak'],mat=key,h=m['h'],vc_only='BLEND_FACTOR_USE_VERTEX_COLOR_ONLY' in fs,blendmap='BLEND_FACTOR_USE_BLEND_MAP_AND_VERTEX_COLOR' in fs,
          shared='BLEND_FACTOR_USE_SHARED_TEXTURE_1_B' in fs,flat=fl,src=src,nrm=[s for s in un.values() if re.search('normal',s)],zproof=g.get('zproof'),ps3p=g['params'],dropped=dropped,
          spec4='FINAL_COLOR_ADD_SPECULAR_LIGHT' in fs,env4=any('ADD_ENVIRONMENT' in y for y in fs),
          spec3=g['params'].get('g_specularBrightness',[0])[0],env3=g['params'].get('g_specularEnvironmentStrength',[0])[0],
          has_env3=any('env-reflection' in s or s.endswith('.dds') for s in un.values())))+'\n');f.flush()
    print(x['pak'],int(time.time()-t0),flush=True)
print('DONE')
