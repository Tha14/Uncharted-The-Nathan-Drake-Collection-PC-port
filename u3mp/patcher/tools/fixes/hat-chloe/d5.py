import sys
sys.dont_write_bytecode=True
sys.path.insert(0,'hat-chloe');sys.path.insert(0,'dctools')
import d4
from d4 import V,DC,a,b
G="{{PS4}}/u3data/build/main/dc1/ps4/net.bin"
r=DC(G);R=V(r);A=V(a);B=V(b)
print('retail',R.n)
for ps in(0x8f89357a,0xd22cbb8d,0x4a7d3938,0xc9d6ac4b):
  print('%08x retail'%ps,R.ents(ps));print('  overlay',A.ents(ps));print('  ps3',B.ents(ps))
nm={R.name(j):j for j in range(R.n)}
for nmx in('chloe-u2','chloe-u2-leather','rescuer','dead-agent','killzone-a','eddie','flynn'):
  print(nmx,'retail idx',nm.get(nmx),[ 'None' if not R.slot(nm[nmx],k) else 'y' for k in range(6)] if nmx in nm else '')
  for V_,tag in((A,'ov'),(B,'ps3')):
    i=[j for j in range(V_.n) if V_.name(j)==nmx][0]
    print(' ',tag,'body',[(x['art'],x['kind'],hex(x['m0'])) for x in V_.first(V_.slot(i,0))])
    if nmx in('eddie','flynn','killzone-a'):print('   acc',[(x['art'],hex(x['m0'])) for x in V_.first(V_.slot(i,2))])
    print('   torso',[(x['art'],hex(x['m0'])) for x in V_.first(V_.slot(i,1))])
