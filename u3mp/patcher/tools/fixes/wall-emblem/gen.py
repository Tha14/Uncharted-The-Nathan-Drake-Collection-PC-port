from lib import *
from psarc_lib import Psarc
import tempfile,json,glob
sys.setrecursionlimit(10000)
U="{{PS3_DISC}}/"
srcs=[U+'build/main/pak23.psarc',"{{PS3_UPDATE}}/dlc-patch18.psarc","{{PS3_UPDATE}}/dlc-patch13.psarc"]
for root,ds,fs in os.walk("{{PS3_DATA2}}/build/main/"):
    for f in fs:
        if f.endswith('.psarc'):srcs.append(os.path.join(root,f))
seen={};res={}
OV="{{PROJECT}}/work/overlay-r3/u3data/build/main/pak23/ps4/"
RET="{{PS4}}/u3data/build/main/pak23/ps4/"
for s in srcs:
    try:a=Psarc(s)
    except Exception as e:print('bad',s,e);continue
    for i,n in enumerate(a.names):
        b=n.split('/')[-1]
        if not b.endswith('.pak') or 'ingame' in b or b in seen:continue
        try:d=a.read(i+1)
        except Exception as e:continue
        if b'-emblem' not in d:continue
        seen[b]=1
        fn='_g.pak';open(fn,'wb').write(d)
        p=Pak(fn);tot=[]
        try:C=G.collection(p)
        except Exception as e:print(b,'ERR',e);continue
        for pi,c in enumerate(C):
            for m,fi,cnt in c['groups']:
                if 'emblem' in m:
                    uv=[];nt=0
                    for k in range(fi,fi+cnt):
                        dd=G.decode_block(p,*c['blocks'][k]);uv.append(dd.get('uv1'));nt+=len(dd['tris'])
                    u=np.vstack([x for x in uv if x is not None]) if any(x is not None for x in uv) else None
                    tot.append(dict(m=m.split('/')[-2],insts=len(c['insts']),tris=nt,uvok=bool(u is not None and u.min()>=-0.2 and u.max()<=1.6)))
        if tot:
            res[b]=dict(n=len(tot),inst=sum(t['insts'] for t in tot),tris=sum(t['tris'] for t in tot),hasuv=all(t['uvok'] for t in tot),ov=os.path.exists(OV+b),ret=os.path.exists(RET+b),multi_inst=sum(1 for t in tot if t['insts']>1),board=[t['m'] for t in tot if 'blackboard' in t['m'] or t['insts']>1][:3])
            print(b,res[b],flush=True)
json.dump(res,open('gen.json','w'),indent=1)
print('MAPS',len(res),'submeshes',sum(r['n'] for r in res.values()),'instances',sum(r['inst'] for r in res.values()),'tris',sum(r['tris'] for r in res.values()),'in overlay',sum(r['ov'] for r in res.values()),'retail',sum(r['ret'] for r in res.values()),'uv ok',sum(r['hasuv'] for r in res.values()))
