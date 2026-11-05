from lib import *
import ps4actor,ps3geom,json,pickle
import ps4check
s=S3()
TARGETS='mp-hero-french-torso-coop mp-coop-cutter mp-hero-ngp-torso mp-hero-sol-med-b-1a mp-hero-ramses-equip-torso mp-hero-subway-torso mp-hero-fat-talbot-torso mp-hero-cutter-torso-w-scarf pirate-armor-a-full-mp mp-hero-agent-b-torso mp-hero-make-believe-torso mp-hero-ramses-equip-legs mp-hero-guerro-torso mp-hero-thug-e-torso mp-hero-flynn-hair mp-hero-flynn-coop'.split()
# retail index
R={}
for f in [RET+'/'+x for x in os.listdir(RET) if x.endswith('.pak')]:
    b=os.path.basename(f)[:-4]
    if not re.match(r'mp-|hero-|.*-mp$|pirate|cutter|sol-',b): continue
    try:
        q=Pak(f)
        if not q.items_of('GEOMETRY_1') or not q.items_of('FOREGROUND_SECTION_2'): continue
        fi=ps4actor.tmpl_fg_info(q)
        M=ps4actor.tmpl_materials(q)
        R[b]=dict(skel=fi[0],hier=fi[1],nj=fi[3],jh=hashlib.sha1(fi[2]).hexdigest()[:8],mats=[(m['name'].split(',')[0],m['shader'],len(m['binds'])) for m in M])
    except Exception as e: print("rx",b,repr(e)[:100])
pickle.dump(R,open('retail.pkl','wb'))
print('retail',len(R))
def probs(f):
    try:
        import io,contextlib
        b=io.StringIO()
        with contextlib.redirect_stdout(b): r=ps4check.check(f,quiet=True)
        return r
    except Exception as e: return 'EXC %s'%e
for n in TARGETS:
    f,l=find('actor33',n); q=Pak(f); fi=ps4actor.tmpl_fg_info(q)
    print('==',n,l,os.path.getsize(f),'vram',hex(q.u32(0x20)),'skel %x hier %x nj %d jh %s'%(fi[0],fi[1],fi[3],hashlib.sha1(fi[2]).hexdigest()[:8]))
    print('  types',dict(collections.Counter(i.type for i in q.items if i.type not in('RAW_DATA','EFFECT','VRAM_DESC'))))
    print('  check',str(probs(f))[:300])
    src,d=s.get(n);p=Pak(d);ms=set()
    pl=ps3geom.prototypes(p)
    for v in pl.values(): ms|=set(ps4actor.ps3_block_materials(p,v['proto']))
    print('  ps3',src,len(d),'types',dict(collections.Counter(i.type for i in p.items if i.type not in('RAW_DATA','VRAM_DESC'))))
    for m in sorted(ms): print('     ',m)
    keys=set(m.rsplit('/',1)[0]+':'+m.rsplit('/',1)[1] for m in ms)
    c=[]
    for b,r in R.items():
        same=(r['skel'],r['hier'],r['jh'])==(fi[0],fi[1],hashlib.sha1(fi[2]).hexdigest()[:8])
        ex=len(keys&set(x[0] for x in r['mats']))
        c.append((same,ex,len(r['mats']),b))
    c.sort(reverse=True); print('  tmpl',c[:8]); print('  samejoint',sum(1 for x in c if x[0]))
