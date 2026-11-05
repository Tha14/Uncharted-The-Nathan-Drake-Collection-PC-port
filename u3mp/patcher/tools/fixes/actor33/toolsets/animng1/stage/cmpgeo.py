import sys,struct; sys.path.insert(0,"/home/claude/u3/paktools/stage"); sys.path.insert(0,"/home/claude/u3/paktools")
import numpy as np
from ndpak import Pak
from bggeom import *
from ps4bg import geometry
SLOT={1:0,0x11:1,0x12:5,0x15:4,2:2,3:3}
def merged(p,pr):
    subs=[]
    for mn,first,cnt in pr['groups']:
        bl=[]
        for bi in range(first,first+cnt):
            blk=pr['blocks'][bi]
            d=decode_block(p,*blk)
            d['fmt']=read_format(p,*p.resolve(blk[0],blk[1]+0x28))
            bl.append(d)
        subs.append(dict(mat=mn,first=first,blocks=bl))
    return subs
if __name__=='__main__':
    p=Pak(sys.argv[1]); q=Pak(sys.argv[2]); G=geometry(q)
    C=collection(p)
    S4={s['name'].split('.',1)[1]:s for s in G['subs']}
    worst={}
    for pi,pr in enumerate(C):
        for s in merged(p,pr):
            nm='Prototype%d.Submesh%d'%(pi,s['first'])
            r=S4.get(nm)
            if r is None: print('missing',nm); continue
            nv=sum(b['nverts'] for b in s['blocks'])
            if nv!=r['nv']: print(nm,'nv',nv,r['nv']); continue
            tris=[];base=0
            for b in s['blocks']:
                tris+= [(x+base,y+base,z+base) for x,y,z in b['tris']]; base+=b['nverts']
            idx=np.array(tris).reshape(-1)
            if len(idx)!=r['ni'] or (idx!=r['idx']).any(): print(nm,'idx mismatch',len(idx),r['ni'])
            for st in r['streams']:
                slot=st['slot']
                name={0:'pos',1:'uv0',2:'normal',3:'tangent',4:'color',5:'uv1'}[slot]
                if not all(name in b for b in s['blocks']): print(nm,'no',name); continue
                a=np.concatenate([b[name] for b in s['blocks']])
                e=st['data'][:,:a.shape[1]]
                err=np.abs(a-e).max(0)
                worst.setdefault(name,[]).append(err.max())
                if name in('tangent',) and st['data'].shape[1]==4:
                    w3=a[:,3] if a.shape[1]==4 else np.ones(len(a))
                    worst.setdefault('tangent_w',[]).append(np.abs(w3-st['data'][:,3]).max())
                if name in('normal',) : worst.setdefault('normal_w',[]).append(np.abs(1-st['data'][:,3]).max())
                if name=='color': worst.setdefault('color_w',[]).append(np.abs(1-st['data'][:,3]).max())
    for k,v in worst.items(): print(k,len(v),'max err',max(v),'median',np.median(v))
