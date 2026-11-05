import pickle,re,sys,bisect
A,T=pickle.load(open('../live/instance-mat/_dis.pkl','rb'))
D=dict(pickle.load(open('../live/coop-tmp/dict.pkl','rb')))
d=open('../live/instance-mat/_text.bin','rb').read()
def cs(v):
    e=d.find(b'\0',v);return d[v:e].decode('latin1')[:70]
for arg in sys.argv[1:]:
    a0,a1=[int(x,16) for x in arg.split(':')]
    for a in A[bisect.bisect_left(A,a0):bisect.bisect_left(A,a1)]:
        t=T[a];x=''
        m=re.search(r'# (0x[0-9a-f]+)',t)
        if m and 0x1440000<=int(m.group(1),16)<0x1650000:
            s=cs(int(m.group(1),16))
            if len(s)>3 and s.isprintable(): x=' "'+s+'"'
        for h in re.findall(r'0x[0-9a-f]{8}\b',t):
            v=int(h,16)
            if v in D: x+=' ='+D[v]
        if x or t.startswith('call'): print(hex(a),t[:60],x)
