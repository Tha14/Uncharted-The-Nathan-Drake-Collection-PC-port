"""Build out/skel-retargets.bin from the retail PS4 skel-retargets.bin: enable the
npc-normal-base -> proto retarget entry (DC entry 2, first dword 1 -> 0) so an NPC on the
hero/proto skeleton can find and play npc-normal clips (Airport IGC villain-d/e).
Everything else is byte-identical to retail."""
import sys,struct,hashlib,os
sys.path.insert(0,'{{PAKTOOLS}}')
from dc import DC,sid
HERE=os.path.dirname(os.path.abspath(__file__))
SRC="{{PS4}}/u3data/build/main/dc1/ps4/skel-retargets.bin"
ENABLE=[('npc-normal-base','proto')]        # (src skeleton name, dst skeleton name) entries to enable
d=bytearray(open(SRC,"rb").read()); c=DC(SRC)
t,o=c.sym('*skel-retarget-tables*'); ptr=c.ptr(o); n=c.u32(o+8)
assert ptr==0x40 and n==26, (hex(ptr),n)
changed=[]
for i in range(n):
    e=ptr+0x30*i; a,b=c.u32(e+0xc),c.u32(e+0x10)
    for s,dn in ENABLE:
        if (a,b)==(sid(s),sid(dn)):
            old=struct.unpack_from('<I',d,e)[0]; struct.pack_into('<I',d,e,0); changed.append((i,s,dn,old))
assert len(changed)==len(ENABLE), changed
out=HERE+'/out/skel-retargets.bin'; os.makedirs(HERE+'/out',exist_ok=True); open(out,'wb').write(d)
DC(out)  # must still parse
src=open(SRC,'rb').read(); diff=[k for k in range(len(src)) if src[k]!=d[k]]
print('changed entries',changed); print('bytes differing',len(diff),[hex(k) for k in diff]); print('sha256',hashlib.sha256(d).hexdigest())
