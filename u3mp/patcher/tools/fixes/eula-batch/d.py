import sys,struct,re,json,os
sys.path.insert(0,r'{{TRANSFER_TOOLS}}')
from psarc_lib import Psarc
p=Psarc(r'{{PS3_UPDATE}}/dlc-patch18.psarc')
for i,n in enumerate(p.names):
  if n.endswith('dc1/net.bin') or n.endswith('text1/2.common') or n.endswith('text1/7.common'):
    open('eula-batch/x/'+n.split('/')[-1],'wb').write(p.read(i+1))
b=open('eula-batch/x/net.bin','rb').read()
for a,z in [(484000,488500)]:
  for m in re.finditer(rb'[ -~]{4,}',b[a:z]):
    if re.search(rb'(?i)eula|page|agree|vtex|tga|dds',m.group()):print(hex(a+m.start()),m.group()[:120])
c=open('eula-batch/x/2.common','rb').read()
print(c[:48].hex())
for a in (49444,54309):
  print(a,c[a-100:a+200])
