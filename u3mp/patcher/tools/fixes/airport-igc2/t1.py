import sys,os,glob,json,hashlib,io,contextlib
AM='../actor-materials';sys.path.insert(0,AM)
sys.dont_write_bytecode=True
from lib import *
import ps4actor_am as A,ps3geom
from ps3mat_am import read_materials
from build import role,pool,kinds_of_binds
HERE='{{PROJECT}}/scratch/airport-igc2';os.chdir(HERE)
def fg(q):
    f=A.tmpl_fg_info(q);return f[0],f[1],f[3],hashlib.sha1(f[2]).hexdigest()[:8]
s=S3()
for name in ['mp-hero-flynn-coop','mp-hero-lazarevic-coop','eddie']:
    src,d=s.get(name)
    if not d: print(name,'no ps3 in S3');continue
    p=Pak(d);mats,texs=read_materials(p);used=set()
    for v in ps3geom.prototypes(p).values():used|=set(A.ps3_block_materials(p,v['proto']))
    print('==',name,src,len(used))
    for m in mats:
        if m['name'] in used:print('  ',m['name'].split('/')[-2:],role(m['name']),[texs[i]['name'].split('/')[-1] for i in m['textures'] if i is not None][:8])
    f,l=find('actor33',name);q=Pak(f);print('   cur',l,fg(q))
    F=fg(q)
    c=[]
    for t in glob.glob(glob.escape(RET)+'/*.pak'):
        try:
            tq=Pak(t)
            if not tq.items_of('FOREGROUND_SECTION_2') or not tq.items_of('GEOMETRY_1'):continue
            if fg(tq)!=F:continue
            tm=A.tmpl_materials(tq)
        except Exception:continue
        ro={}
        for r in ('cloth','skin','hair','eye'):
            try:ro[r]=len(pool(tm,r))
            except Exception:ro[r]=-1
        nt=[m['name'].split(',')[0].split('/')[-1] for m in tm if 'DIFFUSE_LIGHT_CHEAP_CLOTH' in m['feats'] and 'COLOR_1_APPLY_INSTANCE_TINT' not in m['feats'] and not any('OVERLAY' in x or 'UV_3' in x for x in m['feats'])]
        c.append((os.path.basename(t)[:-4],ro,nt[:4]))
    print('   same-skeleton templates',len(c))
    for x in c:
        if x[2]:print('    ',x)
