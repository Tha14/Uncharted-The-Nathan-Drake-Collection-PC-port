import sys,struct
sys.path.insert(0,'{{PROJECT}}/scratch/coop-voice')
import ppc
d=ppc.d;toc=0xd9e370
want={0x116b2f0:'tag-int32 err',0x114018c:'net-get-team err',0x116b2c4:'tag-symbol err',0x116b2ec:'tag-boolean err'}
hits=[]
for va,o,fs,fl in ppc.S:
  if not fl&1:continue
  n=fs//4;hi={}
  for i in range(n):
    w=struct.unpack_from('>I',d,o+4*i)[0];op=w>>26;rt=(w>>21)&31;ra=(w>>16)&31
    if op==15 and ra==2:hi[rt]=(w&0xffff,i)
    elif op in(32,58) and ra in hi and i-hi[ra][1]<40:
      ds=ppc.s16(w&0xffff if op==32 else w&0xfffc);v=toc+(hi[ra][0]<<16)+ds
      if v in want:print('xref %x %s -> %s'%(va+4*i,ppc.dis(va+4*i),want[v]));hits.append(va+4*i)
# find function start: scan back for 'mflr r0' (0x7c0802a6) preceded by non-blr
for h in hits:
  a=h
  while a>0x10000:
    w=ppc.u32(a)
    if w==0x7c0802a6:  # mflr r0
      print('fn start near %x for xref %x'%(a-4 if (ppc.u32(a-4)>>26)==36 else a,h));break
    a-=4
