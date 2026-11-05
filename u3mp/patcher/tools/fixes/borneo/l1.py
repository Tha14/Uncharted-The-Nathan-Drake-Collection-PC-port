import sys,os,re
sys.path.insert(0,"{{TRANSFER_TOOLS}}")
from psarc_lib import Psarc
PS3="{{PS3_DISC}}"
roots=[PS3,"{{PS3_UPDATE}}","{{PS3_DATA2}}"]
for r in roots:
  for dp,_,fs in os.walk(r):
    for f in fs:
      if f.endswith('.psarc'):
        p=os.path.join(dp,f)
        try:a=Psarc(p)
        except Exception as e:print('ERR',p,e);continue
        hits=[n for n in a.names if re.search(r'dc1/.*(coop|adv|dig)|(ss-|/)(coop|.*-adv)|/dig-|borneo',n)]
        print(f,len(a.names),len(hits))
        for h in hits:print('   ',h)
