import sys,struct,pickle
sys.path.insert(0,'{{PAKTOOLS}}')
from dc import DC
P='{{PROJECT}}'
D=dict(pickle.load(open(P+'/scratch/live/coop-tmp/dict.pkl','rb')))
team=0xd50bc34b
for f in sys.argv[1:]:
  c=DC(f);d=c.d;ps=c.ps;e='<' if ps==8 else '>'
  print('##',f.rsplit('/',1)[-1],'ps',ps,'len',len(d))
  s=struct.pack(e+'I',team);k=-1
  while True:
    k=d.find(s,k+1)
    if k<0:break
    if k%4:continue
    nxt=struct.unpack_from(e+'I',d,k+4)[0]
    if nxt!=0xc7cb275c:continue
    print(' entry at',hex(k))
    for a in range(k-0x40,k+0x40,4):
      v=c.u32(a);tag=''
      if a in c.ptrs:
        t=c.ptr(a);tv=c.u32(t);tag='PTR->%x [%08x %s]'%(t,tv,D.get(tv,''))
        try:
          z=c.cstr(t)
          if z and len(z)>2 and all(31<ord(ch)<127 for ch in z[:16]):tag+=' "%s"'%z[:40]
        except Exception:pass
      print('  %s%x: %08x %s %s'%('*' if a==k else ' ',a,v,D.get(v,''),tag))
    # who points to this entry / the table containing it
    refs=[hex(q) for q in c.ptrs if abs(c.ptr(q)-k)<0x40]
    print('  refs near:',refs)
