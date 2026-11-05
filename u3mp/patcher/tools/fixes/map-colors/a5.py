from lib import *
n=sys.argv[1]
q=Pak(OV+n+'.pak');sb={s['name']:s for s in subs(q)};PR=protos(q);I=insts(q)
print(I[5]['M'])
area=collections.Counter();tris={}
info={}
for ins in I:
    if ins['proto'] is None:continue
    for sn in PR[ins['proto']]:
        s=sb.get(sn)
        if not s:continue
        st=streams(q,s);ix=indices(q,s);P=st[0]
        P=P@ins['M'][:3,:3]+ins['M'][3,:3]
        a,b,c=P[ix[:,0]],P[ix[:,1]],P[ix[:,2]];nr=np.cross(b-a,c-a)
        A=np.abs(nr[:,1]).sum()/2 if 1 else 0
        m=s['mat'].split(',')[0]
        area[m]+=A
        info.setdefault(m,set()).add(tuple(sorted(st)))
for m,a in area.most_common(40):print(round(a),m,info[m])
