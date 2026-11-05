"""Second batch of the lost option-default slot (same defect as scratch/enemy-stall/apply_ss_option_defaults.py).

These 15 entries were missed by q14.py because it compared against base bin.psarc only: patch-only files were skipped and options of
scripts that moved or changed between PS3 versions found no (sid,type) match.  Found by q14b.py (PS3 1.19 reference) and q15.py.
Each entry's value pointer (entry+0x10) points at the first word of the next structure instead of a zero word.
Fix: repoint it at file offset 0xc, the zero upper dword of the header's relocation offset, exactly as the first batch.
File sizes, symbol tables and relocation bitmaps do not change, so every .dci stays matched.

usage: python apply_option_defaults_2.py            dry run: verify each entry, including PS3's default for it
       python apply_option_defaults_2.py --apply    back up to work/backup-coop-audit/ then patch
       python apply_option_defaults_2.py --restore  copy the backups back
"""
import os,sys,struct,shutil,hashlib,glob
AUD=os.path.dirname(os.path.abspath(__file__)).replace(chr(92),'/')+'/'
P=os.path.abspath(AUD+'../..').replace(chr(92),'/')+'/'
DC=P+'work/overlay-r3/u3data/build/main/dc1/ps4/'
BK=P+'work/backup-coop-audit/overlay-r3-dc1/'
INT=0xc7cb275c;FLT=0x0f182ec3;MAIN=0x87f13bc2;PLAYER=0xcde4b742;GRAVE=0xc9d0aab2;ZERO=0xc
F='ss-coop-adv-fort.bin';M='ss-multiplayer/'
# file, entry, type, option sid, wrong target, wrong value, owner / effect
FIX=[
 (F,0x3f1a8,INT,0x57af0546,0x3f490,MAIN,'fort adventure'),
 (F,0x40130,INT,0xd50bc34b,0x40308,MAIN,'fort: team, region-enter stall as in Syria'),
 (F,0x56958,INT,0x104e7d46,0x56b90,MAIN,'fort: tower-idols idol-placed counter'),
 (F,0x58c88,INT,0xd50bc34b,0x58df0,MAIN,'fort: team'),
 (F,0x5aeb0,INT,0xad536552,0x5b0c8,MAIN,'fort adventure'),
 (F,0x5cf30,INT,0x104e7d46,0x5d2e0,MAIN,'fort: courtyard-idols idol-placed counter'),
 (F,0x67658,INT,0x104e7d46,0x676e0,MAIN,'fort: idol-placed'),
 (M+'ss-coop-arena-npcs.bin',0x2ee0,INT,0x011d3371,0x3018,MAIN,'ss-shade-medal counter (shade arena medal)'),
 (M+'ss-facility-crane.bin',0xd0,FLT,None,0x168,MAIN,'start-delay'),
 (M+'ss-grave-events.bin',0x46c8,INT,0x74188a62,0x49d8,GRAVE,'ss-grave-container counter'),
 (M+'ss-ldn-streets-events.bin',0x5b8,INT,0xffe68374,0xad8,MAIN,'ss-ldn-streets-drive'),
 (M+'ss-net-breakable-car-fort.bin',0x610,FLT,0xdcde1e21,0xa04,PLAYER,'shader param'),
 (M+'ss-oasis-events.bin',0x170,INT,0xbe00ea93,0x450,MAIN,'ss-cargo-plane-fly'),
 (M+'ss-old-quater-events.bin',0xf10,FLT,0xdcde1e21,0xf90,MAIN,'shader param'),
 (M+'ss-sweeping-building.bin',0x140,FLT,0xe2c35779,0x320,MAIN,'animate speed'),
]
def sha(d):return hashlib.sha256(d).hexdigest()[:16]
def ps3vals(rel,ent_sid,ty):
    """defaults of this (option sid, type) in the extracted PS3 versions: must be all zero"""
    vals={}
    for ver in ('p18','p13','gotyp','bin'):
        f=AUD+'p3/'+ver+'/'+os.path.basename(rel)
        if os.path.exists(f):d=open(f,'rb').read()
        else:
            d=ps3_read(rel,{'p18':'patch18','p13':'patch13','gotyp':'goty-patch','bin':'disc'}[ver])   # recipe: read the user's PS3 data
            if d is None:continue
        rel_=struct.unpack_from('>I',d,8)[0];cnt=struct.unpack_from('>I',d,rel_)[0];bm=d[rel_+4:rel_+4+cnt]
        k=0;pat=struct.pack('>II',ent_sid,ty)
        while True:
            k=d.find(pat,k)
            if k<0:break
            po=k+0xc;bi=po//4
            if k%4==0 and bi//8<len(bm) and bm[bi//8]&(1<<(7-bi%8)):
                t=struct.unpack_from('>I',d,po)[0];vals.setdefault(ver,[]).append(struct.unpack_from('>I',d,t)[0])
            k+=4
    return vals
