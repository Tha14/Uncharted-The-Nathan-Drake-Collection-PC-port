import os,struct,glob
ROOT="{{PROJECT}}/work/"
G="{{PS4}}/u3data/build/main/"
IDS={'map03':0x4df7da2c,'map01':0x40b4fcf5,'dodge':0x2c357ea1,'trench':0x833ebff1,'stadium':0xbc55cab1,'pl':0x4cda20af,'pldesc':0x7e27c8fc}
roots=[ROOT+o+'/u3data/build/main/' for o in('overlay-r3','overlay-r2','overlay-port','overlay-mp')]+[G]
seen=set();hits={k:[] for k in IDS}
for r in roots:
  for f in glob.glob(glob.escape(r)+'text1/**/*',recursive=True):
    if os.path.isfile(f) and os.path.getsize(f)<9000000:
      b=os.path.basename(f);d=open(f,'rb').read()
      for k,v in IDS.items():
        if struct.pack('<I',v) in d or struct.pack('>I',v) in d:hits[k].append(r[-30:-12]+':'+b)
print({k:v[:3] for k,v in hits.items()})
print('text dirs',[ (r[-30:-12],len(glob.glob(glob.escape(r)+'text1/**/*',recursive=True))) for r in roots])
for d in('overlay-mp',):
  p=ROOT+d+'/u3data/build/main/vtex1/ps4/net-levels/lab-stage.dds';print(os.path.getsize(p),open(p,'rb').read(32).hex())
print(open('../labsrc/dds/lab-stadium-v1.dds','rb').read(32).hex(),open('../labsrc/dds/lab-stage.dds','rb').read(32).hex(),os.path.getsize('../labsrc/dds/lab-stage.dds'))
