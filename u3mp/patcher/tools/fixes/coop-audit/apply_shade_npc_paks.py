"""Fix: Shade Arena NPC spawner templates on Fort, Gompa, Shambhala, Village and Warzone.

Fault: <map>-multi-shade-npc-ingame12.pak has 17 or 21 spawners on these five maps; PS3 1.19 (goty-dlc.psarc) has 43 on all
of them.  coop-shade-all's battle managers (bm-shade-survival-N, bm-shade-marked-N-M) name spawners such as npc-shade-armor-1,
npc-shade-light-ak-1 and npc-shade-mark-armor-1 that are therefore absent from those maps.

Fix: the other shade maps' PS4 files are byte-identical copies of one 43-spawner pak with no map name inside
(overlay-r2 yemen/air/desert/facility/highrise/museum/ruins, md5 b397ee3b...).  Copy it over each of the five effective files.
The pak23_ps4.txt blocks of all these packs already list the same actors and symbols.  Paks have no .dci, so sizes need not
match; the file grows from 40632/52968 to 110968 bytes.

usage: python apply_shade_npc_paks.py            dry run: verify source, targets and PS3 equivalence
       python apply_shade_npc_paks.py --apply    back up to work/backup-coop-audit/ then copy
       python apply_shade_npc_paks.py --restore  copy the backups back
"""
import os,sys,shutil,hashlib
AUD=os.path.dirname(os.path.abspath(__file__)).replace(chr(92),'/')+'/'
P=os.path.abspath(AUD+'../..').replace(chr(92),'/')+'/'
BK=P+'work/backup-coop-audit/'
MAIN='/u3data/build/main/pak23/ps4/'
SRC=P+'work/overlay-r2'+MAIN+'yemen-multi-shade-npc-ingame12.pak'
SRC_SIZE=110968;SRC_MD5='b397ee3b'
# map: (overlay holding the effective file, current size, current md5 prefix)
TG={'fort':('overlay-r2',40632,'5016bd65'),'gompa':('overlay-port',40632,'5016bd65'),'shambhala':('overlay-port',40632,'5016bd65'),
    'village':('overlay-port',52968,'48637de9'),'warzone':('overlay-port',52968,'48637de9')}
def md5(f):return hashlib.md5(open(f,'rb').read()).hexdigest()
def tpath(m):return P+'work/'+TG[m][0]+MAIN+m+'-multi-shade-npc-ingame12.pak'
def shadow(m):
    """the target must really be the effective file"""
    for o in ('overlay-r3','overlay-r2','overlay-port','overlay-mp'):
        f=P+'work/'+o+MAIN+m+'-multi-shade-npc-ingame12.pak'
        if os.path.exists(f):return o
def ps3check(m):
    sys.path.insert(0,AUD)
    try:
        from pakcmp import load,spcmp,detect
    except ImportError as e:      # recipe: the audit helpers (scratch/recovered/coop-regions) are not shipped; this PS3
        return None               # comparison is a verification only, source and targets are md5-checked above
    I=detect.ps3idx();n=m+'-multi-shade-npc-ingame12.pak'
    a=load(I[n][0].read(I[n][1]));b=load(SRC)
    return len(a[4]),len(b[4]),spcmp(a[4],b[4]),sorted(set(a[4])^set(b[4]))
def main():
    if '--restore' in sys.argv:
        for m in TG:
            b=BK+TG[m][0]+'/'+os.path.basename(tpath(m))
            if os.path.exists(b):shutil.copy2(b,tpath(m));print('restored',m,md5(tpath(m))[:8])
            else:print('no backup',b)
        return 0
    bad=0
    if os.path.getsize(SRC)!=SRC_SIZE or not md5(SRC).startswith(SRC_MD5):print('source changed:',SRC);bad+=1
    for m,(ov,sz,h) in TG.items():
        f=tpath(m);cur=md5(f)
        if shadow(m)!=ov:print(m,'FAIL effective overlay is',shadow(m),'expected',ov);bad+=1;continue
        if cur.startswith(SRC_MD5):print(m,'already fixed');continue
        if os.path.getsize(f)!=sz or not cur.startswith(h):print(m,'FAIL unexpected file',os.path.getsize(f),cur[:8]);bad+=1;continue
        pc=ps3check(m)
        if pc is None:print('%s: %s %d bytes md5 %s -> %d bytes (PS3 spawner check skipped: audit helpers not available)'%(m,ov,sz,h,SRC_SIZE));continue
        n3,n4,iss,dif=pc
        print('%s: %s %d bytes md5 %s -> %d bytes; PS3 spawners %d, source %d, name diff %s, field/tag diffs vs PS3 %d'%(m,ov,sz,h,SRC_SIZE,n3,n4,dif,len(iss)))
        if n3!=n4 or dif or iss:bad+=1;print('  FAIL',iss[:4])
    if bad:print(bad,'check(s) failed, nothing written');return 1
    if '--apply' not in sys.argv:print('dry run ok');return 0
    for m in TG:
        f=tpath(m)
        if md5(f).startswith(SRC_MD5):continue
        b=BK+TG[m][0]+'/'+os.path.basename(f);os.makedirs(os.path.dirname(b),exist_ok=True)
        if not os.path.exists(b):shutil.copy2(f,b);print('backup',b)
        shutil.copyfile(SRC,f);assert md5(f)==md5(SRC);print('copied',m,f)
    return 0
if __name__=='__main__':sys.exit(main())
