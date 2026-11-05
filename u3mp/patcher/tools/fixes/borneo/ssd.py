import sys,struct,pickle,re
sys.path.insert(0,sys.argv[1]+"/gh2/u3mp/paktools")
from dc import DC,sid
D=pickle.load(open(sys.argv[1]+'/borneo/d/dict.pkl','rb'))
c=DC(sys.argv[2]);fo=open(sys.argv[3],'w',encoding='utf8')
syms=sorted(c.syms,key=lambda x:x[2])
ends=[s[2] for s in syms[1:]]+[c.reloc]
al=[]
for (s,t,o),e in zip(syms,ends):
  fo.write(f'\n#### {D.get(s,hex(s))} type {D.get(t,hex(t))} @{o:x} len {e-o}\n')
  seq=[]
  for p in range(o,e,4):
    v=c.u32(p)
    if v in D and v>0x10000:
      seq.append(D[v])
  fo.write(' '.join(seq)+'\n')
  al.append(set(seq))
