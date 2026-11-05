import sys,os,re,pickle,gzip,json,hashlib
M2=os.path.abspath('../map-colors2');sys.path.insert(0,M2)
from lib import *
from psarc_lib import Psarc
idx=pickle.load(open(M2+'/p3idx.pkl','rb'))
print('p3 entries air', {k:[(os.path.basename(a[0]),a[1]) for a in v] for k,v in idx.items() if k.startswith('air-multi')})
print(list(idx.items())[0][1][0])
src={'ov':OV,'bkmc':W+'backup-mapcolor2/','rt':RT,'bk0':W+'backup-mapcolor/'}
for pak in ['air-multi.pak']:
  for tag,d in src.items():
    fp=d+pak
    if not os.path.exists(fp): print(tag,'absent');continue
    b=open(fp,'rb').read();print('=====',tag,len(b),hashlib.md5(b).hexdigest())
    for m in mats4(Pak(b)):
      if re.search('corrug',m['n']):
        print(m['i'],m['n'].split(',')[0],m['h'])
        print('  F',[f for f in m['f'] if re.match('COLOR|BLEND|VERTEX|TINT',f)])
        print('  B',[(x[0].replace('g_tNdFetch',''),x[1].split('/')[-1]) for x in m['b']])
        print('  P',[(x[0],[round(v,3) for v in x[2]][:x[1] if x[1]<=4 else 4]) for x in m['p'] if re.search('olor|tint|Tint',x[0])])
