import sys,os,json,re,collections,struct,bisect
sys.dont_write_bytecode=True
sys.path.insert(0,'{{PAKTOOLS}}')
from dclib import DC
P='{{PROJECT}}'
dc=DC(open(P+'/work/overlay-r3/u3data/build/main/dc1/ps4/net.bin','rb').read(),'net')
names=[n[:-4] for n in sorted(json.load(open('manifest.json')))]
syms=sorted((v,s,t) for s,t,v in dc.syms); starts=[v for v,_,_ in syms]
def symof(o):
    i=bisect.bisect_right(starts,o)-1
    return syms[i] if i>=0 else None
# every pointer -> its target; build reverse map target -> [ptr offsets]
rev=collections.defaultdict(list)
for p in dc.ptrs: rev[dc.ptr(p)].append(p)
res={}
for n in names:
    b=n.encode(); offs=[m.start() for m in re.finditer(rb'(?<![\w-])'+re.escape(b)+rb'\0',dc.d)]
    ents=[]
    for so in offs:
        for p in rev.get(so,[]):
            s=symof(p); ents.append(('%08x'%s[1],'%08x'%s[2],p-s[0]) if s else ('?', '?', p))
    res[n]=ents
c=collections.Counter(tuple(sorted({(e[0],e[1]) for e in v})) for v in res.values())
for k,v in c.most_common(): print(v,k)
json.dump(res,open('part_syms.json','w'),indent=0)
print('symbols',len(syms))
