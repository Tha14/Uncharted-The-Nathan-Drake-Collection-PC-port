import sys,struct,re,json
sys.path.insert(0,r'{{TRANSFER_TOOLS}}')
from psarc_lib import Psarc
d=open(r'{{PROJECT}}/scratch/ps3-119/EBOOT.elf','rb').read()
for m in re.finditer(rb'[ -~]{0,60}(?i:eula|agreement|supplemental|user-?agree|tos-|terms)[ -~]{0,60}',d):print('E',hex(m.start()),m.group())
for name in ['dlc-patch18.psarc','dlc-patch13.psarc']:
  p=Psarc(r'{{PS3_UPDATE}}/'+name)
  for i,n in enumerate(p.names):
    if n.endswith('.dds') or 'sound' in n or 'actor' in n and 0:continue
    if re.search(r'(?i)eula|agree|legal',n):print('NAME',name,n)
    if p.entries[i+1][2]>3e8:continue
    b=p.read(i+1)
    hits=[(m.start(),m.group()[:60]) for m in re.finditer(rb'(?i)supplemental terms|eula|user.agreement',b)]
    if hits:print(name,n,len(b),hits[:6])
