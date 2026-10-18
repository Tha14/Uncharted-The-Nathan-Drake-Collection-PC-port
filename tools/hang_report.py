"""Freeze reports for a running game (tools/probe_uncharted.py's hang watchdog), written from the
launching process: safe while the game's own threads are stuck.

thread_report(process): every thread's state and stack as text lines. Host frames come from
Windows' stack walker (StackWalk64 with the modules' own unwind tables; no symbol files needed) as
module+offset; guest code keeps frame pointers, so its frames follow the rbp chain as
"guest 0x...". The offsets are symbolized later against the same build
(llvm-addr2line -f -C -i -e Uncharted.exe 0x140000000+offset).

write_minidump(process, path): a Windows minidump (every thread's stack and registers; large,
since the guest fibers' stacks are big allocations)."""
import ctypes
import re
from ctypes import wintypes

TH32CS_SNAPTHREAD = 0x4
THREAD_ACCESS = 0x0002 | 0x0008 | 0x0040 | 0x0800  # suspend/resume, get context, query, query limited
CONTEXT_FULL = 0x0010000B
MAX_FRAMES = 28

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ThreadID", wintypes.DWORD),
                ("th32OwnerProcessID", wintypes.DWORD), ("tpBasePri", wintypes.LONG),
                ("tpDeltaPri", wintypes.LONG), ("dwFlags", wintypes.DWORD)]


class ADDRESS64(ctypes.Structure):
    _fields_ = [("Offset", ctypes.c_uint64), ("Segment", ctypes.c_uint16), ("Mode", ctypes.c_int)]


class KDHELP64(ctypes.Structure):
    _fields_ = [("Thread", ctypes.c_uint64), ("ThCallbackStack", wintypes.DWORD),
                ("ThCallbackBStore", wintypes.DWORD), ("NextCallback", wintypes.DWORD),
                ("FramePointer", wintypes.DWORD), ("KiCallUserMode", ctypes.c_uint64),
                ("KeUserCallbackDispatcher", ctypes.c_uint64), ("SystemRangeStart", ctypes.c_uint64),
                ("KiUserExceptionDispatcher", ctypes.c_uint64), ("StackBase", ctypes.c_uint64),
                ("StackLimit", ctypes.c_uint64), ("BuildVersion", wintypes.DWORD),
                ("RetpolineStubFunctionTableSize", wintypes.DWORD),
                ("RetpolineStubFunctionTable", ctypes.c_uint64), ("RetpolineStubOffset", wintypes.DWORD),
                ("RetpolineStubSize", wintypes.DWORD), ("Reserved0", ctypes.c_uint64 * 2)]


class STACKFRAME64(ctypes.Structure):
    _fields_ = [("AddrPC", ADDRESS64), ("AddrReturn", ADDRESS64), ("AddrFrame", ADDRESS64),
                ("AddrStack", ADDRESS64), ("AddrBStore", ADDRESS64), ("FuncTableEntry", ctypes.c_void_p),
                ("Params", ctypes.c_uint64 * 4), ("Far", wintypes.BOOL), ("Virtual", wintypes.BOOL),
                ("Reserved", ctypes.c_uint64 * 3), ("KdHelp", KDHELP64)]


class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", wintypes.DWORD), ("EntryPoint", ctypes.c_void_p)]


def _setup():
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.OpenThread.restype = wintypes.HANDLE
    kernel32.SuspendThread.argtypes = [wintypes.HANDLE]
    kernel32.ResumeThread.argtypes = [wintypes.HANDLE]
    kernel32.GetThreadContext.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                           ctypes.POINTER(ctypes.c_size_t)]
    psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p), wintypes.DWORD,
                                           ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
    psapi.GetModuleInformation.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MODULEINFO), wintypes.DWORD]
    psapi.GetModuleBaseNameW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.LPWSTR, wintypes.DWORD]


