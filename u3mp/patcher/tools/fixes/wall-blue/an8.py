from ilib import *
import pickle
sys.path.insert(0,'{{PAKTOOLS}}');sys.path.insert(0,'{{PAKTOOLS}}/stage')
import texcodec as tc,hashlib
os.chdir(M2)
from psarc_lib import Psarc
import p3prog
idx=pickle.load(open('p3idx.pkl','rb'));print([k for k in idx if 'air-multi' in k and 'shade' in k]);a,i,n=idx['air-multi-shade.pak'][-1];print(a,i,n)
d=Psarc(a).read(i);open(os.environ['TMPOUT'],'wb').write(d);p=Pak(d)
main=Pak('p3/air-multi.pak')
F=p3prog.facts(p,{});Fm=p3prog.facts(main,{})
diff=0
for k,g in F.items():
  gm=Fm.get(k)
  if not gm:print('only shade',k);continue
  if g['params']!=gm['params']:
    diff+=1;print('PARAM DIFF',k,{x:(g['params'].get(x),gm['params'].get(x)) for x in set(g['params'])|set(gm['params']) if g['params'].get(x)!=gm['params'].get(x)})
print('mats',len(F),len(Fm),'param diffs',diff)
for t in p.textures():
  if 'lightmaps/' in t['path']:
    kind,bpb,comp=tc.ps3_format(t);dd=p.vram_bytes(t['vramOffset'],t['vramSize']);img=tc.decode(dd[:tc.ps3_linear_size(t['width'],t['height'],1,bpb)],t['width'],t['height'],kind);print('ps3 shade',t['path'][-50:],kind,t['width'],img.reshape(-1,4).mean(0).round(1).tolist())
q=Pak(OV+'air-multi-shade.pak')
for t in q.textures():
  if 'lightmaps/' in t['path']:
    kind={0x47:'DXT1',0x4d:'DXT5'}[t['format']];dd=q.vram_bytes(t['vramOffset'],t['vramSize']);img=tc.decode(tc.ps4_mip0_linear(dd,t['width'],t['height'],8 if kind=='DXT1' else 16),t['width'],t['height'],kind);print('ov shade',t['path'][-50:],t['width'],img.reshape(-1,4).mean(0).round(1).tolist())
o,lm=instlight(q,'corrugated-main-blue:');print({k:np.concatenate(v).mean(0).round(3).tolist() for k,v in o.items()},lm)
lt=q.items_of('LIGHT_TABLE_1');print('ov shade lights',len(lt))
