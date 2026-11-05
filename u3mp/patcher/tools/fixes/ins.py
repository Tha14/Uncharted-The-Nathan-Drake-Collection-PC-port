import sys,os,glob,struct,re,hashlib,collections
sys.path.insert(0,"{{TRANSFER_TOOLS}}")
from psarc_lib import Psarc
U="{{PS3_DISC}}"
G="{{PS4}}/u3data/"
W="{{PROJECT}}/work/"
OV=['overlay-r3','overlay-r2','overlay-port','overlay-mp']
p=Psarc(glob.glob(glob.escape(U)+'/**/goty-dlc.psarc',recursive=True)[0])
cat=collections.Counter();miss=collections.Counter();log=[];codec=collections.Counter();bad=[]
for i,n in enumerate(p.names):
  rel=n
  if not re.search(r'\.(xvag|bnk|sb|bin|sfx)$',n) or 'sound' not in n:continue
  d=os.path.dirname(n);cat[d]+=1
  have=os.path.exists(G+rel) or any(os.path.exists(W+o+'/u3data/'+rel) for o in OV)
  if have:continue
  miss[d]+=1
  if n.endswith('.xvag') and d.endswith('streams/sfx'):
    b=p.read(i+1)
    if b[:4]!=b'XVAG':bad.append(n);continue
    fl=b[8];cd=struct.unpack_from('>I',b,0x2c)[0];ch=struct.unpack_from('>I',b,0x28)[0];codec[(hex(fl),cd,ch)]+=1
    out=W+'overlay-r3/u3data/'+rel;open(out,'wb').write(b);log.append('%s\t%d\t%s'%(os.path.basename(n),len(b),hashlib.md5(b).hexdigest()))
    assert open(out,'rb').read()==b
print('dirs',cat.most_common(12));print('missing',miss.most_common(12));print('installed',len(log),sum(int(x.split('\t')[1]) for x in log)/1e6,'MB','codec(flags,codec,ch)',codec,'bad',bad)
open(W+'dlc-sfx-installed.tsv','w').write('\n'.join(log))