def _modules(process_handle):
    needed = wintypes.DWORD()
    array = (ctypes.c_void_p * 1024)()
    if not psapi.EnumProcessModulesEx(process_handle, array, ctypes.sizeof(array), ctypes.byref(needed), 3):
        return []
    result = []
    for base in array[:needed.value // ctypes.sizeof(ctypes.c_void_p)]:
        info, name = MODULEINFO(), ctypes.create_unicode_buffer(260)
        if psapi.GetModuleInformation(process_handle, base, ctypes.byref(info), ctypes.sizeof(info)):
            psapi.GetModuleBaseNameW(process_handle, base, name, 260)
            result.append((info.lpBaseOfDll or 0, info.SizeOfImage, name.value))
    return result


def _describe(address, modules, guest):
    for base, size, name in modules:
        if base <= address < base + size:
            return f"{name}+{address - base:#x}"
    if guest and guest[0] <= address < guest[0] + guest[1]:
        return f"guest {address - guest[0]:#x}"
    return f"{address:#x}"


def _read_u64_pair(process_handle, address):
    buffer, got = (ctypes.c_uint64 * 2)(), ctypes.c_size_t()
    if not kernel32.ReadProcessMemory(process_handle, ctypes.c_void_p(address), buffer, 16, ctypes.byref(got)) \
            or got.value != 16:
        return None
    return buffer[0], buffer[1]


def thread_report(process, guest_base=None, guest_size=0x20000000):
    """Text lines: one per group of threads with the same stack (most threads wait in the same
    places), the thread ids and names, host frames, then guest frames."""
    _setup()
    dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
    process_handle = wintypes.HANDLE(int(process._handle))
    dbghelp.SymSetOptions(0x4 | 0x2)  # SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME
    dbghelp.SymInitializeW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, wintypes.BOOL]
    dbghelp.SymInitializeW(process_handle, None, True)
    dbghelp.StackWalk64.argtypes = [wintypes.DWORD, wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(STACKFRAME64),
                                    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    table_access = ctypes.cast(dbghelp.SymFunctionTableAccess64, ctypes.c_void_p)
    module_base = ctypes.cast(dbghelp.SymGetModuleBase64, ctypes.c_void_p)
    modules = _modules(process_handle)
    guest = (guest_base, guest_size) if guest_base else None
    get_description = getattr(kernel32, "GetThreadDescription", None)
    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    entry = THREADENTRY32(dwSize=ctypes.sizeof(THREADENTRY32))
    groups = {}
    ok = kernel32.Thread32First(snapshot, ctypes.byref(entry))
    context_buffer = ctypes.create_string_buffer(1232 + 16)
    while ok:
        if entry.th32OwnerProcessID == process.pid:
            tid = entry.th32ThreadID
            thread = kernel32.OpenThread(THREAD_ACCESS, False, tid)
            if thread:
                name = ""
                if get_description:
                    text = ctypes.c_wchar_p()
                    if get_description(wintypes.HANDLE(thread), ctypes.byref(text)) >= 0 and text.value:
                        name = text.value
                        kernel32.LocalFree(text)
                frames, guest_frames = [], []
                if kernel32.SuspendThread(thread) != 0xFFFFFFFF:
                    try:
                        address = ctypes.addressof(context_buffer)
                        aligned = (address + 15) & ~15
                        ctypes.memset(aligned, 0, 1232)
                        ctypes.c_uint32.from_address(aligned + 0x30).value = CONTEXT_FULL
                        if kernel32.GetThreadContext(thread, ctypes.c_void_p(aligned)):
                            rip = ctypes.c_uint64.from_address(aligned + 0xF8).value
                            rsp = ctypes.c_uint64.from_address(aligned + 0x98).value
                            rbp = ctypes.c_uint64.from_address(aligned + 0xA0).value
                            frame = STACKFRAME64()
                            frame.AddrPC.Offset, frame.AddrPC.Mode = rip, 3
                            frame.AddrFrame.Offset, frame.AddrFrame.Mode = rbp, 3
                            frame.AddrStack.Offset, frame.AddrStack.Mode = rsp, 3
                            for _ in range(MAX_FRAMES):
                                if not dbghelp.StackWalk64(0x8664, process_handle, wintypes.HANDLE(thread),
                                                           ctypes.byref(frame), ctypes.c_void_p(aligned), None,
                                                           table_access, module_base, None):
                                    break
                                if not frame.AddrPC.Offset:
                                    break
                                frames.append(_describe(frame.AddrPC.Offset, modules, guest))
                            # Guest code keeps frame pointers: the rbp chain from the thread's rbp.
                            chain = rbp
                            for _ in range(16):
                                pair = _read_u64_pair(process_handle, chain) if chain else None
                                if not pair or pair[0] <= chain:
                                    break
                                if guest and guest[0] <= pair[1] < guest[0] + guest[1]:
                                    guest_frames.append(f"{pair[1] - guest[0]:#x}")
                                chain = pair[0]
                    finally:
                        kernel32.ResumeThread(thread)
                kernel32.CloseHandle(thread)
                key = (tuple(frames), tuple(guest_frames))
                groups.setdefault(key, []).append(f"{tid}{' ' + name if name else ''}")
        ok = kernel32.Thread32Next(snapshot, ctypes.byref(entry))
    kernel32.CloseHandle(snapshot)
    dbghelp.SymCleanup(process_handle)
    lines = []
    # Threads in the runtime's or the game's code first; the many idle waiters last.
    def idle(key):
        return bool(re.search(r"ntdll\.dll\+|win32u\.dll\+", key[0][0] if key[0] else "")) and len(key[1]) == 0
    for key, threads in sorted(groups.items(), key=lambda item: (idle(item[0]), -len(item[1]))):
        frames, guest_frames = key
        lines.append(f"  threads {', '.join(threads[:6])}{f' (+{len(threads) - 6} more)' if len(threads) > 6 else ''}:")
        lines.append("    host: " + (" <- ".join(frames) if frames else "(no stack)"))
        if guest_frames:
            lines.append("    guest: " + " <- ".join(guest_frames))
    return lines


def write_minidump(process, path):
    """Every thread's stack and registers (MiniDumpNormal + thread info, TEBs, handles)."""
    _setup()
    dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                     wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    dbghelp.MiniDumpWriteDump.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.HANDLE, wintypes.DWORD,
                                          ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    handle = kernel32.CreateFileW(str(path), 0x40000000, 0, None, 2, 0x80, None)  # GENERIC_WRITE, CREATE_ALWAYS
    if not handle or handle == wintypes.HANDLE(-1).value:
        return False
    try:
        return bool(dbghelp.MiniDumpWriteDump(wintypes.HANDLE(int(process._handle)), process.pid, handle,
                                              0x1000 | 0x100 | 0x20 | 0x4, None, None, None))
    finally:
        kernel32.CloseHandle(handle)
