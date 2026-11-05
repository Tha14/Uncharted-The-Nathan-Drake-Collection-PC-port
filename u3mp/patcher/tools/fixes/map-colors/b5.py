from lib import *
sys.path.insert(0,'tools/gcn');sys.path.insert(0,'tools')
import fx,gcn
pak,h=sys.argv[1],int(sys.argv[2],16)
q=Pak(OV+pak+'.pak');E,et=fx.effects(q)
eit,cl,ep=E[h]
print(eit.type,eit.size,len(cl))
print(collections.Counter(i.type for i in cl))
for i in cl:
    if i.type!='RAW_DATA':continue
    b=q.pbase(i.page)+i.data_off;raw=q.d[b:b+i.size]
    if i.size<0x40:
        continue
    co=struct.unpack_from('<I',raw,4)[0]
    if not(0<co<i.size):continue
    code=raw[co:]
    try:L=gcn.dis(code)
    except Exception as ex:continue
    L=L if isinstance(L,list) else L.split('\n')
    ns=sum('image_sample' in str(l) for l in L);nx=sum('exp' in str(l) for l in L)
    print(i.page,hex(i.off),i.size,'co',hex(co),'lines',len(L),'sample',ns,'exp',nx)
