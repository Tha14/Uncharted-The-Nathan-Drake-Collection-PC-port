from lib import *
# who else uses this effect; does the sibling render fine
q=Pak(OV+'air-multi.pak');by=collections.defaultdict(list)
sb=subs(q);use=collections.Counter(s['mat'].split(',')[0] for s in sb)
for m in mats(q):by[m['hash']].append(m['name'].split(',')[0].rsplit('/',1)[-1])
for h,v in by.items():
    if 'concrete' in ' '.join(v) or 'asphalt' in ' '.join(v):print(h,v)
