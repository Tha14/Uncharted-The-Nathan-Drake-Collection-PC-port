from PIL import Image
import numpy as np
im=Image.open('C:/Users/jonas/AppData/Local/Temp/claude/C--Users-jonas-Documents-ChatGPT-test/aabce4ac-8b71-42ff-bf7d-c961b3b0ff9f/images/113.webp').convert('RGB')
print(im.size)
a=np.asarray(im).astype(float)
W,H=im.size
print('mean rgb',a.mean((0,1)))
# flynn torso region, face, truck window, wall, ground, crouch figure
R={'flynn torso':(1480,300,1560,420),'flynn legs':(1470,650,1540,780),'flynn face':(1475,110,1525,170),'arm':(1370,320,1420,370),'window':(1380,90,1460,180),'truck':(1200,100,1300,200),'wall':(600,200,900,500),'ground':(400,620,900,680),'crouch back':(760,400,820,460),'crouch white':(690,470,760,560),'grass':(1100,840,1300,880)}
for k,(x0,y0,x1,y1) in R.items():
    print(k,a[y0:y1,x0:x1].mean((0,1)).round(1),'max',a[y0:y1,x0:x1].max((0,1)))
# corner vignette profile
L=a.sum(2)
print('row at y540',[int(L[540,x]) for x in range(0,W,160)])
print('col x960',[int(L[y,960]) for y in range(0,H,90)])
# fit blur centre: structure tensor orientation of the dominant gradient => lines along dominant streak
from numpy.lib.stride_tricks import sliding_window_view as sw
g=L/3
gy,gx=np.gradient(g)
pts=[];dirs=[]
bs=48
for y in range(0,H-bs,bs):
    for x in range(0,W-bs,bs):
        Gx=gx[y:y+bs,x:x+bs];Gy=gy[y:y+bs,x:x+bs]
        J11=(Gx*Gx).sum();J22=(Gy*Gy).sum();J12=(Gx*Gy).sum()
        tr=J11+J22
        if tr<1e3: continue
        coh=np.sqrt((J11-J22)**2+4*J12**2)/tr
        if coh<0.5: continue
        th=0.5*np.arctan2(2*J12,J11-J22) # gradient direction; streak perpendicular
        pts.append((x+bs/2,y+bs/2));dirs.append((-np.sin(th),np.cos(th)))
pts=np.array(pts);dirs=np.array(dirs);print(len(pts))
# least squares intersection with robust iterations
c=None;w=np.ones(len(pts))
for it in range(12):
    A=np.zeros((2,2));b=np.zeros(2)
    for p,d,wi in zip(pts,dirs,w):
        n=np.array([-d[1],d[0]]);A+=wi*np.outer(n,n);b+=wi*np.outer(n,n)@p
    c=np.linalg.solve(A,b)
    r=np.abs(((pts-c)*np.stack([-dirs[:,1],dirs[:,0]],1)).sum(1))
    w=1/(1+(r/40)**2)
print('centre',c,'median resid',np.median(r))
im.crop((1250,40,1650,440)).resize((800,800)).save('crop_flynn.png')
im.crop((1250,320,1450,520)).resize((800,800),Image.NEAREST).save('crop_wrist.png')
im.crop((620,290,950,700)).resize((660,820)).save('crop_crouch.png')
