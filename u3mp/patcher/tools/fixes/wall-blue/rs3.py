import sys,struct,os,collections
exec(open('rs1.py').read().split('def get(names):')[0])
S3=sid('*air-rs-sunrise*')
x=ps3['air.bin'];a=x[0].read(x[1]);o=ents3(a)[S3];print('ps3 tail',[round(struct.unpack_from('>f',a,o+q)[0],4) for q in range(0xbe0,0xc20,4)])
c=ps4['rt:air.bin'];o4=ents4(c)[S3];print('ps4 tail',[round(struct.unpack_from('<f',c,o4+q)[0],4) for q in range(0xc20,0xc60,4)])
# all ps3 blocks: last 4 floats
C=collections.Counter();rows=[]
for k,v in ps3.items():
  a=v[0].read(v[1])
  try:e=ents3(a)
  except Exception:continue
  for s,o in e.items():
    if o+0xc20<=len(a):
      t=tuple(round(struct.unpack_from('>f',a,o+q)[0],3) for q in range(0xc00,0xc10,4));C[t]+=1
      if 'multi' in k or k in('air.bin','yemen-multi.bin'):rows.append((k,hex(s),t))
print(len(C),C.most_common(8));print(rows[:16])
d4=collections.Counter()
for k,c in ps4.items():
  for s,o in ents4(c).items():
    if o+0xc60<=len(c):d4[tuple(round(struct.unpack_from('<f',c,o+q)[0],3) for q in range(0xc40,0xc50,4))]+=1
print('ps4 @c40',len(d4),d4.most_common(6))
