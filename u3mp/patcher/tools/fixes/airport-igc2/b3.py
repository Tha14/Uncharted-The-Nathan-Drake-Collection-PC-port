from lib import *
from dc import DC,sid,dump
from psarc_lib import Psarc
import re,struct,pickle
D=dict(pickle.load(open('../live/coop-tmp/dict.pkl','rb')))
c=DC(G+'/u3data/build/main/dc1/ps4/hud-settings.bin')
d=c.d
i=d.find(b'binoculars\0')
print(hex(i),hex(len(d)),hex(c.reloc))
print(re.findall(rb'[ -~]{3,}',d[i-500:i+300]))
# pointers to that string
ps=[o for o in c.ptrs if c.ptr(o)==i]
print([hex(x) for x in ps])
for o in ps:
    # dump struct around
    print('\n'.join(dump(c,o-0x40,36)) if False else '')
    for k in range(-16,40):
        q=o+k*8
        if q in c.ptrs:
            v=c.ptr(q);s=c.cstr(v);print(hex(q),'->',hex(v),s if s and s.isprintable() and len(s)>2 else '')
        else:
            a,b=struct.unpack_from('<II',d,q);fa,fb=struct.unpack_from('<ff',d,q);print(hex(q),hex(a),hex(b),'%.3f %.3f'%(fa,fb),D.get(a,''),D.get(b,''))
print(open(G+'/u3data/build/main/dc1/ps4/hud-settings.dci').read()[:900])
print(sorted(set(hex(t) for s,t,o in c.syms)),len(c.syms))
