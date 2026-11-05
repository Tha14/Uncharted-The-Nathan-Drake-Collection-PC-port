from common import *
MED = 0x980d6543
def rows(x):
    s,t,o=[y for y in x.syms if y[0]==MED][0]
    n=x.u32(o); a=x.ptr(o+x.ps); es=0x48 if x.ps==8 else 0x40
    r=[]
    for i in range(n):
        e=a+i*es
        if x.ps==8: f=[x.u32(e+k) for k in (0,4,8,0xc,0x10,0x14)]+[x.cstr(x.ptr(e+0x18))]+[x.u32(e+k) for k in range(0x20,0x44,4)]+[x.u32(e+0x44)]
        else: f=[x.u32(e+k) for k in (0,4,8,0xc,0x10,0x14)]+[x.cstr(x.ptr(e+0x18))]+[x.u32(e+k) for k in range(0x1c,0x40,4)]+[None]
        r.append(f)
    return o,a,r
