import struct,sys
d=open(r'C:/Users/jonas/Documents/Uncharted_Collection_MP/_claude_transfer/u3_ps4.elf','rb').read()
ph,=struct.unpack_from('<Q',d,0x20);n,=struct.unpack_from('<H',d,0x38)
S=[]
for i in range(n):
  t,f,o,v,p,fs,ms=struct.unpack_from('<IIQQQQQ',d,ph+i*56)
  if t==1:S.append((v,o,fs,ms));print(hex(v),hex(o),hex(fs))
def off(v):
  for a,o,fs,ms in S:
    if a<=v<a+fs:return o+v-a
  return None
p=0x16fb8c0;k=0
while 1:
  o=off(p+k*0x18)
  e,fn,par=struct.unpack_from('<IIQ',d,o)[0],*struct.unpack_from('<QI',d,o+8)
  if not fn and k>0:break
  if e in(0x58,0x59,0x69,0x6a,0x6c,0x6d,0xa,6,5):print(hex(e),hex(fn),hex(par))
  k+=1
print('cnt',k)
