"""Aggregate sample_guest.py output per function (starts from funcs.json). Usage: profile_report.py funcs.json a.json [b.json]"""
import json,sys,bisect,collections
starts=json.load(open(sys.argv[1]))
def fn(a): i=bisect.bisect_right(starts,a)-1; return starts[i] if i>=0 else 0
def load(p):
    d=json.load(open(p)); c=collections.Counter(); inc=collections.Counter()
    for a,n in d['rip'].items(): c[fn(int(a,16))]+=n
    for s,n in d['stacks']:
        for f in set(fn(int(x,16)) for x in s): inc[f]+=n
    tot=sum(d['rip'].values()); return c,inc,tot
A=load(sys.argv[2])
if len(sys.argv)<4:
    c,inc,t=A
    for f,n in c.most_common(40): print(f'{f:#x} self {n/t*100:5.2f}%  incl {inc[f]/t*100:5.2f}%')
else:
    B=load(sys.argv[3])
    keys=set(A[1])|set(B[1])
    rows=[(A[1][f]/A[2]-B[1][f]/B[2],f) for f in keys]
    rows.sort()
    print('more in', sys.argv[3]); [print(f'{f:#x} {sys.argv[2][-12:]} {A[1][f]/A[2]*100:6.2f}%  {sys.argv[3][-12:]} {B[1][f]/B[2]*100:6.2f}%') for d,f in rows[:25]]
    print('more in', sys.argv[2]); [print(f'{f:#x} {sys.argv[2][-12:]} {A[1][f]/A[2]*100:6.2f}%  {sys.argv[3][-12:]} {B[1][f]/B[2]*100:6.2f}%') for d,f in rows[::-1][:40]]
