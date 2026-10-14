"""Read-only snapshot/diff of Uncharted's global data (image 0x986000..0x1300000).

  python -I tools/globals_diff.py save <name>     reads the globals 3 times, 0.7 s apart
  python -I tools/globals_diff.py diff <a> <b>    words stable inside each state but different
                                                  between them (analysis/globals/<a>-vs-<b>.txt)
Snapshots go to analysis/globals/. No target memory is written.
"""
import ctypes as c
from ctypes import wintypes as w
import subprocess, sys, time
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "analysis/globals"
BASE, LO, HI = 0xB400000, 0x986000, 0x1300000

def attach():
    k = c.WinDLL("kernel32", use_last_error=True)
    k.OpenProcess.restype = w.HANDLE
    k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
    rows = [l for l in subprocess.run(["tasklist", "/FI", "IMAGENAME eq gow3-probe.exe", "/FO", "CSV", "/NH"],
                                      capture_output=True, text=True).stdout.splitlines() if "gow3" in l]
    if not rows:
        sys.exit("gow3-probe.exe is not running")
    h = k.OpenProcess(0x410, False, int(rows[0].split('","')[1]))
    def read():
        out = bytearray(HI - LO)
        for at in range(LO, HI, 0x10000):
            n = min(0x10000, HI - at)
            buf = c.create_string_buffer(n); got = c.c_size_t()
            if k.ReadProcessMemory(h, c.c_void_p(BASE + at), buf, n, c.byref(got)):
                out[at - LO:at - LO + got.value] = buf.raw[:got.value]
        return np.frombuffer(bytes(out), np.uint32)
    return read

if sys.argv[1] == "save":
    read = attach()
    reads = []
    for i in range(3):
        reads.append(read()); time.sleep(0.7)
    OUT.mkdir(parents=True, exist_ok=True)
    np.save(OUT / f"{sys.argv[2]}.npy", np.stack(reads))
    print(f"saved {sys.argv[2]}: {len(reads[0]) * 4 >> 20} MB x3")
else:
    a, b = (np.load(OUT / f"{n}.npy") for n in sys.argv[2:4])
    stable = np.all(a == a[0], axis=0) & np.all(b == b[0], axis=0)
    diff = np.where(stable & (a[0] != b[0]))[0]
    lines = []
    for i in diff:
        off = LO + i * 4
        fa, fb = a[0, i:i + 1].view(np.float32)[0], b[0, i:i + 1].view(np.float32)[0]
        lines.append(f"{off:#09x} {a[0, i]:08x} -> {b[0, i]:08x}   ({fa:.6g} -> {fb:.6g})")
    path = OUT / f"{sys.argv[2]}-vs-{sys.argv[3]}.txt"
    path.write_text("\n".join(lines) + "\n")
    print(f"{len(diff)} stable words differ -> {path}")
