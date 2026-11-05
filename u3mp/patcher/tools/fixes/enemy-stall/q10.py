import sys,struct
sys.path.insert(0,'{{PROJECT}}/scratch/coop-voice')
import ppc
TOC=0x11d27d0
n=int(sys.argv[2]) if len(sys.argv)>2 else 200
a=int(sys.argv[1],16)
for i in range(n):
  w=ppc.u32(a+4*i);t=ppc.dis(a+4*i);ex=''
  op=w>>26;ra=(w>>16)&31
  if ra==2 and op in(32,58,14):
    off=ppc.s16(w&0xffff if op!=58 else w&0xfffc);tgt=TOC+off;v=ppc.u32(tgt);ex=' ; toc[%x]=%x'%(tgt,v) if v is not None else ''
    if op==14:ex=' ; =%x'%tgt
    if v and ppc.o(v):
      s=ppc.cstr(v)
      if s and len(s)>3 and all(31<ord(ch)<127 for ch in s[:20]):ex+=' "%s"'%s[:50]
  print('%x: %08x  %s%s'%(a+4*i,w,t,ex))
  if w==0x4e800020 and i>4:break
