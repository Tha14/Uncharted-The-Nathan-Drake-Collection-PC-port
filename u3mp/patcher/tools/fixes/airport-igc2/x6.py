import re,struct,glob,pickle
from dc import sid
d=open('../live/instance-mat/_text.bin','rb').read()
p3=open('{{PROJECT}}/scratch/ps3-119/EBOOT.elf','rb').read()
for v in [0x3f336bec,0x309861e2,sid('binoculars'),sid('scope'),sid('sniper')]:
    print(hex(v),'ps4',[hex(m.start()) for m in re.finditer(re.escape(struct.pack('<I',v)),d)][:12],'ps3',[hex(m.start()) for m in re.finditer(re.escape(struct.pack('>I',v)),p3)][:8])
print('hud',[x for x in glob.glob('{{PS4}}/u3data/build/main/dc1/ps4/*') if re.search('hud|retic|scope',x)])
G='{{PS4}}/u3data/build/main/dc1/ps4/'
for f in glob.glob(G+'**/*.bin',recursive=True):
    b=open(f,'rb').read()
    if struct.pack('<I',sid('binoculars')) in b or b'binoculars' in b or b'scope-' in b: print(f[len(G):],len(b),b.count(struct.pack('<I',sid('binoculars'))),b'scope' in b)
