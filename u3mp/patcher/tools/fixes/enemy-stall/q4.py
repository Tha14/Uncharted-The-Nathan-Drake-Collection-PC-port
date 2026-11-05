import sys,struct
sys.path.insert(0,'{{PROJECT}}/scratch/coop-voice')
import ppc
d=ppc.d
def va_of(off):
  for va,o,fs,fl in ppc.S:
    if o<=off<o+fs:return va+off-o
tgt=int(sys.argv[1],16) if len(sys.argv)>1 else None
strs=[b"tag-int32: Couldn't find tag %s",b"net-get-team: no player tracker"]
want={}
for s in strs:
  off=d.find(s);want[va_of(off)]=s
print({hex(k):v for k,v in want.items()})
# scan text segment for lis/addi pairs
for va,o,fs,fl in ppc.S:
  if not fl&1:continue
  n=fs//4;last={}
  for i in range(n):
    w=struct.unpack_from('>I',d,o+4*i)[0];op=w>>26;rt=(w>>21)&31;ra=(w>>16)&31;imm=w&0xffff
    if op==15 and ra==0:last[rt]=(imm,va+4*i)
    elif op==14 and ra in last:
      hi,at=last[ra];v=(hi<<16)+ppc.s16(imm)
      if v in want:print('xref %x -> %x %s'%(va+4*i,v,want[v]))
    elif op==24 and rt in last and ra==rt:
      hi,at=last[rt];v=(hi<<16)|imm
      if v in want:print('xref ori %x -> %x %s'%(va+4*i,v,want[v]))
