from lib import *
import json
scan=json.load(open('scan.json'))
names=sorted(set(r['n'] for r in scan)|{'mp-hero-flynn-hair','mp-hero-flynn-coop','mp-hero-lazarevic-coop'})
pat={n:(n.encode()+b'\0',struct.pack('<I',sid(n)),struct.pack('>I',sid(n))) for n in names}
res=collections.defaultdict(lambda:collections.defaultdict(list))
def scan_blob(tag,fn,d):
    for n,(a,b,c) in pat.items():
        ca,cb,cc=d.count(a),d.count(b),d.count(c)
        if ca or cb or cc: res[n][tag].append((fn,ca,cb,cc))
seen=set()
for l in LAY:
    root=f'{l}/u3data/build/main'
    for f in glob.glob(root+'/dc[12]/**/*.bin',recursive=True):
        k=os.path.relpath(f,root).replace(chr(92),'/')
        if k in seen: continue
        seen.add(k)
        if os.path.getsize(f)<50000000: scan_blob('ps4dc',k,open(f,'rb').read())
    for f in glob.glob(root+'/pak23/ps4/*.pak'):
        k=os.path.relpath(f,root).replace(chr(92),'/')
        if k in seen: continue
        seen.add(k)
        if os.path.getsize(f)<3000000 and 'ingame12' in f: scan_blob('ps4lvl',os.path.basename(f),open(f,'rb').read())
# PS3 DC (newest wins)
s3=set()
for lab,f in [('patch18',X+'dlc-patch18.psarc'),('patch13',X+'dlc-patch13.psarc'),('disc',DISC+'bin.psarc')]:
    a=Psarc(f)
    for i,n in enumerate(a.names):
        if '/dc' in '/'+n and n.endswith('.bin') and 'vox' not in n and 'render-settings' not in n:
            k=n.split('dc1/')[-1]
            if k in s3: continue
            s3.add(k); scan_blob('ps3dc',k,a.read(i+1))
a=Psarc(DISC+'pak23.psarc')
for i,n in enumerate(a.names):
    if 'ingame12' in n and a.entries[i+1][2]<3000000: scan_blob('ps3lvl',os.path.basename(n),a.read(i+1))
out={n:{t:v for t,v in res[n].items()} for n in names}
json.dump(out,open('usage.json','w'),indent=0)
for n in names:
    u=out[n]; print(n)
    for t,v in u.items(): print('   ',t,len(v),[(x[0][:44],x[1:]) for x in v[:7]])
