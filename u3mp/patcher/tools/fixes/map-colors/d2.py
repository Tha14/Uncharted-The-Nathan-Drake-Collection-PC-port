from lib import *
for path in sys.argv[1:]:
    q=Pak(path);sb={s['name']:s for s in subs(q)};I=insts(q)
    t=[t for t in q.textures() if 'indirect.rgbmax.' in t['path']][0];lm=load_tex(q,t).astype(float)/255;HL,WL=lm.shape[:2]
    il=[];c=[];a=[]
    for ins in I:
        for x in isubs(q,ins):
            s=sb.get(x['name'])
            if not s or 8 not in x['st'] or 5 not in x['st'] or not x['flags']&0x100:continue
            uv=x['st'][5];ok=(uv.max(1)>0)
            px=(uv[ok,0].clip(0,1)*(WL-1)).astype(int);py=(uv[ok,1].clip(0,1)*(HL-1)).astype(int)
            il.append(x['st'][8][ok,:3].astype(float));c.append(lm[py,px,:3]);a.append(lm[py,px,3])
    il,c,a=[np.concatenate(v) for v in (il,c,a)];Y=il.mean(1);C=c.mean(1)
    print(os.path.basename(path),len(Y))
    for lo,hi in zip(np.r_[0,.05,.1,.15,.2,.3,.4,.5,.6,.8],np.r_[.05,.1,.15,.2,.3,.4,.5,.6,.8,1.01]):
        k=(a>=lo)&(a<hi)
        if k.sum()>30:print('  a',lo,k.sum(),'il',Y[k].mean().round(3),'srgb*10',(C[k]*s2l(a[k])*10).mean().round(3),'lin*10',(C[k]*a[k]*10).mean().round(3))
