"""write reflection JSON from _recs.pkl: mkhk_write.py <EBOOT> <ps4elf> <recs.pkl> <outdata>"""
import sys,pickle,json,os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from mkhk import E,finish
a=E(sys.argv[1]);b=E(sys.argv[2]);ra,rb=pickle.load(open(sys.argv[3],'rb'))
for q in ra:
  q['mem']=q['slots'].get(0x70) or 0;q['nmem']=q['slots'].get(0x78) or 0
A=finish(a,ra);B=finish(b,rb)
json.dump(A,open(sys.argv[4]+'/hk2010_ps3.json','w'));json.dump(B,open(sys.argv[4]+'/hk2011_ps4.json','w'))
for n,c in(('ps3',A),('ps4',B)):
  print(n,len(c),sum(1 for v in c.values() if v['members']),{k:(c[k]['parent'],c[k]['size'],len(c[k]['members'])) for k in('hkpRigidBody','hkpEntity','hkpWorldObject','hkpBoxShape','hkpConvexVerticesShape','hkReferencedObject','hkBaseObject') if k in c})
