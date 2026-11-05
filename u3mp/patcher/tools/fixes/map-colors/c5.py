from lib import *
from PIL import Image
n=sys.argv[1];x0,x1,z0,z1=[float(v) for v in sys.argv[2:6]];sc=6;ymax=2.5
q=Pak(OV+n+'.pak');sb={s['name']:s for s in subs(q)};I=insts(q)
W=int((x1-x0)*sc);H=int((z1-z0)*sc);Z=np.full((H,W),-1e9);A=np.zeros((H,W));R=np.zeros((H,W));L=np.zeros((H,W))
lmt=[t for t in q.textures() if 'indirect.rgbmax.' in t['path']][0];lm=load_tex(q,lmt).astype(float)/255;HL,WL=lm.shape[:2]
lmv=lm[...,:3].mean(2)*s2l(lm[...,3])*10
for ins in I:
    for x in isubs(q,ins):
        s=sb.get(x['name'])
        if not s or 8 not in x['st'] or 'shadowblocker' in s['mat']:continue
        st=streams(q,s);ix=indices(q,s);P=st[0]@ins['M'][:3,:3]+ins['M'][3,:3]
        C=x['st'][8];UV=x['st'].get(5)
        tl=P[ix];nr=np.cross(tl[:,1]-tl[:,0],tl[:,2]-tl[:,0])
        keep=(np.abs(nr[:,1])>0.5*np.linalg.norm(nr,axis=1))&(tl[:,:,1].max(1)<ymax)&(tl[:,:,0].max(1)>x0)&(tl[:,:,0].min(1)<x1)&(tl[:,:,2].max(1)>z0)&(tl[:,:,2].min(1)<z1)
        for t in ix[keep]:
            a,b,c=P[t]
            pa=((a[0]-x0)*sc,(a[2]-z0)*sc);pb=((b[0]-x0)*sc,(b[2]-z0)*sc);pc=((c[0]-x0)*sc,(c[2]-z0)*sc)
            xa=int(max(min(pa[0],pb[0],pc[0]),0));xb=int(min(max(pa[0],pb[0],pc[0])+1,W-1));ya=int(max(min(pa[1],pb[1],pc[1]),0));yb=int(min(max(pa[1],pb[1],pc[1])+1,H-1))
            if xb<xa or yb<ya:continue
            den=(pb[1]-pc[1])*(pa[0]-pc[0])+(pc[0]-pb[0])*(pa[1]-pc[1])
            if abs(den)<1e-9:continue
            X,Y=np.meshgrid(np.arange(xa,xb+1)+.5,np.arange(ya,yb+1)+.5)
            u=((pb[1]-pc[1])*(X-pc[0])+(pc[0]-pb[0])*(Y-pc[1]))/den;v=((pc[1]-pa[1])*(X-pc[0])+(pa[0]-pc[0])*(Y-pc[1]))/den;w=1-u-v
            k=(u>=0)&(v>=0)&(w>=0);z=u*a[1]+v*b[1]+w*c[1]
            ZZ=Z[ya:yb+1,xa:xb+1];kk=k&(z>ZZ);ZZ[kk]=z[kk]
            A[ya:yb+1,xa:xb+1][kk]=(u*C[t[0],3]+v*C[t[1],3]+w*C[t[2],3])[kk]
            R[ya:yb+1,xa:xb+1][kk]=(u*C[t[0],:3].mean()+v*C[t[1],:3].mean()+w*C[t[2],:3].mean())[kk]
            if UV is not None and UV[t].max()>0:
                uu=(u*UV[t[0],0]+v*UV[t[1],0]+w*UV[t[2],0]);vv=(u*UV[t[0],1]+v*UV[t[1],1]+w*UV[t[2],1])
                L[ya:yb+1,xa:xb+1][kk]=lmv[(vv.clip(0,1)*(HL-1)).astype(int),(uu.clip(0,1)*(WL-1)).astype(int)][kk]
            else:L[ya:yb+1,xa:xb+1][kk]=0
Image.fromarray(np.c_[(A*255).clip(0,255),(R*255).clip(0,255),(L*600).clip(0,255)].astype('uint8')).save('il.png');print(W,H,A.mean(),R.mean(),L.mean())
