import sys,os,re
sys.path.insert(0,"{{TRANSFER_TOOLS}}")
from psarc_lib import Psarc
PS3="{{PS3_DISC}}"
for r in [PS3,"{{PS3_UPDATE}}","{{PS3_DATA2}}"]:
  for dp,_,fs in os.walk(r):
    for f in fs:
      if f.endswith('.psarc'):
        a=Psarc(os.path.join(dp,f))
        for n in a.names:
          if re.search(r'(dig-coop|dig-adv|adv-dig|world-coop-adv-dig|anim-coop-dig|coop-borneo|sky-dig)',n):print(os.path.relpath(os.path.join(dp,f),r),n)
      elif re.search(r'(dig-coop|dig-adv|adv-dig|world-coop-adv-dig)',f):print('FILE',os.path.join(dp,f))
