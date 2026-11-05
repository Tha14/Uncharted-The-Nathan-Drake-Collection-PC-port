from lib import *
from PIL import Image
q=Pak(OV+'air-multi.pak');sb={s['name']:s for s in subs(q)};I=insts(q)
x0,x1,z0,z1=-60,50,-35,50;sc=6
W=int((x1-x0)*sc);H=int((z1-z0)*sc);Zt=np.full((H,W),-1e9);Zb=np.full((H,W),1e9)
ins=[i for i in I if i['i']==1][0]
print(ins['M'].round(3).tolist())
hist=collections.Counter()
for x in isubs(q,ins):
    s=sb.get(x['name'])
    if not s:continue
    print(x['name'][-22:],s['mat'].split(',')[0].rsplit('/',1)[-1][:40],s['ni']//3,hex(x['flags']))
    if 'shadowblocker' not in s['mat']:continue
    st=streams(q,s);ix=indices(q,s);P=st[0]@ins['M'][:3,:3]+ins['M'][3,:3]
    for t in ix:
        a,b,c=P[t]
        pa=((a[0]-x0)*sc,(a[2]-z0)*sc);pb=((b[0]-x0)*sc,(b[2]-z0)*sc);pc=((c[0]-x0)*sc,(c[2]-z0)*sc)
        xa=int(max(min(pa[0],pb[0],pc[0]),0));xb=int(min(max(pa[0],pb[0],pc[0])+1,W-1));ya=int(max(min(pa[1],pb[1],pc[1]),0));yb=int(min(max(pa[1],pb[1],pc[1])+1,H-1))
        if xb<xa or yb<ya:continue
        den=(pb[1]-pc[1])*(pa[0]-pc[0])+(pc[0]-pb[0])*(pa[1]-pc[1])
        if abs(den)<1e-9:continue
        X,Y=np.meshgrid(np.arange(xa,xb+1)+.5,np.arange(ya,yb+1)+.5)
        u=((pb[1]-pc[1])*(X-pc[0])+(pc[0]-pb[0])*(Y-pc[1]))/den;v=((pc[1]-pa[1])*(X-pc[0])+(pa[0]-pc[0])*(Y-pc[1]))/den;w=1-u-v
        k=(u>=0)&(v>=0)&(w>=0);z=u*a[1]+v*b[1]+w*c[1]
        A=Zt[ya:yb+1,xa:xb+1];kk=k&(z>A);A[kk]=z[kk]
        B=Zb[ya:yb+1,xa:xb+1];kk=k&(z<B);B[kk]=z[kk]
im=np.where(Zt>-1e8,(Zt.clip(-5,25)+5)*8,0).astype('uint8');im2=np.where(Zb<1e8,(Zb.clip(-5,25)+5)*8,0).astype('uint8')
Image.fromarray(np.vstack([im,im2])).save('blk.png');print(W,H)
v=Zb[Zb<1e8];print('lowest-surface hist',np.histogram(v,bins=[-5,-1,-.3,0,.1,.3,1,3,6,10,30])[0],np.histogram(v,bins=[-5,-1,-.3,0,.1,.3,1,3,6,10,30])[1])
