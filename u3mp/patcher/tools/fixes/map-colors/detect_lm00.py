#!/usr/bin/env python3
"""Detector: instsubs flagged 'own lightmap UVs' (+0x20 & 0x100) whose triangles have all-zero lightmap UVs.
Those triangles sample lightmap texel (0,0) in the lightmap pass instead of their vertex light (PS3 uses per-block programs).
usage: detect_lm00.py <out.tsv> <dir or pak>...   Read-only."""
from lib import *
import glob
def scan(path):
    q=Pak(path)
    if not q.items_of('GEOMETRY_1'):return None
    sb={s['name']:s for s in subs(q)};I=insts(q)
    lts=[t for t in q.textures() if 'indirect.rgbmax.' in t['path']]
    lm00=None
    if lts:
        t=lts[0];w,h=t['width'],t['height'];c=load_tex(q,t)[0,0].astype(float)/255;lm00=float(c[:3].mean()*s2l(c[3])*10)
    n=fl=whole=mixed=0;zA=okA=0.0;ils=[];mats=collections.Counter();lst=[]
    for ins in I:
        for x in isubs(q,ins):
            n+=1
            if not x['flags']&0x100:continue
            fl+=1;s=sb.get(x['name'])
            if not s or 5 not in x['st']:continue
            st=streams(q,s);ix=indices(q,s);uv=x['st'][5]
            if len(uv)<s['nv']:continue
            P=st[0]@ins['M'][:3,:3]+ins['M'][3,:3];tl=P[ix];ar=np.linalg.norm(np.cross(tl[:,1]-tl[:,0],tl[:,2]-tl[:,0]),axis=1)/2
            vz=uv.max(1)==0;tz=vz[ix].all(1)
            if not tz.any():okA+=float(ar.sum());continue
            a=float(ar[tz].sum());b=float(ar[~tz].sum());zA+=a;okA+=b
            if tz.all():whole+=1
            else:mixed+=1
            m=s['mat'].split(',')[0];mats[m]+=a
            if 8 in x['st']:ils.append(float(x['st'][8][vz,:3].astype(float).mean()))
            lst.append((ins['i'],x['name'],round(a,1),round(b,1),m))
    return dict(n=n,fl=fl,whole=whole,mixed=mixed,zA=round(zA),okA=round(okA),lm00=lm00,il=round(float(np.mean(ils)),3) if ils else None,top=[(k.rsplit('/',1)[-1],round(v)) for k,v in mats.most_common(6)],lst=lst)
if __name__=='__main__':
    out=open(sys.argv[1],'w',encoding='utf-8');fs=[]
    for a in sys.argv[2:]:
        fs+= sorted(glob.glob(glob.escape(a)+'/*.pak')) if os.path.isdir(a) else [a]
    out.write('pak\tinstsubs\tflagged\twhole\tmixed\tzero_area\tok_area\tlm00\tilight\ttop\n')
    for f in fs:
        if f.endswith('-ingame12.pak') or os.path.getsize(f)<200000:continue
        try:r=scan(f)
        except Exception as e:out.write('%s\tERR %r\n'%(os.path.basename(f),e));out.flush();continue
        if not r:continue
        out.write('\t'.join(str(v) for v in(os.path.basename(f),r['n'],r['fl'],r['whole'],r['mixed'],r['zA'],r['okA'],r['lm00'],r['il'],r['top']))+'\n');out.flush()
