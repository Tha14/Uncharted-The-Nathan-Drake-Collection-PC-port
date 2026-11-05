#!/usr/bin/env python3
"""Airport co-op IGC T-pose fix: install out/skel-retargets.bin (retail PS4 table with the
npc-normal-base -> proto retarget entry enabled) into work/overlay-r3/u3data/build/main/dc1/ps4/.
  (no flags)  dry run, writes nothing
  --apply     backs up the current overlay copy (if any) to work/backup-igc3, then installs
  --restore   puts the backup back (or removes the installed file if there was none before)
Refuses to touch anything while Uncharted.exe is running."""
import argparse,hashlib,os,shutil,subprocess,sys
HERE=os.path.dirname(os.path.abspath(__file__))
WORK=os.path.abspath(os.path.join(HERE,'..','..','work'))
DSTDIR=WORK+'/overlay-r3/u3data/build/main/dc1/ps4'; BAK=WORK+'/backup-igc3'; SRC=HERE+'/out'
FILES={'skel-retargets.bin':'1237caf2ab9c7b1a20660e6d952e63a28148fc94e69f750de15c8d6efc90459d'}
def h(f): return hashlib.sha256(open(f,'rb').read()).hexdigest()
def game_running():
    try: out=subprocess.run(['tasklist'],capture_output=True,text=True).stdout
    except Exception: return False
    return 'uncharted.exe' in out.lower()
a=argparse.ArgumentParser();a.add_argument('--apply',action='store_true');a.add_argument('--restore',action='store_true');o=a.parse_args()
if (o.apply or o.restore) and game_running(): raise SystemExit('Uncharted.exe is running: refusing to change data')
for n,want in FILES.items():
    s,d,b,none=f'{SRC}/{n}',f'{DSTDIR}/{n}',f'{BAK}/{n}',f'{BAK}/{n}.absent'
    if o.restore:
        if os.path.exists(b): print(n,'restore',h(b)[:16]); shutil.copy2(b,d); assert h(b)==h(d)
        elif os.path.exists(none):
            if os.path.exists(d): os.remove(d); print(n,'removed (was not in overlay before)')
        else: raise SystemExit('no backup for '+n)
        continue
    if h(s)!=want: raise SystemExit('source hash mismatch '+s)
    cur=h(d)[:16]+' '+str(os.path.getsize(d)) if os.path.exists(d) else 'not in overlay (retail used)'
    print(n,'current:',cur,'-> new',want[:16],os.path.getsize(s))
    if not o.apply: print('  dry run: no change'); continue
    os.makedirs(BAK,exist_ok=True); os.makedirs(DSTDIR,exist_ok=True)
    if not os.path.exists(b) and not os.path.exists(none):
        if os.path.exists(d): shutil.copy2(d,b); print('  backup',b)
        else: open(none,'w').write('no overlay copy existed before apply_igc3\n'); print('  noted: no previous overlay copy')
    shutil.copy2(s,d); assert h(d)==want; print('  installed',d)
