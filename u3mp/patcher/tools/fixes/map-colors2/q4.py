import gzip,json,re
O=[json.loads(l) for l in gzip.open('ov.jsonl.gz','rt')]
for x in O:
    if x['pak']=='air-multi.pak':
        seen=set()
        for m in x['mats']:
            f=m['f']
            if any('APPLY_OVERLAY' in y for y in f) or 'BLEND_FACTOR_USE_SHARED_TEXTURE_1_B' in f:
                print(m['n'].split(',')[0].split('/')[-1],m['h'],[y for y in f if not re.match('AMBIENT|DIFFUSE_LIGHT|FINAL_COLOR_ADD_(AMB|DIFF)|GLOBAL_SUN|SPECULAR_LIGHT',y)],[(a[9:-3],b.split('/')[-1][10:34]) for a,b in m['b'] if 'LightMap' not in a],[a for a,_,_ in m['p']]);print()
