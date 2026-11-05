#!/usr/bin/env python3
"""Install the corrected weapon packs into work/overlay-r3 (nothing happens without --apply).
Backs up each original to work/backup-skintex first; refuses a second run; checks every SHA-256."""
import argparse,hashlib,json,os,shutil,sys
H=os.path.dirname(os.path.abspath(__file__))
ROOT="{{PROJECT}}/"
DST=ROOT+"work/overlay-r3/u3data/build/main/actor33/ps4/"
BK=ROOT+"work/backup-skintex/"
def sha(f):
    with open(f,'rb') as h:return hashlib.sha256(h.read()).hexdigest()
def main():
    a=argparse.ArgumentParser();a.add_argument('--apply',action='store_true',help='actually install; default only checks');a=a.parse_args()
    packs=json.load(open(H+'/manifest.json'))['packs']
    if os.path.exists(BK):sys.exit('REFUSED: %s exists, already applied (restore from it before running again)'%BK)
    for fn,e in packs.items():
        if sha(H+'/out/'+fn)!=e['out_sha256']:sys.exit('REFUSED: built file changed: '+fn)
        if sha(DST+fn)!=e['src_sha256']:sys.exit('REFUSED: installed file differs from what was built against: '+fn)
    print('checks OK: %d packs, growth %d bytes'%(len(packs),sum(e['new_size']-e['old_size'] for e in packs.values())))
    if not a.apply:print('dry run, nothing changed; pass --apply to install');return
    os.makedirs(BK)
    for fn in packs:shutil.copy2(DST+fn,BK+fn)
    for fn,e in packs.items():
        if sha(BK+fn)!=e['src_sha256']:sys.exit('backup verify failed: '+fn)
    for fn,e in packs.items():
        tmp=DST+fn+'.tmp';shutil.copyfile(H+'/out/'+fn,tmp);os.replace(tmp,DST+fn)
        if sha(DST+fn)!=e['out_sha256']:sys.exit('install verify failed: '+fn)
        print('installed',fn)
    print('backup:',BK)
if __name__=='__main__':main()
