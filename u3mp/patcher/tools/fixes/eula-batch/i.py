import json,bisect,sys
J=json.load(open('p119.json'))
e=sorted(J['ent']);C=J['calls']
def fn(v):
  i=bisect.bisect(e,v)-1;return e[i],e[i+1]
for a in sys.argv[1:]:
  v=int(a,16);l=C.get(hex(v),[]);print(a,'callers',[(hex(x),hex(fn(x)[0])) for x in l][:30])
