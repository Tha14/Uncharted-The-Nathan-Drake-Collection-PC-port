from lib import *
from PIL import Image
n=sys.argv[1];x0,x1,z0,z1=-45,35,-20,37;sc=6
q=Pak(OV+n+'.pak');sb={s['name']:s for s in subs(q)};I=insts(q)
W=int((x1-x0)*sc);H=int((z1-z0)*sc);im=np.zeros((H,W,3),'uint8')
rows=[]
for ins in I:
    for x in isubs(q,ins):
        s=sb.get(x['name'])
        if not s or 'shadowblocker' not in s['mat'] and 'canopy-shadow' not in s['mat']:continue
        st=streams(q,s);ix=indices(q,s);P=st[0]@ins['M'][:3,:3]+ins['M'][3,:3]
        tl=P[ix];nr=np.cross(tl[:,1]-tl[:,0],tl[:,2]-tl[:,0]);area=np.abs(nr[:,1]).sum()/2
        ok=(tl[:,:,0].max(1)>x0)&(tl[:,:,0].min(1)<x1)&(tl[:,:,2].max(1)>z0)&(tl[:,:,2].min(1)<z1)
        if not ok.any():continue
        rows.append((round(area),ins['i'],x['name'][-22:],hex(x['flags']),hex(x['flags2']),P[:,1].min().round(1),P[:,1].max().round(1),P[:,0].min().round(0),P[:,0].max().round(0),P[:,2].min().round(0),P[:,2].max().round(0)))
rows.sort(reverse=True)
for r in rows[:30]:print(r)
print(len(rows),collections.Counter(r[3] for r in rows))
