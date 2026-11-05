import sys,struct
sys.dont_write_bytecode=True
sys.path.insert(0,'dctools')
from common import DC,sid
R="{{PROJECT}}/work/overlay-r3/u3data/build/main/dc1/ps4/net.bin"
a=DC(R);b=DC('_ps3net.bin')
H=0xbd3bb3b9;NONE=0xf114b39d
TABLE=[1,3,0,1,1,2]
class V:
  def __init__(s,c):
    s.c=c;s.w=c.ps==8;s.es=0x38 if s.w else 0x2c
    t,o=c.bysid[H];s.n=c.u32(o);s.a=c.ptr(o+c.ps);s.he=0x70 if s.w else 0x3c
  def slot(s,i,idx):  # idx slot index 0..6: hero offset
    c=s.c;e=s.a+i*s.he
    so=e+(0x18+8*idx if s.w else 0x10+4*idx)
    return c.ptr(so) if c.isptr(so) else 0
  def name(s,i):return s.c.cstr(s.c.ptr(s.a+i*s.he))
  def ents(s,ps):
    c=s.c
    if ps not in c.bysid:return []
    t,o=c.bysid[ps];n=c.u32(o);q=c.ptr(o+c.ps)
    r=[]
    for j in range(n):
      e=q+j*s.es
      if s.w:r.append(dict(art=c.cstr(c.ptr(e+8)),sid=c.u32(e+0x10),kind=c.u32(e+0x18),m0=c.u32(e+0x20),m1=c.u32(e+0x24),m2=c.u32(e+0x28),pal=c.u32(e+0x2c),e=e))
      else:r.append(dict(art=c.cstr(c.ptr(e+4)),sid=c.u32(e+8),kind=c.u32(e+0xc),m0=c.u32(e+0x14),m1=c.u32(e+0x18),m2=c.u32(e+0x1c),pal=c.u32(e+0x20),e=e))
    return r
  def first(s,sl,want=0):
    c=s.c
    if not sl:return []
    cnt=c.u32(sl);q=c.ptr(sl+c.ps)
    return s.ents(c.u32(q)) if cnt else []
  def lst(s,sl,idx=0):
    c=s.c
    if not sl:return []
    cnt=c.u32(sl);q=c.ptr(sl+c.ps)
    return s.ents(c.u32(q+4*(idx if idx<cnt else 0))) if cnt else []
CASE={5:1,6:4,7:2,8:1,9:1,10:3}  # k -> slot index holding the key (hero +0x20,+0x38,+0x28,+0x20,+0x20,+0x30)
def part(v,i,body,k,byte):
  key=0
  e=v.first(v.slot(i,CASE[k]))
  if v.slot(i,CASE[k]):
    if not e:return None
    x=e[byte] if byte<len(e) else e[0]
    key=x['m1'] if k==8 else x['m2'] if k==9 else x['m0']
  for y in v.lst(v.slot(i,0),body):
    if y['kind']==k and y['sid']!=NONE and y['m0' ]==key:return y['art']
  return ''
if __name__=='__main__':
  A=V(a);B=V(b)
  print('heroes',A.n,B.n)
  nm3={B.name(j):j for j in range(B.n)}
  tot=0
  for i in range(A.n):
    nm=A.name(i);j=nm3.get(nm)
    if j is None:continue
    for body in range(max(1,a.u32(A.slot(i,0)) if A.slot(i,0) else 1)):
      for k in (5,6,7,8,9,10):
        L=A.first(A.slot(i,CASE[k]));N=max(1,len(L))
        for byte in range(N):
          x=part(A,i,body,k,byte);y=part(B,j,body,k,byte)
          if x!=y:
            tot+=1
            acc=L[byte]['art'] if L and byte<len(L) else '-'
            print('%2d %-20s body%d k%-2d look[%d]=%-34s PS4=%-30r PS3=%r'%(i,nm,body,k,byte,acc,x,y))
  print('diffs',tot)
  # raw entry diffs
  print('--entry +0x20/24/28 diffs, list by list')
  from parts_common import part_syms
  s4=part_syms(a);s3=part_syms(b)
  for ps,(o,q,es) in s4.items():
    if ps not in s3:continue
    E3=B.ents(ps);E4=A.ents(ps)
    by={x['sid']:x for x in E3}
    for x in E4:
      y=by.get(x['sid'])
      if y and (x['m0'],x['m1'],x['m2'],x['kind'],x['pal'])!=(y['m0'],y['m1'],y['m2'],y['kind'],y['pal']):
        print('%08x %-36s PS4 k%d %x %x %x pal %x | PS3 k%d %x %x %x pal %x'%(ps,x['art'],x['kind'],x['m0'],x['m1'],x['m2'],x['pal'],y['kind'],y['m0'],y['m1'],y['m2'],y['pal']))