_IDX=[]
def ps3_read(rel,layer):
    """the PS3 module from the user's PS3 folders (tools/dcconv/ps3src.py), when the p3/ extracts are not here"""
    if not _IDX:
        pk=os.environ.get('PAKTOOLS') or "{{PAKTOOLS}}"
        sys.path.insert(0,os.path.normpath(os.path.join(pk,'..','dcconv')))
        from ps3src import Index
        _IDX.append(Index("{{PS3_DISC}}","{{PS3_UPDATE}}","{{PS3_DATA2}}"))
    return _IDX[0].read('dc1/ps4/'+rel,layer)
def main():
    files=sorted({x[0] for x in FIX})
    if '--restore' in sys.argv:
        for f in files:
            b=BK+f
            if os.path.exists(b):shutil.copy2(b,DC+f);print('restored',f,sha(open(DC+f,'rb').read()))
            else:print('no backup',b)
        return 0
    bad=0;todo={}
    for f in files:
        d=open(DC+f,'rb').read();ba=bytearray(d);n=0;print('%s %d bytes sha %s'%(f,len(d),sha(d)))
        if struct.unpack_from('<I',d,0xc)[0]:print('  header dword at 0xc not zero');bad+=1;continue
        for (ff,ent,ty,sid,tgt,val,why) in FIX:
            if ff!=f:continue
            s,t=struct.unpack_from('<II',d,ent);p=struct.unpack_from('<Q',d,ent+0x10)[0]
            if t!=ty or (sid is not None and s!=sid):print('  @%x FAIL type/sid %08x %08x'%(ent,s,t));bad+=1;continue
            if p==ZERO:print('  @%x already fixed'%ent);continue
            if p!=tgt or struct.unpack_from('<I',d,p)[0]!=val:print('  @%x FAIL ptr %x value %08x'%(ent,p,struct.unpack_from('<I',d,p)[0]));bad+=1;continue
            v=ps3vals(f,s,ty);nz=[(k,x) for k,l in v.items() for x in l if x]
            print('  @%x %08x %s -> %x(%08x), PS3 defaults %s %s'%(ent,s,why,p,val,{k:len(l) for k,l in v.items()},'NONZERO '+str(nz) if nz else 'all 0'))
            if nz or not v:bad+=1
            struct.pack_into('<Q',ba,ent+0x10,ZERO);n+=1
        if n:todo[f]=bytes(ba)
    if bad:print(bad,'check(s) failed, nothing written');return 1
    if '--apply' not in sys.argv:print('dry run ok: %d file(s) would change'%len(todo));return 0
    for f,nd in todo.items():
        b=BK+f;os.makedirs(os.path.dirname(b),exist_ok=True)
        if not os.path.exists(b):shutil.copy2(DC+f,b);print('backup',b)
        open(DC+f,'wb').write(nd);print('patched',f,sha(nd),len(nd))
    return 0
if __name__=='__main__':sys.exit(main())
