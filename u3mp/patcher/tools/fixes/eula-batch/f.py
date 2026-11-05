import struct,json,sys
d=open(r'{{PROJECT}}/scratch/ps3-119/EBOOT.elf','rb').read()
J=json.load(open('p119.json'));VA,of,SZ=J['seg'][0]
t=d[of:of+SZ]
ins=struct.unpack('>%dI'%(len(t)//4),t[:len(t)//4*4])
ids={'PAGE_EULA':0xda1f28d9,'CONFIRM':0x606636f3,'BTN':0x7ce251cd,'PICS':0x2485c6f,'TITLE':0x54b6e963}
for n,v in ids.items():
  lo=v&0xffff;hi=v>>16;hi2=((v+0x8000)>>16)&0xffff
  for i,w in enumerate(ins):
    op=w>>26
    if op==15 and (w>>16&31)==0 and (w&0xffff) in(hi,hi2):
      r=w>>21&31
      for j in range(1,10):
        x=ins[i+j] if i+j<len(ins) else 0
        o2=x>>26
        if (o2==24 and (x&0xffff)==lo and (x>>21&31)==r) or (o2==14 and (x&0xffff)==lo and (x>>16&31)==r):
          print(n,hex(VA+i*4));break
# also data table scan
for n,v in ids.items():
  b=struct.pack('>I',v);p=d.find(b)
  while p>=0:print(n,'raw',hex(p));p=d.find(b,p+1)
