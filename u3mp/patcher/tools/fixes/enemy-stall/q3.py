import sys,pickle
sys.path.insert(0,'{{PAKTOOLS}}')
from dc import DC
P='{{PROJECT}}'
D=dict(pickle.load(open(P+'/scratch/live/coop-tmp/dict.pkl','rb')))
def dump(f,a):
  c=DC(f);ps=c.ps;code=c.ptr(a);k=c.ptr(a+ps);size=c.u32(a+2*ps+4);n=size//4 if size else (k-code)//4
  print('##',f.rsplit('/',1)[1],'lambda %x code %x keys %x n %d'%(a,code,k,n))
  for i in range(n):
    o=code+4*i;op,x,y,z=c.d[o:o+4];idx=y|(z<<8);ka=k+ps*idx
    extra=''
    if op in(0x15,0x3f,0x44,0x40,0x41,0x42):
      if ka in c.ptrs:extra='"%s"'%c.cstr(c.ptr(ka))
      else:w=c.u32(ka);extra='%x %s'%(w,D.get(w,''))
    print('%3d: op%02x x=%d y=%d z=%d %s'%(i,op,x,y,z,extra))
dump(P+'/work/overlay-r3/u3data/build/main/dc1/ps4/ss-multiplayer/ss-tag-on-enter.bin',0xaa8)
dump(P+'/scratch/enemy-stall/p3-ss-tag-on-enter.bin',0x6f4)
