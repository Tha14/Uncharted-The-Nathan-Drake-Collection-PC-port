import sys
sys.path.insert(0,'{{PAKTOOLS}}')
from dc import DC
P=sys.argv[1];c=DC(P+"/work/overlay-r3/u3data/build/main/dc1/ps4/net.bin")
o=c.bysid[0xbd3bb3b9][1];e=c.ptr(o+8);a=e  # type 0 hunter
for off in (0x18,0x20,0x28,0x30,0x38,0x40,0x48):
    if a+off not in c.ptrs:continue
    p=c.ptr(a+off);q=c.ptr(p+8);s=c.u32(q);t=c.bysid.get(s)
    if not t:print(hex(off),'nosym');continue
    to=t[1];cnt=c.u32(to);it=c.ptr(to+8)
    nm=lambda k:c.cstr(c.ptr(it+k*0x38+8))
    print(hex(off),cnt,'[0]',nm(0),'[3]',nm(3) if cnt>3 else '-','[5]',nm(5) if cnt>5 else '-','[50]',nm(50) if cnt>50 else 'wrap->'+nm(0))
