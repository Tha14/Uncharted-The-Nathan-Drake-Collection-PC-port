import sys,os,re,struct
P='{{PROJECT}}'
sys.path.insert(0,P+'/scratch/recovered/coop-regions');sys.path.insert(0,'{{PAKTOOLS}}')
from detect import ps3idx,eff
from ndpak import Pak
from sg import spawners,spawner_info,rd
import pickle
D=dict(pickle.load(open(P+'/scratch/live/coop-tmp/dict.pkl','rb')))
TEAM=0xd50bc34b
def scan(d,label):
  p=Pak(d);n=0
  for it,sp in spawners(p):
    i=spawner_info(p,sp)
    if TEAM in i['tags']:
      ty,sz,dp=i['tags'][TEAM];val=rd(p,dp[0],dp[1],'I')[0] if dp else None
      print('  ',label,i['name'],hex(i['sid']),'team ty',ty,'sz',sz,'val',hex(val) if val is not None else None,D.get(val,''),'alltags',[(D.get(k,hex(k)),t[0]) for k,t in i['tags'].items()])
      n+=1
  return n
I=ps3idx()
rx=re.compile(sys.argv[1])
for name in sorted(I):
  if not rx.search(name):continue
  f,o=eff(name)
  print('##',name,o)
  if f:scan(open(f,'rb').read(),'PS4')
  p,i,src=I[name];scan(p.read(i),'PS3')
