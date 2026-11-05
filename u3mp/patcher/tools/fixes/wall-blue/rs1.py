import sys,struct,os
PT='{{PAKTOOLS}}'
sys.path.insert(0,PT)
from psarc_lib import Psarc
from ndpak import sid
PS3="{{PS3_DISC}}"
W="{{PROJECT}}/work/"
R="{{PS4}}/u3data/build/main/dc1/ps4/render-settings/game/"
b=Psarc(PS3+'/build/main/bin.psarc');pa=Psarc(PS3+'/goty-patch.psarc')
ps3={}
for a in(b,pa):
  for i,n in enumerate(a.names):
    if 'render-settings/game/' in n and n.endswith('.bin'):ps3[os.path.basename(n)]=(a,i+1)
ps4={}
for tag,d in (('rt',R),('ov',W+'overlay-r3/u3data/build/main/dc1/ps4/render-settings/game/')):
  for x in os.listdir(d):
    if x.endswith('.bin'):ps4[tag+':'+x]=open(d+x,'rb').read()
def ents4(c):
  n=struct.unpack_from('<I',c,0x14)[0];return {struct.unpack_from('<I',c,0x18+i*16)[0]:struct.unpack_from('<Q',c,0x18+i*16+8)[0] for i in range(n)}
def ents3(a):
  n=struct.unpack_from('>I',a,0x14)[0];return {struct.unpack_from('>I',a,0x18+i*12)[0]:struct.unpack_from('>I',a,0x18+i*12+8)[0] for i in range(n)}
def get(names):
  for nm in names:
    s=sid(nm);print('=====',nm,hex(s))
    H3=[]
    for k,v in ps3.items():
      a=v[0].read(v[1])
      try:e=ents3(a)
      except Exception:continue
      if s in e:H3.append((k,a,e[s]))
    H4=[(k,c,ents4(c)[s]) for k,c in ps4.items() if s in ents4(c)]
    print(' ps3',[x[0] for x in H3],'ps4',[x[0] for x in H4])
    for k,a,o in H3[:1]:
      A=[struct.unpack_from('>f',a,o+x)[0] for x in range(0,0xc20,4)]
    for k,c,o in H4:
      Lim=(len(c)-o)//4
      B=[struct.unpack_from('<f',c,o+x)[0] for x in range(0,min(0xc60,Lim*4),4)]
      print('  ',k,'off',hex(o),'len',len(c),'sat@898',B[0x898//4],'tintmul@880',B[0x880//4:0x88c//4],'0x878',B[0x878//4],'0x860',B[0x860//4:0x86c//4])
      if H3:
        nd=0;out=[]
        for x in range(0,0xc20,4):
          if 0x7d0<=x<0x860: y=x
          elif x>=0x860 and x<0xb64: y=x+0x20
          elif x>=0xb64: y=x+0x40
          else: y=x
          if y//4>=len(B):continue
          va,vb=A[x//4],B[y//4]
          if abs(va-vb)>1e-3*max(1,abs(va)) and va==va and vb==vb:
            nd+=1;out.append((hex(x),hex(y),round(va,4),round(vb,4)))
        print('    field diffs vs PS3',nd,out[:45])
    for k,a,o in H3[:1]:
      print('   ps3',k,'sat@878',A[0x878//4],'tintmul@860',A[0x860//4:0x86c//4],'0x898',A[0x898//4])
get(sys.argv[1:])
