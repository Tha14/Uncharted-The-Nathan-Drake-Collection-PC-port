"""Arm (or clear) hardware write watchpoints on every thread of gow3-probe.exe.

  python -I tools/watch_write.py <addr> [addr ...]   up to 4 guest addresses, 4-byte write watch
  python -I tools/watch_write.py clear

Hits are logged by the runtime ("Watch:" lines in analysis/uncharted/boot.log, at most 400).
Each thread is suspended only while its debug registers are set, and always resumed.
"""
import ctypes as c
from ctypes import wintypes as w
import subprocess, sys

k = c.WinDLL("kernel32", use_last_error=True)
k.OpenThread.restype = w.HANDLE
k.CreateToolhelp32Snapshot.restype = w.HANDLE

class TE(c.Structure):
    _fields_ = [("sz", w.DWORD), ("u", w.DWORD), ("tid", w.DWORD), ("pid", w.DWORD),
                ("bp", c.c_long), ("dp", c.c_long), ("fl", w.DWORD)]

class CTX(c.Structure):  # x64 CONTEXT up to the debug registers, padded to full size
    _fields_ = [("home", c.c_ulonglong * 6), ("flags", w.DWORD), ("mxcsr", w.DWORD),
                ("seg", c.c_ushort * 6), ("eflags", w.DWORD), ("dr", c.c_ulonglong * 6),
                ("rest", c.c_ubyte * 0x500)]

rows = [l for l in subprocess.run(["tasklist", "/FI", "IMAGENAME eq gow3-probe.exe", "/FO", "CSV", "/NH"],
                                  capture_output=True, text=True).stdout.splitlines() if "gow3" in l]
if not rows:
    sys.exit("gow3-probe.exe is not running")
pid = int(rows[0].split('","')[1])
addrs = [] if sys.argv[1] == "clear" else [int(a, 16) for a in sys.argv[1:5]]
dr7 = 0
for i in range(len(addrs)):
    dr7 |= (1 << (2 * i)) | (0b01 << (16 + 4 * i)) | (0b11 << (18 + 4 * i))  # local enable, write, 4 bytes

snap = k.CreateToolhelp32Snapshot(4, 0)
te = TE(); te.sz = c.sizeof(TE)
tids = []
ok = k.Thread32First(snap, c.byref(te))
while ok:
    if te.pid == pid:
        tids.append(te.tid)
    ok = k.Thread32Next(snap, c.byref(te))
k.CloseHandle(snap)

buf = (c.c_ubyte * (c.sizeof(CTX) + 16))()
ctx = CTX.from_address((c.addressof(buf) + 15) & ~15)
done = 0
for tid in tids:
    th = k.OpenThread(0x0002 | 0x0008 | 0x0010 | 0x0040, False, tid)
    if not th:
        continue
    try:
        if k.SuspendThread(th) == 0xFFFFFFFF:
            continue
        try:
            ctx.flags = 0x100010  # CONTEXT_DEBUG_REGISTERS
            if not k.GetThreadContext(th, c.byref(ctx)):
                continue
            for i in range(4):
                ctx.dr[i] = addrs[i] if i < len(addrs) else 0
            ctx.dr[4] = 0
            ctx.dr[5] = dr7
            if k.SetThreadContext(th, c.byref(ctx)):
                done += 1
        finally:
            k.ResumeThread(th)
    finally:
        k.CloseHandle(th)
print(f"{'cleared' if not addrs else 'armed ' + ' '.join(hex(a) for a in addrs)} on {done}/{len(tids)} threads")
