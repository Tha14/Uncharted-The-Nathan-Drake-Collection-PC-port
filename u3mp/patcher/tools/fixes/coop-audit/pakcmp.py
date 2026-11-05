"""PS3 1.19 vs effective PS4 -ingame12 paks: regions (sid, tags incl. type, script), spawners (pos, tags incl. type/size/value).
Extends scratch/recovered/coop-regions/detect.py. usage: pakcmp.py [regex] -> pak-report.tsv, pak-summary.txt, pak-names.pkl
pak-names.pkl: {pak: dict(ps3=set(names),ps4=set(names),tags=...)} for the script cross-check."""
import sys,os,re,struct,pickle
AUD=os.path.dirname(os.path.abspath(__file__)).replace(chr(92),'/')+'/'
PK='{{PAKTOOLS}}'
sys.path.insert(0,PK);sys.path.insert(0,AUD+'../recovered/coop-regions')
import detect
from detect import ps3idx,eff,cmp,Pak,R
from sg import spawners,spawner_info,rd
def tagval(p,ty,sz,dp):
    if not dp:return ('null',sz)
    base=p.pbase(dp[0])+dp[1]
    raw=bytes(p.d[base:base+max(sz,0)])
    if ty==5:   # string: PS3 {u32 off}chars, PS4 {u64 off}{8 pad}chars
        q=base+(16 if p.ptrsize==8 else 4);z=p.d.find(b'\0',q)
        return ('str',bytes(p.d[q:z]))
    return raw
def norm(ty,sz,raw,e):
    # numeric values (<=4 bytes) compare after byte-swap; strings raw
    if isinstance(raw,tuple):return raw
    if sz==4:return struct.unpack(e+'I',raw)[0]
    if sz==2:return struct.unpack(e+'H',raw)[0]
    if sz==1:return raw[0]
    if sz==12 or sz==16:return struct.unpack(e+'%dI'%(sz//4),raw)
    return raw.rstrip(b'\0')
def load(src):
    p=Pak(src);r=R(p);rg={};grids=[]
    for it in r.trees():
        g,rs=r.tree(it)
        for a in rs:
            q=r.region(a);rg.setdefault(q['name'],[]).append(q)
        if g:grids.append(r.grid(g))
    sp={}
    try:
        for it,s in spawners(p):
            i=spawner_info(p,s)
            i['tv']={k:(ty,sz,norm(ty,sz,tagval(p,ty,sz,dp),p.e)) for k,(ty,sz,dp) in i['tags'].items()}
            sp.setdefault(i['name'],[]).append(i)
    except Exception as e:sp['!err '+repr(e)[:60]]=[]
    return p,rg,grids,{k:v[0] for k,v in sp.items() if v} ,sp
def spcmp(a,b):
    iss=[]
    for n in sorted(set(a)&set(b)):
        if len(a[n])!=len(b[n]):iss.append(('spawner-dup',n,(len(a[n]),len(b[n]))))
        x,y=a[n][0],b[n][0]
        if x['sid']!=y['sid']:iss.append(('spawner-sid',n,(x['sid'],y['sid'])))
        if any(abs(u-v)>0.01 for u,v in zip(x['pos'][:3],y['pos'][:3])):iss.append(('spawner-pos',n,(x['pos'],y['pos'])))
        if any(abs(u-v)>0.01 for u,v in zip(x['q'],y['q'])):iss.append(('spawner-rot',n,(x['q'],y['q'])))
        ka,kb=x['tv'],y['tv']
        for k in sorted(set(ka)-set(kb)):iss.append(('sptag-missing',n,(hex(k),ka[k])))
        for k in sorted(set(kb)-set(ka)):iss.append(('sptag-extra',n,(hex(k),kb[k])))
        for k in sorted(set(ka)&set(kb)):
            if ka[k][:2]!=kb[k][:2]:iss.append(('sptag-type',n,(hex(k),ka[k][:2],kb[k][:2])))
            elif ka[k][2]!=kb[k][2]:iss.append(('sptag-value',n,(hex(k),ka[k],kb[k])))
    return iss
if __name__=='__main__':
    rx=re.compile(sys.argv[1]) if len(sys.argv)>1 else None
    I=ps3idx()
    tsv=open(AUD+'pak-report.tsv','w',encoding='utf8');sumf=open(AUD+'pak-summary.txt','w',encoding='utf8')
    NAMES={}
    tsv.write('pak\tkind\tname\tdetail\n')
    for n in sorted(I):
        if rx and not rx.search(n):continue
        f,o=eff(n)
        try:
            a=load(I[n][0].read(I[n][1]))
            info=dict(ps3r=set(a[1]),ps3s=set(a[4]),ps3src=I[n][2],org=o)
            if not f:
                if a[1] or a[4]:sumf.write('MISSING-PS4 %s ps3=%s regions=%d spawners=%d\n'%(n,I[n][2],len(a[1]),len(a[4])));tsv.write('%s\tmissing-pak\t\t%d regions %d spawners\n'%(n,len(a[1]),len(a[4])))
                info.update(ps4r=set(),ps4s=set());NAMES[n]=info;continue
            b=load(f)
            iss,r3,r4,s3,s4=cmp(a[:4],b[:4],None,n)
            iss+=spcmp(a[4],b[4])
            info.update(ps4r=set(b[1]),ps4s=set(b[4]));NAMES[n]=info
        except Exception as e:
            sumf.write('ERR %s %s %r\n'%(n,o,e));continue
        c={}
        for k,_,_ in iss:c[k]=c.get(k,0)+1
        sumf.write('%s [%s] ps3src=%s regions %d/%d spawners %d/%d %s\n'%(n,o,I[n][2],r3,r4,s3,s4,c if c else 'OK'))
        for k,m,dt in iss:tsv.write('%s\t%s\t%s\t%s\n'%(n,k,m,str(dt)[:300]))
        sumf.flush();tsv.flush()
    pickle.dump(NAMES,open(AUD+'pak-names.pkl','wb'))
    print('done',len(NAMES))
