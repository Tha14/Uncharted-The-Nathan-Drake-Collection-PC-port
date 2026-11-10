// hang.cpp - freeze reports for a running game, part of uncharted_native.dll (used by the game starter's hang
// watchdog, tools/probe_uncharted.py, through tools/hang_report.py). Everything is read from outside the game process,
// so it works while the game's own threads are stuck.
//
// un_thread_report: every thread of the game, grouped by identical stacks: thread ids and names, then the native
//   frames (module+offset, walked with Windows' StackWalk64 and the modules' own unwind tables, no symbol files), then
//   the game-code frames (game code keeps frame pointers: the rbp chain, offsets from the guest image base).
// un_minidump: a Windows minidump of every thread's stack and registers.

#include "native.h"

#include <cstdlib>
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <mutex>
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

}  // extern "C"

namespace {

// ------------------------------------------------------------------------------------------------ freeze watchdog

struct Watchdog {
    HANDLE process;
    std::wstring log, dumps, root;
    int seconds;
    std::mutex lock;
    std::vector<std::wstring> reports;
};
std::mutex watchdogs_lock;
std::vector<Watchdog *> watchdogs;   // kept for the process's life: the game starter asks for each one's reports

std::wstring parent_of(const std::wstring &p) {
    size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? L"." : p.substr(0, cut);
}

std::wstring env_text(const wchar_t *name) {
    wchar_t buf[256];
    DWORD n = GetEnvironmentVariableW(name, buf, 256);
    return (n && n < 256) ? std::wstring(buf, n) : std::wstring();
}

bool exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// the files matching folder\pattern, sorted by name, full paths
std::vector<std::wstring> sorted_files(const std::wstring &folder, const wchar_t *pattern) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(folder + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::wstring &a, const std::wstring &b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return out;
}

// a line on the game starter's own output (its log), unbuffered
void say(const std::string &line) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    std::string text = line + "\n";
    DWORD put = 0;
    WriteFile(out, text.data(), (DWORD)text.size(), &put, nullptr);
}

std::string report_text(DWORD pid, uint64_t guest_base) {
    std::vector<char> buf(1 << 18);
    for (;;) {
        int status = un_thread_report(pid, guest_base, guest_base ? 0x20000000ULL : 0, buf.data(), (int)buf.size());
        if (status >= 0) return buf.data();
        buf.resize((size_t)-status);
    }
}

