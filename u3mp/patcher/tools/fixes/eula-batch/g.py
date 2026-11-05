import struct,json,sys
d=open(r'{{PROJECT}}/scratch/ps3-119/EBOOT.elf','rb').read()
J=json.load(open('p119.json'));print(J['seg'][:3],J.keys())
VA,of,SZ=J['seg'][0]
t=d[of:of+SZ];ins=struct.unpack('>%dI'%(len(t)//4),t[:len(t)//4*4])
for lo,name in[(0x28d9,'PAGE_EULA'),(0xda1f,'hi'),(0xda20,'hi2'),(0x51cd,'BTN'),(0xe963,'TITLE'),(0x9d98,'x')]:
  r=[hex(VA+i*4) for i,w in enumerate(ins) if (w&0xffff)==lo and w>>26 in(14,15,24,25)]
  print(name,r[:30])
