"""Writes safe_list.txt (parts safe to install with --only) and look_vram.txt (VRAM growth per look). Read-only otherwise."""
import sys,os,json,collections
sys.dont_write_bytecode=True
sys.path.insert(0,'{{PAKTOOLS}}')
from dclib import DC
P='{{PROJECT}}'
dc=DC(open(P+'/work/overlay-r3/u3data/build/main/dc1/ps4/net.bin','rb').read(),'net')
b=json.load(open('batch.json')); man=json.load(open('manifest.json')); use=json.load(open('part_use.json')); cls=json.load(open('part_class.json'))
names=[n[:-4] for n in sorted(man)]
growth={n:b[n]['vram'][1]-b[n]['vram'][0] for n in names}
# exclusions
ex={}
ex['mp-hero-flynn-coop']='supplied by scratch/airport-igc2'
for n in names:
    if use[n]['col']==['0']: ex[n]='tint material but net.bin colour row +0x2c = 0: NetCharacter::Init 0x6901ee never sets the slot tint, mask would draw black'
    elif cls[n]=='unreferenced' and n!='mp-hero-flynn-coop': ex[n]='no net.bin reference (pirate-armor-a-full-mp: looks.bin only = plain Character, no NetCharacter tint writer)' if n=='pirate-armor-a-full-mp' else 'no net.bin reference: not equippable, not shown reachable'
    elif cls[n].startswith('catalog-only:7edb1b0a'): ex[n]='only in the hat-stats table 53ed596a, in no hero slot table: not shown reachable'
    elif cls[n].startswith('catalog-only:0b'): ex[n]='only in table 0b3952e7 (unknown), in no hero slot table: not shown reachable'
safe=[n for n in names if n not in ex]
with open('safe_list.txt','w') as f:
    f.write('# safe for apply_hat_colour.py --only (%d of %d): every use is a NetCharacter look slot (hero slot tables / hat catalogue) with a non-zero colour row\n'%(len(safe),len(names)))
    f.write('# usage: python apply_hat_colour.py --only "$(grep -v ^# safe_list.txt | paste -sd,)"\n')
    for n in safe: f.write(n+'\n')
    f.write('# excluded:\n')
    for n,w in ex.items(): f.write('#   %s: %s\n'%(n,w))
open('safe_only.txt','w').write(','.join(safe))
print('safe',len(safe),'excluded',len(ex)); print('safe VRAM growth %.1f MiB'%(sum(growth[n] for n in safe)/2**20))
# per look: hero slot tables (+0x20..0x40) + hat catalogue entries (ffac56f2 part entries, one hat slot)
hv=dc.symmap[0xbd3bb3b9][1]; nh=dc.u32(hv); hrows=dc.ptr(hv+8)
cat=[n for n in safe if cls[n].startswith('catalog-only:f99a36aa') or any(not h.split(':')[1].startswith('body') for h in use[n]['heroes'])]
hatmax=max(growth[n] for n in cat) if cat else 0
rows=[]
for i in range(nh):
    r=hrows+0x70*i; hn=dc.cstr(dc.ptr(r)).decode(); worst=0; dflt=0; detail=[]
    for off in (0x20,0x28,0x30,0x38,0x40):
        if not dc.isptr(r+off): continue
        q=dc.ptr(r+off)
        if not dc.isptr(q+8): continue
        tv=dc.symmap[dc.u32(dc.ptr(q+8))][1]; ents=[]
        for j in range(dc.u32(tv)):
            o=dc.ptr(tv+8)+0x38*j; ents.append(dc.cstr(dc.ptr(o+8)).decode() if dc.isptr(o+8) else '')
        g=[(growth[n],n) for n in ents if n in safe]
        if g: m=max(g); worst+=m[0]; detail.append(m[1])
        if ents and ents[0] in safe: dflt+=growth[ents[0]]
    rows.append((worst+hatmax,hn,dflt,detail))
rows.sort(reverse=True)
with open('look_vram.txt','w') as f:
    f.write('resident VRAM growth per look (worst choice of the rebuilt parts in each hero slot table + the largest rebuilt hat, %.0f KiB); default = AI/default look\n'%(hatmax/1024))
    for w,hn,d,det in rows: f.write('%-18s worst %6.0f KiB  default %6.0f KiB  %s\n'%(hn,w/1024,d/1024,det))
    comp=sum(w for w,_,_,_ in rows[:10]); coop=sum(w for w,_,_,_ in rows[:3])+sum(d for _,_,d,_ in rows if d)
    f.write('competitive worst (10 players, 10 worst heroes): %.1f MiB\nco-op worst (3 players worst + every AI default look): %.1f MiB\ntotal of all %d safe parts: %.1f MiB (all 249: %.1f MiB)\n'%(comp/2**20,coop/2**20,len(safe),sum(growth[n] for n in safe)/2**20,sum(growth.values())/2**20))
print(open('look_vram.txt').read()[:600]); print('...'); print('\n'.join(open('look_vram.txt').read().split('\n')[-4:]))
