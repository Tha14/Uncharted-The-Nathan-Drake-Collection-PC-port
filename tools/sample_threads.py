"""Read thread contexts/stacks from the exact workspace game runtime on Windows.

Each thread is briefly suspended and always resumed in a finally block. No target
memory is written. Only addresses, thread names and registers are retained.
"""
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT=Path(__file__).resolve().parent.parent
kernel=c.WinDLL("kernel32",use_last_error=True)
kernel.CreateToolhelp32Snapshot.argtypes=[w.DWORD,w.DWORD]; kernel.CreateToolhelp32Snapshot.restype=w.HANDLE
kernel.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD]; kernel.OpenProcess.restype=w.HANDLE
kernel.OpenThread.argtypes=[w.DWORD,w.BOOL,w.DWORD]; kernel.OpenThread.restype=w.HANDLE
kernel.CloseHandle.argtypes=[w.HANDLE]
kernel.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
kernel.QueryFullProcessImageNameW.argtypes=[w.HANDLE,w.DWORD,w.LPWSTR,c.POINTER(w.DWORD)]
kernel.SuspendThread.argtypes=[w.HANDLE]; kernel.SuspendThread.restype=w.DWORD
kernel.ResumeThread.argtypes=[w.HANDLE]; kernel.ResumeThread.restype=w.DWORD
kernel.GetThreadContext.argtypes=[w.HANDLE,c.c_void_p]
class Process(c.Structure):
    _fields_=[("size",w.DWORD),("usage",w.DWORD),("pid",w.DWORD),("heap",c.c_size_t),
              ("module",w.DWORD),("threads",w.DWORD),("parent",w.DWORD),
              ("priority",w.LONG),("flags",w.DWORD),("name",w.WCHAR*260)]
class Thread(c.Structure):
    _fields_=[("size",w.DWORD),("usage",w.DWORD),("tid",w.DWORD),("pid",w.DWORD),
              ("priority",w.LONG),("delta",w.LONG),("flags",w.DWORD)]
class Module(c.Structure):
    _fields_=[("size",w.DWORD),("id",w.DWORD),("pid",w.DWORD),("global_use",w.DWORD),
              ("process_use",w.DWORD),("base",c.c_void_p),("bytes",w.DWORD),
              ("handle",w.HMODULE),("name",w.WCHAR*256),("path",w.WCHAR*260)]
def inventory(flag,kind,first,next,pid=0):
    snapshot=kernel.CreateToolhelp32Snapshot(flag,pid)
    if snapshot==c.c_void_p(-1).value: raise c.WinError(c.get_last_error())
    begin=getattr(kernel,first); advance=getattr(kernel,next)
    for method in (begin,advance): method.argtypes=[w.HANDLE,c.POINTER(kind)]
    item=kind(); item.size=c.sizeof(item)
    try:
        valid=begin(snapshot,c.byref(item))
        while valid:
            yield kind.from_buffer_copy(item)
            valid=advance(snapshot,c.byref(item))
    finally: kernel.CloseHandle(snapshot)
def identify():
    expected=ROOT / "gow3_pc/out/gow3-probe.exe"
    for process in inventory(2,Process,"Process32FirstW","Process32NextW"):
        if process.name.lower()!=expected.name.lower(): continue
        handle=kernel.OpenProcess(0x1000|0x10,False,process.pid)
        if not handle: continue
        path=c.create_unicode_buffer(32768); size=w.DWORD(len(path))
        if kernel.QueryFullProcessImageNameW(handle,0,path,c.byref(size)) and Path(path.value).resolve()==expected.resolve():
            return process.pid,handle
        kernel.CloseHandle(handle)
    raise RuntimeError("The exact workspace runtime is not running")
pid,process=identify()
def read(address,size):
    data=c.create_string_buffer(size); used=c.c_size_t()
    kernel.ReadProcessMemory(process,address,data,size,c.byref(used))
    return data.raw[:used.value]
modules=list(inventory(8|16,Module,"Module32FirstW","Module32NextW",pid))
native=next(m for m in modules if m.name.lower()=="gow3-probe.exe")
symbols=subprocess.check_output(["C:/msys64/clang64/bin/llvm-nm.exe",str(ROOT/"gow3_pc/out/gow3-probe.exe")],text=True)
slot_symbol=re.search(r"^([0-9a-fA-F]+) [a-zA-Z] tls_slot$",symbols,re.M)
if not slot_symbol: raise RuntimeError("Runtime TLS symbol unavailable; rebuild with symbols")
slot_data=read(native.base+int(slot_symbol[1],16)-0x140000000,4)
tls_slot=struct.unpack("<I",slot_data)[0] if len(slot_data)==4 else 64
ntdll=c.WinDLL("ntdll")
class ThreadInfo(c.Structure):
    _fields_=[("exit",w.LONG),("teb",c.c_void_p),("pid",c.c_void_p),
              ("tid",c.c_void_p),("affinity",c.c_size_t),("priority",w.LONG),("base_priority",w.LONG)]
