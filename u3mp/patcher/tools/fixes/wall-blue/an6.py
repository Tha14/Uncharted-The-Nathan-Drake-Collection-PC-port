from ilib import *
RX='corrugated|sheet-wall|roof|ground-concrete-multi:|container'
for tag,fn in (('ov air-multi',OV+'air-multi.pak'),('rt airport-rest',RT+'airport-rest.pak'),('rt airport-1',RT+'airport-1.pak'),('ov coop-3a',OV+'coop-airport-3a.pak'),('rt 3a',RT+'airport-3a.pak')):
  if not os.path.exists(fn):print(tag,'absent');continue
  o,lm=instlight(Pak(fn),RX);print('==',tag,'lit submesh-instances with lm flag',sum(lm.values()))
  T=[x for v in o.values() for x in v]
  if T:
    A=np.concatenate(T);print('  ALL',A.shape,'mean',A.mean(0).round(3).tolist(),'b/r mean', (A[:,2].mean()/A[:,0].mean()).round(2))
  for k,v in sorted(o.items()):
    if re.search('corrugated-main|hangar',k):
      x=np.concatenate(v);print(' ',k.split('/')[-1],len(v),x.shape[0],'mean',x.mean(0).round(3).tolist(),'p50',np.median(x,0).round(3).tolist(),'lm',lm.get(k))
