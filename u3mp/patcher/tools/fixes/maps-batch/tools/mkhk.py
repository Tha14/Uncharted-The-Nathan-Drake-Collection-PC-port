"""Rebuild paktools/data/hk2010_ps3.json + hk2011_ps4.json (hkconvert reflection tables) from the executables.
hkClass objects are built by static constructors, so class name/parent/size/member table come from the ctor
call sites; the hkClassMember arrays are static data.
usage: mkhk.py <EBOOT.elf> <u3_ps4.elf> <ps4.dis> <outdir>"""
import struct,sys,json,os,re,collections
TY="VOID BOOL CHAR INT8 UINT8 INT16 UINT16 INT32 UINT32 INT64 UINT64 REAL VECTOR4 QUATERNION MATRIX3 ROTATION QSTRANSFORM MATRIX4 TRANSFORM ZERO POINTER FUNCTIONPOINTER ARRAY INPLACEARRAY ENUM STRUCT SIMPLEARRAY HOMOGENEOUSARRAY VARIANT CSTRING ULONG FLAGS HALF STRINGPTR RELARRAY".split()
class E:
  def __init__(s,path):
    s.d=d=open(path,'rb').read();assert d[:4]==b'\x7fELF'
    s.e='>' if d[5]==2 else '<';s.ps=4 if s.e=='>' else 8;s.seg=[]
    phoff=struct.unpack_from(s.e+'Q',d,0x20)[0];es,n=struct.unpack_from(s.e+'HH',d,0x36)
    for i in range(n):
      t,fl,of,va,pa,fs,ms,al=struct.unpack_from(s.e+'IIQQQQQQ',d,phoff+i*es)
      if t in(1,0x61000010,0x61000000) and fs:s.seg.append((va,of,fs,ms))
    s.entry=struct.unpack_from(s.e+'Q',d,0x18)[0]
  def off(s,va):
    for v,o,f,m in s.seg:
      if v<=va<v+f:return o+va-v
  def u32(s,va):
    o=s.off(va);return None if o is None else struct.unpack_from(s.e+'I',s.d,o)[0]
  def cstr(s,va):
    o=s.off(va) if va else None
    if o is None:return None
    e=s.d.find(b'\0',o,o+200)
    if e<=o:return None
    try:x=s.d[o:e].decode('ascii')
    except UnicodeDecodeError:return None
    return x if x.isprintable() else None
def members(x,mem,n,names):
  ms=[];sz=0x18 if x.ps==4 else 0x28
  mo=x.off(mem) if mem else None
  if mo is None or not n:return []
  for k in range(n):
    r=mo+k*sz
    if x.ps==4:
      mn,c=struct.unpack_from('>II',x.d,r);t,st,ca,fl,of=struct.unpack_from('>BBhHH',x.d,r+0xc)
    else:
      mn,c=struct.unpack_from('<QQ',x.d,r);t,st,ca,fl,of=struct.unpack_from('<BBhHH',x.d,r+0x18)
    nm=x.cstr(mn)
    if nm is None or t>=len(TY) or st>=len(TY):raise ValueError('member %x %d'%(mem,k))
    ms.append(dict(name=nm,type=TY[t],sub=TY[st],carr=ca,flags=fl,off=of,cls=names.get(c) or x.cstr(c)))
  return ms
def finish(x,recs):
  names={r['this']:r['name'] for r in recs}
  out={}
  for r in recs:
    try:m=members(x,r['mem'],r['nmem'],names)
    except ValueError as e:print('bad',r['name'],e);m=[]
    e=dict(parent=names.get(r['parent']),size=r['size'],members=m)
    if r['name'] not in out or (m and not out[r['name']]['members']):out[r['name']]=e
  return out
