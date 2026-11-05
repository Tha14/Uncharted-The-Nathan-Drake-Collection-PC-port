from lib import *
import ps4actor,ps3geom,json
s=S3()
def subs(q):
    g=q.items_of('GEOMETRY_1')[0];pg,b=g.page,g.data_off
    ns=q.pu32(pg,b+8);r=q.resolve(pg,b+0x10);c=collections.Counter()
    for i in range(ns):
        x=q.resolve(r[0],r[1]+0x88*i+0x48); c[q.pstr(*x).split(',')[0] if x else None]+=1
    return ns,c
def ps3n(d):
    p=Pak(d);m=set()
    for v in ps3geom.prototypes(p).values(): m|=set(ps4actor.ps3_block_materials(p,v['proto']))
    return m
seen={};out=[]
for l in LAY[:4]:
    D=l+'/u3data/build/main/actor33/ps4'
    if not os.path.isdir(D): continue
    for fn in sorted(os.listdir(D)):
        if not fn.endswith('.pak') or fn in seen: continue
        seen[fn]=os.path.basename(l)
        n=fn[:-4]
        al=s.all(n)
        if not al: continue
        try:
            q=Pak(D+'/'+fn)
            if not q.items_of('GEOMETRY_1'): continue
            ns,c=subs(q)
        except Exception as e: print('ps4err',fn,e);continue
        vers=[]
        for src,d in al:
            try: vers.append((src,len(ps3n(d))))
            except Exception as e: vers.append((src,'err'))
        top=vers[0][1]
        if isinstance(top,int) and len(c)<top: out.append(dict(n=n,layer=seen[fn],subs=ns,ps4=len(c),ps3=top,src=vers))
json.dump(out,open('scan.json','w'),indent=0)
print(len(seen),len(out))
for r in sorted(out,key=lambda r:r['ps4']-r['ps3']): print(r)
