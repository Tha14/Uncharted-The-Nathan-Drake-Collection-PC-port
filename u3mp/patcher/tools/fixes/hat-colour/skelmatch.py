import sys,os,pickle,hashlib
sys.dont_write_bytecode=True
AM='{{PROJECT}}/scratch/actor-materials'
sys.path.insert(0,AM)
from lib import *
import ps4actor as A
R=pickle.load(open(AM+'/retail.pkl','rb'))
k0=next(iter(R)); print('retail.pkl entries',len(R),'sample',k0,{k:(v if not isinstance(v,list) else v[:2]) for k,v in R[k0].items()})
def fg(q):
    f=A.tmpl_fg_info(q); return f[0],f[1],f[3],hashlib.sha1(f[2]).hexdigest()[:8]
for n in sys.argv[1:]:
    f,_=find('actor33',n); q=Pak(f); s=fg(q); print('==',n,[hex(s[0]),hex(s[1]),s[2],s[3]])
    for b,r in R.items():
        if (r['skel'],r['hier'],r['jh'])==(s[0],s[1],s[3]):
            tq=Pak(RET+'/'+b+'.pak'); ms=A.tmpl_materials(tq)
            print('  ',b,'nj',r.get('nj'),'vram %#x'%tq.u32(0x20))
            for m in ms:
                fe=set(m['feats']); tag=('TINT' if 'COLOR_1_APPLY_INSTANCE_TINT' in fe else '    ')+(' MASK' if 'COLOR_1_APPLY_COLOR_TINT_WITH_MASK' in fe else '     ')+(' OVL' if 'COLOR_1_APPLY_OVERLAY_TEXTURE' in fe else '    ')+(' HAIRA' if any('HairAlpha' in x[0] for x in m['binds']) else '      ')
                print('     ',tag,m['name'].split(',')[0],'x3',m['x3'],[x[0][9:] for x in m['binds']])
