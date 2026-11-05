import json,collections,re
R=[json.loads(l) for l in open('rows.jsonl')]
print(len(R),len({r['pak'] for r in R}))
c=collections.Counter()
def nrm(s): return s and re.search(r'normal',s)
ex=collections.defaultdict(list)
for r in R:
    s=r['src']
    if s and nrm(s[1]) and s[2]=='z':
        k='Z:'+('shared' if r['shared'] else 'vc_only' if r['vc_only'] else 'flat' if any('flat' in x for x in r['flat']) else 'blendmap_real')
        c[k]+=1;ex[k].append(r)
    elif s: c['src:'+('blend' if re.search('blend|height',s[1] or '') else 'other')+(':vc_only' if r['vc_only'] else ':flat' if r['flat'] and r['blendmap'] else '')]+=1
    elif r['vc_only'] or r['blendmap']: c['nosrc'+(':vc_only' if r['vc_only'] else ':bm')]+=1
    d=[x for x in r['dropped'] if x[2] in ('color',)]
    if d: c['dropcolor'+(':uv2' if any('zw' in x[3] for x in d) else '')]+=1
    if r['spec3']>0 and not r['spec4']: c['speclost']+=1
    if r['has_env3'] and r['env3']>0 and not r['env4']: c['envlost']+=1
print(c)
for k,v in ex.items():
    print(k,[ (x['pak'][:12],x['mat'].split('/')[-1]) for x in v[:5]])
for x in R:
    if x['pak']=='air-multi.pak' and re.search('hazard|striped|-red:|concrete-multi',x['mat']): print(x['mat'].split('/')[-1],x['src'],[d[1:] for d in x['dropped']],x['spec3'],x['spec4'],x['env3'],x['env4'])
