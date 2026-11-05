import sys,re,struct,json
sys.path.insert(0,'gh2/u3mp/paktools')
from dc import DC,sid
c=DC('eula-batch/x/net.bin')
ss=[(hex(s),hex(t),hex(o)) for s,t,o in c.syms if 0x47c00<=o<=0x47d20];print(ss)
d=open(r'{{PROJECT}}/scratch/ps3-119/EBOOT.elf','rb').read()
J=json.load(open('p119.json'));VA,of,SZ=J['seg'][0]
t=d[of:of+SZ];ins=struct.unpack('>%dI'%(len(t)//4),t[:len(t)//4*4])
def find(v):
  lo=v&0xffff;hi=v>>16;res=[]
  for i,w in enumerate(ins):
    if (w>>26) in(14,15,24,25) and (w&0xffff) in(hi,):
      for j in range(-6,8):
        x=ins[i+j]
        if (x>>26) in(14,24) and (x&0xffff)==lo:res.append(hex(VA+i*4));break
  return res
for s,t_,o in ss:print(s,find(int(s,16)))
p=d.find(b'Load http jpeg');print('str',hex(p))
for n in (b'%s.jpg',b'Load Icon Jpeg',b'cellJpgDec'):print(n,hex(d.find(n)))
print(hex(J['toc']) if isinstance(J['toc'],int) else J['toc'])
