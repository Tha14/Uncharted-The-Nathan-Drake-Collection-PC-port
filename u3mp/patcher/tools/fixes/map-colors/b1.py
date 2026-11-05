from lib import *
q=Pak(OV+'air-multi.pak');sb=subs(q)
want=sys.argv[1:] or ['multi-ground-sand-2-asphalt:default','multi-ground-asphalt-broken:default','ground-sidewalk:default','concrete-multi:polished','ground-asphalt-broken-wide']
for w in want:
    tot=collections.defaultdict(list)
    for s in sb:
        if w in s['mat']:
            st=streams(q,s)
            for k,v in st.items():tot[k].append(v)
    print(w)
    for k,v in sorted(tot.items()):
        a=np.concatenate(v);print('  slot',k,a.shape,'min',a.min(0).round(3),'mean',a.mean(0).round(3),'max',a.max(0).round(3))
