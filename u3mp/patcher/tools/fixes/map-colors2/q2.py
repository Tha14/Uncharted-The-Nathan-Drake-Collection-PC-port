import gzip,json,collections,re
R=[json.loads(l) for l in gzip.open('rt.jsonl.gz','rt')]
n=0;S=collections.Counter()
for x in R:
    for m in x.get('mats',[]):
        if 'BLEND_FACTOR_USE_SHARED_TEXTURE_1_B' in m['f']:
            k=tuple(sorted(f for f in m['f'] if not re.match('AMBIENT|DIFFUSE_LIGHT|SPECULAR_LIGHT|FINAL_COLOR_ADD|GLOBAL_SUN',f)))
            S[k]+=1
            if n<3:
                print(x['pak'],m['n'].split(',')[0],m['h']);print(' ',[(a[9:-3],b.split('/')[-1]) for a,b in m['b']]);n+=1
print(len(S))
for k,c in S.most_common(6): print(c,k)
O=collections.Counter()
for x in R:
    for m in x.get('mats',[]):
        f=m['f']
        if any(re.match(r'COLOR_[123]_APPLY_OVERLAY',y) for y in f) and 'COLOR_1_BLEND_COLOR_2' in f and 'COLOR_3_APPLY_OVERLAY_TEXTURE' not in f:
            O[tuple(y for y in f if 'OVERLAY' in y)]+=1
print(O.most_common(5))
