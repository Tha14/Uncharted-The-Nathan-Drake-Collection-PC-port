from lib import *
import ps3params as PP, ps3mat
q=Pak(OV+'air-multi.pak');p=Pak(S+'emblem-batch/p3/air-multi.pak')
par=PP.all_material_params(p)
M3,T3=ps3mat.read_materials(p);mm={m['name']:m for m in M3}
mt=p.items_of('MATERIAL_TABLE_1')[0]
def samplers(n):
    pg,b=mt.page,mt.data_off;nmat=p.pu32(pg,b+0xc);ma=p.resolve(pg,b+0x20)
    for i in range(nmat):
        m=p.resolve(ma[0],ma[1]+4*i)
        if p.pstr(*p.resolve(*m))==n:
            out=[]
            for k,e,node in PP._walk(p,m):
                if e>0:break
                tbl=p.resolve(node[0],node[1]+0x14)
                if not tbl or not PP._is_param_table(p,tbl):continue
                tb=p.pbase(tbl[0])+tbl[1]
                for j in range(256):
                    rr=p.resolve(tbl[0],tbl[1]+0x10*j+0xc)
                    if not rr:break
                    nm=p.pstr(*rr)
                    if not nm or not nm.isprintable():break
                    idx,z,size,cls,kind,off=struct.unpack_from('>HHHBBI',p.d,tb+0x10*j)
                    if kind==1 and (nm,idx) not in out:out.append((nm,idx))
            return out
for m in mats(q):
    n=m['name'].split(',')[0]
    if not re.search('air-yem-ground-concrete-multi:polished|multi-ground-asphalt-broken:def|ground-sidewalk:def|sand-2-asphalt:def',n):continue
    k3=n.replace(':','/')
    print('\n==',n,'hash',m['hash']);print(' feats',[f for f in m['feats'] if re.match('COLOR|BLEND|AMBIENT|PARTIAL',f)])
    print(' P4',[(a,[round(x,3) for x in v[:c]]) for a,c,v in m['params']])
    print(' P3',{a:[round(x,3) for x in b] for a,b in par.get(k3,{}).items() if not re.search('Light|Sun|Shadow|Matrix|Camera|View|Fog',a)})
    print(' B4',[(a.replace('g_tNdFetch',''),b.rsplit('/',1)[-1]) for a,b in m['binds']])
    x=mm.get(k3);print(' T3',[T3[i]['name'].split('/')[-2] if i is not None else None for i in x['textures']] if x else None)
    print(' S3',samplers(k3))
