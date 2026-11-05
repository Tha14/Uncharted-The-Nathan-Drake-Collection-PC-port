import sys,os,glob,collections
exec(open('m1.py').read().split("for n in sys.argv")[0])
RET=glob.escape(G)+'/u3data/build/main/actor33/ps4/*.pak'
tot=collections.Counter();ex=collections.defaultdict(list)
for f in glob.glob(RET):
    n=os.path.basename(f)[:-4]
    try:M=mats(f)
    except Exception:continue
    for nm,sh,ef,fe,par,bi,u in M:
        if 'COLOR_1_APPLY_INSTANCE_TINT' in fe:
            k='mask' if 'COLOR_1_APPLY_COLOR_TINT_WITH_MASK' in fe else 'NOMASK'
            cl='mp' if n.startswith('mp-') else 'npc'
            tot[k,cl]+=1;ex[k,cl].append(n+':'+nm.split(',')[0].split('/')[-1])
print(tot)
for k in ex:
    print(k,len(ex[k]),ex[k][:25])
