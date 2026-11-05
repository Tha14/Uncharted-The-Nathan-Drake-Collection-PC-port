import sys,os,glob
sys.path.insert(0,'{{PAKTOOLS}}')
from psarc_lib import Psarc
U="{{PS3_UPDATE}}/";G="{{PS3_DISC}}/"
fs=glob.glob(U+'*.psarc')+glob.glob(U+'**/*.psarc',recursive=True)+glob.glob(G+'*.psarc')+glob.glob(G+'**/*.psarc',recursive=True)
seen=set()
for pf in fs:
  if pf in seen:continue
  seen.add(pf)
  try:p=Psarc(pf)
  except Exception as e:continue
  for i,n in enumerate(p.names):
    if n.endswith('ss-tag-on-enter.bin'):
      d=p.read(i+1);out='p3-ss-tag-on-enter-%s.bin'%os.path.basename(pf).replace('.psarc','')
      open(out,'wb').write(d);print(pf,n,len(d),'->',out)
