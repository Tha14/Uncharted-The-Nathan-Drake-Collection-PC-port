from lib import *
import glob
for f in sorted(glob.glob('p3/*.pak')):
    p=Pak(f)
    try:C=G.collection(p)
    except Exception as e:print(f,'ERR',e);continue
    tot=0
    for pi,c in enumerate(C):
        hits=[(m,fi,cnt) for m,fi,cnt in c['groups'] if 'emblem' in m]
        if not hits:continue
        ms=collections.Counter()
        for m,fi,cnt in hits:
            nv=nt=0
            bb=[]
            for k in range(fi,fi+cnt):
                pg,o=c['blocks'][k];d=G.decode_block(p,pg,o);nv+=d['nverts'];nt+=len(d['tris']);bb.append(d['pos']);attrs=sorted(k2 for k2 in d if k2 not in('raw','order','tris','nverts'))
            a=np.vstack(bb)
            print(os.path.basename(f),'proto',pi,'lodhdr',p.d[p.pbase(c['hdr'][0])+c['hdr'][1]],'ninst',len(c['insts']),'nblk',cnt,'v',nv,'t',nt,m.split(',')[0][-50:],attrs,'bb',np.round(a.min(0),1).tolist(),np.round(a.max(0),1).tolist(),'allgroups',len(c['groups']))
        tot+=1
    print(f,'protos',len(C),'emblemprotos',tot)
