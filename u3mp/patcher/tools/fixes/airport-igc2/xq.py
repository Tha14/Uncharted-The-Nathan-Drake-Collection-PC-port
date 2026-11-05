import numpy as np,sys
t=np.load('rip_tg.npy',mmap_mode='r')
for x in sys.argv[1:]:
    a=int(x,16)
    r=[]
    for k in (0,1,4,5,8):
        r+= [int(q) for q in np.where(np.asarray(t)==a-k)[0]]
    print(x,[hex(q) for q in sorted(set(r))][:30])
