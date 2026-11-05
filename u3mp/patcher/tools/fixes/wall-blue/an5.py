import sys,os,re,json
M2=os.path.abspath('scratch/map-colors2');sys.path.insert(0,M2)
from lib import *
a=open(OV+'air-multi-strip.pak','rb').read();b=open(W+'backup-excess/air-multi-strip.pak','rb').read();print(len(a),len(b));d=[i for i in range(len(a)) if a[i]!=b[i]];print('strip byte diffs',len(d),hex(d[0]),hex(d[-1]))
for tag,pk in (('main','air-multi.pak'),('stage','air-multi-stage.pak'),('shade','air-multi-shade.pak')):
  A={m['n'].split(',')[0]:m for m in mats4(Pak(OV+pk))};B={m['n'].split(',')[0]:m for m in mats4(Pak(W+'backup-mapcolor2/'+pk))}
  ch=[k for k in A if k in B and (A[k]['p']!=B[k]['p'] or A[k]['b']!=B[k]['b'] or A[k]['h']!=B[k]['h'])]
  print(tag,'materials changed by mapcolor2:',len(A),len(ch));
  for k in ch:
    c=A[k];e=B[k];print(' ',k,e['h'],'->',c['h'],'params diff',[(x,y) for x,y in zip(c['p'],e['p']) if x!=y][:6], 'bind diff',[(x[0],x[1].split('/')[-1]) for x in c['b'] if x not in e['b']])
for m in mats4(Pak(OV+'air-multi.pak')):
  if re.search('ground-concrete-multi:|plane-hanger-roof|roof',m['n']):print(m['n'].split(',')[0],[(q[0],[round(v,3) for v in q[2][:q[1]]]) for q in m['p'] if 'olor' in q[0]],[(x[0].replace('g_tNdFetch',''),x[1].split('/')[-1]) for x in m['b'] if 'Light' not in x[0]])
