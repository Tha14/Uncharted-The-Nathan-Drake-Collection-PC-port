import pickle,re,struct,sys,bisect
A,T=pickle.load(open('../live/instance-mat/_dis.pkl','rb'))
d=open('../live/instance-mat/_text.bin','rb').read()
def cs(v):
    e=d.find(b'\0',v);return d[v:e].decode('latin1')
lo=bisect.bisect(A,0xe35cd0);hi=bisect.bisect(A,0xe37900)
for a in A[lo:hi]:
    t=T[a]
    m=re.search(r'# (0x[0-9a-f]+)',t)
    if m:
        v=int(m.group(1),16)
        if 0x1480000<=v<0x1500000: print(hex(a),'STR',cs(v))
        else: print(hex(a),t)
    elif re.search(r'\+0x[0-9a-f]{3,}\]',t): print(hex(a),t)
