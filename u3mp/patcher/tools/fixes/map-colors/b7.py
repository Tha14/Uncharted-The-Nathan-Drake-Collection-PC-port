from lib import *
sys.path.insert(0,'tools')
import gcn
pak,pg,off=sys.argv[1],int(sys.argv[2]),int(sys.argv[3],16)
q=Pak(OV+pak+'.pak')
it=[i for i in q.items if i.page==pg and i.off==off][0];b=q.pbase(pg)+it.data_off;raw=q.d[b:b+it.size]
co=struct.unpack_from('<I',raw,4)[0]
L=gcn.dis(raw[co:]);print('\n'.join(str(l) for l in L))
