"""Read-only sampling profiler for guest code in gow3-probe.exe.
Usage: sample_guest.py <seconds> <out.json>
Each round briefly suspends each thread (always resumed), records the guest RIP (image offset)
and guest return addresses found near the top of its stack."""
import ctypes as c, ctypes.wintypes as w, subprocess, struct, sys, time, json, collections
k=c.WinDLL('kernel32',use_last_error=True); k.OpenProcess.restype=w.HANDLE; k.OpenThread.restype=w.HANDLE
k.CreateToolhelp32Snapshot.restype=w.HANDLE
k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
pid=int([l for l in subprocess.run(['tasklist','/FI','IMAGENAME eq gow3-probe.exe','/FO','CSV','/NH'],capture_output=True,text=True).stdout.splitlines() if 'gow3' in l][0].split('","')[1])
h=k.OpenProcess(0x10|0x400,False,pid)
def rd(a,n):
    b=c.create_string_buffer(n); got=c.c_size_t(); k.ReadProcessMemory(h,c.c_void_p(a),b,n,c.byref(got)); return b.raw[:got.value]
class TE(c.Structure): _fields_=[('sz',w.DWORD),('u',w.DWORD),('tid',w.DWORD),('pid',w.DWORD),('bp',c.c_long),('dp',c.c_long),('fl',w.DWORD)]
class CTX(c.Structure): _fields_=[('pad',c.c_ubyte*0x30),('flags',w.DWORD),('mx',w.DWORD),('seg',c.c_ushort*6),('ef',w.DWORD),('dr',c.c_ulonglong*6),('rax',c.c_ulonglong),('rcx',c.c_ulonglong),('rdx',c.c_ulonglong),('rbx',c.c_ulonglong),('rsp',c.c_ulonglong),('rbp',c.c_ulonglong),('rsi',c.c_ulonglong),('rdi',c.c_ulonglong),('r',c.c_ulonglong*8),('rip',c.c_ulonglong),('rest',c.c_ubyte*0x400)]
def threads():
    snap=k.CreateToolhelp32Snapshot(4,0); te=TE(); te.sz=c.sizeof(TE); out=[]
    ok=k.Thread32First(snap,c.byref(te))
    while ok:
        if te.pid==pid: out.append(te.tid)
        ok=k.Thread32Next(snap,c.byref(te))
    k.CloseHandle(snap); return out
B=0xb400000; LO=B+0x1000; HI=B+0x95d060
buf=(c.c_ubyte*(c.sizeof(CTX)+16))(); addr=(c.addressof(buf)+15)&~15; ctx=CTX.from_address(addr)
samples=collections.Counter(); stacks=collections.Counter()
end=time.time()+float(sys.argv[1]); rounds=0; tids=threads()
while time.time()<end:
    rounds+=1
    if rounds%50==0: tids=threads()
    for tid in tids:
        th=k.OpenThread(0x0002|0x0008|0x0040,False,tid)
        if not th: continue
        try:
            if k.SuspendThread(th)==0xFFFFFFFF: continue
            try:
                ctx.flags=0x100003
                if not k.GetThreadContext(th,c.c_void_p(addr)): continue
                rip,rsp=ctx.rip,ctx.rsp
                st=rd(rsp,0x400) if LO<=rip<HI else b''
            finally: k.ResumeThread(th)
        finally: k.CloseHandle(th)
        if not LO<=rip<HI: continue
        rets=[v-B for v in (struct.unpack_from('<Q',st,i)[0] for i in range(0,len(st)-7,8)) if LO<=v<HI][:4]
        samples[rip-B]+=1; stacks[tuple([rip-B]+rets)]+=1
    time.sleep(0.002)
json.dump({'rounds':rounds,'rip':{hex(a):n for a,n in samples.items()},'stacks':[[list(map(hex,s)),n] for s,n in stacks.most_common(3000)]},open(sys.argv[2],'w'))
print('rounds',rounds,'guest samples',sum(samples.values()))
