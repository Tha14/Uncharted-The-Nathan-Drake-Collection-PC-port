import re
L=[l.rstrip() for l in open('ps4.dis',errors='replace')]
seen=0
for i,l in enumerate(L):
  if re.search(r'mov\s+(BYTE|WORD) PTR \[[^\]]*\+0x60\],',l) or re.search(r'inc\s+BYTE PTR \[[^\]]*\+0x60\]',l):
    ctx=L[max(0,i-400):i+400]
    if any(re.search(r'0x68$',c) for c in ctx) and any(re.search(r',0x18$',c) for c in ctx):
      print(l[:90]);seen+=1
print(seen)
