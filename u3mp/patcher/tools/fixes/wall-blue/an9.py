from ilib import *
sys.path.insert(0,'{{PAKTOOLS}}/stage')
os.chdir(M2)
import ps3params,fp
p=Pak('p3/air-multi.pak')
mt=p.items_of('MATERIAL_TABLE_1')[0];pg,b=mt.page,mt.data_off
nmat=p.pu32(pg,b+0xc);ma=p.resolve(pg,b+0x20);th=p.resolve(pg,b+0x18);nm=p.resolve(pg,b+0x30);ntex=p.pu32(pg,b+4);sr=p.resolve(pg,b+0x1c)
def tn(rr):
  if rr and rr[0]==th[0] and th[1]<=rr[1]<th[1]+4*ntex: return p.pstr(*p.resolve(nm[0],nm[1]+4*((rr[1]-th[1])//4))).split('/')[-2]
print('th',th,'ntex',ntex)
for i in range(nmat):
  m=p.resolve(ma[0],ma[1]+4*i);name=p.pstr(*p.resolve(*m))
  if not re.search(r'corrugated-main-blue/|corrugated-main-blue-striped/|corrugated-main-white/',name):continue
  k,e,node=ps3params._walk(p,m)[0]
  B=p.pbase(node[0])+node[1];print('==',name,'node',node,p.d[B:B+0x40].hex())
  pid=struct.unpack_from('>H',p.d,B)[0]
  # program header
  t=p.resolve(sr[0],sr[1]+0x10*pid+12);Bp=p.pbase(t[0])+t[1];h=struct.unpack_from('>16I',p.d,Bp);print(' prog hdr',[hex(x) for x in h])
  L=fp.dis(p.d[Bp+h[4]:Bp+h[4]+h[0]]);used=sorted({int(x) for l in L for x in re.findall(r'tex(\d+)',l)});print(' units used',used)
  for off in (0x10,0x14,0x18,0x1c):
    r=p.resolve(node[0],node[1]+off)
    if not r:continue
    Bq=p.pbase(r[0])+r[1];print(' +%#x ->'%off,r,p.d[Bq:Bq+0xc0].hex())
    if off==0x10:
      for o in range(0,0xd0,4):
        rr=p.resolve(r[0],r[1]+o) if p.is_ptr(r[0],r[1]+o) else None
        n=tn(rr)
        if rr:print('    @%#x'%o,'ptr',rr,n)
        else:
          print('    @%#x'%o,hex(struct.unpack_from('>I',p.d,Bq+o)[0]))
