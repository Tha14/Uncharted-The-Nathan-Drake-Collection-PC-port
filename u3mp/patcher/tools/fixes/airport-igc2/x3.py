import pickle,re,struct,sys,bisect,json
sys.path.insert(0,'{{PAKTOOLS}}')
from dc import DC,sid
A,T=pickle.load(open('../live/instance-mat/_dis.pkl','rb'))
d=open('../live/instance-mat/_text.bin','rb').read()
lo=bisect.bisect(A,0x1210800);hi=bisect.bisect(A,0x1214a00)
def cs(v):
    e=d.find(b'\0',v);return d[v:e].decode('latin1')
# key lea then loads *after* the format? order: loads before fmt? detect from dump above: load printed in pend before lea => loads come BEFORE lea
S=[];pend=[];sec='';
for a in A[lo:hi]:
    t=T[a]
    m=re.search(r'# (0x[0-9a-f]+)',t)
    if t.startswith('lea') and m and int(m.group(1),16)>=0x14b0000:
        s=cs(int(m.group(1),16))
        S.append((s,pend));pend=[]
    else:
        mm=re.search(r'\[r(?:15|13|14|12|bx|bp)\+(0x[0-9a-f]+)\]',t) or re.search(r'\[r(?:15)\]',t)
        if mm and ('xmm' in t or t.startswith('mov')): pend.append(t)
Sch=[];sec=''
for s,p in S:
    m=re.match(r'\s*\(?(new-render-settings-[\w-]+|:[\w-]+)',s)
    if '(new-render-settings' in s: sec=re.search(r'new-render-settings-?[\w-]*',s).group(0).replace('new-render-settings-','')
    if not m: continue
    key=re.match(r'\s*:?([\w-]+)',s).group(1) if s.strip().startswith(':') else None
    fmts=re.findall(r'%[0-9.]*[fdusx]',s)
    of=[int(x,16) for t in p for x in re.findall(r'\[r15\+(0x[0-9a-f]+)\]',t)]
    if key and of:
        Sch.append((sec,key,fmts,of))
json.dump(Sch,open('rs_schema.json','w'))
print(len(Sch),Sch[:3],Sch[-3:])
print(sorted(set(x[0] for x in Sch)))
print(max(o for x in Sch for o in x[3]))
