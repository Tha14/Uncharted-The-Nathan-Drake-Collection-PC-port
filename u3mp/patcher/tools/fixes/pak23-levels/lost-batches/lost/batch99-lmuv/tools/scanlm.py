"""scanlm.py OUT.jsonl PAK...: instsubs with the own-lightmap flag (+0x20 bit 0x100) whose per-instance lightmap-uv stream
(slot 5, type 0x22) holds only integer rows outside [0,1] (PS3 placeholder stream of a vertex-lit block).
Those are the writes batch99-lmuv makes (flag 0x100 cleared)."""
import sys,os,json,hashlib,struct
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from ndpak import Pak
from ps4bg import streams
def scan(path):
  q=Pak(path);its=q.items_of('GEOMETRY_1')
  if not its: return None
  g=its[0];pg,b=g.page,g.data_off;il=q.resolve(pg,b+0x30);n=q.u64(q.pbase(pg)+b+0x28)
  if not il: return []
  nsub=q.u64(q.pbase(pg)+b+8);sref=q.resolve(pg,b+0x10);mat={}
  for k in range(nsub):
    o=sref[1]+0x88*k;nm=q.pstr(*q.resolve(sref[0],o+8));mr=q.resolve(sref[0],o+0x48);mat[nm]=(q.pstr(*mr) if mr else '').split(',')[0]
  out=[];tot=0
  for i in range(n):
    r=q.resolve(il[0],il[1]+8*i);ns=q.pu32(r[0],r[1]+0x20);sr=q.resolve(r[0],r[1]+0x30)
    if not sr: continue
    for k in range(ns):
      o=sr[1]+0x50*k;f=q.pu32(sr[0],o+0x20)
      if not f&0x100: continue
      tot+=1
      nst=q.pu32(sr[0],o+0x10);ref=q.resolve(sr[0],o+0x18)
      if not ref: continue
      for s in streams(q,ref,nst):
        if s['slot']==5 and s['type']==0x22 and s['data'] is not None:
          d=s['data']
          import numpy as np
          if (d.max()>1.001 or d.min()<-0.001) and np.all(np.abs(d-np.round(d))<1e-4):
            out.append([i,k,q.pbase(sr[0])+o+0x20,f,f&~0x100,mat.get(q.pstr(*q.resolve(sr[0],o)),'?').split('/')[-1],float(d.max()),float((d==0).all(1).mean())])
  return out,tot
if __name__=='__main__':
  outp=sys.argv[1];done=set()
  if os.path.exists(outp):
    for l in open(outp): done.add(json.loads(l)['path'])
  for p in sys.argv[2:]:
    if p in done: continue
    try:
      r=scan(p)
      if r is None: rec=dict(path=p,geom=False)
      else:
        W,tot=r;rec=dict(path=p,geom=True,flagged=tot,bad=W,md5=hashlib.md5(open(p,'rb').read()).hexdigest() if W else None)
    except Exception as e: rec=dict(path=p,err=repr(e))
    open(outp,'a').write(json.dumps(rec)+'\n');print(os.path.basename(p),rec.get('flagged'),len(rec.get('bad') or []),flush=True)
