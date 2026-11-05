import pickle,re,sys,bisect
A,T=pickle.load(open('../live/instance-mat/_dis.pkl','rb'))
d=open('../live/instance-mat/_text.bin','rb').read()
def cs(v):
    e=d.find(b'\0',v);return d[v:e].decode('latin1')[:70]
for arg in sys.argv[1:]:
    a0,n=arg.split(':');a0=int(a0,16);n=int(n)
    i=bisect.bisect_left(A,a0)
    print('---',hex(a0))
    for a in A[i:i+n]:
        t=T[a];m=re.search(r'# (0x[0-9a-f]+)',t)
        if m and 0x1440000<=int(m.group(1),16)<0x1650000: t+='  "'+cs(int(m.group(1),16))+'"'
        print(hex(a),t)
