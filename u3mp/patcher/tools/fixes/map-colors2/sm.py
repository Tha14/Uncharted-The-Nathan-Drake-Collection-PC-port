from lib import *
def subs(q):
    g=q.items_of('GEOMETRY_1')[0];pg,b=g.page,g.data_off
    n=q.u64(q.pbase(pg)+b+8);sr=q.resolve(pg,b+0x10);out=collections.defaultdict(list)
    for si in range(n):
        so=sr[1]+0x88*si
        mr=q.resolve(sr[0],so+0x48)
        if not mr: continue
        name=q.pstr(*mr).split(',')[0]
        sig=[]
        for which,co in ((0x30,0x28),(0x38,0x2c)):
            ns=q.pu32(sr[0],so+co);st=q.resolve(sr[0],so+which);L=[]
            for k in range(ns if st else 0):
                e=st[1]+0x18*k;f=q.resolve(st[0],e+8);fb=q.pbase(f[0])+f[1]
                L.append((q.d[fb],q.pu32(st[0],e)>>16,q.d[fb+3]))
            sig.append(tuple(L))
        ni=struct.unpack_from('<I',q.d,q.pbase(sr[0])+so+0x24)[0]
        out[name].append((tuple(sig),ni))
    return out
if __name__=='__main__':
    q=Pak(OV+'air-multi.pak');U=subs(q)
    for k,v in U.items():
        if re.search('hazard|striped|concrete-multi|-red:|runway-striped',k): print(k.split('/')[-1],len(v),sum(x[1] for x in v)//3,set(x[0] for x in v))
    T={t['path'].split('/')[-2]:t for t in q.textures()};print('road-striped' ,'air-yem-m-ground-road-striped-color.tga' in T,'hazard','air-yem-m-ground-concrete-hazard-color.tga' in T)
