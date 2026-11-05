from lib import *
from PIL import Image
n=sys.argv[1];pat=sys.argv[2]
x0,x1,z0,z1=[float(v) for v in sys.argv[3:7]];sc=8
q=Pak(OV+n+'.pak');sb={s['name']:s for s in subs(q)};PR=protos(q);I=insts(q)
W=int((x1-x0)*sc);H=int((z1-z0)*sc);Z=np.full((H,W),-1e9);V=np.zeros((H,W,3))
for ins in I:
    if ins['proto'] is None:continue
    for sn in PR[ins['proto']]:
        s=sb.get(sn)
        if not s or pat not in s['mat']:continue
        st=streams(q,s);ix=indices(q,s);P=st[0]@ins['M'][:3,:3]+ins['M'][3,:3];C=st.get(4,np.ones((len(P),4)))
        for t in ix:
            a,b,c=P[t]
            pa=((a[0]-x0)*sc,(a[2]-z0)*sc);pb=((b[0]-x0)*sc,(b[2]-z0)*sc);pc=((c[0]-x0)*sc,(c[2]-z0)*sc)
            xa=int(max(min(pa[0],pb[0],pc[0]),0));xb=int(min(max(pa[0],pb[0],pc[0])+1,W-1));ya=int(max(min(pa[1],pb[1],pc[1]),0));yb=int(min(max(pa[1],pb[1],pc[1])+1,H-1))
            if xb<xa or yb<ya:continue
            den=(pb[1]-pc[1])*(pa[0]-pc[0])+(pc[0]-pb[0])*(pa[1]-pc[1])
            if abs(den)<1e-9:continue
            X,Y=np.meshgrid(np.arange(xa,xb+1)+.5,np.arange(ya,yb+1)+.5)
            u=((pb[1]-pc[1])*(X-pc[0])+(pc[0]-pb[0])*(Y-pc[1]))/den;v=((pc[1]-pa[1])*(X-pc[0])+(pa[0]-pc[0])*(Y-pc[1]))/den;w=1-u-v
            k=(u>=0)&(v>=0)&(w>=0)
            ca,cb,cc=C[t[0]],C[t[1]],C[t[2]]
            col=(u[...,None]*ca[:3]+v[...,None]*cb[:3]+w[...,None]*cc[:3])
            V[ya:yb+1,xa:xb+1][k]=col[k]
Image.fromarray((V*255).clip(0,255).astype('uint8')).save('vc.png');print(W,H)
