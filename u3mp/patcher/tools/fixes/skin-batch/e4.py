import sys,os,pickle,re,glob,collections,json
H=os.path.dirname(os.path.abspath(__file__))
G="{{PS4}}/u3data/build/main/"
R="{{PROJECT}}/"
sys.path.insert(0,H+'/../gh2/u3mp/paktools');sys.path.insert(0,"{{TRANSFER_TOOLS}}")
from ndpak import Pak,sid
from psarc_lib import Psarc
idx=pickle.load(open(H+'/tstrm-idx.pkl','rb'))
p3=Psarc("{{PS3_UPDATE}}/dlc-patch18.psarc")
names3={os.path.basename(n):i for i,n in enumerate(p3.names) if n.startswith('build/main/actor33/') and n.endswith('.pak')}
print('ps3 actor33 paks',len(names3),'p3 mp',sum(n.endswith('-mp.pak') for n in names3))
ov=[R+'work/'+o+'/u3data/build/main/actor33/ps4/' for o in('overlay-r3','overlay-r2','overlay-port','overlay-mp')]+[G+'actor33/ps4/']
seen={}
for d in ov:
    for f in sorted(glob.glob(d+'*-mp.pak')):
        seen.setdefault(os.path.basename(f),f)
print('ps4 mp paks',len(seen),collections.Counter(('overlay' if 'overlay' in v else 'retail') for v in seen.values()))
out={}
p=Pak(seen['hk416-mp.pak'])
for t in p.textures()[:3]:
    k=t['path'].split('texture2/')[-1];print(k,hex(t['hash']),hex(sid(k)),idx.get(k,{}).get('hash'),hex(idx.get(k,{}).get('hash',0)),t['unk20'],idx.get(k,{}).get('w'))
tot=collections.Counter()
for n,f in sorted(seen.items()):
    try:p=Pak(f)
    except Exception as e:print('ERR',n,e);continue
    wpn='/weapons/' in ' '.join(t['path'] for t in p.textures())
    rec=[]
    for t in p.textures():
        k=t['path'].split('texture2/')[-1];e=idx.get(k);rec.append(dict(k=k,w=t['width'],h=t['height'],m=t['mips'],fmt=t['format'],fl=t['unk34'],st=(e['pak'],e['w'],e['h']) if e else None))
    out[n]=dict(path=f,src='ov' if 'overlay' in f else 'ret',wpn=wpn,tex=rec,size=len(p.d),pages=[x[1] for x in p.pages],vram=p.hdr['vramSize'])
json.dump(out,open(H+'/survey.json','w'))
for n,o in sorted(out.items()):
    if not o['wpn']:continue
    miss=[r['k'].split('/')[-2] for r in o['tex'] if not r['st'] and r['fl']==2 and 'color' in r['k']]
    full=[r['k'].split('/')[-2] for r in o['tex'] if r['w']>64 and 'color' in r['k']]
    print(n,o['src'],len(o['tex']),'vram',o['vram'],'pages',[hex(x) for x in o['pages']],'color-no-stream:',miss[:12],'res>64:',len(full))
