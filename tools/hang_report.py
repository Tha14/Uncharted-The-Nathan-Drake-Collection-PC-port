"""Freeze reports and resource tracking for a running game, for the game starter (tools/probe_uncharted.py): the
Python side of uncharted_native.dll's functions (launcher/native/hang.cpp, memstat.cpp). Everything is read from outside
the game process, so it works while its threads are stuck.

thread_report(process, guest_base): text lines, threads grouped by identical stacks (native frames as module+offset,
game-code frames as offsets from the guest image base, to symbolize later against the same build).
write_minidump(process, path): a Windows minidump of every thread's stack and registers; True when written.
memory_monitor(process, folder, seconds): starts the native tracker, one CSV row of the game's memory, handles,
threads and GUI objects (and the PC's free memory) every `seconds` until the game ends."""
import ctypes
from pathlib import Path

NATIVE = Path(__file__).resolve().parent.parent / "launcher" / "native" / "uncharted_native.dll"
_dll = None


def _native():
    global _dll
    if _dll is None:
        _dll = ctypes.CDLL(str(NATIVE))
        _dll.un_thread_report.argtypes = [ctypes.c_ulong, ctypes.c_ulonglong, ctypes.c_ulonglong, ctypes.c_char_p,
                                          ctypes.c_int]
        _dll.un_thread_report.restype = ctypes.c_int
        _dll.un_minidump.argtypes = [ctypes.c_ulong, ctypes.c_wchar_p]
        _dll.un_minidump.restype = ctypes.c_int
        _dll.un_memory_monitor.argtypes = [ctypes.c_ulong, ctypes.c_wchar_p, ctypes.c_double, ctypes.c_char_p,
                                           ctypes.c_int]
        _dll.un_memory_monitor.restype = ctypes.c_int
    return _dll


def thread_report(process, guest_base=None, guest_size=0x20000000):
    size = 1 << 18
    while True:
        out = ctypes.create_string_buffer(size)
        status = _native().un_thread_report(process.pid, guest_base or 0, guest_size if guest_base else 0, out, size)
        if status >= 0:
            return out.value.decode("utf-8", "replace").splitlines()
        size = -status


def write_minidump(process, path):
    return bool(_native().un_minidump(process.pid, str(path)))


def memory_monitor(process, folder, seconds=10):
    """Starts the native tracker (its own thread): <folder>/memory-<time>.csv until the game ends. Returns the file's
    Path, or None when it could not start. Never raises: tracking must not disturb the game or the launcher."""
    try:
        out = ctypes.create_string_buffer(1024)
        if _native().un_memory_monitor(process.pid, str(folder), float(seconds), out, len(out)) == 0:
            return Path(out.value.decode("utf-8"))
    except Exception:  # noqa: BLE001 - see the docstring
        pass
    return None
