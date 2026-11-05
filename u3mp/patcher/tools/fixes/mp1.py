import sys,os,collections
sys.path.insert(0,"{{TRANSFER_TOOLS}}")
from ndpak import Pak
from psarc_lib import Psarc
W="{{PROJECT}}/work/"
RET="{{PS4}}/u3data/build/main/pak23/ps4/"
PS3="{{PS3_DISC}}"
pk=Psarc(PS3+'/build/main/pak23.psarc')
def p4(n):
  for o in('overlay-r3','overlay-r2','overlay-port','overlay-mp'):
    f=W+o+'/u3data/build/main/pak23/ps4/'+n+'.pak'
    if os.path.exists(f):return open(f,'rb').read(),o
  f=RET+n+'.pak'
  return (open(f,'rb').read(),'retail') if os.path.exists(f) else (None,None)
def p3(n):
  c=[x for x in pk.names if x.endswith('/'+n+'.pak')]
  return pk.read(pk.names.index(c[0])+1) if c else None
if __name__=='__main__':
  for n in sys.argv[1:]:
    d,o=p4(n);P=Pak(d) if d else None
    d3=p3(n);Q=Pak(d3) if d3 else None
    print('==',n,o,len(d) if d else 0,'ps3',len(d3) if d3 else 0)
    for lab,x in(('ps4',P),('ps3',Q)):
      if not x:continue
      c=collections.Counter();s=collections.Counter()
      for it in x.items:c[it.type]+=1;s[it.type]+=it.size
      print(lab,sorted((k,c[k],s[k]) for k in c))
