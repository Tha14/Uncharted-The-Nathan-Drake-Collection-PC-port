from lib import *
n=sys.argv[1]
q=Pak(OV+n+'.pak');sb={s['name']:s for s in subs(q)};I=insts(q)
lmt=[t for t in q.textures() if 'indirect.rgbmax.' in t['path']][0];lm=load_tex(q,lmt).astype(float)/255;HL,WL=lm.shape[:2]
il=[];c=[];a=[]
for ins in I:
    for x in isubs(q,ins):
        s=sb.get(x['name'])
        if not s or 8 not in x['st'] or 5 not in x['st'] or not x['flags']&0x100:continue
        uv=x['st'][5];ok=(uv.max(1)>0)
        px=(uv[ok,0].clip(0,1)*(WL-1)).astype(int);py=(uv[ok,1].clip(0,1)*(HL-1)).astype(int)
        il.append(x['st'][8][ok,:3]);c.append(lm[py,px,:3]);a.append(lm[py,px,3])
il,c,a=[np.concatenate(v) for v in (il,c,a)]
Y=il.mean(1);C=c.mean(1);print('verts',len(Y),'ilight mean',Y.mean().round(3),'lm rgb',C.mean().round(3),'alpha mean',a.mean().round(3))
bins=np.linspace(0,1,21)
print('alpha bin: count, ilight mean, rgb mean, shader L(a^2.4*10*rgb)')
for lo,hi in zip(bins[:-1],bins[1:]):
    k=(a>=lo)&(a<hi)
    if k.sum()>20:print(round(lo,2),k.sum(),Y[k].mean().round(3),C[k].mean().round(3),(C[k]*s2l(a[k])*10).mean().round(3),'ratio il/L',(Y[k].mean()/(C[k]*s2l(a[k])*10).mean()).round(2))
L=C*s2l(a)*10;o=(L>1e-3)&(Y>.01)
print('median il/L',np.median(Y[o]/L[o]).round(2),'corr',np.corrcoef(Y[o],L[o])[0,1].round(3))
Lg=np.log(Y[o]);A=np.c_[np.log(C[o]),np.log(a[o]+1e-3),np.ones(o.sum())];print('fit log il= p*log(rgb)+q*log(a)+k',np.linalg.lstsq(A,Lg,rcond=None)[0].round(3))
