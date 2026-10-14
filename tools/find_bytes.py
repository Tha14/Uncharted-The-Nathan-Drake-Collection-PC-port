"""Read-only: find byte patterns from a file in gow3-probe.exe memory.
Usage: find_bytes.py <file> <offset> <length> [<offset> <length> ...]"""
import ctypes as c, ctypes.wintypes as w, subprocess, sys
k=c.WinDLL('kernel32'); k.OpenProcess.restype=w.HANDLE
k.VirtualQueryEx.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t]
k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
class MBI(c.Structure):
    _fields_=[("base",c.c_void_p),("alloc",c.c_void_p),("aprot",w.DWORD),("part",w.WORD),("size",c.c_size_t),("state",w.DWORD),("protect",w.DWORD),("type",w.DWORD)]
pid=int([l for l in subprocess.run(['tasklist','/FI','IMAGENAME eq gow3-probe.exe','/FO','CSV','/NH'],capture_output=True,text=True).stdout.splitlines() if 'gow3' in l][0].split('","')[1])
h=k.OpenProcess(0x410,False,pid)
data=open(sys.argv[1],'rb').read()
pats=[(int(sys.argv[i],0),data[int(sys.argv[i],0):int(sys.argv[i],0)+int(sys.argv[i+1],0)]) for i in range(2,len(sys.argv),2)]
found={o:[] for o,_ in pats}
addr=0; m=MBI()
while addr<0x7FFFFFFF0000 and k.VirtualQueryEx(h,c.c_void_p(addr),c.byref(m),c.sizeof(m)):
    base=m.base or 0; size=m.size
    if m.state==0x1000 and (m.protect&0xFF) in (2,4,8,0x20,0x40,0x80) and not m.protect&0x100 and base<0x7ff000000000:
        for off in range(0,size,1<<24):
            n=min(1<<24,size-off); b=c.create_string_buffer(n); g=c.c_size_t()
            if not k.ReadProcessMemory(h,c.c_void_p(base+off),b,n,c.byref(g)): continue
            d=b.raw[:g.value]
            for o,p in pats:
                i=d.find(p)
                while i>=0 and len(found[o])<8: found[o].append(hex(base+off+i)); i=d.find(p,i+1)
    addr=base+size
for o,_ in pats: print(f'file offset {o:#x}:',found[o])
