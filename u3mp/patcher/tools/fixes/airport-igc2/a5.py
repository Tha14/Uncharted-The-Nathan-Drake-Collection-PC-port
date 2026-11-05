import struct,pickle,re,sys
sys.path.insert(0,'{{PAKTOOLS}}')
from dc import DC,sid
D=dict(pickle.load(open('../live/coop-tmp/dict.pkl','rb')))
G='{{PS4}}/u3data/build/main/dc1/ps4/render-settings/game/'
d=open(G+'airport.dci','rb').read()
print(len(d),d[:120])
c=DC(G+'airport.bin'); c3=DC('rsdc/ps3-airport.bin')
print(len(c.d),hex(c.reloc),sorted(set(hex(t) for s,t,o in c.syms)))
of=sorted(o for s,t,o in c.syms); print('ps4 offs',[hex(x) for x in of[:5]], 'sz',of[1]-of[0], of[2]-of[1])
of3=sorted(o for s,t,o in c3.syms); print('ps3 offs',[hex(x) for x in of3[:5]], of3[1]-of3[0])
S=0x18cd68be
t,o=c.bysid[S];t3,o3=c3.bysid[S];print(hex(o),hex(o3))
sz3=min(x for x in of3 if x>o3)-o3 if o3<of3[-1] else c3.reloc-o3
sz=min(x for x in of if x>o)-o if o<of[-1] else c.reloc-o
print('sizes',sz,sz3)
# float compare, aligned dump
n=0
for i in range(0,min(sz,sz3),4):
    a=c.d[o+i:o+i+4];b=c3.d[o3+i:o3+i+4][::-1]
    if a!=b:
        n+=1
        if n<40: print(hex(i),a.hex(),b.hex(),struct.unpack('<f',a)[0],struct.unpack('<f',b)[0])
print('diffs',n,'of',min(sz,sz3)//4)
