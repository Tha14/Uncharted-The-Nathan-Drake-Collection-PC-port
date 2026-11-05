from lib import *
import collections,re,struct,pickle
D=dict(pickle.load(open('../live/coop-tmp/dict.pkl','rb')))
p=Pak('../live/airport-igc/ps3/anim-coop-airport-intro.pak')
print(collections.Counter(i.type for i in p.items))
for i in p.items:
    if i.type not in('LIGHTING','X'): print(i.type,i.name,hex(i.size))
s=open('../live/coop-tmp/air.txt').read()
for k in ['0x9213ca14','0x59e837ee','0x3cba72fb','0x31f95422','0xeba87806','0x96d2d7a3','0xdf8f7d8e','0xdb4e6039','0xd60d46e0','0x9f50eccd']:
    print(k,D.get(int(k,16)))
