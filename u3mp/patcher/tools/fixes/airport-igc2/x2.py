import pickle,re,struct
A,T=pickle.load(open('../live/instance-mat/_dis.pkl','rb'))
d=open('../live/instance-mat/_text.bin','rb').read()
import bisect
lo=bisect.bisect(A,0x1212000);hi=bisect.bisect(A,0x1214400)
def cs(v):
    e=d.find(b'\0',v);return d[v:e].decode('latin1')
pend=[];out=[]
for a in A[lo:hi]:
    t=T[a]
    m=re.search(r'# (0x[0-9a-f]+)',t)
    if t.startswith('lea') and m and int(m.group(1),16)>=0x14b0000:
        s=cs(int(m.group(1),16))
        # flush
        out.append((a,s,pend));pend=[]
    else:
        mm=re.search(r'\[(r\w+)\+(0x[0-9a-f]+)\]',t)
        if mm: pend.append((a,t))
open('rsdump.txt','w').write('\n'.join(f'{hex(a)} {s!r}\n   '+' ; '.join(x[1] for x in p) for a,s,p in out))
print(len(out))
