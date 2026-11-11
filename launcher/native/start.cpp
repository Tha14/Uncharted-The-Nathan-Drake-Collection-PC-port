// start.cpp - game starter inside uncharted_native.dll. When the player presses PLAY, the launcher calls this to get a
// Collection game ready and run it on the local runtime. It accepts the same command line as the old
// tools/probe_uncharted.py (--boot --seconds 0 --executable <exe> --game <folder>).
//
// Order of work, all in native code:
//   1. Look for an update beside the game and merge it in, as a folder of links or, failing that, as copies of just
//      the files the preparation needs.
//   2. Prepare the image (prepare.cpp writes boot.bin, link.cpp writes boot-linked.bin, plus the content profile).
//   3. Swap in the Uncharted 3 multiplayer image if one is set up, otherwise apply the code patches.
//   4. Patch the render size, compute the runtime environment, pick the window icon.
//   5. Launch Uncharted.exe with output going to <out>\boot.log, along with the freeze watchdog and the memory
//      tracker; afterwards write boot.json and keep copies of the log and JSON in <out>\attempts.
// Exit code 30 together with loadexec.txt means the game wants a different Collection executable (the menu opening
// Uncharted 2 or 3, or returning to the menu); the starter then loops and runs that one.
//
// Progress lines go to stdout, laid out as the Python starter's were; the reason for an abort goes to stderr.

#include "native.h"
#include "json.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <windows.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

extern "C" {
UN_API int un_link_modules(const wchar_t *game, const wchar_t *out_dir, const char *executable, const char *modules,
                           char *out, int cap);
UN_API int un_content_profile(const wchar_t *game, const wchar_t *out_dir, const char *sku, char *out, int cap);
}

namespace {

using json::Value;

const char *const EXECUTABLES[] = {"eboot.bin", "big2-ps4_Shipping.elf", "big3-ps4_Shipping.elf"};
// runtime_services.c exits with this code from sceSystemServiceLoadExec, meaning the game wants to switch to another
// Collection executable. The /app0 path and arguments of that executable are in the file named by GOW3_LOADEXEC_FILE.
const unsigned long LOADEXEC_EXIT = 30;

// Thrown to abort the start (where the Python version raised SystemExit or let an exception escape). The message is
// written to stderr and the code is returned to the caller.
struct Stop {
    std::string message;
    int code = 1;
};

std::string utf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring widen(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// Writes text to stdout or stderr with \n expanded to \r\n, which is what a text-mode Python stream produced.
void write_std(DWORD which, const std::string &text) {
    std::string o;
    for (char c : text) { if (c == '\n') o += "\r\n"; else o += c; }
    HANDLE h = GetStdHandle(which);
    DWORD put = 0;
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, o.data(), (DWORD)o.size(), &put, nullptr);
}
void print(const std::string &line) { write_std(STD_OUTPUT_HANDLE, line + "\n"); }
// Prints the multi-line text a native step handed back, adding a final newline if it lacks one.
void print_text(const std::string &text) {
    if (!text.empty()) write_std(STD_OUTPUT_HANDLE, text.back() == '\n' ? text : text + "\n");
}

std::wstring env(const wchar_t *name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return L"";
    std::wstring v(n, L'\0');
    n = GetEnvironmentVariableW(name, &v[0], n);
    v.resize(n);
    return v;
}
bool has_env(const wchar_t *name) {   // true for a defined variable, including one with an empty value
    SetLastError(0);
    return GetEnvironmentVariableW(name, nullptr, 0) > 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

std::wstring upper(const std::wstring &s) {
    std::wstring o(s);
    if (!o.empty()) LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, s.c_str(), (int)s.size(), &o[0], (int)o.size(),
                                  nullptr, nullptr, 0);
    return o;
}
std::wstring lower(const std::wstring &s) {
    std::wstring o(s);
    if (!o.empty()) LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), (int)s.size(), &o[0], (int)o.size(),
                                  nullptr, nullptr, 0);
    return o;
}

// ------------------------------------------------------------------------------------------------ paths (pathlib)

