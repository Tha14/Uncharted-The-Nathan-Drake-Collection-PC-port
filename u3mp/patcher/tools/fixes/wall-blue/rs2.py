import sys,struct,os,difflib
exec(open('rs1.py').read().split('def get(names):')[0])
for nm in sys.argv[1:]:
  s=sid(nm);a=None
  for k,v in ps3.items():
    x=v[0].read(v[1])
    try:
      e=ents3(x)
    except Exception:continue
    if s in e:a=(k,x,e[s])
  H4=[(k,c,ents4(c)[s]) for k,c in ps4.items() if s in ents4(c)]
  print(nm,'ps3',a[0],'ps4',[h[0] for h in H4]);k,c,o=H4[-1]
  A=[round(struct.unpack_from('>f',a[1],a[2]+x)[0],4) for x in range(0,0xc20,4)]
  B=[round(struct.unpack_from('<f',c,o+x)[0],4) for x in range(0,0xc60,4)]
  sm=difflib.SequenceMatcher(None,A,B,autojunk=False)
  for t,i1,i2,j1,j2 in sm.get_opcodes():
    if t!='equal':print('  ',t,hex(i1*4),A[i1:i2][:14],'->',hex(j1*4),B[j1:j2][:14])
    else:print('   eq',hex(i1*4),hex(j1*4),i2-i1) if i2-i1>30 else None
