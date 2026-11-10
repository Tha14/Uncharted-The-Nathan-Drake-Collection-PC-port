// starter_files.cpp - the game starter's files (tools/probe_uncharted.py), part of uncharted_native.dll: the runtime as
// Uncharted.exe, a second instance's user folder, and the update's copies for drives that hold no links.

#include "native.h"
#include "json.h"

#include <cstdint>
#include <cstring>
#include <windows.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {

using json::Value;

int give(const std::string &s, char *out, int cap) {
    if ((int)s.size() + 1 > cap) return -(int)(s.size() + 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

std::string utf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

std::wstring env(const wchar_t *name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return L"";
    std::wstring v(n, L'\0');
    n = GetEnvironmentVariableW(name, v.data(), n);
    v.resize(n);
    return v;
}

std::wstring join(const std::wstring &a, const std::wstring &b) {
    if (a.empty()) return b;
    return (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b;
}

std::wstring base_name(const std::wstring &p) {
    size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? p : p.substr(cut + 1);
}

// the last Windows error as Python words an OSError: "[WinError n] text: 'path'"
std::string os_error(const std::wstring &path, DWORD code = GetLastError()) {
    wchar_t *text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                   0, (LPWSTR)&text, 0, nullptr);
    std::wstring t = text ? text : L"error";
    if (text) LocalFree(text);
    while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r' || t.back() == L' ' || t.back() == L'.')) t.pop_back();
    std::string shown;   // the path as Python's repr shows it
    for (char c : utf8(path)) shown += c == '\\' ? std::string("\\\\") : std::string(1, c);
    return "[WinError " + std::to_string(code) + "] " + utf8(t) + ": '" + shown + "'";
}

bool attributes(const std::wstring &p, WIN32_FILE_ATTRIBUTE_DATA &a) {
    return GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) != 0;
}
bool is_file(const std::wstring &p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    return attributes(p, a) && !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}
bool is_dir(const std::wstring &p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    return attributes(p, a) && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}
uint64_t size_of(const WIN32_FILE_ATTRIBUTE_DATA &a) { return ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow; }
uint64_t ticks(const FILETIME &f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }   // 100 ns

bool make_dirs(const std::wstring &p) {
    if (is_dir(p)) return true;
    size_t cut = p.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 2) make_dirs(p.substr(0, cut));
    return CreateDirectoryW(p.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

struct Failure { std::string message; };

// a folder copied with its files' times (shutil.copytree)
void copy_tree(const std::wstring &from, const std::wstring &to) {
    if (!CreateDirectoryW(to.c_str(), nullptr)) throw Failure{os_error(to)};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(join(from, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring a = join(from, fd.cFileName), b = join(to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copy_tree(a, b);
        else if (!CopyFileW(a.c_str(), b.c_str(), FALSE)) { FindClose(h); throw Failure{os_error(a)}; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// The runtime as Uncharted.exe (Task Manager, Windows' graphics settings and overlays show this name), copied from the
// build output whenever they differ. UNCHARTED_TEST_PROBE_EXE=1: the build output itself (tests); UNCHARTED_PROBE_EXE=
// <file>: another build run as Uncharted.exe (copied back at the next normal start). Lines: the executable, a message.
std::string game_executable(const std::wstring &root) {
    std::wstring built = join(root, L"gow3_pc\\out\\gow3-probe.exe"), game_exe = join(root, L"gow3_pc\\out\\Uncharted.exe");
    if (env(L"UNCHARTED_TEST_PROBE_EXE") == L"1") return utf8(built) + "\n";
    std::wstring other = env(L"UNCHARTED_PROBE_EXE");
    if (!other.empty()) built = other;
    WIN32_FILE_ATTRIBUTE_DATA b, g;
    if (!attributes(built, b))   // still running from a previous start: the build output
        return utf8(built) + "\nUsing " + utf8(base_name(built)) + ": " + os_error(built);
    bool have = attributes(game_exe, g);
    if (!have || ticks(b.ftLastWriteTime) != ticks(g.ftLastWriteTime) || size_of(b) != size_of(g)) {
        if (!CopyFileW(built.c_str(), game_exe.c_str(), FALSE))
            return utf8(built) + "\nUsing " + utf8(base_name(built)) + ": " + os_error(game_exe);
    }
    return utf8(game_exe) + "\n";
}

// The user folder (saves, settings, caches) of this instance: a second one (UNCHARTED_INSTANCE=<n>) starts with copies
// of the main one's settings, saves, trophies and shader caches (an empty cache would compile for minutes).
std::string user_dir(const std::wstring &root, const std::string &suffix) {
    std::wstring main_user = join(root, L"analysis\\uncharted-user");
    if (suffix.empty()) return utf8(main_user) + "\n";
    std::wstring user = join(root, L"analysis\\uncharted-user" + widen(suffix));
    if (GetFileAttributesW(user.c_str()) != INVALID_FILE_ATTRIBUTES) return utf8(user) + "\n";
    if (!make_dirs(user)) throw Failure{os_error(user)};
    for (const wchar_t *name : {L"probe.ini", L"probe-u1.ini", L"probe-u2.ini", L"probe-u3.ini", L"disabled-optimizations.txt",
                                L"skip.txt", L"savedata", L"trophy", L"cache", L"shader"}) {
        std::wstring source = join(main_user, name), dest = join(user, name);
        if (is_dir(source)) copy_tree(source, dest);
        else if (is_file(source) && !CopyFileW(source.c_str(), dest.c_str(), FALSE)) throw Failure{os_error(source)};
    }
    return utf8(user) + "\nInstance " + suffix.substr(2) + ": new user folder " + utf8(user) +
           " (settings, saves and caches copied)";
}

void files_under(const std::wstring &folder, const std::wstring &relative, std::vector<std::wstring> &out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(join(folder, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring rel = relative + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) files_under(join(folder, fd.cFileName), rel, out);
        else out.push_back(rel);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::string sfo_text(const std::wstring &folder, const char *key) {
    std::vector<char> buf(1 << 16);
    if (un_system("sfo", folder.c_str(), buf.data(), (int)buf.size()) != 0) return "";
    Value v;
    if (!json::parse(buf.data(), v)) return "";
    const Value *f = v.get(key);
    if (!f) return "";
    return f->kind == Value::String ? f->s : f->kind == Value::Int ? std::to_string(f->i) : "";
}

// For drives that hold no links (exFAT, FAT32): copies of the files the prepare scripts read (the executables,
// sce_module, sce_sys), the update's over the game's, in analysis\views (about 90 MB, copied again only when a source
// changes). The runtime reads the update over the game itself (--app0-overlay).
std::string boot_view(const std::wstring &root, const std::wstring &base, const std::wstring &update) {
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> chosen;   // lower relative -> (relative, source)
    std::vector<std::wstring> order;
    auto choose = [&](const std::wstring &relative, const std::wstring &source) {
        std::wstring key = lower(relative);
        if (!chosen.count(key)) order.push_back(key);
        chosen[key] = {relative, source};
    };
    for (const std::wstring &layer : {base, update}) {   // the update last: its files win
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(join(layer, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) throw Failure{os_error(layer)};
        do {
            std::wstring name = fd.cFileName, low = lower(name);
            if (name == L"." || name == L"..") continue;
            bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!dir && (low == L"eboot.bin" || low == L"big2-ps4_shipping.elf" || low == L"big3-ps4_shipping.elf")) {
                choose(name, join(layer, name));
            } else if (dir && (low == L"sce_module" || low == L"sce_sys")) {
                std::vector<std::wstring> files;
                files_under(join(layer, name), name, files);
                for (const std::wstring &rel : files) choose(rel, join(layer, rel));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::string title = sfo_text(update, "TITLE_ID"), version = sfo_text(update, "APP_VER");
    std::wstring view = join(root, L"analysis\\views\\boot-" + widen(title.empty() ? "game" : title) + L"-" +
                                       widen(version.empty() ? "update" : version));
    for (const std::wstring &key : order) {
        const auto &entry = chosen[key];
        std::wstring dest = join(view, entry.first), source = entry.second;
        WIN32_FILE_ATTRIBUTE_DATA wanted, current;
        if (!attributes(source, wanted)) throw Failure{os_error(source)};
        // FAT32 keeps write times in 2-second steps.
        if (attributes(dest, current) && size_of(current) == size_of(wanted)) {
            uint64_t a = ticks(current.ftLastWriteTime), b = ticks(wanted.ftLastWriteTime);
            if ((a > b ? a - b : b - a) <= 20000000ULL) continue;
        }
        make_dirs(dest.substr(0, dest.find_last_of(L'\\')));
        // a plain copy, then the source's times: a read-only source attribute would make the next refresh fail
        HANDLE in = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr);
        if (in == INVALID_HANDLE_VALUE) throw Failure{os_error(source)};
        HANDLE outf = CreateFileW(dest.c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (outf == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            CloseHandle(in);
            throw Failure{os_error(dest, code)};
        }
        std::vector<char> buf(1 << 20);
        DWORD got = 0, put = 0;
        bool ok = true;
        while (ok && ReadFile(in, buf.data(), (DWORD)buf.size(), &got, nullptr) && got)
            ok = WriteFile(outf, buf.data(), got, &put, nullptr) && put == got;
        ok = ok && SetFileTime(outf, nullptr, &wanted.ftLastAccessTime, &wanted.ftLastWriteTime);
        DWORD code = GetLastError();
        CloseHandle(in);
        CloseHandle(outf);
        if (!ok) throw Failure{os_error(dest, code)};
    }
    return utf8(view);
}

std::wstring arg_at(const Value &args, size_t i) {
    return i < args.items.size() && args.items[i].kind == Value::String ? widen(args.items[i].s) : L"";
}

}  // namespace

extern "C" {

// what: game_executable [root] (lines: the executable, a message), user_dir [root, suffix] (lines: the folder, a
// message), boot_view [root, game, update] (the folder). args_json: the arguments as a JSON array. 0 ok; 1 failed
// (out: why, worded as Python's OSError); 2 unknown what.
UN_API int un_starter_files(const char *what, const char *args_json, char *out, int cap) {
    std::string w = what ? what : "";
    Value args;
    if (!json::parse(args_json ? args_json : "[]", args) || args.kind != Value::Array) return 2;
    try {
        if (w == "game_executable") return give(game_executable(arg_at(args, 0)), out, cap);
        if (w == "user_dir") return give(user_dir(arg_at(args, 0), utf8(arg_at(args, 1))), out, cap);
        if (w == "boot_view") return give(boot_view(arg_at(args, 0), arg_at(args, 1), arg_at(args, 2)), out, cap);
    } catch (const Failure &f) {
        int status = give(f.message, out, cap);
        return status < 0 ? status : 1;
    }
    return 2;
}

}  // extern "C"
