import sys,os
sys.dont_write_bytecode=True
sys.path.insert(0,'{{PAKTOOLS}}')
from ndpak import sid
W='{{PROJECT}}/work'
G='{{PS4}}'
want={0x4476eaf7,0xf9b797a4,0x718e4549}
names=set()
for l in [W+'/overlay-r3',W+'/overlay-r2',W+'/overlay-port',W+'/overlay-mp',G]:
    d=l+'/u3data/build/main/actor33/ps4'
    if os.path.isdir(d):
        for f in os.listdir(d): names.add(f[:-4])
for n in sorted(names):
    for v in (n, n+'.pak', n.replace('-','_')):
        if sid(v) in want: print('%08x'%sid(v), v)
