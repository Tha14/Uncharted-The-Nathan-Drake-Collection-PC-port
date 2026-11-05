import struct,os,sys
R="{{PROJECT}}/work/overlay-r3/u3data/build/main/text1/ps4/"
fs=[f for f in os.listdir(R) if f.startswith('1.') or f.startswith('2.')]
print(fs[:12])
for a in sys.argv[1:]:
  s=int(a,16);o=[]
  for f in fs:
    d=open(R+f,'rb').read();p=d.find(struct.pack('<I',s))
    while p>=0 and len(o)<4:
      x,y=struct.unpack_from('<II',d,p+4) if p+12<len(d) else (0,0)
      for off in(x,y):
        if 0<off<len(d):
          e=d.find(b'\0',off);t=d[off:e]
          if t and all(c>=9 for c in t[:20]):o.append(f+':'+t[:50].decode('utf8','replace'))
      p=d.find(struct.pack('<I',s),p+1)
  print(a,o[:3])
