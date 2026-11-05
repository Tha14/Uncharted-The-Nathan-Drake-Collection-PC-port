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
for i in range(nh):
    r=hrows+0x70*i; hn=dc.cstr(dc.ptr(r)).decode()
    if not dc.isptr(r+0x18): continue
    l=dc.ptr(r+0x18); parts=[]
    dflt=[]
    for k in range(dc.u32(l)):              # one option list per body slot; entry 0 = default
        tv=dc.symmap[dc.u32(dc.ptr(l+8)+4*k)][1]
        for j in range(dc.u32(tv)):
            o=dc.ptr(tv+8)+0x38*j; nm=dc.cstr(dc.ptr(o+8)).decode() if dc.isptr(o+8) else ''
            parts.append((nm,dc.u32(o+0x18),dc.u32(o+0x2c)))
            if j==0: dflt.append(nm)
    rt=[(nm,'%x'%c) for nm,k,c in parts if tint(nm)]; rd=[nm for nm in dflt if tint(nm)]
    print('%-18s lists %d parts %2d retail-tinted %2d default-tinted %s'%(hn,dc.u32(l),len(parts),len(rt),rd[:3]))