// Canonical spelling of a Windows path, equal to Python's str(Path(p)): backslashes only, single separators, no
// trailing separator and no "." components.
std::wstring path_str(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    std::wstring prefix;
    size_t at = 0;
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') {   // \\server\share
        size_t a = p.find(L'\\', 2), b = a == std::wstring::npos ? a : p.find(L'\\', a + 1);
        prefix = p.substr(0, b == std::wstring::npos ? p.size() : b);
        at = prefix.size();
    } else if (p.size() >= 2 && p[1] == L':') {
        prefix = p.substr(0, 2);
        at = 2;
    }
    const bool rooted = at < p.size() && p[at] == L'\\';
    std::vector<std::wstring> parts;
    size_t i = at;
    while (i <= p.size()) {
        size_t j = p.find(L'\\', i);
        if (j == std::wstring::npos) j = p.size();
        std::wstring part = p.substr(i, j - i);
        if (!part.empty() && part != L".") parts.push_back(part);
        i = j + 1;
    }
    std::wstring o = prefix + (rooted ? L"\\" : L"");
    for (size_t k = 0; k < parts.size(); ++k) o += (k ? L"\\" : L"") + parts[k];
    if (o.empty()) o = L".";
    return o;
}
std::wstring join(const std::wstring &a, const std::wstring &b) {
    if (a.empty()) return path_str(b);
    return path_str(a + L"\\" + b);
}
std::wstring parent(const std::wstring &p) {
    const std::wstring s = path_str(p);
    size_t cut = s.find_last_of(L'\\');
    if (cut == std::wstring::npos) return L".";
    if (cut == 2 && s[1] == L':') return s.substr(0, 3);
    return s.substr(0, cut);
}
std::wstring name_of(const std::wstring &p) {
    const std::wstring s = path_str(p);
    size_t cut = s.find_last_of(L"\\:");
    return cut == std::wstring::npos ? s : s.substr(cut + 1);
}
std::wstring anchor(const std::wstring &p) {   // drive part plus root, e.g. "D:\" (Python's Path.anchor)
    const std::wstring s = path_str(p);
    if (s.size() >= 2 && s[1] == L':') return s.substr(0, 2) + (s.size() > 2 && s[2] == L'\\' ? L"\\" : L"");
    return L"";
}
// Like Python's Path.resolve(): absolute path with links followed; if the target can't be opened, just the
// absolute path.
std::wstring resolved(const std::wstring &p) {
    std::vector<wchar_t> full(32768);
    DWORD n = GetFullPathNameW(p.c_str(), 32768, full.data(), nullptr);
    std::wstring f = n && n < 32768 ? std::wstring(full.data(), n) : p;
    HANDLE h = CreateFileW(f.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return path_str(f);
    n = GetFinalPathNameByHandleW(h, full.data(), 32768, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (!n || n >= 32768) return path_str(f);
    std::wstring r(full.data(), n);
    if (r.compare(0, 8, L"\\\\?\\UNC\\") == 0) return path_str(L"\\" + r.substr(7));
    if (r.compare(0, 4, L"\\\\?\\") == 0) return path_str(r.substr(4));
    return path_str(r);
}
bool is_file(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool is_dir(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
void make_dirs(const std::wstring &p) {
    if (is_dir(p)) return;
    const std::wstring up = parent(p);
    if (up != p && !up.empty()) make_dirs(up);
    CreateDirectoryW(p.c_str(), nullptr);
}

// ------------------------------------------------------------------------------------------------ the native steps

// Calls a native text function, retrying with the size it asks for when the buffer was too small. Only safe for
// functions that can be called twice without side effects.
template <typename F> int call_grow(F f, std::string &text, int size = 1 << 16) {
    while (true) {
        std::vector<char> buf((size_t)size);
        int status = f(buf.data(), size);
        if (status >= 0) {
            text = buf.data();
            return status;
        }
        size = -status;
    }
}

std::wstring g_root;

std::string starter(const char *what, const std::string &arg = "") {
    std::string text;
    int status = call_grow([&](char *o, int c) { return un_starter(what, arg.c_str(), o, c); }, text, 1 << 12);
    if (status > 0) throw Stop{std::string("ValueError: unknown starter setting ") + what};
    return text;
}

// Asks system.cpp (game detection) a question; `found` is false when it has nothing to report.
std::string system_text(const char *what, const std::wstring &arg, bool &found) {
    std::string text;
    int status = call_grow([&](char *o, int c) { return un_system(what, arg.c_str(), o, c); }, text, 1 << 12);
    found = status == 0;
    return found ? text : "";
}

// Runs one of starter_files.cpp's file operations. They copy or link files, so each is called exactly once with a
// generous buffer instead of call_grow. Status 0 = success, 1 = file system error, 3 = bad input (game_view only).
int starter_files(const char *what, const std::vector<std::wstring> &args, std::string &text) {
    Value list;
    list.kind = Value::Array;
    for (auto &a : args) list.items.push_back(Value::text(utf8(a)));
    std::vector<char> buf(1 << 16);
    int status = un_starter_files(what, json::dump(list).c_str(), buf.data(), (int)buf.size());
    text = buf.data();
    if (status == 2) throw Stop{std::string("ValueError: starter_files ") + what + ": status 2"};
    return status;
}

std::string instance_suffix() { return starter("instance_suffix"); }

// Folder that holds this instance's saves, settings and caches, as starter_files.cpp chooses it.
std::wstring user_dir() {
    std::string text;
    if (starter_files("user_dir", {g_root, widen(instance_suffix())}, text) == 1) throw Stop{"OSError: " + text};
    size_t nl = text.find('\n');
    std::string path = text.substr(0, nl), message = nl == std::string::npos ? "" : text.substr(nl + 1);
    if (!message.empty()) print(message);
    return path_str(widen(path));
}

// Path of Uncharted.exe; starter_files.cpp refreshes it from the build output if the two copies differ.
std::wstring game_executable() {
    std::string text;
    if (starter_files("game_executable", {g_root}, text) == 1) throw Stop{"OSError: " + text};
    size_t nl = text.find('\n');
    std::string message = nl == std::string::npos ? "" : text.substr(nl + 1);
    if (!message.empty()) print(message);
    return path_str(widen(text.substr(0, nl)));
}

// Icon file for the game window, taken from the game itself by starter_files.cpp; empty if the game has none.
std::wstring game_window_icon(const std::wstring &game) {
    std::string text;
    if (starter_files("window_icon", {g_root, game}, text) == 1) throw Stop{"OSError: " + text};
    size_t nl = text.find('\n');
    std::string path = text.substr(0, nl), message = nl == std::string::npos ? "" : text.substr(nl + 1);
    if (!message.empty()) print(message);
    return path.empty() ? L"" : path_str(widen(path));
}

// Update folder located beside the game by system.cpp's find_update; empty if there isn't one.
std::wstring find_update(const std::wstring &game) {
    bool found = false;
    std::string text = system_text("find_update", game, found) + "\n";
    size_t nl = text.find('\n');
    std::string path = text.substr(0, nl), rest = text.substr(nl + 1);
    std::string message = rest.substr(0, rest.find('\n'));
    if (!message.empty()) print(message);
    return path.empty() ? L"" : path_str(widen(path));
}

std::string sfo_value(const std::wstring &game, const char *key) {
    bool found = false;
    std::string text = system_text("sfo", game, found);
    Value v;
    if (!found || !json::parse(text, v)) return "";
    const Value *f = v.get(key);
    if (!f) return "";
    return f->kind == Value::String ? f->s : f->kind == Value::Int ? std::to_string(f->i) : "";
}

std::string game_version(const std::wstring &game) {
    bool found = false;
    std::string text = system_text("starter_version", game, found);
    return found ? text : "";
}

// ------------------------------------------------------------------------------------------------ the runtime process

// Quotes arguments into one command line using the MSVC rules (same result as Python's subprocess.list2cmdline).
std::wstring command_line(const std::vector<std::wstring> &args) {
    std::wstring result;
    for (const std::wstring &arg : args) {
        std::wstring bs;
        if (!result.empty()) result += L' ';
        const bool quote = arg.find(L' ') != std::wstring::npos || arg.find(L'\t') != std::wstring::npos || arg.empty();
        if (quote) result += L'"';
        for (wchar_t c : arg) {
            if (c == L'\\') {
                bs += c;
            } else if (c == L'"') {
                result += std::wstring(bs.size() * 2, L'\\');
                bs.clear();
                result += L"\\\"";
            } else {
                result += bs;
                bs.clear();
                result += c;
            }
        }
        result += bs;
        if (quote) {
            result += bs;
            result += L'"';
        }
    }
    return result;
}

// Builds a CreateProcess environment block; Windows expects the names in case-insensitive sorted order.
std::vector<wchar_t> environment_block(const std::vector<std::pair<std::wstring, std::wstring>> &vars) {
    std::vector<std::pair<std::wstring, std::wstring>> sorted = vars;
    std::stable_sort(sorted.begin(), sorted.end(), [](auto &a, auto &b) { return upper(a.first) < upper(b.first); });
    std::vector<wchar_t> block;
    for (auto &kv : sorted) {
        if (kv.first.empty() || kv.first.find(L'=', 1) != std::wstring::npos) throw Stop{"ValueError: illegal environment variable name"};
        std::wstring line = kv.first + L"=" + kv.second;
        block.insert(block.end(), line.begin(), line.end());
        block.push_back(0);
    }
    block.push_back(0);
    if (block.size() == 1) block.push_back(0);
    return block;
}

std::string win_error_text(DWORD code) {
    wchar_t *text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                   0, (LPWSTR)&text, 0, nullptr);
    std::wstring t = text ? text : L"error";
    if (text) LocalFree(text);
    while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r' || t.back() == L' ')) t.pop_back();
    return "[WinError " + std::to_string(code) + "] " + utf8(t);
}

struct Process {
    HANDLE handle = nullptr;
    DWORD pid = 0;
};

// Launches the runtime with stdout and stderr both going to the log handle. With u3mp set it follows the
// multiplayer test runner's recipe: mitigation policy 0x220000 turns off bottom-up and high-entropy ASLR for this
// child only, the process is created suspended, and it is resumed only after confirming that the PS4 reserve range
// 0x1000000000..0xfc00000000 is still empty. Nothing in the image, the registry or system settings is modified.
Process start_process(const std::vector<std::wstring> &command, const std::wstring &cwd,
                      const std::vector<std::pair<std::wstring, std::wstring>> &vars, HANDLE log, bool u3mp) {
    if (u3mp && !is_file(command[0])) throw std::string("runtime executable missing");
    HANDLE self = GetCurrentProcess(), out = nullptr, in = nullptr;
    if (!DuplicateHandle(self, log, self, &out, 0, TRUE, DUPLICATE_SAME_ACCESS)) throw win_error_text(GetLastError());
    HANDLE source_in = u3mp ? nullptr : GetStdHandle(STD_INPUT_HANDLE);
    bool own_in = false;
    if (source_in && source_in != INVALID_HANDLE_VALUE &&
        DuplicateHandle(self, source_in, self, &in, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
    } else {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        own_in = true;
    }
    (void)own_in;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
    std::vector<char> attr_buf(size);
    auto attr = (LPPROC_THREAD_ATTRIBUTE_LIST)attr_buf.data();
    PROCESS_INFORMATION pi{};
    std::string failure;
    bool initialized = false;
    DWORD64 policy = 0x220000;
    HANDLE handles[2] = {in, out};
    std::vector<wchar_t> block = environment_block(vars);
    std::wstring cmd = command_line(command);
    do {
        if (!InitializeProcThreadAttributeList(attr, 2, 0, &size)) { failure = win_error_text(GetLastError()); break; }
        initialized = true;
        if (u3mp && !UpdateProcThreadAttribute(attr, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &policy, sizeof policy,
                                               nullptr, nullptr)) {
            failure = win_error_text(GetLastError());
            break;
        }
        if (!UpdateProcThreadAttribute(attr, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof handles, nullptr, nullptr)) {
            failure = win_error_text(GetLastError());
            break;
        }
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = in;
        si.StartupInfo.hStdOutput = out;
        si.StartupInfo.hStdError = out;
        si.lpAttributeList = attr;
        DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
        if (u3mp) flags |= CREATE_SUSPENDED | CREATE_NO_WINDOW;
        std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
        cmd_buf.push_back(0);
        if (!CreateProcessW(u3mp ? command[0].c_str() : nullptr, cmd_buf.data(), nullptr, nullptr, TRUE, flags, block.data(),
                            cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi)) {
            failure = win_error_text(GetLastError());
            if (!u3mp) failure += ": '" + utf8(command[0]) + "'";
            break;
        }
        if (u3mp) {
            std::string conflicts;
            for (uint64_t at = 0x1000000000ull; at < 0xfc00000000ull;) {
                MEMORY_BASIC_INFORMATION m{};
                if (!VirtualQueryEx(pi.hProcess, (void *)at, &m, sizeof m)) { failure = win_error_text(GetLastError()); break; }
                if (!m.RegionSize) { failure = "VirtualQueryEx returned an empty region"; break; }
                if (m.State != MEM_FREE) {
                    char row[160];
                    std::snprintf(row, sizeof row, "%s{'base': '0x%llx', 'size': '0x%llx', 'state': '0x%lx'}",
                                  conflicts.empty() ? "" : ", ", (unsigned long long)(uintptr_t)m.BaseAddress,
                                  (unsigned long long)m.RegionSize, (unsigned long)m.State);
                    conflicts += row;
                }
                at = (uint64_t)(uintptr_t)m.BaseAddress + m.RegionSize;
            }
            if (failure.empty() && !conflicts.empty())
                failure = "Child address layout conflicts with PS4 reserve: [" + conflicts + "]";
            if (failure.empty() && ResumeThread(pi.hThread) == (DWORD)-1) failure = win_error_text(GetLastError());
        }
    } while (false);
    if (pi.hThread) CloseHandle(pi.hThread);
    if (!failure.empty() && pi.hProcess) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        pi.hProcess = nullptr;
    }
    if (initialized) DeleteProcThreadAttributeList(attr);
    CloseHandle(out);
    if (in && in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if (!failure.empty()) throw failure;
    return {pi.hProcess, pi.dwProcessId};
}

double monotonic() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

unsigned long long time_ns() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) * 100;
}

void write_crlf(const std::wstring &path, const std::string &text, bool append = false) {
    std::string o;
    for (char c : text) { if (c == '\n') o += "\r\n"; else o += c; }
    HANDLE h = CreateFileW(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw Stop{"OSError: " + win_error_text(GetLastError()) + ": '" + utf8(path) + "'"};
    DWORD put = 0;
    WriteFile(h, o.data(), (DWORD)o.size(), &put, nullptr);
    CloseHandle(h);
}

// ------------------------------------------------------------------------------------------------ the start sequence

struct Args {
    std::wstring game;
    bool boot = false, collection_menu = false, cpu_only = false, capture = false;
    std::string executable = "eboot.bin";
    std::vector<std::string> guest_args;
    bool guest_given = false;
    long long seconds = 30;
};

struct Result {
    Value json;   // contents written to boot.json
    unsigned long long exit_code = 0;
    bool timed_out = false;
};

std::string game_key_of(const std::string &executable) {
    return executable == "eboot.bin" ? "u1" : executable == "big2-ps4_Shipping.elf" ? "u2" : "u3";
}

// Snapshot of this process's environment, names upper-cased (as Python's os.environ shows them on Windows), in the
// order Windows lists them.
std::vector<std::pair<std::string, std::string>> process_environment() {
    std::vector<std::pair<std::string, std::string>> vars;
    wchar_t *block = GetEnvironmentStringsW();
    for (wchar_t *p = block; p && *p; p += wcslen(p) + 1) {
        std::wstring entry = p;
        size_t eq = entry.find(L'=', 1);
        if (eq == std::wstring::npos) continue;
        const std::string key = utf8(upper(entry.substr(0, eq))), value = utf8(entry.substr(eq + 1));
        bool replaced = false;
        for (auto &kv : vars) if (kv.first == key) { kv.second = value; replaced = true; }
        if (!replaced) vars.push_back({key, value});
    }
    if (block) FreeEnvironmentStringsW(block);
    return vars;
}

int parse_int(const std::wstring &s, bool &ok) {   // decimal parse accepting what Python's int() accepts
    std::wstring t = s;
    while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    ok = !t.empty();
    size_t i = (t.size() && (t[0] == L'+' || t[0] == L'-')) ? 1 : 0;
    if (i >= t.size()) ok = false;
    for (size_t k = i; k < t.size(); ++k) if (!iswdigit(t[k]) && t[k] != L'_') ok = false;
    std::wstring digits;
    for (wchar_t c : t) if (c != L'_') digits += c;
    return ok ? _wtoi(digits.c_str()) : 0;
}

Result run_game(const Args &args, const std::wstring &game, const std::string &executable,
                const std::vector<std::string> &guest_args, const std::wstring &loadexec, const std::wstring &app0 = L"",
                const std::vector<std::wstring> &extra_overlays = {}) {
    const std::wstring stem = executable == "eboot.bin" ? L"uncharted" : widen(executable.substr(0, executable.rfind('.')));
    const std::wstring out = join(join(g_root, L"analysis"), stem + widen(instance_suffix()));
    const std::wstring user = user_dir();
    make_dirs(user);
    // Preparation step: prepare.cpp and link.cpp write boot.bin, boot-linked.bin and the content profile into `out`.
    {
        std::vector<char> buf(1 << 16);
        int status = un_prepare(game.c_str(), out.c_str(), executable.c_str(), buf.data(), (int)buf.size());
        if (status != 0) throw Stop{"ValueError: " + std::string(buf.data())};
        print_text(buf.data());
        status = un_link_modules(game.c_str(), out.c_str(), executable.c_str(), "", buf.data(), (int)buf.size());
        if (status != 0) throw Stop{"ValueError: " + std::string(buf.data())};
        print_text(buf.data());
        status = un_content_profile(game.c_str(), out.c_str(), "full", buf.data(), (int)buf.size());
        if (status != 0) throw Stop{"ValueError: " + std::string(buf.data())};
        print_text(buf.data());
    }
    const std::wstring image = join(out, L"boot-linked.bin");
    // If Uncharted 3 multiplayer is set up (Multiplayer page), the image its import linked on this machine is copied
    // over the freshly prepared one. That image already contains the code patches it was tested with, so the patch
    // step is skipped for it.
    std::string mp_text = starter("u3mp_image", executable);
    size_t nl = mp_text.find('\n');
    const std::string mp_message = mp_text.substr(0, nl), mp_found = nl == std::string::npos ? "" : mp_text.substr(nl + 1);
    if (!mp_message.empty()) print(mp_message);
    bool mp = false;
    std::vector<std::wstring> mp_overlays;
    if (!mp_found.empty()) {
        Value info;
        json::parse(mp_found, info);
        mp = true;
        const std::wstring mp_image = path_str(widen(info.get("image") ? info.get("image")->s : ""));
        if (const Value *o = info.get("overlays")) for (auto &p : o->items) mp_overlays.push_back(path_str(widen(p.s)));
        if (!CopyFileW(mp_image.c_str(), image.c_str(), FALSE))
            throw Stop{"OSError: " + win_error_text(GetLastError()) + ": '" + utf8(mp_image) + "'"};
        const Value *version = info.get("version");
        print("Using Uncharted 3 multiplayer v" + (version ? (version->kind == Value::String ? version->s : std::to_string(version->i)) : "") +
              ": " + utf8(mp_image) + " with " + std::to_string(mp_overlays.size()) + " overlays");
    } else {
        // Apply the code patches that match the current settings (tables in patch_tables.cpp, applied by patcher.cpp).
        std::string text;
        int status = call_grow([&](char *o, int c) { return un_apply_image_patches(image.c_str(), executable.c_str(), o, c); },
                               text);
        std::vector<std::string> lines;
        for (size_t a = 0; a < text.size();) {
            size_t b = text.find('\n', a);
            if (b == std::string::npos) b = text.size();
            lines.push_back(text.substr(a, b - a));
            a = b + 1;
        }
        if (status == 1) {
            for (size_t k = 0; k + 1 < lines.size(); ++k) print(lines[k]);
            throw Stop{lines.empty() ? "Patches not applied" : lines.back()};
        }
        for (auto &l : lines) print(l);
    }
    // The launcher stores settings separately for each game. Read the ones for the executable being started, which
    // matters when the Collection menu hands over to another game.
    const std::string game_key = game_key_of(executable);
    std::wstring resolution = env(widen("UNCHARTED_RESOLUTION_" + std::string(game_key == "u1" ? "U1" : game_key == "u2" ? "U2" : "U3")).c_str());
    if (resolution.empty()) resolution = env(L"UNCHARTED_RESOLUTION");
    resolution = lower(resolution);
    int width = 1920, height = 1080;
    if (!resolution.empty()) {
        // Split on "x" into exactly two integers; errors match the Python unpacking's, checked in the same order
        std::vector<std::wstring> parts;
        for (size_t a = 0;;) {
            size_t b = resolution.find(L'x', a);
            parts.push_back(resolution.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a));
            if (b == std::wstring::npos) break;
            a = b + 1;
        }
        int values[2] = {0, 0};
        for (size_t k = 0; k < parts.size() && k < 3; ++k) {
            bool ok = false;
            int v = parse_int(parts[k], ok);
            if (!ok) throw Stop{"ValueError: invalid literal for int() with base 10: '" + utf8(parts[k]) + "'"};
            if (k < 2) values[k] = v;
        }
        if (parts.size() < 2)
            throw Stop{"ValueError: not enough values to unpack (expected 2, got " + std::to_string(parts.size()) + ")"};
        if (parts.size() > 2) throw Stop{"ValueError: too many values to unpack (expected 2)"};
        width = values[0];
        height = values[1];
    }
    // patcher.cpp writes the render size into the linked image: the requested size if the game's memory can hold it,
    // otherwise the biggest smaller size that fits, together with how much the memory areas have to grow.
    int render_w = width, render_h = height;
    long long extra_mib = 0;
    {
        std::string text;
        int status = call_grow([&](char *o, int c) {
            return un_render_resolution(image.c_str(), executable.c_str(), width, height, mp ? 1 : 0, o, c);
        }, text);
        std::vector<std::string> lines;
        for (size_t a = 0; a < text.size();) {
            size_t b = text.find('\n', a);
            if (b == std::string::npos) b = text.size();
            lines.push_back(text.substr(a, b - a));
            a = b + 1;
        }
        if (status == 1) {
            for (size_t k = 0; k + 1 < lines.size(); ++k) print(lines[k]);
            throw Stop{lines.empty() ? "Render resolution failed" : lines.back()};
        }
        if (!lines.empty()) std::sscanf(lines[0].c_str(), "%d %d %lld", &render_w, &render_h, &extra_mib);
        for (size_t k = 1; k < lines.size(); ++k) print(lines[k]);
    }
    // runtime_env.cpp turns the launcher's UNCHARTED_* settings, the game and its render size into the runtime's GOW3_*
    // variables; the reasoning behind each one is documented there.
    std::vector<std::pair<std::wstring, std::wstring>> vars;
    std::wstring capture_dir, trigger;
    {
        Value inputs = Value::object(), env_list;
        env_list.kind = Value::Array;
        for (auto &kv : process_environment()) {
            Value pair;
            pair.kind = Value::Array;
            pair.items = {Value::text(kv.first), Value::text(kv.second)};
            env_list.items.push_back(pair);
        }
        auto int_pair = [](long long a, long long b) {
            Value v;
            v.kind = Value::Array;
            v.items = {Value::integer(a), Value::integer(b)};
            return v;
        };
        inputs.set("env", env_list);
        inputs.set("executable", Value::text(executable));
        inputs.set("game", Value::text(utf8(game)));
        inputs.set("game_key", Value::text(game_key));
        inputs.set("user", Value::text(utf8(user)));
        inputs.set("out", Value::text(utf8(out)));
        inputs.set("root", Value::text(utf8(g_root)));
        inputs.set("loadexec", Value::text(utf8(loadexec)));
        inputs.set("render", int_pair(render_w, render_h));
        inputs.set("size", int_pair(width, height));
        inputs.set("extra_mib", Value::integer(extra_mib));
        inputs.set("capture", Value::boolean(args.capture));
        std::vector<char> buf(1 << 23);
        int status = un_runtime_env(json::dump(inputs).c_str(), buf.data(), (int)buf.size());
        if (status != 0) throw Stop{"Runtime settings: " + (status > 0 ? std::string(buf.data()) : std::string("too large"))};
        Value result;
        json::parse(buf.data(), result);
        if (const Value *p = result.get("print")) for (auto &line : p->items) print(line.s);
        if (const Value *e = result.get("env"))
            for (auto &pair : e->items) {
                if (pair.items.size() != 2) continue;
                const std::wstring k = widen(pair.items[0].s), v = widen(pair.items[1].s);
                bool replaced = false;
                for (auto &kv : vars) if (kv.first == k) { kv.second = v; replaced = true; }
                if (!replaced) vars.push_back({k, v});
            }
        if (const Value *c = result.get("capture")) if (c->kind == Value::String && !c->s.empty()) capture_dir = path_str(widen(c->s));
        if (const Value *t = result.get("trigger")) if (t->kind == Value::String && !t->s.empty()) trigger = path_str(widen(t->s));
    }
    auto var = [&](const std::wstring &k) -> std::wstring * {
        for (auto &kv : vars) if (kv.first == k) return &kv.second;
        return nullptr;
    };
    const std::wstring window_icon = game_window_icon(game);
    if (!window_icon.empty() && !var(L"GOW3_WINDOW_ICON")) vars.push_back({L"GOW3_WINDOW_ICON", window_icon});
    std::vector<std::wstring> command = {game_executable(), image, L"--no-game-hooks", L"--low-image", L"--content-profile",
                                         join(out, L"content.bin"), L"--app0", app0.empty() ? game : app0, L"--user", user};
    // Uncharted 2 multiplayer: the launcher puts a ';'-separated folder list in UNCHARTED_U2MP_OVERLAY. Each folder is
    // overlaid on /app0 with priority over the update; where folders overlap, the earlier one's file is used.
    if (executable == "big2-ps4_Shipping.elf") {
        const std::wstring list = env(L"UNCHARTED_U2MP_OVERLAY");
        for (size_t a = 0; a <= list.size();) {
            size_t b = list.find(L';', a);
            if (b == std::wstring::npos) b = list.size();
            const std::wstring folder = list.substr(a, b - a);
            if (!folder.empty() && is_dir(folder)) command.insert(command.end(), {L"--app0-overlay", folder});
            a = b + 1;
        }
    }
    // Uncharted 3 multiplayer adds four overlay folders, also ranked above the update, earliest first.
    for (auto &o : mp_overlays) command.insert(command.end(), {L"--app0-overlay", o});
    for (auto &o : extra_overlays) command.insert(command.end(), {L"--app0-overlay", o});
    // guest_args holds the full argv for the game, with the program name in slot 0.
    command.push_back(L"--guest-path");
    command.push_back(guest_args.empty() ? widen("/app0/" + executable) : widen(guest_args[0]));
    for (size_t k = 1; k < guest_args.size(); ++k) command.insert(command.end(), {L"--guest-arg", widen(guest_args[k])});
    // Debugging aid: UNCHARTED_RENDERDOC may name a RenderDoc install folder or its renderdoccmd.exe. The game then
    // loads RenderDoc's Vulkan layer and F12 grabs a frame. The layer is announced only through the child's
    // environment variables; nothing gets registered on the system.
    const std::wstring renderdoc = env(L"UNCHARTED_RENDERDOC");
    if (!renderdoc.empty()) {
        std::wstring layer_dir = path_str(renderdoc);
        if (!is_dir(layer_dir)) layer_dir = parent(layer_dir);
        if (is_file(join(layer_dir, L"renderdoc.json"))) {
            std::wstring *existing = var(L"VK_ADD_LAYER_PATH");
            const std::wstring value = layer_dir + (existing && !existing->empty() ? L";" + *existing : L"");
            if (existing) *existing = value; else vars.push_back({L"VK_ADD_LAYER_PATH", value});
            for (auto kv : std::vector<std::pair<std::wstring, std::wstring>>{{L"VK_INSTANCE_LAYERS", L"VK_LAYER_RENDERDOC_Capture"},
                                                                             {L"ENABLE_VULKAN_RENDERDOC_CAPTURE", L"1"},
                                                                             {L"GOW3_PIPELINE_BINARIES", L"0"}}) {
                if (std::wstring *v = var(kv.first)) *v = kv.second; else vars.push_back(kv);
            }
            print("RenderDoc: loading its Vulkan layer from " + utf8(layer_dir));
        }
    }
    if (!args.boot) command.push_back(L"--check-imports");
    if (args.cpu_only) command.push_back(L"--cpu-only");
    std::wstring name = args.boot ? L"boot" : L"check-imports";
    if (args.cpu_only) name += L"-cpu";
    Value result = Value::object(), command_v;
    command_v.kind = Value::Array;
    for (auto &c : command) command_v.items.push_back(Value::text(utf8(c)));
    result.set("executable", Value::text(executable));
    result.set("command", command_v);
    result.set("guest_execution_requested", Value::boolean(args.boot));
    result.set("timeout_seconds", Value::integer(args.seconds));
    if (args.capture) result.set("capture_directory", Value::text(capture_dir.empty() ? "None" : utf8(capture_dir)));
    const double start = monotonic();
    const std::wstring log_path = join(out, name + L".log");
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) throw Stop{"OSError: " + win_error_text(GetLastError()) + ": '" + utf8(log_path) + "'"};
    Process process;
    bool started = false;
    if (mp) {
        try {
            process = start_process(command, g_root, vars, log, true);
            started = true;
        } catch (const std::string &error) {
            print("Uncharted 3 multiplayer: could not reserve its address layout, starting it the normal way (" + error + ")");
        } catch (const Stop &stop) {
            print("Uncharted 3 multiplayer: could not reserve its address layout, starting it the normal way (" +
                  stop.message + ")");
        }
    }
    if (!started) {
        try {
            process = start_process(command, g_root, vars, log, false);
        } catch (const std::string &error) {
            CloseHandle(log);
            throw Stop{"FileNotFoundError: " + error};
        }
    }
    // Freeze watchdog from hang.cpp. It watches for the runtime's Frame stats lines, so it only works with diagnostics
    // enabled. UNCHARTED_HANG_SECONDS changes how long it waits (default 30 s); 0 disables it.
    long long hang_seconds = 30;
    if (has_env(L"UNCHARTED_HANG_SECONDS")) {
        bool ok = false;
        const std::wstring v = env(L"UNCHARTED_HANG_SECONDS");
        hang_seconds = v.empty() ? 0 : parse_int(v, ok);
    }
    int watchdog = 0;
    const std::wstring *frame_stats = var(L"GOW3_FRAME_STATS");
    if (args.boot && hang_seconds > 0 && frame_stats && *frame_stats == L"1")
        watchdog = un_hang_watchdog(process.pid, log_path.c_str(), join(out, L"dumps").c_str(), (int)hang_seconds,
                                    g_root.c_str());
    // Memory use over long sessions (memstat.cpp): a memory-<time>.csv next to the log gets one row per
    // UNCHARTED_MEMORY_SECONDS seconds (default 10; 0 disables it).
    double memory_seconds = 10;
    if (has_env(L"UNCHARTED_MEMORY_SECONDS")) {
        const std::wstring v = env(L"UNCHARTED_MEMORY_SECONDS");
        memory_seconds = v.empty() ? 0 : _wtof(v.c_str());
    }
    std::wstring memory_csv;
    if (args.boot && memory_seconds > 0) {
        char buf[1024];
        if (un_memory_monitor(process.pid, out.c_str(), memory_seconds, buf, sizeof buf) == 0) memory_csv = widen(buf);
    }
    DWORD code = 0;
    bool timed_out = false;
    auto wait_for = [&](double seconds) -> bool {   // returns true once the process has exited
        DWORD millis = seconds < 0 ? INFINITE : (DWORD)std::min<double>(0xfffffffe, std::max<double>(0, seconds * 1000));
        return WaitForSingleObject(process.handle, millis) == WAIT_OBJECT_0;
    };
    if (args.capture) {
        while (!wait_for(0)) {
            const double remaining = args.seconds ? (double)args.seconds - (monotonic() - start) : 3;
            if (remaining <= 0) { timed_out = true; break; }
            if (!wait_for(std::min(3.0, remaining)) && !trigger.empty()) {
                HANDLE t = CreateFileW(trigger.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (t != INVALID_HANDLE_VALUE) {
                    FILETIME now;
                    GetSystemTimeAsFileTime(&now);
                    SetFileTime(t, nullptr, &now, &now);
                    CloseHandle(t);
                }
            }
        }
    } else if (!wait_for(args.seconds ? (double)args.seconds : -1)) {
        timed_out = true;
    }
    if (timed_out) {
        TerminateProcess(process.handle, 1);
        WaitForSingleObject(process.handle, INFINITE);
    }
    GetExitCodeProcess(process.handle, &code);
    CloseHandle(process.handle);
    CloseHandle(log);
    result.set("exit_code", Value::integer((long long)code));
    result.set("timed_out", Value::boolean(timed_out));
    Value elapsed;
    elapsed.kind = Value::Float;
    char rounded[64];
    std::snprintf(rounded, sizeof rounded, "%.3f", monotonic() - start);
    elapsed.f = std::strtod(rounded, nullptr);
    result.set("elapsed_seconds", elapsed);
    const std::wstring json_path = join(out, name + L".json");
    write_crlf(json_path, json::dump(result));
    // Only now that the runtime has stopped writing the log is it safe to append the freeze report paths to it; the
    // launcher reads them from there.
    std::vector<std::wstring> hang_reports;
    if (args.boot && watchdog) {
        std::string text;
        call_grow([&](char *o, int c) { return un_hang_reports(watchdog, o, c); }, text, 1 << 12);
        for (size_t a = 0; a < text.size();) {
            size_t b = text.find('\n', a);
            if (b == std::string::npos) b = text.size();
            if (b > a) hang_reports.push_back(path_str(widen(text.substr(a, b - a))));
            a = b + 1;
        }
    }
    if (args.boot && !hang_reports.empty()) {
        std::string lines;
        for (auto &r : hang_reports) lines += "\nHang watchdog: the game froze; report " + utf8(r);
        lines += "\n";
        HANDLE h = CreateFileW(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD put = 0;
            WriteFile(h, lines.data(), (DWORD)lines.size(), &put, nullptr);
            CloseHandle(h);
        }
    }
    const std::wstring attempt = join(join(out, L"attempts"), name + L"-" + std::to_wstring(time_ns()));
    make_dirs(attempt);
    CopyFileW(log_path.c_str(), join(attempt, name + L".log").c_str(), FALSE);
    CopyFileW(json_path.c_str(), join(attempt, name + L".json").c_str(), FALSE);
    if (args.boot)
        for (auto &r : hang_reports) CopyFileW(r.c_str(), join(attempt, name_of(r)).c_str(), FALSE);
    if (args.boot && !memory_csv.empty() && is_file(memory_csv))
        CopyFileW(memory_csv.c_str(), join(attempt, name_of(memory_csv)).c_str(), FALSE);
    print(json::dump(result));
    print("Log written to: " + utf8(log_path));
    return {result, code, timed_out};
}

Result run_once(const Args &args, const std::string &executable, const std::vector<std::string> &guest_args,
                const std::wstring &loadexec) {
    const std::wstring base = resolved(args.game);
    const std::wstring update = find_update(base);
    if (update.empty()) {
        const std::string version = game_version(base);
        if (!version.empty() && version < "01.02") {
            const std::wstring title = lower(name_of(base)) == L"uroot" ? parent(base) : base;
            print("Game version " + version + " has no update 1.02, so the fixes made for 1.02 are left out (they show "
                  "as NOT applied). Unpack update 1.02 beside the game folder as " + utf8(name_of(title)) + "-patch.");
        }
        return run_game(args, base, executable, guest_args, loadexec);
    }
    const std::string app_ver = sfo_value(update, "APP_VER");
    print("Update " + (app_ver.empty() ? std::string("?") : app_ver) + ": " + utf8(update));
    const std::string update_version = game_version(update);
    if (!update_version.empty() && update_version < "01.02")
        print("The update folder's executables are version " + update_version + " even though its param.sfo says "
              "otherwise, so the fixes made for 1.02 are left out. Unpack update 1.02 once more.");
    // Build a merged view of game plus update out of file links (starter_files.cpp game_view). It is placed on the
    // same drive as the game if possible, because links cannot point across drives (elsewhere it falls back to
    // copies).
    std::wstring view;
    std::string not_combined;
    if (env(L"UNCHARTED_TEST_OVERLAY") == L"1") {   // testing aid: force the path used on drives without links
        not_combined = "UNCHARTED_TEST_OVERLAY=1";
    } else {
        std::vector<std::wstring> places = {join(g_root, L"analysis\\views")};
        if (lower(resolved(anchor(base))) != lower(resolved(anchor(g_root))))
            places.insert(places.begin(), join(parent(parent(base)), L".uncharted-views"));
        for (auto &place : places) {
            std::string text;
            const int status = starter_files("game_view", {base, update, place}, text);
            if (status == 0) { view = path_str(widen(text)); break; }
            if (status == 3) throw Stop{"ValueError: " + text};
            print("Making the game view in " + utf8(place) + " failed: " + text);
        }
        if (view.empty()) not_combined = "The game and its update could not be joined";
    }
    if (view.empty()) {
        // Drives formatted exFAT or FAT32 (typically USB sticks) cannot store links. In that case the runtime itself
        // overlays the update on the game, and preparation works from copies of the handful of files it reads.
        print(not_combined + ": the drive holds no links, so the runtime overlays the update on the game itself");
        std::string text;
        if (starter_files("boot_view", {g_root, base, update}, text) == 1) {
            print("Copying the update files failed (" + text + "): the game starts without its update");
            return run_game(args, base, executable, guest_args, loadexec);
        }
        return run_game(args, path_str(widen(text)), executable, guest_args, loadexec, base, {update});
    }
    try {
        Result r = run_game(args, view, executable, guest_args, loadexec);
        std::string text;
        if (starter_files("remove_view", {view}, text) == 1)
            print("Game view " + utf8(view) + " could not be deleted: " + text);
        return r;
    } catch (...) {
        std::string text;
        try {
            if (starter_files("remove_view", {view}, text) == 1)
                print("Game view " + utf8(view) + " could not be deleted: " + text);
        } catch (...) {}
        throw;
    }
}

const char *USAGE = "usage: probe_uncharted.py [-h] [--game GAME] [--boot]\n"
                    "                          [--executable {eboot.bin,big2-ps4_Shipping.elf,big3-ps4_Shipping.elf}]\n"
                    "                          [--guest-arg GUEST_ARG] [--collection-menu]\n"
                    "                          [--cpu-only] [--capture] [--seconds SECONDS]\n";

// Command-line parsing compatible with the old argparse setup: an option may be abbreviated to any unambiguous
// prefix, and --name=value is accepted as well as --name value.
Args parse_args(const std::vector<std::string> &argv) {
    static const char *const options[] = {"--game", "--boot", "--executable", "--guest-arg", "--collection-menu",
                                          "--cpu-only", "--capture", "--seconds", "--help"};
    auto fail = [](const std::string &why) { return Stop{std::string(USAGE) + "probe_uncharted.py: error: " + why, 2}; };
    Args a;
    a.game = join(g_root, L"game\\CUSA02344\\uroot");
    for (size_t i = 0; i < argv.size(); ++i) {
        std::string arg = argv[i], value;
        bool has_value = false;
        size_t eq = arg.find('=');
        if (arg.rfind("--", 0) == 0 && eq != std::string::npos) {
            value = arg.substr(eq + 1);
            arg = arg.substr(0, eq);
            has_value = true;
        }
        std::string option;
        for (const char *o : options) {
            if (arg == o) { option = o; break; }
            if (arg.size() > 2 && std::string(o).rfind(arg, 0) == 0) {
                if (!option.empty()) throw fail("ambiguous option: " + arg);
                option = o;
            }
        }
        if (option.empty()) throw fail("unrecognized arguments: " + argv[i]);
        auto take = [&]() -> std::string {
            if (has_value) return value;
            // a following word starting with '-' is treated as the next option unless it parses as a negative number
            // (argparse behaves this way when none of the defined options look numeric)
            auto negative_number = [](const std::string &s) {
                if (s.size() < 2 || s[0] != '-') return false;
                char *end = nullptr;
                std::strtod(s.c_str(), &end);
                return end && !*end;
            };
            if (i + 1 >= argv.size() ||
                (argv[i + 1].rfind("-", 0) == 0 && argv[i + 1].size() > 1 && !negative_number(argv[i + 1])))
                throw fail("argument " + option + ": expected one argument");
            return argv[++i];
        };
        if (option == "--help") throw Stop{std::string(USAGE), 0};
        if (option == "--game") a.game = path_str(widen(take()));
        else if (option == "--boot") a.boot = true;
        else if (option == "--collection-menu") a.collection_menu = true;
        else if (option == "--cpu-only") a.cpu_only = true;
        else if (option == "--capture") a.capture = true;
        else if (option == "--guest-arg") { a.guest_args.push_back(take()); a.guest_given = true; }
        else if (option == "--executable") {
            a.executable = take();
            bool known = false;
            for (const char *e : EXECUTABLES) known = known || a.executable == e;
            if (!known)
                throw fail("argument --executable: invalid choice: '" + a.executable +
                           "' (choose from 'eboot.bin', 'big2-ps4_Shipping.elf', 'big3-ps4_Shipping.elf')");
        } else if (option == "--seconds") {
            const std::string v = take();
            bool ok = false;
            a.seconds = parse_int(widen(v), ok);
            if (!ok) throw fail("argument --seconds: invalid int value: '" + v + "'");
        }
    }
    if (a.seconds < 0) throw fail("--seconds cannot be negative");
    return a;
}

long long start_game(const std::vector<std::string> &argv) {
    Args args = parse_args(argv);
    const std::wstring user = user_dir();
    make_dirs(user);
    const std::wstring loadexec = join(user, L"loadexec.txt");
    std::string executable = args.executable;
    std::vector<std::string> guest_args = args.guest_args;
    if (!args.guest_given) {
        // Mirror the arguments the Collection menu passes. With "ispartofuccollection", eboot.bin goes straight into
        // Drake's Fortune; without it, eboot.bin opens the Collection's game picker.
        if (executable == "eboot.bin" && args.collection_menu)
            guest_args = {"/app0/eboot.bin"};
        else
            guest_args = {"eboot.bin", "ispartofuccollection"};
    }
    Result result;
    while (true) {
        DeleteFileW(loadexec.c_str());
        result = run_once(args, executable, guest_args, loadexec);
        if (!args.boot || result.timed_out || result.exit_code != LOADEXEC_EXIT || !exists(loadexec)) break;
        // A game switch inside the Collection: the menu opened Uncharted 2 or 3, or a game returned to the menu.
        std::string text;
        {
            HANDLE h = CreateFileW(loadexec.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                char buf[4096];
                DWORD got = 0;
                while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) text.append(buf, got);
                CloseHandle(h);
            }
        }
        std::vector<std::string> lines;   // split on \r\n, \r or \n, like Python's str.splitlines()
        for (size_t a = 0; a < text.size();) {
            size_t b = text.find_first_of("\r\n", a);
            if (b == std::string::npos) { lines.push_back(text.substr(a)); break; }
            lines.push_back(text.substr(a, b - a));
            a = b + (text[b] == '\r' && b + 1 < text.size() && text[b + 1] == '\n' ? 2 : 1);
        }
        std::string first = lines.empty() ? "" : lines[0];
        std::string trimmed = first;
        while (!trimmed.empty() && isspace((unsigned char)trimmed.back())) trimmed.pop_back();
        while (!trimmed.empty() && isspace((unsigned char)trimmed.front())) trimmed.erase(trimmed.begin());
        const std::string target = trimmed.empty() ? "" : utf8(name_of(widen(trimmed)));
        bool known = false;
        for (const char *e : EXECUTABLES) known = known || target == e;
        if (!known) {
            print("The game requested " + (lines.empty() ? std::string("(nothing)") : first) +
                  ", which is not one of the Collection's games");
            break;
        }
        // The argv passed to LoadExec is used unchanged as the next program's whole argv.
        executable = target;
        guest_args.assign(lines.begin() + 1, lines.end());
        if (guest_args.empty()) guest_args = {"/app0/" + target};
        std::string joined;
        for (size_t k = 0; k < guest_args.size(); ++k) joined += (k ? " " : "") + guest_args[k];
        print("Collection: switching to " + executable + " " + joined);
    }
    return result.timed_out ? 124 : (long long)result.exit_code;
}

}  // namespace

extern "C" {

// Entry point of the game starter.
//   root:      the launcher's install folder
//   args_json: JSON array of strings, the same arguments probe_uncharted.py used to take
// Output goes to stdout/stderr in the Python starter's format. Return value: the game's exit code, or 124 if it was
// stopped on timeout, 2 for an invalid command line, 1 if starting failed.
UN_API long long un_start_game(const wchar_t *root, const char *args_json) {
    g_root = path_str(root ? root : L".");
    Value args;
    std::vector<std::string> argv;
    if (json::parse(args_json ? args_json : "[]", args))
        for (auto &a : args.items) argv.push_back(a.s);
    try {
        return start_game(argv);
    } catch (const Stop &stop) {
        write_std(stop.code == 0 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE, stop.message + (stop.message.empty() || stop.message.back() != '\n' ? "\n" : ""));
        return stop.code;
    }
}

}  // extern "C"
