from lib import *
n=OV+'air-multi.pak';q=Pak(n);old=open(R+'work/backup-wall-emblem/pak23/ps4/air-multi.pak','rb').read();d=q.d
sb=subs(q);iv=[(s['ib'],s['ib']+s['ni']*2,s['name'],s['mat'].split(',')[0]) for s in sb]
A=memoryview(d);B=memoryview(old);hits=collections.Counter();other=0
for o in range(0,len(d),1<<16):
    if A[o:o+1<<16]!=B[o:o+1<<16]:
        pass
idx=[i for i in range(0,len(d)) if False]
import numpy as _n
x=_n.frombuffer(d,'u1');y=_n.frombuffer(old,'u1');di=_n.nonzero(x!=y)[0];print(len(di))
for i in di:
    m=[v for v in iv if v[0]<=i<v[1]]
    if m:hits[(m[0][2][-22:],m[0][3].rsplit('/',1)[-1])]+=1
    else:other+=1
print(hits,other)
