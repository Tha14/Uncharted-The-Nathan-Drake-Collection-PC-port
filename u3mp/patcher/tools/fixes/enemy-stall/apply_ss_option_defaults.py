"""Fix mis-converted state-script option defaults in the port's PS4 dc1 files (Syria co-op enemy stall).

Cause: in the PS3->PS4 conversion of work/overlay-r3/.../dc1/ps4/ss-multiplayer/*.bin the LAST declared option of a
state script lost its default-value slot: its value pointer (entry+0x10) points at the first word of the following
track table, which is the track-name symbol 'main' (0x87f13bc2).  ss-tag-on-enter.bin: ss-coop-region-on-enter declares
(team int32 0); the engine's tag-int32 fallback (PS4 0x123eb30 -> 0x123e580 = PS3 0x9f0d84 -> 0x9f0c6c) reads the
script's declared default, so (tag-int32 'team self) returns 0x87f13bc2 instead of 0, net-get-team(sender)=0 compares
unequal and client-net-go(region-entered) is never sent (u3-tag-trace.txt: TagInt32 a0=d50bc34b ret=ffffffff87f13bc2,
no ClientNetGo).  The PS3 file has the slot (p3-ss-tag-on-enter.bin 0x638 = 0).

Fix: repoint each bad entry's value pointer (entry+0x10) at file offset 0xc: the upper dword of the 64-bit relocation-table
offset in the DC header, which is 0 in every DC file and never written at runtime (the whole file is loaded and pointers
are relocated in place, so base+0xc is inside the image).  File size, symbol table and the relocation bitmap are
unchanged (the field was already a relocated pointer; only its target offset changes), so the .dci size still matches.
All nine entries found by q14.py have PS3 default 0, so a zero word is exactly PS3's value.

usage: python apply_ss_option_defaults.py            dry run: verify every byte it would touch
       python apply_ss_option_defaults.py --apply    back up to work/backup-enemy-stall/ then patch
       python apply_ss_option_defaults.py --restore  copy the backups back
       --only-tag-on-enter                           limit to ss-tag-on-enter.bin (the Syria/Airport region trigger)
"""
import os,sys,struct,hashlib,shutil
P=os.path.abspath(os.path.join(os.path.dirname(__file__),'..','..')).replace('\\','/')+'/'
DC=P+'work/overlay-r3/u3data/build/main/dc1/ps4/ss-multiplayer/'
BK=P+'work/backup-enemy-stall/'
MAIN=0x87f13bc2;PLAYER=0xcde4b742;ZERO=0xc
# (file, entry offset, type sid, option sid, bad pointer target seen, bad value seen)   from q14.py (PS3 value 0 for all)
FIX=[
 ('ss-tag-on-enter.bin',      0x808, 0xc7cb275c,0xd50bc34b,0x970, MAIN),    # team int32  <- Syria reg-turret1-door-guards-1 and every coop region
 ('ss-airport-bomb-drop.bin', 0xd610,0xc7cb275c,0xa0114308,0xd710,MAIN),    # int32
 ('ss-coop-combat-tension.bin',0x148,0xc7cb275c,0x2353ff86,0x440, MAIN),    # int32
 ('ss-desert-events.bin',     0x1b0, 0x0f182ec3,0xc3cc433c,0x318, MAIN),    # float 'check'
 ('ss-facility-events.bin',   0x118, 0x0f182ec3,0x3e50c397,0x1b0, MAIN),    # float
 ('ss-facility-events.bin',   0x620, 0x0f182ec3,0x3e50c397,0x760, MAIN),    # float
 ('ss-heli-attack.bin',       0x5c78,0x0f182ec3,0xba9fbc94,0x60d0,MAIN),    # float
 ('ss-net-breakable-car.bin', 0x7d0, 0x0f182ec3,0xdcde1e21,0xbc4, PLAYER),  # float
 ('ss-syria-events.bin',      0x5358,0x0f182ec3,0xdcde1e21,0x53d8,MAIN),    # float
]
def sha(d):return hashlib.sha256(d).hexdigest()[:16]
def check(d,f,ent,ty,sid,tgt,val,strict):
    """returns (ok, message, already_fixed)"""
    if ent+0x18>len(d):return False,'entry beyond file',False
    s,t=struct.unpack_from('<II',d,ent);pad=struct.unpack_from('<Q',d,ent+8)[0];p=struct.unpack_from('<Q',d,ent+0x10)[0]
    if t!=ty:return False,'type %08x != %08x'%(t,ty),False
    if strict and s!=sid:return False,'sid %08x != %08x'%(s,sid),False
    if struct.unpack_from('<I',d,0xc)[0]:return False,'header dword at 0xc not zero',False
    if p==ZERO:return True,'already fixed (ptr -> 0xc)',True
    if p!=tgt:return False,'ptr %x != expected %x'%(p,tgt),False
    v=struct.unpack_from('<I',d,p)[0]
    if v!=val:return False,'value at %x is %08x, expected %08x'%(p,v,val),False
    return True,'ptr %x -> %x (%08x), will point to %x (0)'%(ent+0x10,p,v,ZERO),False
def main():
    mode='dry'
    if '--apply' in sys.argv:mode='apply'
    if '--restore' in sys.argv:mode='restore'
    only='--only-tag-on-enter' in sys.argv
    fixes=[x for x in FIX if not only or x[0]=='ss-tag-on-enter.bin']
    files=sorted({x[0] for x in fixes})
    if mode=='restore':
        for f in files:
            b=BK+f
            if not os.path.exists(b):print('no backup',b);continue
            shutil.copy2(b,DC+f);print('restored',f,sha(open(DC+f,'rb').read()))
        return
    bad=0;todo={}
    for f in files:
        d=open(DC+f,'rb').read();print('%s %d bytes sha %s'%(f,len(d),sha(d)))
        ba=bytearray(d);n=0
        for (ff,ent,ty,sid,tgt,val) in fixes:
            if ff!=f:continue
            ok,msg,done=check(d,f,ent,ty,sid,tgt,val,strict=True)
            print('  @%x %s: %s'%(ent,'OK' if ok else 'FAIL',msg))
            if not ok:bad+=1
            elif not done:struct.pack_into('<Q',ba,ent+0x10,ZERO);n+=1
        if n:todo[f]=bytes(ba)
    if bad:print('%d check(s) failed, nothing written'%bad);return 1
    if mode=='dry':print('dry run: %d file(s) would change: %s'%(len(todo),', '.join(todo)));return 0
    os.makedirs(BK,exist_ok=True)
    for f,nd in todo.items():
        if not os.path.exists(BK+f):shutil.copy2(DC+f,BK+f);print('backup',BK+f)
        open(DC+f,'wb').write(nd)
        # verify
        d=open(DC+f,'rb').read()
        for (ff,ent,ty,sid,tgt,val) in fixes:
            if ff==f:assert struct.unpack_from('<Q',d,ent+0x10)[0]==ZERO and struct.unpack_from('<I',d,ZERO)[0]==0
        print('patched',f,'sha',sha(d),'size',len(d))
    return 0
if __name__=='__main__':sys.exit(main())
