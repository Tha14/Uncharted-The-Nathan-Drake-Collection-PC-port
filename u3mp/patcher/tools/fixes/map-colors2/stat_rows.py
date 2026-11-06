import json,collections,re,gzip
R=[json.loads(l) for l in open('rows.jsonl')]
c=collections.Counter();pk=collections.Counter();Z=[]
for r in R:
    s=r['src']
    z=s and s[2]=='z'
    if z:
        nn=s[1] and 'normal' in s[1]
        k=('shared' if r['shared'] else 'vc_only' if r['vc_only'] else 'flat' if r['flat'] else 'real');c['Z%s:%s'%('' if nn else '?',k)]+=1
        if k in('vc_only','flat'): pk[r['pak']]+=1;Z.append(r)
    elif s and r['vc_only']: c['vc_only_srcmap']+=1
    elif s and r['flat'] and r['blendmap']: c['flat_srcmap']+=1
    elif not s and r['flat'] and r['blendmap']: c['flat_nosrc']+=1
    elif not s and r['vc_only']: c['vc_only_nosrc']+=1
print(len(R),c);print(pk.most_common(60));print(len(pk),len(set(r['mat'] for r in Z)))
print(collections.Counter(r['mat'].split('/')[-1] for r in Z if 'air' in r['pak'] or 'yem' in r['mat']).most_common(14))
D=[r for r in R if any(d[2]=='color' and 'spec' not in d[1] and not re.search('inc|env',d[1]) for d in r['dropped'])]
print('dropcolor',len(D),collections.Counter(r['pak'] for r in D).most_common(25))
print([(r['mat'].split('/')[-1],[d[1] for d in r['dropped'] if d[2]=='color']) for r in D if r['pak']=='air-multi.pak'])
