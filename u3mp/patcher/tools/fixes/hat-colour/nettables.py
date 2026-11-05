import sys,os,json,re,collections,struct,bisect
sys.dont_write_bytecode=True
sys.path.insert(0,'{{PAKTOOLS}}')
from dclib import DC
P='{{PROJECT}}'
dc=DC(open(P+'/work/overlay-r3/u3data/build/main/dc1/ps4/net.bin','rb').read(),'net')
names=[n[:-4] for n in sorted(json.load(open('manifest.json')))]
syms=sorted((v,s,t) for s,t,v in dc.syms); starts=[v for v,_,_ in syms]
def symof(o):
    i=bisect.bisect_right(starts,o)-1; return syms[i] if i>=0 else None
rev=collections.defaultdict(list)
for p in dc.ptrs: rev[dc.ptr(p)].append(p)
print('symbol types',collections.Counter('%08x'%t for _,_,t in dc.syms).most_common(12))
# hero table
hv=dc.symmap[0xbd3bb3b9][1]; nh=dc.u32(hv); hrows=dc.ptr(hv+8)
hero_lists={}   # list sym value -> set of (hero idx, hero name, slot tag)
hero_name={}
for i in range(nh):
    r=hrows+0x70*i; hn=dc.cstr(dc.ptr(r)).decode() if dc.isptr(r) else '?'; hero_name[i]=hn
    if dc.isptr(r+0x18):
        l=dc.ptr(r+0x18)
        for k in range(dc.u32(l)):
            s=dc.u32(dc.ptr(l+8)+4*k); hero_lists.setdefault(s,set()).add((i,hn,'body%d'%k))
    for off in (0x20,0x28,0x30,0x38,0x40):
        if dc.isptr(r+off):
            q=dc.ptr(r+off)
            if dc.isptr(q+8): hero_lists.setdefault(dc.u32(dc.ptr(q+8)),set()).add((i,hn,hex(off)))
print('heroes',nh,'lists from heroes',len(hero_lists))
print('hero names',[hero_name[i] for i in range(nh)])
# who points at each 2a8027cf list symbol (other than heroes)?
lists={s:v for s,t,v in dc.syms if t==0x2a8027cf}
other=collections.Counter()
for s,v in lists.items():
    for p in rev.get(v,[]):
        so=symof(p); other['%08x/%08x'%(so[1],so[2]) if so else '?']+=1
print('pointers into part lists by symbol:',other.most_common(10))
# per part: hero rows using it, +0x2c colour, kind; entries in other tables
out={}
for n in names:
    b=n.encode(); offs=[m.start() for m in re.finditer(rb'(?<![\w-])'+re.escape(b)+rb'\0',dc.d)]
    heroes=set(); tabs=set(); col=set()
    for so in offs:
        for p in rev.get(so,[]):
            s=symof(p); t='%08x'%s[2]
            if t=='2a8027cf':
                e=p-8; col.add('%x'%dc.u32(e+0x2c))
                for h in hero_lists.get(s[1],()): heroes.add(h[1]+':'+h[2])
                if s[1] not in hero_lists: tabs.add('list-not-from-hero %08x'%s[1])
            else: tabs.add(t)
    out[n]=dict(heroes=sorted(heroes),col=sorted(col),other=sorted(tabs))
json.dump(out,open('part_use.json','w'),indent=0)
print('parts with no hero row:',[n for n,v in out.items() if not v['heroes']])
print('parts in list not from hero:',[n for n,v in out.items() if any(x.startswith('list-not') for x in v['other'])])
print('colour row zero:',[n for n,v in out.items() if v['col'] and v['col']==['0']])
