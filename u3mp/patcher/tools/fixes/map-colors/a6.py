from lib import *
from PIL import Image
n=sys.argv[1];ymax=float(sys.argv[2]) if len(sys.argv)>2 else 2.5
q=Pak(OV+n+'.pak');sb={s['name']:s for s in subs(q)};PR=protos(q);I=insts(q)
T=[]
matidx={};
for ins in I:
    if ins['proto'] is None:continue
    for sn in PR[ins['proto']]:
        s=sb.get(sn)
        if not s:continue
        st=streams(q,s);ix=indices(q,s)
        P=st[0]@ins['M'][:3,:3]+ins['M'][3,:3]
        m=s['mat'].split(',')[0];mi=matidx.setdefault(m,len(matidx))
        a,b,c=P[ix[:,0]],P[ix[:,1]],P[ix[:,2]];nr=np.cross(b-a,c-a);ok=np.abs(nr[:,1])>0.5*np.linalg.norm(nr,axis=1)
        ok&=np.max(np.c_[a[:,1],b[:,1],c[:,1]],1)<ymax
        for k in np.nonzero(ok)[0]:T.append((a[k],b[k],c[k],mi))
print(len(T),len(matidx))
pts=np.array([t[0] for t in T]);lo=pts.min(0);hi=pts.max(0);print(lo,hi)
sc=4.0;W=int((hi[0]-lo[0])*sc)+2;H=int((hi[2]-lo[2])*sc)+2
Z=np.full((H,W),-1e9);ID=np.full((H,W),-1,int)
for a,b,c,mi in T:
    pa=np.array([(a[0]-lo[0])*sc,(a[2]-lo[2])*sc]);pb=np.array([(b[0]-lo[0])*sc,(b[2]-lo[2])*sc]);pc=np.array([(c[0]-lo[0])*sc,(c[2]-lo[2])*sc])
    x0,x1=int(max(min(pa[0],pb[0],pc[0]),0)),int(min(max(pa[0],pb[0],pc[0])+1,W-1));y0,y1=int(max(min(pa[1],pb[1],pc[1]),0)),int(min(max(pa[1],pb[1],pc[1])+1,H-1))
    if x1<x0 or y1<y0:continue
    X,Y=np.meshgrid(np.arange(x0,x1+1)+.5,np.arange(y0,y1+1)+.5)
    den=(pb[1]-pc[1])*(pa[0]-pc[0])+(pc[0]-pb[0])*(pa[1]-pc[1])
    if abs(den)<1e-9:continue
    u=((pb[1]-pc[1])*(X-pc[0])+(pc[0]-pb[0])*(Y-pc[1]))/den;v=((pc[1]-pa[1])*(X-pc[0])+(pa[0]-pc[0])*(Y-pc[1]))/den;w=1-u-v
    ins_=(u>=0)&(v>=0)&(w>=0);z=u*a[1]+v*b[1]+w*c[1]
    sub=Z[y0:y1+1,x0:x1+1];u_=ins_&(z>sub);sub[u_]=z[u_];ID[y0:y1+1,x0:x1+1][u_]=mi
names={v:k for k,v in matidx.items()}
c=collections.Counter(ID.ravel().tolist());
print('cover m2:');rows=[]
for k,v in c.most_common(30):
    if k>=0:print(round(v/sc/sc),names[k])
np.savez('top-%s.npz'%n,Z=Z,ID=ID,lo=lo,sc=sc);json.dump(names,open('top-%s.json'%n,'w'))
rng=np.random.RandomState(3);pal=rng.randint(40,255,(len(matidx)+1,3));pal[-1]=(0,0,0)
for k,v in names.items():
    if 'shadowblocker' in v:pal[k]=(255,0,0)
Image.fromarray(pal[ID].astype('uint8')).save('top-%s.png'%n)
print(W,H)
