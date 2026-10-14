"""Read-only scan of the running gow3-probe.exe for the grey-ground data.

Searches every committed readable region for:
  colour  - the lighting block seen at constants [158..170] of ps 45d0299c in good frames
            (1,1,1, 107/255,88/255,73/255, 5,25,4,60)
  vertex  - the first ground vertex position (-8.41523,-0.356361,8.79578), 28-byte stride
Usage: python -I tools/scan_ground_state.py [label]   (appends to analysis/ground-scan.log)
No target memory is written.
"""
import ctypes as c
from ctypes import wintypes as w
import subprocess, sys, time
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
k = c.WinDLL("kernel32", use_last_error=True)
k.OpenProcess.restype = w.HANDLE
k.VirtualQueryEx.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t]
k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]

class MBI(c.Structure):
    _fields_ = [("base", c.c_void_p), ("alloc", c.c_void_p), ("aprot", w.DWORD), ("part", w.WORD),
                ("size", c.c_size_t), ("state", w.DWORD), ("protect", w.DWORD), ("type", w.DWORD)]

out = [l for l in subprocess.run(["tasklist", "/FI", "IMAGENAME eq gow3-probe.exe", "/FO", "CSV", "/NH"],
                                 capture_output=True, text=True).stdout.splitlines() if "gow3" in l]
if not out:
    sys.exit("gow3-probe.exe is not running")
pid = int(out[0].split('","')[1])
h = k.OpenProcess(0x400 | 0x10, False, pid)

colour = np.array([1, 1, 1, 107 / 255, 88 / 255, 73 / 255, 5, 25, 4, 60], np.float32)
vertex = np.array([-8.41523, -0.356361, 8.79578], np.float32)
hits = []
addr, mbi, chunk = 0, MBI(), 1 << 24
readable = {0x02, 0x04, 0x08, 0x20, 0x40, 0x80}
while addr < 0x7FFFFFFF0000 and k.VirtualQueryEx(h, c.c_void_p(addr), c.byref(mbi), c.sizeof(mbi)):
    base, size = mbi.base or 0, mbi.size
    if mbi.state == 0x1000 and (mbi.protect & 0xFF) in readable and not mbi.protect & 0x100:
        for off in range(0, size, chunk):
            n = min(chunk, size - off)
            buf = c.create_string_buffer(n); got = c.c_size_t()
            if not k.ReadProcessMemory(h, c.c_void_p(base + off), buf, n, c.byref(got)) or got.value < 64:
                continue
            a = np.frombuffer(buf.raw[:got.value // 4 * 4], np.float32)
            with np.errstate(invalid="ignore"):
                for i in np.where(np.abs(a[:-10] - 1) < 1e-6)[0]:
                    if np.all(np.abs(a[i:i + 10] - colour) < 1e-4):
                        hits.append(("colour", base + off + i * 4, a[i - 3:i + 13]))
                for i in np.where(np.abs(a[:-3] - vertex[0]) < 1e-4)[0]:
                    if abs(a[i + 1] - vertex[1]) < 1e-4 and abs(a[i + 2] - vertex[2]) < 1e-4:
                        hits.append(("vertex", base + off + i * 4, a[i - 4:i + 10]))
    addr = base + size

label = sys.argv[1] if len(sys.argv) > 1 else ""
with open(ROOT / "analysis/ground-scan.log", "a") as log:
    log.write(f"== {time.strftime('%H:%M:%S')} pid {pid} {label}: {len(hits)} hits\n")
    for kind, at, v in hits[:200]:
        line = f"{kind} {at:#x} " + " ".join(f"{x:.6g}" for x in v)
        log.write(line + "\n"); print(line)
print(f"{len(hits)} hits")
