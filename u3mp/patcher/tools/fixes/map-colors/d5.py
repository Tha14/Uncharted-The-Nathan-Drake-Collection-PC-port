from lib import *
q=Pak(OV+'air-multi.pak');sb={s['name']:s for s in subs(q)};I=insts(q)
lmt=[t for t in q.textures() if 'indirect.rgbmax.' in t['path']][0];lm=load_tex(q,lmt).astype(float)/255;HL,WL=lm.shape[:2]
x0,x1,z0,z1=-45,35,-20,37
print('lm(0,0)',lm[0,0])
rows=collections.defaultdict(list)
for ins in I:
    for x in isubs(q,ins):
        s=sb.get(x['name'])
        if not s or 8 not in x['st']:continue
        st=streams(q,s);P=st[0]@ins['M'][:3,:3]+ins['M'][3,:3]
        k=(P[:,0]>x0)&(P[:,0]<x1)&(P[:,2]>z0)&(P[:,2]<z1)&(st[2][:,1]>.7)&(P[:,1]<1)
        if k.sum()<3:continue
        uv=x['st'].get(5);uvz=(uv[k].max(1)==0)
        px=(uv[k,0].clip(0,1)*(WL-1)).astype(int);py=(uv[k,1].clip(0,1)*(HL-1)).astype(int);c=lm[py,px]
        L=c[:,:3].mean(1)*s2l(c[:,3])*10;Ll=c[:,:3].mean(1)*c[:,3]*10
        m=s['mat'].split(',')[0].rsplit('/',1)[-1][:42]
        rows[(m,hex(x['flags']))].append((k.sum(),uvz.mean(),L[~uvz].mean() if (~uvz).any() else -1,Ll[~uvz].mean() if (~uvz).any() else -1,x['st'][8][k,:3].mean(),x['st'][8][k,3].mean()))
print('material flags verts uvzero% L(srgb) L(linear) ilight ilightA')
for k,v in sorted(rows.items(),key=lambda kv:-sum(a[0] for a in kv[1])):
    n=sum(a[0] for a in v);f=lambda i:round(float(sum(a[0]*a[i] for a in v)/n),3)
    print(k,n,f(1),f(2),f(3),f(4),f(5))
