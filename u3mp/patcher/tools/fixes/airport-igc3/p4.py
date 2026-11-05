"""PS4 u3_ps4.elf helpers (file offset = VA in _text.bin).
  p4.py x <hexaddr>        : lea/rip-relative references to <hexaddr> anywhere in text (numpy scan), with containing function (dis.pkl, >=0x700000)
  p4.py f <hexaddr> [n]    : print the function containing <hexaddr> (dis.pkl)  (n = max lines)
  p4.py r <a>-<b>          : print dis.pkl range
  p4.py g <regex> <a> <b>  : grep dis text in range
  p4.py c <hexaddr>        : callers of <hexaddr>
  p4.py s <text>           : find string, print address"""
import pickle,re,sys,bisect,os,numpy as np
L='{{PROJECT}}/scratch/live/instance-mat/'
d=open(L+'_text.bin','rb').read()
_AT=None
def dis():
    global _AT
    if _AT is None: _AT=pickle.load(open(L+'_dis.pkl','rb'))
    return _AT
def cs(v):
    e=d.find(b'\0',v);return d[v:e].decode('latin1')[:80]
def fn(a):
    A,T=dis();i=bisect.bisect_right(A,a)-1
    while i>0 and not T[A[i]].startswith('push rbp'): i-=1
    return A[i]
def leas():
    f=L+'_lea.npz'
    if os.path.exists(f):
        z=np.load(f);return z['idx'],z['tg']
    a=np.frombuffer(d,dtype=np.uint8)
    idx=np.where((a[:-7]&0xfb==0x48)&(a[1:-6]==0x8d)&((a[2:-5]&0xc7)==0x05))[0]
    dv=(a[idx+3].astype(np.int64)|a[idx+4].astype(np.int64)<<8|a[idx+5].astype(np.int64)<<16|a[idx+6].astype(np.int64)<<24)
    dv=np.where(dv>=2**31,dv-2**32,dv);tg=idx+7+dv
    np.savez(f,idx=idx,tg=tg);return idx,tg
def annot(t):
    m=re.search(r'# (0x[0-9a-f]+)',t)
    if m:
        v=int(m.group(1),16)
        if 0x1380000<=v<0x1650000: t+='  "'+cs(v)+'"'
    return t
def pr_range(lo,hi,maxn=4000):
    A,T=dis();n=0
    for v in A[bisect.bisect_left(A,lo):bisect.bisect_left(A,hi)]:
        print(hex(v),annot(T[v]));n+=1
        if n>=maxn: print('...');break
def pr_fn(a,maxn=400):
    A,T=dis();s=fn(a);i=bisect.bisect_left(A,s);n=0
    print('--- function',hex(s))
    while i<len(A):
        v=A[i];t=T[v];print(hex(v),annot(t));i+=1;n+=1
        if n>=maxn: print('...');break
        if i<len(A) and T[A[i]].startswith('push rbp') and (t.startswith('ret') or t.startswith('jmp') or t.startswith('int3') or t.startswith('nop')): break
if __name__=='__main__':
    c=sys.argv[1]
    if c=='s':
        for m in re.finditer(re.escape(sys.argv[2].encode()),d): 
            j=d.rfind(b'\0',0,m.start())+1; print(hex(j),cs(j))
    if c=='x':
        v=int(sys.argv[2],16);idx,tg=leas()
        for x in idx[tg==v]:
            x=int(x);print(hex(x),'in fn',hex(fn(x)) if x>=0x700000 else '<0x700000')
    if c=='f':
        pr_fn(int(sys.argv[2],16),int(sys.argv[3]) if len(sys.argv)>3 else 400)
    if c=='r':
        a,b=[int(x,16) for x in sys.argv[2].split('-')];pr_range(a,b)
    if c=='g':
        p=re.compile(sys.argv[2]);A,T=dis();lo=int(sys.argv[3],16);hi=int(sys.argv[4],16)
        for v in A[bisect.bisect_left(A,lo):bisect.bisect_left(A,hi)]:
            if p.search(T[v]):print(hex(v),annot(T[v]))
    if c=='c':
        A,T=dis()
        for t in sys.argv[2:]:
            print(t,[hex(v)+'<'+hex(fn(v)) for v in A if T[v] in('call 0x'+t,'jmp 0x'+t)])
