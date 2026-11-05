import sys,struct
sys.dont_write_bytecode=True
sys.path.insert(0,'dctools')
from common import DC,sid
from heroes_common import hero_rows,part_list
R="{{PROJECT}}/work/overlay-r3/u3data/build/main/dc1/ps4/net.bin"
a=DC(R);b=DC('_ps3net.bin')
for c,tag in((a,'PS4'),(b,'PS3')):
  rows=hero_rows(c)
  print(tag,len(rows))
  for r in rows:
    print(r['i'],r['name'],'%08x'%r['sid'],'%08x'%r['sid2'],[ (len(s['parts']) if isinstance(s,dict) else s) for s in r['slots']])
