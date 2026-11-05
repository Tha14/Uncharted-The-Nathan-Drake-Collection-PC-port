import sys,json
sys.path.insert(0,'.')
from common import DC
from verify_dc import check
base,new,p3,lp=sys.argv[1:]
log=json.load(open(lp))
check(base,new,[tuple(x) for x in log['patched_fields']])
A=DC(base);N=DC(new);B=DC(p3)
t,o=N.bysid[0x03ffae77]; n=N.u32(o); a=N.ptr(o+8)
t,o3=B.bysid[0x03ffae77]; a3=B.ptr(o3+4)
t,oA=A.bysid[0x03ffae77]; aA=A.ptr(oA+8)
assert n==79
for i in range(79):
    e=a+i*0x18; s=a3+i*0xc
    assert N.u32(e)==B.u32(s) and N.cstr(N.ptr(e+8))==B.cstr(B.ptr(s+4))
    if i<67: assert N.d[e:e+0x18]==A.d[aA+i*0x18:aA+i*0x18+0x18] and N.isptr(e+0x10)==A.isptr(aA+i*0x18+0x10)
    assert N.isptr(e+0x10)==B.isptr(s+8)
    if not B.isptr(s+8): continue
    d4=N.ptr(e+0x10); d3=B.ptr(s+8); c=N.u32(d4+4)
    assert N.u32(d4-4)==0x937276d5 and N.u32(d4)==B.u32(d3) and c==B.u32(d3+4)
    assert [N.u32(N.ptr(d4+8)+4*w) for w in range(3*c)]==[B.u32(B.ptr(d3+8)+4*w) for w in range(3*c)]
t,o=A.bysid[0xe2e8998e]; nA=A.u32(o); aA=A.ptr(o+8); nN=N.u32(o); aN=N.ptr(o+8)
ins=log['unlock_insert_after']; k=len(log['unlock_added'])
old=[A.d[aA+i*0x28:aA+i*0x28+0x28] for i in range(nA)]
newl=[N.d[aN+i*0x28:aN+i*0x28+0x28] for i in range(nN)]
assert newl[:ins+1]+newl[ins+1+k:]==old and nN==nA+k
print('emblems OK',n,nN)
