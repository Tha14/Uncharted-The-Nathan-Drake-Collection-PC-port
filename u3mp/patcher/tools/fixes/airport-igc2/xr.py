import numpy as np,sys
d=np.fromfile('../live/instance-mat/_text.bin',dtype=np.uint8)[:0x1440000]
v=d[:-3].astype(np.int64)|d[1:-2].astype(np.int64)<<8|d[2:-1].astype(np.int64)<<16|d[3:].astype(np.int64)<<24
v=np.where(v>=2**31,v-2**32,v)
pos=np.arange(len(v))
np.save('rip_tg.npy',(pos+4+v).astype(np.int64))
