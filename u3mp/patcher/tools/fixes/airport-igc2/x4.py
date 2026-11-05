import json,struct,sys
sys.path.insert(0,'{{PAKTOOLS}}')
from dc import DC,sid
G='{{PS4}}/u3data/build/main/dc1/ps4/render-settings/game/'
Sch=json.load(open('rs_schema.json'))
c=DC(G+'airport.bin')
names=['*airport-rs-coop-dawn-igc-intro*','*airport-rs-coop-dawn-igc-any*','*airport-rs-coop-dawn*','*airport-rs-coop-dawn-stage*','*airport-rs-coop-dawn-sun*','*airport-rs-coop-dawn-3b*','*airport-rs-110-chase-start-dawn-a*']
for n in names: print(n,hex(sid(n)),c.sym(n) and hex(c.sym(n)[1]))
base=[c.sym(n)[1] for n in names]
sec=sys.argv[1].split(',')
print('short: '+' | '.join(n.replace('*airport-rs-','')[:14] for n in names))
for s,k,f,of in Sch:
    if s not in sec: continue
    for o,fm in zip(of,f+['%.5f']*9):
        vs=[]
        for b in base:
            if fm.endswith('f'): vs.append('%.3f'%c.f32(b+o))
            else:
                vs.append(str(c.u32(b+o)))
        if len(set(vs))>1 or '--all' in sys.argv: print(f'{s[:5]} {k[:34]:34} +{o:#05x} '+' '.join(f'{v:>9}' for v in vs))
