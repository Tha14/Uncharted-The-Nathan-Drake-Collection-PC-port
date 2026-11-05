#!/usr/bin/env python3
"""Install the rebuilt Flynn co-op pak (no INSTANCE_TINT on shirt, trousers, boots; each with its own PS3 textures).
  (no flags)  dry run, writes nothing
  --apply     backs up the current overlay-r3 copy to work/backup-igc2, then installs
  --restore   puts the backup back"""
import argparse,hashlib,os,shutil
HERE=os.path.dirname(os.path.abspath(__file__))
WORK=os.path.abspath(os.path.join(HERE,'..','..','work'))
DST=WORK+'/overlay-r3/u3data/build/main/actor33/ps4'; BAK=WORK+'/backup-igc2'; SRC=HERE+'/out'
FILES={'mp-hero-flynn-coop.pak':'686e426036295d94699f8566f44fdd423bcbe9a1d14ce48b7ddc1757692bc0f6'}
def h(f): return hashlib.sha256(open(f,'rb').read()).hexdigest()
a=argparse.ArgumentParser();a.add_argument('--apply',action='store_true');a.add_argument('--restore',action='store_true');o=a.parse_args()
for n,want in FILES.items():
    s,d,b=f'{SRC}/{n}',f'{DST}/{n}',f'{BAK}/{n}'
    if o.restore:
        if not os.path.exists(b): raise SystemExit('no backup '+b)
        print(n,'restore',h(b)[:16],'over',h(d)[:16]); shutil.copy2(b,d); assert h(b)==h(d); continue
    if h(s)!=want: raise SystemExit('source hash mismatch '+s)
    print(n,'current',h(d)[:16],os.path.getsize(d),'-> new',want[:16],os.path.getsize(s))
    if not o.apply: print('  dry run: no change'); continue
    os.makedirs(BAK,exist_ok=True)
    if not os.path.exists(b): shutil.copy2(d,b); print('  backup',b)
    shutil.copy2(s,d); assert h(d)==want; print('  installed')
