import struct
d=open(r'D:/1. PC Games/Uncharted-windows/_claude_transfer/u3_ps4.elf','rb').read()
sd=d[0x165c000:0x165c000+0x98000]
for name,a in[('op5',0x764700),('op9',0x763f80),('op8',0x7640d0),('op4',0x764580),('op6',0x764af0)]:
  pass
# scan rodata for runs of pointers into 0x763f00..0x766100
hits=[]
for o in range(0,len(sd)-8,8):
  v=struct.unpack_from('<Q',sd,o)[0]
  if 0x763f00<=v<0x766100:hits.append((0x164c000+o,v))
print([(hex(a),hex(v)) for a,v in hits[:60]])
