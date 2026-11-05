import sys,struct
sys.path[:0]=['/home/claude/u3/paktools','/home/claude/u3/paktools/stage','.']
from ndpak import Pak
import ps3params,fp
def material_programs(p,matname):
  mt=p.items_of('MATERIAL_TABLE_1')[0];pg,b=mt.page,mt.data_off
  nmat=p.pu32(pg,b+0xc);ma=p.resolve(pg,b+0x20);sr=p.resolve(pg,b+0x1c)
  for i in range(nmat):
    m=p.resolve(ma[0],ma[1]+4*i);name=p.pstr(*p.resolve(*m))
    if name==matname or name.endswith(matname):
      res=[]
      for k,e,node in ps3params._walk(p,m):
        pid=struct.unpack_from('>H',p.d,p.pbase(node[0])+node[1])[0]
        t=p.resolve(sr[0],sr[1]+0x10*pid+12)
        if not t: continue
        B=p.pbase(t[0])+t[1];hdr=struct.unpack_from('>8I',p.d,B)
        code=p.d[B+hdr[4]:B+hdr[4]+hdr[0]]
        res.append((k,e,pid,node,code))
      return name,res
  return None,[]
if __name__=='__main__':
  p=Pak(sys.argv[1]);name,res=material_programs(p,sys.argv[2])
  print(name)
  seen=set()
  for k,e,pid,node,code in res:
    if pid in seen: continue
    seen.add(pid)
    L=fp.dis(code);print('== pass',k,'entry',e,'prog',hex(pid),'instr',len(L),'tex',sum(l.startswith('TEX') or l.startswith('TXP') for l in L))
    if len(sys.argv)>3 and int(sys.argv[3],16)==pid: print('\n'.join(L))
