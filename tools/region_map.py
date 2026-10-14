"""Read-only: nonzero-byte map of a guest range, one character per block. Usage: region_map.py start size [block]"""
import ctypes as c, ctypes.wintypes as w, subprocess, sys
k=c.WinDLL('kernel32'); k.OpenProcess.restype=w.HANDLE
k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
pid=int([l for l in subprocess.run(['tasklist','/FI','IMAGENAME eq gow3-probe.exe','/FO','CSV','/NH'],capture_output=True,text=True).stdout.splitlines() if 'gow3' in l][0].split('","')[1])
h=k.OpenProcess(0x410,False,pid)
a=int(sys.argv[1],16); n=int(sys.argv[2],0); blk=int(sys.argv[3],0) if len(sys.argv)>3 else 0x10000
line=''
for i,at in enumerate(range(a,a+n,blk)):
    b=c.create_string_buffer(blk); g=c.c_size_t(); ok=k.ReadProcessMemory(h,c.c_void_p(at),b,blk,c.byref(g))
    d=b.raw[:g.value]
    nz=sum(1 for x in d[::16] if x) if d else -1
    ch='?' if nz<0 else '.' if nz==0 else str(min(9,nz*10//max(1,len(d)//16)))
    line+=ch
    if len(line)==64: print(f'{at-63*blk:#x} {line}'); line=''
if line: print(line)
