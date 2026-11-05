from lib import *
from PIL import Image,ImageDraw
n='air-multi';D=np.load('top-%s.npz'%n);names={int(k):v for k,v in json.load(open('top-%s.json'%n)).items()}
ID=D['ID'];lo=D['lo'];sc=float(D['sc'])
x0,x1,z0,z1=[int(v) for v in sys.argv[1:5]]
px=lambda x,z:(int((x-lo[0])*sc),int((z-lo[2])*sc))
a=px(x0,z0);b=px(x1,z1);C=ID[a[1]:b[1],a[0]:b[0]]
c=collections.Counter(C.ravel().tolist());top=[k for k,v in c.most_common(14) if k>=0]
print([(round(c[k]/sc/sc),names[k].rsplit('/',1)[-1]) for k in top])
PAL=[(230,25,75),(60,180,75),(255,225,25),(0,130,200),(245,130,48),(145,30,180),(70,240,240),(240,50,230),(210,245,60),(250,190,190),(0,128,128),(220,190,255),(170,110,40),(128,128,128)]
im=np.full(C.shape+(3,),30,'uint8')
for i,k in enumerate(top):im[C==k]=PAL[i]
I=Image.fromarray(im).resize((C.shape[1]*2,C.shape[0]*2),0);dr=ImageDraw.Draw(I)
for i,k in enumerate(top):dr.rectangle((0,i*12,10,i*12+10),fill=PAL[i]);dr.text((12,i*12),names[k].rsplit('/',1)[-1][:40],fill=(255,255,255))
I.save('crop.png');print(I.size)
