#!/usr/bin/env python3
"""Clear the 'own lightmap UVs' flag (instsub +0x20 bit 0x100) on instsubs that hold triangles with all-zero lightmap UVs.
Cause: ps4stage merges up to 20 PS3 blocks per submesh and sets the flag if ANY block has lightmap UVs; blocks without
them then sample lightmap texel (0,0) instead of their vertex light (Airstrip: 0.019 vs 0.33 = black floor).
Same-size edit, 4 bytes per instsub. Those instsubs fall back to vertex light, which their PS3 UV-less blocks use anyway;
their few UV-carrying blocks lose lightmap detail (reported as lost_lm_m2).
usage: apply_mapcolor.py                 dry run, lists every change
       apply_mapcolor.py --apply         back up to work/backup-mapcolor, then write
       apply_mapcolor.py --revert        restore from the backup
options: --root DIR (overlay pak dir)  --backup DIR  --only a.pak,b.pak
Run only while the game is stopped."""
from lib import *
import argparse,glob,shutil,hashlib
ap=argparse.ArgumentParser();ap.add_argument('--apply',action='store_true');ap.add_argument('--revert',action='store_true')
ap.add_argument('--root',default=OV);ap.add_argument('--backup',default=R+'work/backup-mapcolor');ap.add_argument('--only',default='')
A=ap.parse_args()
md5=lambda b:hashlib.md5(b).hexdigest()
MAN=os.path.join(A.backup,'manifest.json')
if A.revert:
    m=json.load(open(MAN))
    for f,e in m.items():
        b=open(os.path.join(A.backup,f),'rb').read();assert md5(b)==e['md5_before'],f
        shutil.copyfile(os.path.join(A.backup,f),os.path.join(A.root,f));print('restored',f)
    sys.exit(0)
def plan(path):
    q=Pak(path)
    if not q.items_of('GEOMETRY_1'):return []
    sb={s['name']:s for s in subs(q)};out=[]
    for ins in insts(q):
        t=ins['t'];a=q.pbase(t[0])+t[1];ns=struct.unpack_from('<I',q.d,a+0x20)[0];r=q.resolve(t[0],t[1]+0x30)
        X=isubs(q,ins)
        for k,x in enumerate(X):
            if not x['flags']&0x100 or 5 not in x['st']:continue
            s=sb.get(x['name'])
            if not s or len(x['st'][5])<s['nv']:continue
            ix=indices(q,s);tz=(x['st'][5].max(1)==0)[ix].all(1)
            if not tz.any():continue
            P=streams(q,s)[0]@ins['M'][:3,:3]+ins['M'][3,:3];tl=P[ix];ar=np.linalg.norm(np.cross(tl[:,1]-tl[:,0],tl[:,2]-tl[:,0]),axis=1)/2
            off=q.pbase(r[0])+r[1]+0x50*k+0x20
            assert struct.unpack_from('<I',q.d,off)[0]==x['flags']
            out.append(dict(off=off,flags=x['flags'],inst=ins['i'],sub=x['name'],mat=s['mat'].split(',')[0],zero_m2=float(ar[tz].sum()),lost_lm_m2=float(ar[~tz].sum())))
    return out
fs=[f for f in sorted(glob.glob(glob.escape(A.root)+'/*.pak')) if not f.endswith('-ingame12.pak') and os.path.getsize(f)>=200000]
if A.only:fs=[f for f in fs if os.path.basename(f) in A.only.split(',')]
man={};
for f in fs:
    L=plan(f)
    if not L:continue
    n=os.path.basename(f);print('%s: %d instsubs, zero-UV %d m2, lightmap detail lost %d m2'%(n,len(L),sum(e['zero_m2'] for e in L),sum(e['lost_lm_m2'] for e in L)))
    for e in L:print('   inst %d %s @%#x %#x->%#x %s zero %.0f lost %.0f'%(e['inst'],e['sub'],e['off'],e['flags'],e['flags']&~0x100,e['mat'],e['zero_m2'],e['lost_lm_m2']))
    if A.apply:
        os.makedirs(A.backup,exist_ok=True);bk=os.path.join(A.backup,n);d=bytearray(open(f,'rb').read())
        if not os.path.exists(bk):shutil.copyfile(f,bk)
        man[n]=dict(md5_before=md5(open(bk,'rb').read()),edits=L)
        for e in L:struct.pack_into('<I',d,e['off'],e['flags']&~0x100)
        tmp=f+'.tmp';open(tmp,'wb').write(d);os.replace(tmp,f);man[n]['md5_after']=md5(bytes(d))
if A.apply:
    old=json.load(open(MAN)) if os.path.exists(MAN) else {}
    for k,v in old.items():man.setdefault(k,v)
    json.dump(man,open(MAN,'w'),indent=1);print('applied; manifest',MAN)
else:print('DRY RUN: nothing written. Add --apply to write.')
