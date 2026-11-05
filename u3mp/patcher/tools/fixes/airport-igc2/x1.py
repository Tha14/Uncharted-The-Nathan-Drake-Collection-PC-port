import numpy as np,pathlib,sys,re,pickle
d=pathlib.Path('../live/instance-mat/_text.bin').read_bytes()
a=np.frombuffer(d[:0x1480000],dtype=np.uint8)
# rex + 8d + modrm rip (mod00 rm101)
idx=np.where((a[:-7]&0xfb==0x48)&(a[1:-6]==0x8d)&((a[2:-5]&0xc7)==0x05))[0]
disp=np.frombuffer(d,dtype=np.uint8)
print(len(idx))
dv=(a[idx+3].astype(np.int64)|a[idx+4].astype(np.int64)<<8|a[idx+5].astype(np.int64)<<16|a[idx+6].astype(np.int64)<<24)
dv=np.where(dv>=2**31,dv-2**32,dv)
tg=idx+7+dv
np.save('leaidx.npy',idx);np.save('leatg.npy',tg)
for s in [b':vignette-enable',b'Enable Vignette',b'Radial Fade Blend In',b':tint-rgb-multiplier',b':exposure-scalar',b':dof-blur-radius',b'Color Blur Radius']:
    i=d.find(s);j=d.rfind(b'\0',0,i)+1
    print(s,hex(j),[hex(x) for x in idx[tg==j]][:6])
