import glob,os,struct,sys,pickle,collections
P='{{PROJECT}}'
D=dict(pickle.load(open(P+'/scratch/live/coop-tmp/dict.pkl','rb')))
team=0xd50bc34b
fs=glob.glob(glob.escape(P+'/work/overlay-r3/u3data/build/main')+'/**/*',recursive=True)+glob.glob(glob.escape("{{PS4}}/u3data/build/main")+'/**/*',recursive=True)+[P+'/scratch/live/instance-mat/_text.bin']
seen=set()
for f in fs:
  if not os.path.isfile(f):continue
  b=os.path.basename(f)
  if b in seen:continue
  seen.add(b);d=open(f,'rb').read();s=struct.pack('<I',team);k=-1;h=collections.Counter()
  while True:
    k=d.find(s,k+1)
    if k<0:break
    if k%4:continue
    nxt=struct.unpack_from('<I',d,k+4)[0];h[nxt]+=1
  if h:print(b,[(D.get(v,hex(v)),c) for v,c in h.most_common(6)])
