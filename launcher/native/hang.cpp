// hang.cpp - freeze reports for a running game, part of uncharted_native.dll (used by the game starter's hang
// watchdog, tools/probe_uncharted.py, through tools/hang_report.py). Everything is read from outside the game process,
// so it works while the game's own threads are stuck.
//
// un_thread_report: every thread of the game, grouped by identical stacks: thread ids and names, then the native
//   frames (module+offset, walked with Windows' StackWalk64 and the modules' own unwind tables, no symbol files), then
//   the game-code frames (game code keeps frame pointers: the rbp chain, offsets from the guest image base).
// un_minidump: a Windows minidump of every thread's stack and registers.

#include "native.h"

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

const int MAX_FRAMES = 28;

struct Module {
    uint64_t base, size;
    std::string name;
};

std::string utf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

std::vector<Module> modules_of(HANDLE process) {
    std::vector<Module> out;
    std::vector<HMODULE> list(1024);
    DWORD needed = 0;
    if (!K32EnumProcessModulesEx(process, list.data(), (DWORD)(list.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL))
        return out;
    list.resize(std::min<size_t>(list.size(), needed / sizeof(HMODULE)));
    for (HMODULE m : list) {
        MODULEINFO info{};
        wchar_t name[MAX_PATH];
        if (!K32GetModuleInformation(process, m, &info, sizeof info)) continue;
        DWORD n = K32GetModuleBaseNameW(process, m, name, MAX_PATH);
        out.push_back({(uint64_t)info.lpBaseOfDll, info.SizeOfImage, n ? utf8(std::wstring(name, n)) : "?"});
    }
    return out;
}

std::string place(uint64_t address, const std::vector<Module> &modules, uint64_t guest, uint64_t guest_size) {
    if (guest && address >= guest && address < guest + guest_size) return "guest " + hex(address - guest);
    for (const Module &m : modules)
        if (address >= m.base && address < m.base + m.size) return m.name + "+" + hex(address - m.base);
    return hex(address);
}

std::string thread_name(DWORD id) {
    typedef HRESULT(WINAPI * Describe)(HANDLE, PWSTR *);
    static Describe describe = (Describe)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    if (!describe) return std::string();
    HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!t) return std::string();
    PWSTR text = nullptr;
    std::string out;
    if (SUCCEEDED(describe(t, &text)) && text) {
        out = utf8(text);
        LocalFree(text);
    }
    CloseHandle(t);
    return out;
}

struct Stack {
    std::vector<std::string> native, game;
    bool operator<(const Stack &o) const { return native != o.native ? native < o.native : game < o.game; }
};

Stack walk(HANDLE process, HANDLE thread, const std::vector<Module> &modules, uint64_t guest, uint64_t guest_size) {
    Stack s;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &ctx)) return s;
    // the game-code frames: follow the frame pointers (rbp -> {previous rbp, return address})
    uint64_t rbp = ctx.Rbp;
    if (guest && ctx.Rip >= guest && ctx.Rip < guest + guest_size) s.game.push_back(hex(ctx.Rip - guest));
    for (int i = 0; i < MAX_FRAMES && rbp; ++i) {
        uint64_t pair[2];
        SIZE_T got = 0;
        if (!ReadProcessMemory(process, (LPCVOID)rbp, pair, sizeof pair, &got) || got != sizeof pair) break;
        if (guest && pair[1] >= guest && pair[1] < guest + guest_size) s.game.push_back(hex(pair[1] - guest));
        if (pair[0] <= rbp) break;
        rbp = pair[0];
    }
    // the native frames
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    for (int i = 0; i < MAX_FRAMES; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
            break;
        s.native.push_back(place(frame.AddrPC.Offset, modules, guest, guest_size));
    }
    return s;
}

std::string joined(const std::vector<std::string> &parts, const char *sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? sep : "") + parts[i];
    return out;
}

int give(const std::string &text, char *out, int cap) {
    if ((int)text.size() + 1 > cap) return -(int)(text.size() + 1);
    std::memcpy(out, text.c_str(), text.size() + 1);
    return 0;
}

}  // namespace

extern "C" {

UN_API int un_thread_report(unsigned long pid, unsigned long long guest_base, unsigned long long guest_size, char *out,
                            int cap) {
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return give("  (the game process cannot be opened)", out, cap);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    bool symbols = SymInitializeW(process, nullptr, TRUE) != FALSE;
    std::vector<Module> modules = modules_of(process);
    std::map<Stack, std::vector<std::string>> groups;
    std::vector<Stack> order;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    for (BOOL more = snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        Stack s;
        if (SuspendThread(t) != (DWORD)-1) {
            s = walk(process, t, modules, guest_base, guest_size);
            ResumeThread(t);
        }
        CloseHandle(t);
        std::string name = thread_name(te.th32ThreadID);
        std::string who = std::to_string(te.th32ThreadID) + (name.empty() ? "" : " " + name);
        if (!groups.count(s)) order.push_back(s);
        groups[s].push_back(who);
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    if (symbols) SymCleanup(process);
    CloseHandle(process);
    std::string text;
    for (const Stack &s : order) {
        const std::vector<std::string> &who = groups[s];
        std::vector<std::string> shown(who.begin(), who.begin() + std::min<size_t>(who.size(), 6));
        text += "  threads " + joined(shown, ", ") +
                (who.size() > 6 ? " (+" + std::to_string(who.size() - 6) + " more)" : std::string()) + ":\n";
        text += "    host: " + (s.native.empty() ? std::string("(no stack)") : joined(s.native, " <- ")) + "\n";
        if (!s.game.empty()) text += "    guest: " + joined(s.game, " <- ") + "\n";
    }
    return give(text, out, cap);
}

UN_API int un_minidump(unsigned long pid, const wchar_t *path) {
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
    if (!process) return 0;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    BOOL ok = FALSE;
    if (file != INVALID_HANDLE_VALUE) {
        ok = MiniDumpWriteDump(process, pid, file,
                               (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithProcessThreadData |
                                               MiniDumpWithHandleData),
                               nullptr, nullptr, nullptr);
        CloseHandle(file);
        if (!ok) DeleteFileW(path);
    }
    CloseHandle(process);
    return ok ? 1 : 0;
}

}  // extern "C"
