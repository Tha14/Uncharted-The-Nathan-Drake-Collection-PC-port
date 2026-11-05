from lib import *
sys.path.insert(0,'tools')
import ps3fp,fp,pol
p=Pak(S+'emblem-batch/p3/air-multi.pak')
for mn in sys.argv[1:]:
    name,res=ps3fp.material_programs(p,mn)
    print('==',name,len(res))
    r=res[0];print(r[:4]);L=fp.dis(r[4]);print(pol.polarity(L));print('\n'.join(L))
