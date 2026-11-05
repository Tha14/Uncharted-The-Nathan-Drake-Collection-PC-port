"""ps3mat.read_materials with page tracking and a bound on the binding count (paktools version assumes one page
and loops for minutes when a block count is not a count). Scans every slot of every group for texture refs."""
import sys
sys.path.insert(0,'{{PAKTOOLS}}')
from ndpak import sid
def read_materials(p):
    mts=p.items_of('MATERIAL_TABLE_1')
    if not mts: return [],[]
    mt=mts[0];pg,b=mt.page,mt.data_off
    if p.pu32(pg,b)!=0x79: raise ValueError('version')
    ntex,nmat=p.pu32(pg,b+4),p.pu32(pg,b+0xc)
    tp,texh=p.resolve(pg,b+0x18);mp,mats=p.resolve(pg,b+0x20);np_,names=p.resolve(pg,b+0x30)
    textures=[]
    for i in range(ntex):
        r=p.resolve(np_,names+4*i)
        textures.append(dict(hash=p.pu32(tp,texh+4*i),name=p.pstr(*r)))
    out=[]
    for i in range(nmat):
        a=p.resolve(mp,mats+4*i)
        name=p.pstr(*p.resolve(*a))
        rec=dict(name=name,sid=p.pu32(a[0],a[1]+0x18),textures=[],slots=p.pu32(a[0],a[1]+0xc),groups=0)
        r=p.resolve(a[0],a[1]+0x10)
        if r:
            seen=set()
            for slot in range(6):                # group 0 = this material; union of its slots
                node=p.resolve(r[0],r[1]+4*slot)
                if not node: continue
                bb=p.resolve(node[0],node[1]+0x10)
                if not bb: continue
                n=p.pu32(bb[0],bb[1]+4)
                if n>32: continue
                for k in range(n):
                    rr=p.resolve(bb[0],bb[1]+0x50+0x10*k)
                    if rr and rr[0]==tp and texh<=rr[1]<texh+4*ntex:
                        i_=(rr[1]-texh)//4
                        if i_ not in seen and 'missingtexture' not in textures[i_]['name']:
                            seen.add(i_); rec['textures'].append(i_)
        out.append(rec)
    return out,textures
