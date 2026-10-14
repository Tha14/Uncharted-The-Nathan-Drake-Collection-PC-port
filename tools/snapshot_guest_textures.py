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

import hashlib
import argparse
parser=argparse.ArgumentParser()
parser.add_argument('directory',type=Path)
args=parser.parse_args()
try:
    for metadata in args.directory.glob('guest_*.txt'):
        fields=dict(re.findall(r'(\w+)=(\w+)',metadata.read_text().splitlines()[0]))
        address=int(fields['address'],16); size=int(fields['guest_size'],16)
        if not 0<size<=64*1024*1024: continue
        data=read(address,size)
        if len(data)!=size:
            print(metadata.name,'partial read',len(data),size);continue
        target=metadata.with_suffix('.cpu-tiled.bin')
        target.write_bytes(data)
        print(target.name,len(data),hashlib.sha256(data).hexdigest())
finally:
    kernel.CloseHandle(process)
