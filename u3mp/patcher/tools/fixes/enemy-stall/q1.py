import sys,os,re
P='{{PROJECT}}'
sys.path.insert(0,P+'/scratch/recovered/coop-regions');sys.path.insert(0,"{{TRANSFER_TOOLS}}")
sys.path.insert(0,'{{PAKTOOLS}}')
from detect import ps3idx,eff
from regs import R,Pak
rx=re.compile(sys.argv[2]);name=sys.argv[1]
def show(d,label):
  p=Pak(d);r=R(p);print('##',label,p.platform)
  for it in r.trees():
    g,rs=r.tree(it)
    for a in rs:
      x=r.region(a)
      if not rx.search(x['name'] or ''):continue
      print(x['name'],hex(x['sid']),'nt',x['nt'],'A',x['A'],'sp',x['sp'],'tags',[(hex(k),t,v if isinstance(v,str) else hex(v)) for k,t,v in x['tags']])
      if x['script']:print('   script',hex(x['script']['sid']),x['script']['code'].hex(),x['script']['k'])
f,o=eff(name);print('ps4 file',f,o)
if f:show(open(f,'rb').read(),'PS4 '+o)
I=ps3idx()
if name in I:
  p,i,src=I[name];show(p.read(i),'PS3 '+src)
else:print('no ps3',name,[n for n in I if 'syria' in n])
