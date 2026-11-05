import re
L=[l.rstrip() for l in open('ps4.dis',errors='replace')]
A={}
idx={}
for i,l in enumerate(L):
  m=re.match(r'\s*([0-9a-f]+):',l)
  if m:idx[int(m.group(1),16)]=i
# dispatchers
disp=[]
for a in sorted(k for k in idx if 0x763f80<=k<0x7660a0):
  l=L[idx[a]]
  m=re.search(r'mov\s+DWORD PTR \[rsp\],(0x[0-9a-f]+|\d+)',l)
  if m:disp.append((a,int(m.group(1),0)))
print([(hex(a),o) for a,o in disp])
