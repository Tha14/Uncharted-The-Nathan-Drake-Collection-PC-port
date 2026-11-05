import json,struct,sys,glob,os,collections
sys.path.insert(0,'{{PAKTOOLS}}')
from dc import DC,sid
G='{{PS4}}/u3data/build/main/dc1/ps4/render-settings/game/'
RS=[]
for f in glob.glob(glob.escape(G)+'*.bin'):
    c=DC(f)
    names=open(f[:-4]+'.dci').read().split()
    M={sid(n):n for n in names}
    for s,t,o in c.syms:
        if t!=0x9ec73b22: continue
        RS.append((os.path.basename(f)[:-4],M.get(s,hex(s)),c,o))
print(len(RS))
OE=0x9a4;OT=0x9a8;OF=0x9ac;OR=0x9b4;VE=0x9d4;MBE=0x9d0
on=[r for r in RS if r[2].f32(r[3]+OE)>0]
print('light-beam on',len(on),collections.Counter(r[0] for r in on).most_common(60))
print('thresh on',collections.Counter(round(r[2].f32(r[3]+OT),0) for r in on).most_common(20))
print('vignette on',[ (r[0],r[1]) for r in RS if r[2].u32(r[3]+VE)][:30])
for b in ['yemen-multi','fort-multi','air-multi','shambhala-multi','cave-multi','air']:
    x=[r for r in RS if r[0]==b][:3]
    print(b,[(r[1][:30],r[2].f32(r[3]+OE),r[2].f32(r[3]+OT),round(r[2].f32(r[3]+OF),3)) for r in x])