void freeze_report(Watchdog *w, uint64_t guest_base) {
    char stamp[32];
    time_t now = time(nullptr);
    struct tm local;
    localtime_s(&local, &now);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    std::vector<std::string> lines = {"Hang watchdog: no new frame for " + std::to_string(w->seconds) +
                                      " s (game frozen) at " + stamp + "; stacks of every thread:"};
    ULONGLONG started = GetTickCount64();
    std::string text = report_text(GetProcessId(w->process), guest_base);
    for (size_t at = 0; at < text.size();) {   // its lines (Python's splitlines: no empty last line)
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        lines.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    char took[64];
    std::snprintf(took, sizeof took, "(report took %.1f s)", (GetTickCount64() - started) / 1000.0);
    lines.push_back(took);
    // a minidump too (hundreds of MB): in the source tree or with UNCHARTED_HANG_DUMP=1
    std::wstring dump_env = env_text(L"UNCHARTED_HANG_DUMP");
    SetLastError(0);
    if (!GetEnvironmentVariableW(L"UNCHARTED_HANG_DUMP", nullptr, 0) && GetLastError() == ERROR_ENVVAR_NOT_FOUND) dump_env = exists(w->root + L"\\VERSION") ? L"0" : L"1";
    if (dump_env == L"1") {
        CreateDirectoryW(parent_of(w->dumps).c_str(), nullptr);
        CreateDirectoryW(w->dumps.c_str(), nullptr);
        std::vector<std::wstring> old = sorted_files(w->dumps, L"hang-*.dmp");
        for (size_t i = 0; i + 1 < old.size(); ++i) DeleteFileW(old[i].c_str());   // with the new one, the newest 2 stay
        std::wstring dump = w->dumps + L"\\hang-" + std::wstring(stamp, stamp + std::strlen(stamp)) + L".dmp";
        lines.push_back(un_minidump(GetProcessId(w->process), dump.c_str()) ? "Hang watchdog: minidump " + utf8(dump)
                                                                            : "Hang watchdog: the minidump failed");
    }
    std::wstring folder = parent_of(w->log);
    std::vector<std::wstring> old = sorted_files(folder, L"hang-*.txt");
    for (size_t i = 0; i + 9 < old.size(); ++i) DeleteFileW(old[i].c_str());   // with the new one, the newest 10 stay
    std::wstring report = folder + L"\\hang-" + std::wstring(stamp, stamp + std::strlen(stamp)) + L".txt";
    std::string body;
    for (auto &l : lines) body += l + "\n";
    HANDLE f = CreateFileW(report.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        WriteFile(f, body.data(), (DWORD)body.size(), &put, nullptr);
        CloseHandle(f);
    }
    {
        std::lock_guard<std::mutex> hold(w->lock);
        w->reports.push_back(report);
    }
    say("Hang watchdog: the game froze; report " + utf8(report));
}

// The runtime logs a Frame stats line every 5 s while frames are shown: none for `seconds` while the game runs is a
// freeze. Once per freeze; frames coming back re-arm it.
DWORD WINAPI watch(void *arg) {
    Watchdog *w = (Watchdog *)arg;
    uint64_t position = 0, guest_base = 0;
    bool armed = true, seen = false, have_base = false;
    ULONGLONG last_frame = GetTickCount64();
    while (WaitForSingleObject(w->process, 2000) == WAIT_TIMEOUT) {
        HANDLE f = CreateFileW(w->log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        std::string chunk;
        LARGE_INTEGER size{}, to{};
        to.QuadPart = (LONGLONG)position;
        if (GetFileSizeEx(f, &size) && (uint64_t)size.QuadPart > position && SetFilePointerEx(f, to, nullptr, FILE_BEGIN)) {
            chunk.resize((size_t)((uint64_t)size.QuadPart - position));
            DWORD got = 0;
            size_t done = 0;
            while (done < chunk.size() &&
                   ReadFile(f, &chunk[done], (DWORD)std::min<size_t>(chunk.size() - done, 1 << 24), &got, nullptr) && got)
                done += got;
            chunk.resize(done);
        }
        CloseHandle(f);
        position += chunk.size();
        if (!have_base) {
            size_t at = chunk.find("Guest image base: ");
            if (at != std::string::npos) {
                size_t s = at + 18, e = s;
                while (e < chunk.size() && std::isxdigit((unsigned char)chunk[e])) ++e;
                if (e > s) { guest_base = std::strtoull(chunk.substr(s, e - s).c_str(), nullptr, 16); have_base = true; }
            }
        }
        if (chunk.find("Frame stats") != std::string::npos) {
            last_frame = GetTickCount64();
            armed = seen = true;
            continue;
        }
        if (!(seen && armed) || GetTickCount64() - last_frame < (ULONGLONG)w->seconds * 1000 ||
            WaitForSingleObject(w->process, 0) != WAIT_TIMEOUT)
            continue;
        armed = false;
        freeze_report(w, have_base ? guest_base : 0);
    }
    return 0;
}

}  // namespace

extern "C" {

// Starts the freeze watchdog on its own thread: game `pid`, its log, the minidump folder, the seconds without a new
// frame that count as a freeze, the launcher's folder (a package has VERSION there: no minidumps unless
// UNCHARTED_HANG_DUMP=1). Returns its id (> 0), 0 when it could not start.
UN_API int un_hang_watchdog(unsigned long pid, const wchar_t *log, const wchar_t *dumps, int seconds, const wchar_t *root) {
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return 0;
    Watchdog *w = new Watchdog{process, log ? log : L"", dumps ? dumps : L"", root ? root : L".", seconds, {}, {}};
    int id;
    {
        std::lock_guard<std::mutex> hold(watchdogs_lock);
        watchdogs.push_back(w);
        id = (int)watchdogs.size();
    }
    HANDLE thread = CreateThread(nullptr, 0, watch, w, 0, nullptr);
    if (!thread) return 0;
    CloseHandle(thread);
    return id;
}

// out: the reports a watchdog wrote so far, one path per line
UN_API int un_hang_reports(int id, char *out, int cap) {
    Watchdog *w = nullptr;
    {
        std::lock_guard<std::mutex> hold(watchdogs_lock);
        if (id >= 1 && id <= (int)watchdogs.size()) w = watchdogs[id - 1];
    }
    std::string text;
    if (w) {
        std::lock_guard<std::mutex> hold(w->lock);
        for (auto &r : w->reports) text += utf8(r) + "\n";
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
