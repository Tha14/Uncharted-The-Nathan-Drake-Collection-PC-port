from lib import *
sys.path.insert(0,'tools')
import ps3fp,fp
p=Pak(S+'emblem-batch/p3/air-multi.pak')
name,res=ps3fp.material_programs(p,sys.argv[1]);seen=set()
for k,e,pid,node,code in res:
    if pid in seen:continue
    seen.add(pid);L=fp.dis(code);print('== pass',k,'e',e,'prog',hex(pid),'n',len(L),'tex',sum(l.startswith('TEX') or l.startswith('TXP') for l in L))
    if len(sys.argv)>2 and hex(pid) in sys.argv[2:]:print('\n'.join(L))
