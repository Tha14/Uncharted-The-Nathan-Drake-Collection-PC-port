from lib import *
from PIL import Image,ImageDraw
n='air-multi';D=np.load('top-%s.npz'%n);names={int(k):v for k,v in json.load(open('top-%s.json'%n)).items()}
ID=D['ID'];lo=D['lo'];sc=float(D['sc'])
for pat in ['concrete-multi-red']:
    ks=[k for k,v in names.items() if pat in v]
    yy,xx=np.nonzero(np.isin(ID,ks));print(pat,xx.min()/sc+lo[0],xx.max()/sc+lo[0],yy.min()/sc+lo[2],yy.max()/sc+lo[2],len(xx))
x0,x1,z0,z1=int(xx.min()/sc+lo[0])-12,int(xx.max()/sc+lo[0])+12,int(yy.min()/sc+lo[2])-12,int(yy.max()/sc+lo[2])+12
if len(sys.argv)>4:x0,x1,z0,z1=[int(v) for v in sys.argv[1:5]]
a=(int((x0-lo[0])*sc),int((z0-lo[2])*sc));b=(int((x1-lo[0])*sc),int((z1-lo[2])*sc));C=ID[a[1]:b[1],a[0]:b[0]]
c=collections.Counter(C.ravel().tolist());top=[k for k,v in c.most_common(16) if k>=0]
PAL=[(230,25,75),(60,180,75),(255,225,25),(0,130,200),(245,130,48),(145,30,180),(70,240,240),(240,50,230),(210,245,60),(250,190,190),(0,128,128),(220,190,255),(170,110,40),(128,128,128),(255,255,255),(100,100,0)]
im=np.full(C.shape+(3,),30,'uint8')
for i,k in enumerate(top):im[C==k]=PAL[i]
f=max(1,int(1200/C.shape[1]));I=Image.fromarray(im).resize((C.shape[1]*f,C.shape[0]*f),0);dr=ImageDraw.Draw(I)
for i,k in enumerate(top):dr.rectangle((0,i*12,10,i*12+10),fill=PAL[i]);dr.text((12,i*12),names[k].rsplit('/',1)[-1][:40]+' %d'%(c[k]/sc/sc),fill=(255,255,255))
I.save('crop2.png');print(I.size,x0,x1,z0,z1,f)
