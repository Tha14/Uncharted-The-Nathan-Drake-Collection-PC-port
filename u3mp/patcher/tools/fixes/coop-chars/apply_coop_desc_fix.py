#!/usr/bin/env python3
"""Fix byte order of Co-op Adventure hero descs in overlay net.bin.
PS4 net.bin stores each desc's four u8 at +0 reversed (00 00 00 TT), so native 0x7294c1 copies a hunter look.
Dry run by default. --apply writes net.bin after saving net.bin.bak-coopdesc. Idempotent."""
import sys,os,shutil
sys.dont_write_bytecode=True
sys.path.insert(0,'{{PAKTOOLS}}')
from dc import DC
P="{{PROJECT}}"
F=P+"/work/overlay-r3/u3data/build/main/dc1/ps4/net.bin"
c=DC(F);d=bytearray(c.d)
o=c.bysid[0x3b9a067d][1];n=c.u32(o);e=c.ptr(o+8)
seen=set();fix=[]
for i in range(n):
    a=e+i*0xd0
    if c.i32(a+0xc)!=11 or a+0x70 not in c.ptrs:continue
    t=c.ptr(a+0x70)
    for k in range(3):
        q=c.ptr(t+k*8)
        if q in seen:continue
        seen.add(q);b=d[q:q+4]
        if b[0]==0 and b[1]==0 and b[2]==0 and 0<b[3]<0x40:
            fix.append((i,k,q,b[3]));d[q]=b[3];d[q+3]=0
        else:print('skip',i,k,hex(q),b.hex())
for i,k,q,ty in fix:print('netgame %d slot %d desc %#x type %d'%(i,k,q,ty))
print(len(fix),'descs to fix')
if '--apply' in sys.argv and fix:
    bak=F+'.bak-coopdesc'
    if not os.path.exists(bak):shutil.copy2(F,bak)
    open(F,'wb').write(d);print('written',F)
else:print('dry run, nothing written')
