from lib import *
from PIL import Image,ImageDraw
n=sys.argv[1];x0,x1,z0,z1=[float(v) for v in sys.argv[2:6]];sc=float(sys.argv[6]) if len(sys.argv)>6 else 8;ymax=2.5
q=Pak(OV+n+'.pak');sb={s['name']:s for s in subs(q)};I=insts(q)
W=int((x1-x0)*sc);H=int((z1-z0)*sc);Z=np.full((H,W),-1e9);C=np.zeros((H,W,3),'uint8')
tot=collections.Counter();mat=collections.Counter()
for ins in I:
    for x in isubs(q,ins):
        s=sb.get(x['name'])
        if not s or 'shadowblocker' in s['mat']:continue
        st=streams(q,s);ix=indices(q,s);P=st[0]@ins['M'][:3,:3]+ins['M'][3,:3]
        uv=x['st'].get(5);lmf=bool(x['flags']&0x100)
        tl=P[ix];nr=np.cross(tl[:,1]-tl[:,0],tl[:,2]-tl[:,0]);ar=np.linalg.norm(nr,axis=1)/2
        keep=(np.abs(nr[:,1])>0.5*(2*ar+1e-9))&(tl[:,:,1].max(1)<ymax)&(tl[:,:,0].max(1)>x0)&(tl[:,:,0].min(1)<x1)&(tl[:,:,2].max(1)>z0)&(tl[:,:,2].min(1)<z1)
        for t in ix[keep]:
            a,b,c=P[t]
            uz=uv is not None and (uv[t].max(1)==0).all()
            col=(255,0,0) if lmf and uz else ((60,60,220) if lmf else (0,160,0))
            pa=((a[0]-x0)*sc,(a[2]-z0)*sc);pb=((b[0]-x0)*sc,(b[2]-z0)*sc);pc=((c[0]-x0)*sc,(c[2]-z0)*sc)
            xa=int(max(min(pa[0],pb[0],pc[0]),0));xb=int(min(max(pa[0],pb[0],pc[0])+1,W-1));ya=int(max(min(pa[1],pb[1],pc[1]),0));yb=int(min(max(pa[1],pb[1],pc[1])+1,H-1))
            if xb<xa or yb<ya:continue
            den=(pb[1]-pc[1])*(pa[0]-pc[0])+(pc[0]-pb[0])*(pa[1]-pc[1])
            if abs(den)<1e-9:continue
            X,Y=np.meshgrid(np.arange(xa,xb+1)+.5,np.arange(ya,yb+1)+.5)
            u=((pb[1]-pc[1])*(X-pc[0])+(pc[0]-pb[0])*(Y-pc[1]))/den;v=((pc[1]-pa[1])*(X-pc[0])+(pa[0]-pc[0])*(Y-pc[1]))/den;w=1-u-v
            k=(u>=0)&(v>=0)&(w>=0);z=u*a[1]+v*b[1]+w*c[1]
            ZZ=Z[ya:yb+1,xa:xb+1];kk=k&(z>ZZ);ZZ[kk]=z[kk];C[ya:yb+1,xa:xb+1][kk]=col
Image.fromarray(C).save('lmflag.png');print(W,H,'red px',(C[...,0]==255).sum()/sc/sc,'blue',(C[...,2]==220).sum()/sc/sc,'green',(C[...,1]==160).sum()/sc/sc)
