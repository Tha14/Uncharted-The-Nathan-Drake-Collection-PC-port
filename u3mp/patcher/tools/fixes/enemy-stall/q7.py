import sys,struct
sys.path.insert(0,'{{PROJECT}}/scratch/coop-voice')
import ppc
d=ppc.d
want={0x116b2f0:'tag-int32 err',0x114018c:'net-get-team err',0x116b2c4:'tag-symbol err',0x116b2ec:'tag-boolean err',0xe08090:'tag-int32 str direct'}
hits=[]
for va,o,fs,fl in ppc.S:
  if not fl&1:continue
  n=fs//4;regs={}
  for i in range(n):
    w=struct.unpack_from('>I',d,o+4*i)[0];op=w>>26;rt=(w>>21)&31;ra=(w>>16)&31;imm=w&0xffff
    if op==15 and ra==0:regs[rt]=(imm<<16)
    elif op==14 and ra in regs:regs[rt]=(regs[ra]+ppc.s16(imm))&0xffffffff
    elif op==24 and rt in regs and ra==rt:regs[rt]=regs[rt]|imm
    elif op in(32,58) and ra in regs:
      v=(regs[ra]+ppc.s16(imm if op==32 else w&0xfffc))&0xffffffff
      if v in want:print('xref %x %s -> %s (base r%d=%x)'%(va+4*i,ppc.dis(va+4*i),want[v],ra,regs[ra]));hits.append(va+4*i)
    if (w>>26)==19 and ((w>>1)&0x3ff)==16:regs={}  # blr resets
for h in hits:
  a=h
  while a>0x10000:
    if ppc.u32(a)==0x7c0802a6:print('fn start near %x for xref %x'%(a,h));break
    a-=4
