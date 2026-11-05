from lib import *
def run(path):
    q=Pak(path);g=q.items_of('GEOMETRY_1')
    if not g:return None
    sb={s['name']:s for s in subs(q)};I=insts(q)
    lts=[t for t in q.textures() if 'indirect.rgbmax.' in t['path']]
    if not lts:return 'nolm'
    t=lts[0];lm=load_tex(q,t).astype(float)/255;HL,WL=lm.shape[:2]
    il=[];c=[];a=[]
    for ins in I:
        for x in isubs(q,ins):
            s=sb.get(x['name'])
            if not s or 8 not in x['st'] or 5 not in x['st'] or not x['flags']&0x100:continue
            uv=x['st'][5];ok=(uv.max(1)>0)
            px=(uv[ok,0].clip(0,1)*(WL-1)).astype(int);py=(uv[ok,1].clip(0,1)*(HL-1)).astype(int)
            il.append(x['st'][8][ok,:3].astype(float));c.append(lm[py,px,:3]);a.append(lm[py,px,3])
    if not il:return 'noinst'
    il,c,a=[np.concatenate(v) for v in (il,c,a)]
    Y=il.mean(1);C=c.mean(1)
    L=C*s2l(a)*10;o=(L>1e-3)&(Y>.01)
    out=dict(fmt=hex(t['format']),w=t['width'],n=len(Y),il=round(float(Y.mean()),3),rgb=round(float(C.mean()),3),a=round(float(a.mean()),3),L=round(float(L.mean()),3),ratio_med=round(float(np.median(Y[o]/L[o])),2) if o.sum() else None,corr=round(float(np.corrcoef(Y[o],L[o])[0,1]),3) if o.sum()>5 else None)
    return out
for f in sys.argv[1:]:
    print(os.path.basename(f),run(f))
