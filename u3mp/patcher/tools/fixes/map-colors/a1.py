from lib import *
q=Pak(OV+'air-multi.pak')
M=mats(q);print(len(M))
c=collections.Counter(m['shader'] for m in M);print(c.most_common(10))
sb=subs(q);print(len(sb))
use=collections.Counter()
for s in sb:use[s['mat'].split(',')[0]]+=s['ni']//3
print([k for k,v in use.most_common(40)])
for m in M:
    n=m['name'].split(',')[0]
    if re.search('(?i)asphalt|tarmac|ground|road|concrete|floor|dirt',n):
        print(n,use[n],m['shader'],m['feats'],[b for b in m['binds'] if 'Light' not in b[0]][:8],m['x3'])
