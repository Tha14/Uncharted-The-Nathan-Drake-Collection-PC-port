// gameupdate.cpp (in uncharted_native.dll) installs the official game update and the game itself from PKG files the
// player owns (Advanced -> GAME UPDATE, and "Install the game from .pkg"; #32). Nothing from the game comes with the
// port: the code below only extracts what the player hands it, by running PkgTool (LibOrbisPkg, LGPL-3.0, found in
// tools/pkgtool) as a separate process.
//
// Operations offered through un_game_update:
//   check_update    lists what is wrong with an update's uroot as far as the port is concerned: absent files, and
//                   bin.psarc archives the games would fail to load (#43: one broken archive halted all three games
//                   during startup with "ASSERTION: root")
//   check_folder    takes an update that was extracted by hand, at any location, runs the same checks a PKG gets,
//                   and returns its uroot
//   existing_update finds an update folder sitting beside the game (<game>-patch, -update, _patch or _update, case
//                   ignored)
//   install         extracts an update PKG beside the game as <game folder>-patch, replacing any older one there
//   install_game    extracts the player's base game PKG into <folder>\<title id>; its uroot becomes the game folder
// An extraction by PkgTool does not include the metadata entries stored in the PKG (param.sfo, playgo-chunk.dat and so
// on). We write those into sce_sys ourselves because both the launcher and the update check rely on them.

#include "native.h"
#include "json.h"
#include "inflate.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using json::Value;

// Thrown when installing can't go ahead; the message is written for the player (UpdateError in the Python version).
struct UpdateError { std::string message; };
// Thrown when a file system call fails midway; the system's text is passed on unchanged (Python's OSError).
struct OsError { std::string message; };

int give(const std::string &s, char *out, int cap) {
    if ((int)s.size() + 1 > cap) return -(int)(s.size() + 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}
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
std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

// ------------------------------------------------------------------------------------------------ path helpers (pathlib)

// Same result as str(Path(p)) gives on Windows
std::wstring path_str(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    std::wstring o;
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == L'\\' && i > 1 && !o.empty() && o.back() == L'\\') continue;
        o += p[i];
    }
    while (o.size() > 3 && o.back() == L'\\') o.pop_back();
    if (o.size() > 1 && o.back() == L'.' && o[o.size() - 2] == L'\\') o.resize(o.size() - 2);
    return o.empty() ? L"." : o;
}
std::wstring join(const std::wstring &a, const std::wstring &b) {
    return path_str(a + L"\\" + b);
}
std::wstring parent(const std::wstring &p) {
    const std::wstring s = path_str(p);
    const size_t cut = s.find_last_of(L'\\');
    if (cut == std::wstring::npos) return L".";
    if (cut == 2 && s[1] == L':') return s.substr(0, 3);
    return s.substr(0, cut);
}
std::wstring name_of(const std::wstring &p) {
    const std::wstring s = path_str(p);
    const size_t cut = s.find_last_of(L"\\:");
    return cut == std::wstring::npos ? s : s.substr(cut + 1);
}
std::wstring stem_of(const std::wstring &p) {
    const std::wstring n = name_of(p);
    const size_t dot = n.find_last_of(L'.');
    return dot == std::wstring::npos || dot == 0 ? n : n.substr(0, dot);
}

DWORD attrs(const std::wstring &p) { return GetFileAttributesW(p.c_str()); }
bool exists(const std::wstring &p) {   // follows links to their target
    HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}
bool is_dir(const std::wstring &p) { return exists(p) && (attrs(p) & FILE_ATTRIBUTE_DIRECTORY); }
bool is_file(const std::wstring &p) { return exists(p) && !(attrs(p) & FILE_ATTRIBUTE_DIRECTORY); }
long long file_size(const std::wstring &p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return -1;
    return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

std::string win_error(DWORD code) {
    wchar_t *text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                   0, (LPWSTR)&text, 0, nullptr);
    std::wstring t = text ? text : L"error";
    if (text) LocalFree(text);
    while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r' || t.back() == L' ' || t.back() == L'.')) t.pop_back();
    return "[WinError " + std::to_string(code) + "] " + utf8(t);
}
std::string shown(const std::wstring &p) {   // quoted and escaped like repr() of a Python path string
    std::string o;
    for (char c : utf8(p)) o += c == '\\' ? std::string("\\\\") : std::string(1, c);
    return "'" + o + "'";
}

