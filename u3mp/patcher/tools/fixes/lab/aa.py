import sys,os,json,glob
sys.path.insert(0,sys.argv[1])
from ndpak import Pak
W="{{PROJECT}}/work/"
R="{{PS4}}/u3data/build/main/actor33/ps4/"
s=set();c=set();e=0
for d,st in[(R,s)]+[(W+o+'/u3data/build/main/actor33/ps4/',c) for o in('overlay-r3','overlay-r2','overlay-port','overlay-mp')]:
  if not os.path.isdir(d):continue
  for f in os.listdir(d):
    if not f.endswith('.pak'):continue
    try:
      if Pak(d+f).items_of('ANIM'):st.add(f[:-4])
    except Exception:e+=1
json.dump(sorted(s),open(sys.argv[1]+'/data/ps4_anim_actors.json','w'));json.dump(sorted(c),open(sys.argv[1]+'/data/conv_anim_actors.json','w'));print('ret',len(s),'conv',len(c),'err',e)
