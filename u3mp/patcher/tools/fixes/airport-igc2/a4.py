import sys,re,pickle,struct
from lib import *
from dc import DC,sid
D=dict(pickle.load(open('../live/coop-tmp/dict.pkl','rb')))
nm=lambda v:D.get(v,hex(v))
f=W+'/overlay-r2/u3data/build/main/pak23/ps4/coop-airport-const-ingame12.pak'
d=open(f,'rb').read()
for m in re.finditer(rb'rr-airport-rs-coop-dawn[-a-z0-9]*',d):
    print(hex(m.start()),m.group())
i=d.find(b'rr-airport-rs-coop-dawn-igc-intro')
print(d[i-300:i+200])
c=DC('rsdc/ps3-coop.bin')
s,t,o=c.syms[0]
print(hex(s),hex(t),hex(o), len(c.d),hex(c.reloc))
from dc import dump
print('\n'.join(dump(c,o,60)))
