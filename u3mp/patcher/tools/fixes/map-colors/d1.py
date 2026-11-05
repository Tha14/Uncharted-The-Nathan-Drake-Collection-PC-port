import sys
a=open(sys.argv[1],'rb').read();b=open(sys.argv[2],'rb').read()
print(len(a),len(b))
n=min(len(a),len(b));i=0;runs=[];s=None
A=memoryview(a);B=memoryview(b);step=1<<16
tot=0
for o in range(0,n,step):
  if A[o:o+step]!=B[o:o+step]:
    for k in range(o,min(o+step,n)):
      if a[k]!=b[k]:
        tot+=1
        if s is None:s=k;e=k
        elif k-e>32:runs.append((s,e));s=k;e=k
        else:e=k
if s is not None:runs.append((s,e))
print('diff bytes',tot,'runs',len(runs),[(hex(x),y-x+1) for x,y in runs[:30]])
