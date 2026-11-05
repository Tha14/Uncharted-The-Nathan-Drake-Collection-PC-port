import sys,pickle
sys.path.insert(0,'{{PAKTOOLS}}')
from dc import DC,sid
P=sys.argv[1]
D=dict(pickle.load(open(P+'/scratch/live/coop-tmp/dict.pkl','rb')))
L={0xd3925240:'air',0x8a746a66:'dig',0x52592c11:'dig2',0x1548154e:'ldn',0xae270117:'mon',0xab8d7bad:'syria',0x44b4af22:'fort',0x6bb2b65f:'monN',0xf04f4c84:'fortN',0x1c37ceb7:'airN',0xcd368052:'syrN',0x9cb9a5b3:'ldnN'}
for tag,f,es,dp in (('OVL',P+"/work/overlay-r3/u3data/build/main/dc1/ps4/net.bin",0xd0,0x70),('PS3',P+"/scratch/live/coop-tmp/p3-net.bin",0xe4,0x74)):
    c=DC(f);o=c.bysid[0x3b9a067d][1];n=c.u32(o);e=c.ptr(o+(8 if c.ps==8 else 4))
    print(tag,n)
    for i in range(n):
        a=e+i*es;h=[L[c.u32(a+k)] for k in (0,4) if c.u32(a+k) in L]
        hit=set(L[c.u32(a+k)] for k in range(0,es,4) if c.u32(a+k) in L)
        if hit:
            d=c.ptr(a+dp) if a+dp in c.ptrs else 0
            t=[(c.d[c.ptr(d+k*c.ps)],c.d[c.ptr(d+k*c.ps)+3]) for k in range(3) if d+k*c.ps in c.ptrs] if d else None
            print(i,hex(a),'type',c.i32(a+0xc),sorted(hit),D.get(c.u32(a),hex(c.u32(a))),D.get(c.u32(a+4),hex(c.u32(a+4))),'desc',t)
