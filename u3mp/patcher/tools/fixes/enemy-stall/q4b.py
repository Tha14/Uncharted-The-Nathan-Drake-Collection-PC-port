import sys,struct,collections
sys.path.insert(0,'{{PROJECT}}/scratch/coop-voice')
import ppc
d=ppc.d
def va_of(off):
  for va,o,fs,fl in ppc.S:
    if o<=off<o+fs:return va+off-o
# TOC: most common u64 in data segments, then opd-like triples
cnt=collections.Counter()
for va,o,fs,fl in ppc.S:
  if fl&1:continue
  for i in range(0,fs-8,8):
    v=struct.unpack_from('>Q',d,o+i)[0]
    if 0x100000<v<0x2000000:cnt[v]+=1
print('top u64',[(hex(k),c) for k,c in cnt.most_common(4)])
toc=cnt.most_common(1)[0][0]
want={}
for s in [b"tag-int32: Couldn't find tag %s",b"net-get-team: no player tracker",b"tag-symbol: Couldn't find tag",b"tag-boolean: Couldn't find tag"]:
  off=d.find(s);v=va_of(off)
  # pointer in data
  for va,o,fs,fl in ppc.S:
    if fl&1:continue
    j=-1
    while True:
      j=d.find(struct.pack('>I',v),o+max(j,0)+1 if j>=0 else o,o+fs)
      if j<0:break
      pv=va+j-o;want[pv]=s;print('ptr to %s at va %x (toc off %#x)'%(s[:12],pv,pv-toc))
for va,o,fs,fl in ppc.S:
  if not fl&1:continue
  n=fs//4
  for i in range(n):
    w=struct.unpack_from('>I',d,o+4*i)[0];op=w>>26;rt=(w>>21)&31;ra=(w>>16)&31
    if ra==2 and op in(32,58):
      ds=ppc.s16(w&0xffff if op==32 else w&0xfffc);v=toc+ds
      if v in want:print('xref %x %s -> %s'%(va+4*i,ppc.dis(va+4*i),want[v][:16]))
