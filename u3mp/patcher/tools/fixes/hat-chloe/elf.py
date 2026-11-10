import struct
d=open("C:/Users/jonas/Documents/Uncharted_Collection_MP/_claude_transfer/u3_ps4.elf",'rb').read()
ph=struct.unpack('<Q',d[32:40])[0];n=struct.unpack('<H',d[56:58])[0]
S=[]
for i in range(n):
  t,f,of,va,pa,fs,ms,al=struct.unpack('<IIQQQQQQ',d[ph+i*56:ph+i*56+56]);
  if t in(1,0x61000010):S.append((va,of,fs))
def rd(a,k):
  for va,of,fs in S:
    if va<=a and a+k<=va+fs:return d[of+a-va:of+a-va+k]
  raise ValueError(hex(a))
