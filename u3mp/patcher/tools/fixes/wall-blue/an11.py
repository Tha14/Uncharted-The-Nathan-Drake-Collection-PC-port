from ilib import *
os.chdir(M2)
for tag,fn in (('ps3','p3/air-multi.pak'),('ov',OV+'air-multi.pak')):
  d=open(fn,'rb').read()
  for kw in (b'saturat',b'tint-mult',b'color-mult',b'ambient',b'-exposure',b'sky-fog',b'light-tint'):
    hits=[m.start() for m in re.finditer(kw,d)]
    print(tag,kw,len(hits),[d[max(0,h-24):h+32].split(b'\0')[-1 if False else 0][:50] for h in hits[:3]])
  q=Pak(fn);print(collections.Counter(i.type for i in q.items).most_common(25))