ntdll.NtQueryInformationThread.argtypes=[w.HANDLE,w.ULONG,c.c_void_p,w.ULONG,c.c_void_p]
log=(ROOT/"analysis/uncharted/boot.log").read_text(errors="replace")
match=re.search(r"Guest image base: ([\da-fA-F]+)",log)
base=int(match[1],16) if match else 0
analysis=json.loads((ROOT/"analysis/uncharted/analysis.json").read_text(encoding="utf-8"))
executable=[(base+p["vaddr"],base+p["vaddr"]+p["memsz"]) for p in analysis["program_headers"] if p["type"]==1 and p["flags"]&1]
def label(address):
    if base<=address<base+0x2000000: return f"guest+0x{address-base:x}"
    for module in modules:
        if module.base<=address<module.base+module.bytes: return f"{module.name}+0x{address-module.base:x}"
    return hex(address)
gpu_symbol=re.search(r"^([0-9a-fA-F]+) [a-zA-Z] _ZN9Gow3Stats13gpu_thread_idE$",symbols,re.M)
gpu_tid=struct.unpack("<I",read(native.base+int(gpu_symbol[1],16)-0x140000000,4))[0] if gpu_symbol else 0
report={"pid":pid,"guest_base":hex(base),"native_base":hex(native.base),"tls_slot":tls_slot,"gpu_tid":gpu_tid,"threads":[]}
try:
    for thread in inventory(4,Thread,"Thread32First","Thread32Next"):
        if thread.pid!=pid: continue
        handle=kernel.OpenThread(0x800|0x40|0x8|0x2,False,thread.tid)
        if not handle: continue
        item={"tid":thread.tid,"name":""}
        if thread.tid==gpu_tid: item["name"]="GPU command processor"
        description=c.c_wchar_p()
        kernel.GetThreadDescription.argtypes=[w.HANDLE,c.POINTER(c.c_wchar_p)]
        if kernel.GetThreadDescription(handle,c.byref(description))==0 and description:
            item["name"]=description.value
            kernel.LocalFree.argtypes=[c.c_void_p]
            kernel.LocalFree(c.cast(description,c.c_void_p))
        suspended=False
        try:
            suspended=kernel.SuspendThread(handle)!=0xffffffff
            if not suspended: continue
            info=ThreadInfo()
            if tls_slot<64 and ntdll.NtQueryInformationThread(handle,0,c.byref(info),c.sizeof(info),None)==0:
                slot=read(info.teb+0x1480+8*tls_slot,8)
                tcb=read(struct.unpack("<Q",slot)[0],32) if len(slot)==8 else b""
                if len(tcb)==32:
                    owner=struct.unpack_from("<Q",tcb,16)[0]
                    name=read(owner+104,32).split(b"\0")[0].decode(errors="replace")
                    item["name"]=name
                    item["guest_entry"]=label(struct.unpack("<Q",read(owner+24,8))[0])
                    argument=struct.unpack("<Q",read(owner+32,8))[0]
                    item["guest_argument"]=hex(argument)
                    if name=="brbDriver":
                        driver=read(argument,0x1d0)
                        if len(driver)==0x1d0:
                            item["audio_driver"]={
                                "flags":hex(struct.unpack_from("<I",driver,0)[0]),
                                "callback":label(struct.unpack_from("<Q",driver,8)[0]),
                                "callback_argument":hex(struct.unpack_from("<Q",driver,0x18)[0]),
                                "mix_position":struct.unpack_from("<d",driver,0x40)[0],
                                "ring_fields":list(struct.unpack_from("<4I",driver,0x48)),
                                "period_fields":list(struct.unpack_from("<3I",driver,0x1bc)),
                                "mix_step":struct.unpack_from("<d",driver,0x1c8)[0]}
            storage=c.create_string_buffer(1248); context=(c.addressof(storage)+15)&~15
            c.c_uint32.from_address(context+48).value=0x100003
            if not kernel.GetThreadContext(handle,context):
                item["error"]=c.get_last_error(); continue
            raw=c.string_at(context,256)
            regs={name:struct.unpack_from("<Q",raw,offset)[0] for name,offset in
                  (("rip",248),("rsp",152),("rbp",160),("rax",120),("rcx",128),("rdx",136))}
            item["registers"]={name:hex(value) for name,value in regs.items()}
            item["pc"]=label(regs["rip"])
            chain=[]; bp=regs["rbp"]
            for _ in range(24):
                frame=read(bp,16)
                if len(frame)!=16: break
                parent,return_address=struct.unpack("<QQ",frame)
                chain.append(label(return_address))
                if parent<=bp or parent-bp>0x1000000: break
                bp=parent
            item["frame_chain"]=chain
            stack=read(regs["rsp"],32768)
            candidates=[]
            for offset in range(0,len(stack)-7,8):
                value=struct.unpack_from("<Q",stack,offset)[0]
                if any(start<=value<end for start,end in executable):
                    candidates.append({"stack_offset":hex(offset),"address":label(value)})
            item["guest_stack_candidates"]=candidates[:50]
            item["native_stack_candidates"]=[{"stack_offset":hex(offset),"address":hex(value-native.base+0x140000000)}
                for offset in range(0,len(stack)-7,8)
                if native.base+0x1000<=(value:=struct.unpack_from("<Q",stack,offset)[0])<native.base+0x330000][:50]
        finally:
            if suspended: kernel.ResumeThread(handle)
            kernel.CloseHandle(handle)
            report["threads"].append(item)
finally: kernel.CloseHandle(process)
out=ROOT/"analysis/thread-sample.json"
out.write_text(json.dumps(report,indent=2),encoding="utf-8")
for item in report["threads"]:
    print(item["tid"],item["name"],item.get("pc",item.get("error")))
    print("  "+" -> ".join(item.get("frame_chain",[])[:6]))
print(f"Saved {out}")
