"""Compare state-script option defaults: port-converted PS4 dc1 bins vs PS3 originals (bin.psarc)."""
import sys,os,glob,struct,pickle
sys.path.insert(0,'{{PAKTOOLS}}')
from psarc_lib import Psarc
from dc import DC
P='{{PROJECT}}'
D=dict(pickle.load(open(P+'/scratch/live/coop-tmp/dict.pkl','rb')))
G="{{PS3_DISC}}/"
TY={0xc7cb275c:'int32',0x8b4e76ff:'boolean',0xf182ec3:'float',0xc4ab6121:'symbol',0xb3952e7:'string'}
pf=glob.glob(glob.escape(G)+'**/bin.psarc',recursive=True)[0];ps=Psarc(pf);names={n:i for i,n in enumerate(ps.names)}
class M:
  def __init__(s,d):s.d=d
def entries(c):
  d=c.d;e=c.e;out=[];k=0;stride=0x18 if c.ps==8 else 0x10
  while k+stride<=len(d):
    sid=struct.unpack_from(e+'I',d,k)[0];ty=struct.unpack_from(e+'I',d,k+4)[0]
    po=k+0x10 if c.ps==8 else k+0xc
    if ty in TY and po in c.ptrs and sid:
      t=c.ptr(po)
      if t and t+4<=len(d):
        v=c.u32(t);out.append((k,sid,TY[ty],t,v))
    k+=4
  return out
import tempfile
tot=0;bad=0
for o in ('overlay-r3','overlay-r2','overlay-port','overlay-mp'):
  for f in sorted(glob.glob(glob.escape(P+'/work/'+o+'/u3data/build/main/dc1/ps4')+'/**/*.bin',recursive=True)):
    rel=f.replace(chr(92),'/').split('/dc1/ps4/')[1];p3='dc1/'+rel
    if p3 not in names:continue
    raw=ps.read(names[p3]+1)
    tmp=P+'/scratch/enemy-stall/_tmp_p3.bin';open(tmp,'wb').write(raw)
    try:a=DC(f);b=DC(tmp)
    except Exception as ex:print('ERR',rel,ex);continue
    ea=entries(a);eb=entries(b)
    # match by (sid,type) in order
    j=0
    for (ka,sa,ta,pa,va) in ea:
      m=None
      for q in range(j,len(eb)):
        if eb[q][1]==sa and eb[q][2]==ta:m=q;break
      if m is None:continue
      j=m+1;kb,sb,tb,pb,vb=eb[m];tot+=1
      if va!=vb:
        bad+=1;print('%s %s @%x ptr@%x->%x %s %s ps4=%08x(%s) ps3=%08x(%s)'%(o,rel,ka,ka+0x10,pa,D.get(sa,hex(sa)),ta,va,D.get(va,''),vb,D.get(vb,'')))
print('compared',tot,'bad',bad)
