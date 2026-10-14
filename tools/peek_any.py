"""Read-only: dump guest/host memory of gow3-probe.exe as floats/hex. Usage: peek_any.py addr [bytes]"""
import ctypes as c, ctypes.wintypes as w, subprocess, struct, sys
k=c.WinDLL('kernel32'); k.OpenProcess.restype=w.HANDLE
k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
pid=int([l for l in subprocess.run(['tasklist','/FI','IMAGENAME eq gow3-probe.exe','/FO','CSV','/NH'],capture_output=True,text=True).stdout.splitlines() if 'gow3' in l][0].split('","')[1])
h=k.OpenProcess(0x410,False,pid)
a=int(sys.argv[1],16); n=int(sys.argv[2],0) if len(sys.argv)>2 else 0x100
b=c.create_string_buffer(n); g=c.c_size_t(); k.ReadProcessMemory(h,c.c_void_p(a),b,n,c.byref(g)); d=b.raw[:g.value]
for i in range(0,len(d),32):
    ws=struct.unpack_from('<8I',d.ljust(i+32,b'\0'),i)
    fs=struct.unpack_from('<8f',d.ljust(i+32,b'\0'),i)
    print(f'{a+i:#x}',' '.join(f'{f:>9.4g}' if 1e-5<abs(f)<1e7 or w_==0 else f'{w_:#9x}' for f,w_ in zip(fs,ws)))