void make_dirs(const std::wstring &p) {
    if (is_dir(p)) return;
    const std::wstring up = parent(p);
    if (up != p) make_dirs(up);
    if (!CreateDirectoryW(p.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        throw OsError{win_error(GetLastError()) + ": " + shown(p)};
}

// Behaves like shutil.rmtree(ignore_errors=True); a link is deleted itself, its target is left alone
void remove_tree(const std::wstring &p) {
    const DWORD a = attrs(p);
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if ((a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((p + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring n = fd.cFileName;
                if (n != L"." && n != L"..") remove_tree(p + L"\\" + n);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    if (a & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str()); else DeleteFileW(p.c_str());
}

void rename(const std::wstring &from, const std::wstring &to) {
    if (!MoveFileExW(from.c_str(), to.c_str(), 0)) throw OsError{win_error(GetLastError()) + ": " + shown(from) + " -> " + shown(to)};
}

unsigned long long free_space(const std::wstring &folder) {
    ULARGE_INTEGER caller{}, total{}, free{};
    if (!GetDiskFreeSpaceExW(folder.c_str(), &caller, &total, &free)) throw OsError{win_error(GetLastError()) + ": " + shown(folder)};
    return free.QuadPart;
}

// Canonical, lower-cased form of a folder path (as Path.resolve would give), so two folders can be compared.
std::wstring resolved(const std::wstring &p) {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetFullPathNameW(p.c_str(), 32768, buf.data(), nullptr);
    std::wstring f = n && n < 32768 ? std::wstring(buf.data(), n) : p;
    HANDLE h = CreateFileW(f.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        n = GetFinalPathNameByHandleW(h, buf.data(), 32768, FILE_NAME_NORMALIZED);
        CloseHandle(h);
        if (n && n < 32768) {
            f.assign(buf.data(), n);
            if (f.compare(0, 4, L"\\\\?\\") == 0) f = f.substr(4);
        }
    }
    return lower(path_str(f));
}

// ------------------------------------------------------------------------------------------------ progress

std::mutex g_lock;
std::string g_step;
void log_step(const std::string &text) {
    std::lock_guard<std::mutex> guard(g_lock);
    g_step = text;
}

// ------------------------------------------------------------------------------------------------ param.sfo

std::string str_of(const Value *v) {   // a param.sfo value as text (as str() would print it); "?" if there is none
    if (!v) return "?";
    return v->kind == Value::Int ? std::to_string(v->i) : v->s;
}

// Reads every key of a param.sfo into an object (text and integer values). If the file is unreadable or malformed
// the player gets told, through an UpdateError.
Value sfo_values(const std::wstring &path) {
    std::string d;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {   // worded with the errno text Python's open() would raise
        const DWORD e = GetLastError();
        const std::string why = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? "[Errno 2] No such file or directory"
                                : e == ERROR_ACCESS_DENIED ? "[Errno 13] Permission denied" : win_error(e);
        throw UpdateError{"The update's param.sfo can't be read (" + why + ": " + shown(path) + ")."};
    }
    char buf[1 << 16];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) d.append(buf, got);
    CloseHandle(h);
    auto damaged = [](const std::string &why) { return UpdateError{"The update's param.sfo can't be read (" + why + ")."}; };
    auto u32 = [&](size_t at) -> uint32_t {
        if (at + 4 > d.size()) throw damaged("unpack_from requires a buffer of at least " + std::to_string(at + 4) +
                                             " bytes for unpacking 4 bytes at offset " + std::to_string(at));
        uint32_t v;
        std::memcpy(&v, d.data() + at, 4);
        return v;
    };
    auto u16 = [&](size_t at) -> uint32_t {
        if (at + 2 > d.size()) throw damaged("unpack_from requires a buffer of at least " + std::to_string(at + 2) + " bytes");
        return (uint8_t)d[at] | (uint8_t)d[at + 1] << 8;
    };
    if (d.size() < 20) throw damaged("unpack_from requires a buffer of at least 20 bytes for unpacking 20 bytes at offset 0 "
                                     "(actual buffer size is " + std::to_string(d.size()) + ")");
    if (u32(0) != 0x46535000) throw UpdateError{"The update's param.sfo can't be read."};
    const size_t keys = u32(8), values = u32(12), count = u32(16);
    Value found = Value::object();
    for (size_t i = 0; i < count; ++i) {
        const size_t at = 20 + i * 16;
        if (at + 16 > d.size())
            throw damaged("unpack_from requires a buffer of at least " + std::to_string(at + 16) + " bytes for unpacking 16 "
                          "bytes at offset " + std::to_string(at) + " (actual buffer size is " + std::to_string(d.size()) + ")");
        const size_t key_at = u16(at), kind = u16(at + 2), length = u32(at + 4), value_at = u32(at + 12);
        const size_t key_start = keys + key_at;
        if (key_start > d.size()) throw damaged("subsection not found");
        const size_t key_end = d.find('\0', key_start);
        if (key_end == std::string::npos) throw damaged("subsection not found");
        std::string key = d.substr(key_start, key_end - key_start);
        for (auto &c : key) if ((unsigned char)c >= 0x80) c = '?';   // keys are ASCII; other bytes become '?'
        const size_t from = std::min(d.size(), values + value_at), to = std::min(d.size(), values + value_at + length);
        std::string raw = from < to ? d.substr(from, to - from) : std::string();
        if (kind == 0x0404) {
            if (raw.size() < 4) throw damaged("unpack requires a buffer of 4 bytes");
            uint32_t v;
            std::memcpy(&v, raw.data(), 4);
            found.set(key, Value::integer(v));
        } else {
            while (!raw.empty() && raw.back() == '\0') raw.pop_back();
            found.set(key, Value::text(utf8(widen(raw))));   // decoded as UTF-8, bad sequences replaced
        }
    }
    return found;
}

// ------------------------------------------------------------------------------------------------ PkgTool

std::wstring g_root;

std::wstring tool_path() { return join(g_root, L"tools\\pkgtool\\PkgTool.exe"); }

std::wstring quote(const std::wstring &arg) {   // one argument quoted by the rules subprocess.list2cmdline follows
    std::wstring result, bs;
    const bool q = arg.find(L' ') != std::wstring::npos || arg.find(L'\t') != std::wstring::npos || arg.empty();
    if (q) result += L'"';
    for (wchar_t c : arg) {
        if (c == L'\\') bs += c;
        else if (c == L'"') { result += std::wstring(bs.size() * 2, L'\\'); bs.clear(); result += L"\\\""; }
        else { result += bs; bs.clear(); result += c; }
    }
    result += bs;
    if (q) { result += bs; result += L'"'; }
    return result;
}

std::string read_all(HANDLE pipe) {
    std::string out;
    char buf[1 << 14];
    DWORD got = 0;
    while (ReadFile(pipe, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
    return out;
}

// Turns raw tool output into what a Python text-mode read returns: decoded as UTF-8 with bad bytes replaced, and
// every \r\n or lone \r changed to \n.
std::string text_mode(const std::string &bytes) {
    std::string s = utf8(widen(bytes)), o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') { o += '\n'; if (i + 1 < s.size() && s[i + 1] == '\n') ++i; }
        else o += s[i];
    }
    return o;
}
std::vector<std::string> lines_of(const std::string &s) {
    std::vector<std::string> lines;
    size_t a = 0;
    while (a < s.size()) {
        size_t b = s.find('\n', a);
        if (b == std::string::npos) b = s.size();
        lines.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return lines;
}
std::string strip(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Runs PkgTool with the given arguments and returns what it printed. Any failure becomes an UpdateError that tells the
// player what to do.
std::string run_tool(const std::vector<std::wstring> &arguments) {
    const std::wstring tool = tool_path();
    if (!is_file(tool)) throw UpdateError{"PkgTool was not found (" + utf8(tool) + "). Install the port again."};
    std::wstring cmd = quote(tool);
    for (auto &a : arguments) cmd += L" " + quote(a);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out_r, out_w, err_r, err_w;
    CreatePipe(&out_r, &out_w, &sa, 0);
    CreatePipe(&err_r, &err_w, &sa, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    const BOOL ok = CreateProcessW(tool.c_str(), buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    const DWORD error = GetLastError();
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if (!ok) {
        CloseHandle(out_r);
        CloseHandle(err_r);
        throw UpdateError{"PkgTool failed to launch: " + win_error(error) + ". It requires the .NET Framework that comes with "
                          "Windows 10/11."};
    }
    std::string err;
    std::thread reader([&] { err = read_all(err_r); });
    const std::string out = read_all(out_r);
    reader.join();
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(out_r);
    CloseHandle(err_r);
    const std::string stdout_text = text_mode(out), stderr_text = text_mode(err);
    if (code != 0) {
        const std::vector<std::string> detail = lines_of(strip(!stderr_text.empty() ? stderr_text : stdout_text));
        throw UpdateError{"PkgTool failed to read this file" + (detail.empty() ? std::string(".") : ": " + detail.back())};
    }
    return stdout_text;
}

struct Entry {
    std::string index;
    long long size = 0;
    bool encrypted = false;
};

bool all_of(const std::string &s, int (*test)(int)) {
    if (s.empty()) return false;
    for (unsigned char c : s) if (!test(c)) return false;
    return true;
}
bool hex_token(const std::string &s) { return s.size() > 2 && s[0] == '0' && s[1] == 'x' && all_of(s.substr(2), isxdigit); }

// Lists the PKG's entries through "PkgTool pkg_listentries" and keys them by name, keeping each one's index, size and
// encrypted flag. Every row PkgTool prints has the columns offset, size, flags, index, an optional extra number, name.
std::map<std::string, Entry> entries(const std::wstring &pkg) {
    std::map<std::string, Entry> rows;
    for (const std::string &raw : lines_of(run_tool({L"pkg_listentries", pkg}))) {
        std::vector<std::string> t;
        const std::string line = strip(raw);
        for (size_t a = 0; a < line.size();) {
            while (a < line.size() && isspace((unsigned char)line[a])) ++a;
            size_t b = a;
            while (b < line.size() && !isspace((unsigned char)line[b])) ++b;
            if (b > a) t.push_back(line.substr(a, b - a));
            a = b;
        }
        if (t.size() != 5 && t.size() != 6) continue;
        if (!hex_token(t[0]) || !hex_token(t[1]) || t[2].size() != 8 || !all_of(t[2], isxdigit) || !all_of(t[3], isdigit)) continue;
        if (t.size() == 6 && !all_of(t[4], isdigit)) continue;
        Entry e;
        e.index = t[3];
        e.size = std::stoll(t[1].substr(2), nullptr, 16);
        e.encrypted = (std::stoul(t[2], nullptr, 16) & 0x80000000u) != 0;
        rows[t.back()] = e;
    }
    return rows;
}

// ------------------------------------------------------------------------------------------------ bin.psarc checks

uint32_t be(const std::string &d, size_t at, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = v << 8 | (uint8_t)d[at + i];
    return v;
}
unsigned long long be_wide(const std::string &d, size_t at, int n) {
    unsigned long long v = 0;
    for (int i = 0; i < n; ++i) v = v << 8 | (uint8_t)d[at + i];
    return v;
}

// Inflates a complete zlib stream the way zlib.decompress does, validating the header and the Adler-32 trailer.
// Returns false for anything that is not a well-formed stream.
bool zlib_decompress(const std::string &in, std::string &out) {
    if (in.size() < 6) return false;
    const unsigned cmf = (uint8_t)in[0], flg = (uint8_t)in[1];
    if ((cmf & 15) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) return false;
    out.clear();
    try {
        inflate::Inflater inf(reinterpret_cast<const unsigned char *>(in.data()) + 2, in.size() - 2, out);
        inf.run();
        const size_t end = 2 + inf.consumed();
        if (end + 4 > in.size()) return false;
        uint32_t a = 1, b = 0;
        for (unsigned char c : out) { a = (a + c) % 65521; b = (b + a) % 65521; }
        return be(in, end, 4) == (b << 16 | a);
    } catch (const inflate::InflateError &) {
        return false;
    }
}

// Looks in a PSARC's manifest for bin/shaders/src/files.json. Returns 1 if the name is listed, 0 if not, and -1 if
// the file can't be treated as a PSARC: a bad header, or a file size other than table of contents plus all stored
// blocks (a truncated or padded archive).
int psarc_has_shader_list(const std::wstring &path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    auto read = [&](size_t n) {
        std::string s(n, '\0');
        DWORD got = 0;
        if (n && !ReadFile(h, &s[0], (DWORD)n, &got, nullptr)) got = 0;
        s.resize(got);
        return s;
    };
    int result = -1;
    do {
        const std::string head = read(32);
        if (head.size() < 32) break;
        const std::string compression = head.substr(8, 4);
        const uint32_t toc_length = be(head, 12, 4), entry_size = be(head, 16, 4), count = be(head, 20, 4),
                       block_size = be(head, 24, 4);
        if (head.compare(0, 4, "PSAR") != 0 || count < 1 || entry_size < 30 ||
            (unsigned long long)toc_length < 32ull + (unsigned long long)count * entry_size)
            break;
        const std::string toc = read(toc_length - 32);
        if (toc.size() < 30) break;
        const uint32_t first_block = be(toc, 16, 4);
        const unsigned long long size = be_wide(toc, 20, 5), offset = be_wide(toc, 25, 5);
        const int width = block_size <= 0x10000 ? 2 : block_size <= 0x1000000 ? 3 : 4;
        const size_t sizes_at = (size_t)count * entry_size;
        const std::string sizes = sizes_at < toc.size() ? toc.substr(sizes_at) : std::string();
        unsigned long long stored = 0;
        for (size_t i = 0; i + width <= sizes.size(); i += width) {
            const uint32_t s = be(sizes, i, width);
            stored += s ? s : block_size;
        }
        const long long actual = file_size(path);
        if (actual < 0 || (unsigned long long)actual != toc_length + stored) break;
        LARGE_INTEGER to;
        to.QuadPart = (LONGLONG)offset;
        SetFilePointerEx(h, to, nullptr, FILE_BEGIN);
        std::string manifest;
        bool ok = true;
        for (uint32_t block = first_block; manifest.size() < size; ++block) {
            const size_t at = (size_t)block * width;
            const uint32_t s = at + width <= sizes.size() ? be(sizes, at, width) : 0;
            std::string chunk = read(s ? s : block_size);
            if (chunk.empty()) { ok = false; break; }
            if (compression == "zlib" && chunk.size() < block_size && (uint8_t)chunk[0] == 0x78) {
                std::string plain;
                if (!zlib_decompress(chunk, plain)) { ok = false; break; }
                chunk.swap(plain);
            }
            manifest += chunk;
        }
        if (!ok) break;
        manifest.resize((size_t)size);
        const std::string names = "\n" + utf8(widen(manifest)) + "\n";
        result = names.find("\nbin/shaders/src/files.json\n") != std::string::npos ? 1 : 0;
    } while (false);
    CloseHandle(h);
    return result;
}

const char *const REQUIRED[] = {"eboot.bin", "big2-ps4_Shipping.elf", "big3-ps4_Shipping.elf", "sce_sys/param.sfo",
                                "u1data/data.psarc", "u2data/data.psarc", "u3data/data.psarc",
                                "u1data/build/main/bin.psarc", "u2data/build/main/bin.psarc", "u3data/build/main/bin.psarc"};

// Every problem the port would have with `folder`, an update's uroot; empty when it is usable.
std::vector<std::string> check_update(const std::wstring &folder) {
    std::vector<std::string> problems;
    for (const char *name : REQUIRED) {
        std::wstring rel = widen(name);
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        if (!is_file(join(folder, rel))) problems.push_back(name);
    }
    for (const char *game : {"u1data", "u2data", "u3data"}) {
        const std::wstring archive = join(folder, widen(game) + L"\\build\\main\\bin.psarc");
        if (!is_file(archive)) continue;
        const int names = psarc_has_shader_list(archive);
        if (names < 0) problems.push_back(std::string(game) + "/build/main/bin.psarc (damaged: the archive can't be read)");
        else if (!names) problems.push_back(std::string(game) + "/build/main/bin.psarc (incomplete: the shader list is missing)");
    }
    return problems;
}

std::string joined(const std::vector<std::string> &items) {
    std::string o;
    for (size_t k = 0; k < items.size(); ++k) o += (k ? ", " : "") + items[k];
    return o;
}

// ------------------------------------------------------------------------------------------------ operations

std::wstring title_folder(const std::wstring &game) {
    return lower(name_of(game)) == L"uroot" ? parent(game) : path_str(game);
}

// Finds an update folder beside the game using the same name rules as the game starter; empty if there is none.
std::wstring existing_update(const std::wstring &game) {
    const std::wstring title = title_folder(game), base = lower(name_of(title)), up = parent(title);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((up + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        const std::wstring n = fd.cFileName, l = lower(n);
        for (const wchar_t *suffix : {L"-patch", L"-update", L"_patch", L"_update"}) {
            if (l == base + suffix && is_dir(join(up, n))) { found = join(up, n); break; }
        }
    } while (found.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// Scratch folder in %TEMP% that deletes itself when it goes out of scope (like tempfile.TemporaryDirectory).
struct TempDir {
    std::wstring path;
    TempDir() {
        wchar_t base[MAX_PATH + 1];
        GetTempPathW(MAX_PATH + 1, base);
        for (unsigned n = GetTickCount() ^ GetCurrentProcessId(); ; ++n) {
            path = path_str(std::wstring(base) + L"tmpgu" + std::to_wstring(n));
            if (CreateDirectoryW(path.c_str(), nullptr)) break;
            if (GetLastError() != ERROR_ALREADY_EXISTS) throw OsError{win_error(GetLastError()) + ": " + shown(path)};
        }
    }
    ~TempDir() { remove_tree(path); }
};

// Pulls PARAM_SFO out of the PKG with PkgTool and parses it; an encrypted entry ends in `encrypted_message`.
Value pkg_sfo(const std::wstring &pkg, const std::map<std::string, Entry> &listing, const std::string &encrypted_message) {
    auto it = listing.find("PARAM_SFO");
    if (it->second.encrypted) throw UpdateError{encrypted_message};
    TempDir temporary;
    const std::wstring sfo = join(temporary.path, L"param.sfo");
    run_tool({L"pkg_extractentry", pkg, widen(it->second.index), sfo});
    return sfo_values(sfo);
}

// Confirms that `pkg` holds an update matching the game installed in `game`. Returns the update's param.sfo values and
// fills `base` with the game's.
Value inspect(const std::wstring &pkg, const std::wstring &game, Value &base) {
    const auto listing = entries(pkg);
    if (!listing.count("PARAM_SFO")) throw UpdateError{"This file has no param.sfo, so it is not a game update."};
    Value update = pkg_sfo(pkg, listing, "This is an encrypted store PKG. The update is needed as a decrypted (fake) PKG.");
    const std::wstring game_sfo = join(game, L"sce_sys\\param.sfo");
    if (!is_file(game_sfo)) throw UpdateError{"Pick the game folder before this (the folder holding eboot.bin and sce_sys)."};
    base = sfo_values(game_sfo);
    const Value *category = update.get("CATEGORY");
    if (!category || category->kind != Value::String || category->s != "gp")
        throw UpdateError{"This PKG is no update; it holds '" + str_of(update.get("TITLE")) + "' (category " +
                          str_of(category) + ")."};
    const Value *a = update.get("TITLE_ID"), *b = base.get("TITLE_ID");
    if (str_of(a) != str_of(b) || (a == nullptr) != (b == nullptr) || (a && b && a->kind != b->kind))
        throw UpdateError{"This update belongs to " + str_of(a) + " while your game is " + str_of(b) +
                          ". Get the update for your game's region."};
    return update;
}

// Takes an update the player extracted on their own, wherever it is, and returns its uroot. The player may have picked
// either the update's top folder or the uroot inside it.
std::wstring check_folder(const std::wstring &folder_in, const std::wstring &game) {
    const std::wstring folder = path_str(folder_in);
    std::wstring root;
    for (const std::wstring &candidate : {join(folder, L"uroot"), folder}) {
        if (is_file(join(candidate, L"eboot.bin")) && is_file(join(candidate, L"sce_sys\\param.sfo"))) {
            root = candidate;
            break;
        }
    }
    if (root.empty())
        throw UpdateError{"No extracted game update found in this folder: neither it nor its uroot folder holds eboot.bin "
                          "and sce_sys\\param.sfo."};
    const std::wstring game_sfo = join(game, L"sce_sys\\param.sfo");
    if (!is_file(game_sfo)) throw UpdateError{"Pick the game folder before this (the folder holding eboot.bin and sce_sys)."};
    if (resolved(root) == resolved(game)) throw UpdateError{"This is the game folder, not an update. Pick the folder of the update."};
    const Value update = sfo_values(join(root, L"sce_sys\\param.sfo")), base = sfo_values(game_sfo);
    const Value *category = update.get("CATEGORY");
    if (!category || category->kind != Value::String || category->s != "gp")
        throw UpdateError{"This folder contains a game, not an update ('" + str_of(update.get("TITLE")) + "', version " +
                          str_of(update.get("APP_VER")) + ")."};
    const Value *a = update.get("TITLE_ID"), *b = base.get("TITLE_ID");
    if (str_of(a) != str_of(b) || (a == nullptr) != (b == nullptr) || (a && b && a->kind != b->kind))
        throw UpdateError{"This update belongs to " + str_of(a) + " while your game is " + str_of(b) +
                          ". Get the update for your game's region."};
    const auto missing = check_update(root);
    if (!missing.empty())
        throw UpdateError{"Files in the update folder are missing or damaged (extract the update once more): " + joined(missing)};
    return root;
}

// PKG metadata entry names and the sce_sys file each one is written to.
const std::pair<const char *, const wchar_t *> METADATA[] = {
    {"PARAM_SFO", L"param.sfo"}, {"PLAYGO_CHUNK_DAT", L"playgo-chunk.dat"}, {"PLAYGO_CHUNK_SHA", L"playgo-chunk.sha"},
    {"PLAYGO_MANIFEST_XML", L"playgo-manifest.xml"}, {"SHAREPARAM_JSON", L"shareparam.json"},
    {"CHANGEINFO__CHANGEINFO_XML", L"changeinfo\\changeinfo.xml"}};

void add_metadata(const std::wstring &pkg, const std::wstring &uroot, const std::map<std::string, Entry> &listing) {
    for (auto &m : METADATA) {
        auto it = listing.find(m.first);
        if (it == listing.end() || it->second.encrypted) continue;
        const std::wstring destination = join(join(uroot, L"sce_sys"), m.second);
        make_dirs(parent(destination));
        run_tool({L"pkg_extractentry", pkg, widen(it->second.index), destination});
    }
}

std::string gigabytes(double bytes, int decimals) {
    char b[64];
    std::snprintf(b, sizeof b, decimals ? "%.1f" : "%.0f", bytes / 1073741824.0);
    return b;
}

// Extracts the update PKG beside the game folder under the name <game folder>-patch and returns that copy's uroot.
std::wstring install(const std::wstring &pkg_in, const std::wstring &game_in) {
    const std::wstring pkg = path_str(pkg_in), game = path_str(game_in);
    Value base;
    const Value update = inspect(pkg, game, base);
    const std::wstring title = title_folder(game), up = parent(title);
    std::wstring target = existing_update(game);
    if (target.empty()) target = join(up, name_of(title) + L"-patch");
    const long long need = (long long)((double)file_size(pkg) * 2.3) + (1LL << 28);
    const unsigned long long free = free_space(up);
    if ((long long)free < need)
        throw UpdateError{"That drive is short of free space: " + gigabytes((double)need, 1) + " GB required, " +
                          gigabytes((double)free, 1) + " GB available."};
    const std::wstring partial = join(up, name_of(title) + L"-patch.partial");
    remove_tree(partial);
    try {
        log_step("Extracting update " + str_of(update.get("APP_VER")) + "...");
        run_tool({L"pkg_extract", pkg, partial});
        const std::wstring uroot = join(partial, L"uroot");
        if (!is_dir(uroot)) throw UpdateError{"No game files came out of the PKG (it has no uroot folder)."};
        log_step("Writing the update's metadata...");
        add_metadata(pkg, uroot, entries(pkg));
        const auto missing = check_update(uroot);
        if (!missing.empty()) throw UpdateError{"Files of the update are missing or damaged: " + joined(missing)};
        log_step("Moving it into place...");
        std::wstring old;
        if (exists(target) || attrs(target) != INVALID_FILE_ATTRIBUTES) {
            old = target + L".old";
            remove_tree(old);
            rename(target, old);
        }
        try {
            rename(partial, target);
        } catch (...) {
            if (!old.empty()) rename(old, target);   // put the previous update folder back untouched
            throw;
        }
        if (!old.empty()) remove_tree(old);
    } catch (...) {
        remove_tree(partial);
        throw;
    }
    return join(target, L"uroot");
}

const char *const BASE_REQUIRED[] = {"eboot.bin", "big2-ps4_Shipping.elf", "big3-ps4_Shipping.elf", "sce_sys/param.sfo",
                                     "u1data", "u2data", "u3data"};

// Extracts a base game PKG the player owns into a <parent>\<title id> folder. The result is that folder's uroot, which
// the launcher then uses as the game folder.
std::wstring install_game(const std::wstring &pkg_in, const std::wstring &parent_in) {
    const std::wstring pkg = path_str(pkg_in), into = path_str(parent_in);
    const auto listing = entries(pkg);
    if (!listing.count("PARAM_SFO")) throw UpdateError{"This file has no param.sfo, so it is not a PS4 game package."};
    const Value values = pkg_sfo(pkg, listing, "This is an encrypted store PKG. The game is needed as a decrypted (fake) PKG.");
    const Value *category = values.get("CATEGORY");
    if (category && str_of(category).compare(0, 2, "gp") == 0)
        throw UpdateError{"This PKG holds a game update rather than the game itself. Once the game is installed, add it with "
                          "GAME UPDATE -> Install from .pkg."};
    const Value *id = values.get("TITLE_ID");
    std::string title = id && id->truthy() ? str_of(id) : utf8(stem_of(pkg));
    title = strip(title);
    const std::wstring target = join(into, widen(title));
    if (is_file(join(target, L"uroot\\eboot.bin")) || is_file(join(target, L"eboot.bin")))
        throw UpdateError{"The game is already installed here:\n" + utf8(target) + "\n\nSelect it with Change game folder."};
    const long long need = (long long)((double)file_size(pkg) * 1.45) + (1LL << 30);
    const unsigned long long free = free_space(into);
    if ((long long)free < need)
        throw UpdateError{"That drive is short of free space: " + gigabytes((double)need, 0) + " GB required, " +
                          gigabytes((double)free, 0) + " GB available."};
    const std::wstring partial = join(into, widen(title) + L".partial");
    remove_tree(partial);
    try {
        const Value *name = values.get("TITLE");
        log_step("Extracting " + (name ? str_of(name) : std::string("the game")) + " (" + title + "), this takes a few minutes...");
        run_tool({L"pkg_extract", pkg, partial});
        const std::wstring uroot = join(partial, L"uroot");
        if (!is_dir(uroot)) throw UpdateError{"No game files came out of the PKG (it has no uroot folder)."};
        log_step("Writing the game's metadata...");
        add_metadata(pkg, uroot, listing);
        std::vector<std::string> missing;
        for (const char *n : BASE_REQUIRED) {
            std::wstring rel = widen(n);
            std::replace(rel.begin(), rel.end(), L'/', L'\\');
            if (!exists(join(uroot, rel))) missing.push_back(n);
        }
        if (!missing.empty())
            throw UpdateError{"This PKG is damaged or holds something other than The Nathan Drake Collection: missing " + joined(missing)};
        log_step("Moving it into place...");
        rename(partial, target);
    } catch (...) {
        remove_tree(partial);
        throw;
    }
    return join(target, L"uroot");
}

std::wstring arg_at(const Value &args, size_t i) {
    return i < args.items.size() && args.items[i].kind == Value::String ? widen(args.items[i].s) : L"";
}

}  // namespace

extern "C" {

// Entry point for the operations listed at the top of this file. `what` names one, args_json holds its arguments:
//   check_update [folder]          out = one problem per line
//   check_folder [folder, game]    out = the update's uroot
//   existing_update [game]         out = the folder found, or empty
//   install [pkg, game]            out = the uroot that was installed
//   install_game [pkg, folder]     out = the uroot that was installed
// While the two install operations run, un_game_update_step reports their current step. `root` is the launcher's
// folder, used to find tools\pkgtool. Returns 0 on success, 1 with a message meant for the player (UpdateError), 2 for
// an unrecognised `what`, 3 with a file system error text (OSError).
UN_API int un_game_update(const char *what, const wchar_t *root, const char *args_json, char *out, int cap) {
    const std::string w = what ? what : "";
    g_root = path_str(root ? root : L".");
    Value args;
    if (!json::parse(args_json ? args_json : "[]", args)) return 2;
    try {
        if (w == "check_update") {
            std::string text;
            for (auto &p : check_update(path_str(arg_at(args, 0)))) text += p + "\n";
            return give(text, out, cap);
        }
        if (w == "check_folder") return give(utf8(check_folder(arg_at(args, 0), path_str(arg_at(args, 1)))), out, cap);
        if (w == "existing_update") return give(utf8(existing_update(path_str(arg_at(args, 0)))), out, cap);
        if (w == "install") {
            log_step("");
            return give(utf8(install(arg_at(args, 0), arg_at(args, 1))), out, cap);
        }
        if (w == "install_game") {
            log_step("");
            return give(utf8(install_game(arg_at(args, 0), arg_at(args, 1))), out, cap);
        }
    } catch (const UpdateError &e) {
        int status = give(e.message, out, cap);
        return status < 0 ? status : 1;
    } catch (const OsError &e) {
        int status = give(e.message, out, cap);
        return status < 0 ? status : 3;
    }
    return 2;
}

// Current step of an install in progress, e.g. "Extracting update 01.02...".
UN_API int un_game_update_step(char *out, int cap) {
    std::lock_guard<std::mutex> guard(g_lock);
    return give(g_step, out, cap);
}

}  // extern "C"
