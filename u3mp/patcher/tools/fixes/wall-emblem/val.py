from lib import *
import ps4check,ps4verify,crashcheck,glob,subprocess,skel,io,contextlib
for f in sorted(glob.glob('out/actor33/ps4/*.pak'))[:]:
    art=os.path.basename(f)[:-4]
    print('==',art)
    e=ps4check.check(f,quiet=True);print(' ps4check',e if not isinstance(e,tuple) else e[0])
    e=ps4verify.verify(f,name=art,quiet=True);print(' ps4verify',e if not isinstance(e,tuple) else e[0],e[1] if isinstance(e,tuple) and len(e)>1 else '')
    r=crashcheck.check(f);print(' crashcheck',r if not isinstance(r,dict) else {k:r[k] for k in list(r)[:6]})
for f in ['out/pak23/ps4/air-multi-ingame12.pak','out/pak23/ps4/air-multi.pak']:
    print('==',f);r=crashcheck.check(f);print(str(r)[:800])
q=Pak('out/pak23/ps4/air-multi-ingame12.pak')
from sg import spawners,spawner_info
n=0
for it,(pg,o) in spawners(q):
    ar=q.resolve(pg,o+0x48)
    if ar and 'emblem' in q.pstr(*ar):
        i=spawner_info(q,(pg,o));print(i['name'],np.round(i['pos'][:3],2).tolist(),hex(i['sid']),q.pstr(*ar),len(i['tags']),hex(q.pu32(pg,o+0x40)));n+=1
it=q.items_of('SPAWNER_GROUP')[0];b=q.pbase(it.page)+it.data_off;print('count',struct.unpack_from('<I',q.d,b+0x10),struct.unpack_from('<I',q.d,b+0x20))
srt=q.resolve(it.page,it.data_off+0x18);ks=[q.pu32(srt[0],srt[1]+16*k) for k in range(struct.unpack_from('<I',q.d,b+0x20)[0])];print('sorted',ks==sorted(ks),len(set(ks))==len(ks))
sk=Pak("{{PS4}}/u3data/build/main/actor33/ps4/cargo-plane-int-moving-multi.pak");j=sk.items_of('JOINT_HIERARCHY')[0];bb=sk.pbase(j.page)+j.data_off
fl=np.frombuffer(sk.d[bb:bb+j.size],'<f4');print('JH floats near 1:',[ (i,round(float(x),3)) for i,x in enumerate(fl) if 0.5<abs(x)<3000 and i<210][:40])
