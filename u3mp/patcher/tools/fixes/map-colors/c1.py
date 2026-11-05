from lib import *
n=sys.argv[1]
q=Pak(OV+n+'.pak');sb={s['name']:s for s in subs(q)};I=insts(q)
tx=[t for t in q.textures() if 'indirect.rgbmax.' in t['path']]
for t in q.textures():
    if 'indirect' in t['path']:print(t['path'][-50:],t['width'],t['height'],hex(t['format']),t['mips'],t['unk34'])
lm=load_tex(q,tx[0]).astype(float)/255;print('lm mean',lm.reshape(-1,4).mean(0).round(3));H,W=lm.shape[:2]
res=collections.defaultdict(lambda:[0,0,0.0,[],[]])
for ins in I:
    for x in isubs(q,ins):
        s=sb.get(x['name'])
        if not s:continue
        m=s['mat'].split(',')[0];r=res[m];r[0]+=1
        if 5 in x['st']:
            r[1]+=1;uv=x['st'][5];px=(np.clip(uv[:,0],0,1)*(W-1)).astype(int);py=(np.clip(uv[:,1],0,1)*(H-1)).astype(int)
            c=lm[py,px];val=c[:,:3]*s2l(c[:,3:4])*10
            r[3].append(val);r[4].append(uv)
P=sys.argv[2] if len(sys.argv)>2 else 'ground'
for m,r in sorted(res.items(),key=lambda kv:-kv[1][0]):
    if P in m:
        if r[3]:
            v=np.concatenate(r[3]);u=np.concatenate(r[4]);print(m.rsplit('/',1)[-1][:46],'n',r[0],'lm',r[1],'L mean',v.mean(0).round(3),'p5',np.percentile(v.mean(1),5).round(3),'uv',u.min(0).round(3),u.max(0).round(3))
        else:print(m.rsplit('/',1)[-1][:46],'n',r[0],'NO LM')
