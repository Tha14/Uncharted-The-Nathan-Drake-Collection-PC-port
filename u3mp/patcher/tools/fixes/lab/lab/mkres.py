import sys,os,json,gzip,struct
T=r"{{SCRATCH}}/gh2/u3mp/paktools"
sys.path.insert(0,T)
from ndpak import Pak
R="{{PS4}}/u3data/build/main/pak23/ps4/"
def mats(q):
  out=[]
  for g in q.items_of('GEOMETRY_1'):
    pg,b=g.page,g.data_off;flags=q.pu32(pg,b+4);n=q.u64(q.pbase(pg)+b+0x48);r=q.resolve(pg,b+0x50)
    if not r:continue
    mp,mo=r
    for i in range(n):
      o=mo+i*0x48;S=lambda off:q.pstr(*q.resolve(mp,o+off)) if q.resolve(mp,o+off) else None
      nf,npar,nb,x3=struct.unpack_from('<4I',q.d,q.pbase(mp)+o+0x30);feats=[];fr=q.resolve(mp,o+0x10)
      for k in range(nf if fr else 0):feats.append(q.pstr(*q.resolve(fr[0],fr[1]+8*k)))
      binds=[];br=q.resolve(mp,o+0x28)
      for k in range(nb if br else 0):
        bo=br[1]+0x28*k;binds.append((q.pstr(*q.resolve(br[0],bo)),q.pstr(*q.resolve(br[0],bo+8))))
      out.append(dict(n=S(0),s=S(8),h=S(0x18),f=feats,b=binds,fl=flags))
  return out
a=gzip.open(sys.argv[1]+'/ps4mats.jsonl.gz','wt');t=open(sys.argv[1]+'/ps4tex.tsv','w',encoding='utf-8');n=0;e=0
for fn in sorted(os.listdir(R)):
  if not fn.endswith('.pak') or fn.endswith('-ingame12.pak'):continue
  try:
    q=Pak(R+fn);a.write(json.dumps(dict(pak=fn,size=len(q.d),nfx=len(q.items_of('EFFECT')),mats=mats(q)))+'\n')
    for x in q.textures():
      t.write('\t'.join(map(str,[fn,x.get('path',''),x.get('width',0),x.get('height',0),x.get('format',0),x.get('mips',0),x.get('vramOffset',0),x.get('vramSize',0),0,0,0]))+'\n')
    n+=1
  except Exception as ex:
    e+=1;a.write(json.dumps(dict(pak=fn,err=str(ex)))+'\n')
a.close();t.close();print('paks',n,'errors',e)
