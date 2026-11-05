from lib import *
def table(q):
    et=q.items_of('EFFECT_TABLE')[0];t=q.resolve(et.page,et.data_off+0x18);n=q.pu32(*t)
    return [q.pstr(*q.resolve(t[0],t[1]+8+8*i)) for i in range(n)]
def inputs(q,tab=None):
    tab=tab or table(q)
    et=q.items_of('EFFECT_TABLE')[0];n=q.pu32(et.page,et.data_off);arr=q.resolve(et.page,et.data_off+8);out={}
    for k in range(n):
        o=arr[1]+0x18*k;ep=q.resolve(arr[0],o);h='%016X'%q.u64(q.pbase(arr[0])+o+0x10)
        pp=q.resolve(*ep);np_=q.pu32(ep[0],ep[1]+8);S=set()
        for j in range(np_):
            po=pp[1]+0xd0*j;ir=q.resolve(pp[0],po+0x10);ni=q.pu32(pp[0],po+0x18)
            for i in range(ni if ir else 0):
                a,b,c,d=struct.unpack_from('<4I',q.d,q.pbase(ir[0])+ir[1]+16*i);S.add((tab[a] if a<len(tab) else a,b,c))
        out[h]=S
    return out
if __name__=='__main__':
    q=Pak(OV+'air-multi.pak');I=inputs(q)
    for h in sys.argv[1:]: print(h,sorted(I[h],key=str))
    al=set().union(*I.values());print(sorted(al,key=str))
