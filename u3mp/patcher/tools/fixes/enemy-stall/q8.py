import sys,pickle,struct
sys.path.insert(0,'{{PAKTOOLS}}')
from dc import DC
P='{{PROJECT}}'
D=dict(pickle.load(open(P+'/scratch/live/coop-tmp/dict.pkl','rb')))
f=sys.argv[1];needle=sys.argv[2].encode();c=DC(f);ps=c.ps
d=c.d;j=d.find(needle);print('str at',hex(j),'ps',ps)
refs=[q for q in c.ptrs if c.ptr(q)==j]
print('ptr refs',[hex(q) for q in refs])
for q in refs:
  lo=q-0x60;hi=q+0x80
  for a in range(lo,hi,4):
    v=c.u32(a);tag='P->%x'%c.ptr(a) if a in c.ptrs else ''
    if a in c.ptrs and c.ptr(a) in c.ptrs or (a in c.ptrs):
      try:tag+=' "%s"'%c.cstr(c.ptr(a))
      except Exception:pass
    print('%s%x: %08x %s %s'%('*' if a==q else ' ',a,v,D.get(v,''),tag))
