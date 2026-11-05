import sys,os,json,collections
sys.dont_write_bytecode=True
sys.path.insert(0,'{{PAKTOOLS}}')
from dclib import DC
P='{{PROJECT}}'
G='{{PS4}}/u3data/build/main/actor33/ps4/'
dc=DC(open(P+'/work/overlay-r3/u3data/build/main/dc1/ps4/net.bin','rb').read(),'net')
hv=dc.symmap[0xbd3bb3b9][1]; nh=dc.u32(hv); hrows=dc.ptr(hv+8)
cache={}
def tint(n):
    if n not in cache:
        f=G+n+'.pak'; cache[n]=(open(f,'rb').read().count(b'COLOR_1_APPLY_INSTANCE_TINT')>0) if os.path.exists(f) else None
    return cache[n]
tot=collections.Counter()
for i in range(nh):
    r=hrows+0x70*i; hn=dc.cstr(dc.ptr(r)).decode(); row=[]
    for off in (0x20,0x28,0x30,0x38,0x40):
        if not dc.isptr(r+off): row.append('-'); continue
        q=dc.ptr(r+off)
        if not dc.isptr(q+8): row.append('-'); continue
        tv=dc.symmap[dc.u32(dc.ptr(q+8))][1]; n=dc.u32(tv); ents=[]
        for j in range(n):
            o=dc.ptr(tv+8)+0x38*j; nm=dc.cstr(dc.ptr(o+8)).decode() if dc.isptr(o+8) else ''
            ents.append((nm,dc.u32(o+0x18),dc.u32(o+0x2c),tint(nm)))
        d=ents[0] if ents else None
        nt=sum(1 for e in ents if e[3]); tot['entries']+=n; tot['retail-tinted']+=nt
        if d and d[3]: tot['default-retail-tinted']+=1
        row.append('%d/%d%s'%(nt,n,' D='+d[0]+':%x'%d[2] if d and d[3] else ''))
    print('%-18s'%hn,' | '.join(row))
print(tot)
