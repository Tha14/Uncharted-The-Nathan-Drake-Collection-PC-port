import sys,pickle,struct
P='{{PROJECT}}'
sys.path.insert(0,P+'/scratch/recovered/coop-regions');sys.path.insert(0,'{{PAKTOOLS}}')
from detect import ps3idx,eff
D=dict(pickle.load(open(P+'/scratch/live/coop-tmp/dict.pkl','rb')))
name='coop-syria-pak-ingame12.pak'
f,o=eff(name);ps4=open(f,'rb').read()
I=ps3idx();p,i,src=I[name];ps3=p.read(i)
def ctx(d,e,lab,before=0x40,after=0x40):
  s=struct.pack(e+'I',0x746415d7);k=-1;c=0
  while True:
    k=d.find(s,k+1)
    if k<0 or c>=int(sys.argv[1]):break
    c+=1;print('##',lab,'at',hex(k))
    for a in range(k-before,k+after,4):
      v=struct.unpack_from(e+'I',d,a)[0]
      st=''
      if 0x20<v<len(d):
        try:
          z=d[v:v+40].split(b'\0')[0]
          if len(z)>3 and all(31<ch<127 for ch in z):st='-> "%s"'%z.decode()
        except Exception:pass
      print('  %s%x: %08x %s %s'%('*' if a==k else ' ',a,v,D.get(v,''),st))
ctx(ps4,'<','PS4');ctx(ps3,'>','PS3')