def scan_ps3(x):
  d=x.d;opd=x.off(x.entry);toc=struct.unpack_from('>I',d,opd+4)[0]
  v,o,f,_=x.seg[0];end=o+f;calls=[]
  def sim(i):
    r={2:toc};st={}
    for j in range(max(o,i-4*70),i,4):
      w=struct.unpack_from('>I',d,j)[0];op=w>>26;rt=(w>>21)&31;ra=(w>>16)&31;si=w&0xffff;s=si-0x10000 if si&0x8000 else si
      if op==14:
        if ra==0:r[rt]=s&0xffffffff
        elif ra in r:r[rt]=(r[ra]+s)&0xffffffff
        else:r.pop(rt,None)
      elif op==15:r[rt]=((r.get(ra,0) if ra else 0)+(s<<16))&0xffffffff
      elif op==24:
        if rt in r:r[ra]=r[rt]|si
      elif op==32:
        val=x.u32((r[ra]+s)&0xffffffff) if ra in r else None
        if val is not None:r[rt]=val
        else:r.pop(rt,None)
      elif op==31 and ((w>>1)&1023)==444 and rt==((w>>11)&31):
        if rt in r:r[ra]=r[rt]
        else:r.pop(ra,None)
      elif op in(36,62) and ra==1:
        k=s&~3 if op==62 else s
        if rt in r:st[k]=r[rt]
      elif op in(58,34,40):r.pop(rt,None)
      elif op==18 and w&1:
        for q in range(3,13):r.pop(q,None)
    return r,st
  for i in range(o,end,4):
    w=struct.unpack_from('>I',d,i)[0]
    if w>>26==18 and w&3==1:
      li=w&0x3fffffc;li=li-0x4000000 if li&0x2000000 else li
      calls.append((i,v+i-o+li))
  by=collections.defaultdict(list)
  for i,t in calls:by[t].append(i)
  best=None
  for t,l in sorted(by.items(),key=lambda a:-len(a[1])):
    if len(l)<100:break
    n=0
    for i in l[:40]:
      r,_=sim(i);s=x.cstr(r.get(4,0))
      if s and s.startswith('hk'):n+=1
    if n>=30:best=t;break
  print('ps3 ctor',hex(best or 0),'callers',len(by.get(best,[])),'toc',hex(toc))
  recs=[]
  for i in by[best]:
    r,st=sim(i);nm=x.cstr(r.get(4,0))
    if nm:recs.append(dict(this=r.get(3),name=nm,parent=r.get(5),size=r.get(6),mem=0,nmem=0,slots=st,r=dict(r)))
  return recs
def scan_ps4(x,dis):
  recs=[]
  lines=collections.deque(maxlen=90)
  re_lea=re.compile(r'lea\s+(\w+),\[rip[^\]]*\]\s+# (0x[0-9a-f]+)')
  re_mi=re.compile(r'mov\s+(?:\w+ PTR )?\[rsp(?:\+(0x[0-9a-f]+))?\],(0x[0-9a-f]+)$')
  re_ms=re.compile(r'mov\s+(?:\w+ PTR )?\[rsp(?:\+(0x[0-9a-f]+))?\],(\w+)$')
  re_mr=re.compile(r'mov\s+(\w+),(\w+)$')
  re_mimm=re.compile(r'mov\s+(\w+),(0x[0-9a-f]+)$')
  re_xor=re.compile(r'xor\s+(\w+),(\w+)$')
  low={'esi':'rsi','edi':'rdi','edx':'rdx','ecx':'rcx','r8d':'r8','r9d':'r9','eax':'rax','ebx':'rbx','ebp':'rbp'}
  n=lambda a:low.get(a,a)
  for ln in open(dis,errors='replace'):
    if 'call   0x122c40' in ln:
      r={};st={}
      for l in lines:
        t=l.split('\t')[-1].strip()
        m=re_lea.search(t)
        if m:r[n(m.group(1))]=int(m.group(2),16);continue
        m=re_mi.search(t)
        if m:st[int(m.group(1) or '0',16)]=int(m.group(2),16);continue
        m=re_ms.search(t)
        if m:
          if n(m.group(2)) in r:st[int(m.group(1) or '0',16)]=r[n(m.group(2))]
          continue
        m=re_mimm.search(t)
        if m:r[n(m.group(1))]=int(m.group(2),16);continue
        m=re_mr.search(t)
        if m:
          if n(m.group(2)) in r:r[n(m.group(1))]=r[n(m.group(2))]
          else:r.pop(n(m.group(1)),None)
          continue
        m=re_xor.search(t)
        if m and m.group(1)==m.group(2):r[n(m.group(1))]=0
      nm=x.cstr(r.get('rsi',0))
      if nm:recs.append(dict(this=r.get('rdi'),name=nm,parent=r.get('rdx'),size=r.get('rcx'),mem=st.get(0x10,0),nmem=st.get(0x18,0)))
      lines.clear()
    else:
      lines.append(ln)
  return recs
if __name__=='__main__':
  a=E(sys.argv[1]);b=E(sys.argv[2])
  ra=scan_ps3(a)
  print('ps3 recs',len(ra))
  for q in ra:
    if q['name'] in('hkpBoxShape','hkpRigidBody','hkpEntity'):print(q['name'],q['size'],hex(q['this'] or 0),hex(q['parent'] or 0),{hex(k):hex(v) for k,v in sorted(q['slots'].items())},{k:hex(v) for k,v in sorted(q['r'].items())})
  rb=scan_ps4(b,sys.argv[3]);print('ps4 recs',len(rb),[(q['name'],q['size'],hex(q['mem'] or 0),q['nmem']) for q in rb[:5]])
  import pickle;pickle.dump((ra,rb),open(sys.argv[4]+'/_recs.pkl','wb'))
