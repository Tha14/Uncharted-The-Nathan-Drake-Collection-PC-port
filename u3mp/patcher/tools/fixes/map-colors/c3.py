from lib import *
q=Pak(OV+'air-multi.pak');M={m['name'].split(',')[0].rsplit('/',1)[-1]:m for m in mats(q)}
a=set(M['air-yem-ground-concrete-multi:polished-dust']['feats']);b=set(M['air-yem-multi-ground-asphalt-broken:default']['feats'])
print('only concrete-multi',sorted(a-b));print('only asphalt',sorted(b-a));print(len(a),len(b));print(sorted(a))
