// gameupdate.cpp (in uncharted_native.dll) checks the official game update the player extracted on their own and points
// the launcher at it (SETUP step 2). Nothing from the game comes with the port. Installing the game or the update from
// .pkg files was removed on 2026-11-12: those were decrypted ("fake") PKGs, which the launcher had to decrypt to extract;
// the port takes only folders the player already has.
//
// Operations offered through un_game_update:
//   check_update    lists what is wrong with an update's uroot as far as the port is concerned: absent files, and
//                   bin.psarc archives the games would fail to load (#43: one broken archive halted all three games
//                   during startup with "ASSERTION: root")
//   check_folder    takes an update that was extracted by hand, at any location, runs the same checks a PKG gets,
//                   and returns its uroot
//   existing_update finds an update folder sitting beside the game (<game>-patch, -update, _patch or _update, case
//                   ignored)

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

std::wstring arg_at(const Value &args, size_t i) {
    return i < args.items.size() && args.items[i].kind == Value::String ? widen(args.items[i].s) : L"";
}

}  // namespace

extern "C" {

// Entry point for the operations listed at the top of this file. `what` names one, args_json holds its arguments:
//   check_update [folder]          out = one problem per line
//   check_folder [folder, game]    out = the update's uroot
//   existing_update [game]         out = the folder found, or empty
// `root` (the launcher's folder) is no longer used. Returns 0 on success, 1 with a message meant for the player
// (UpdateError), 2 for an unrecognised `what` (also the removed install / install_game), 3 with a file system error text
// (OSError).
UN_API int un_game_update(const char *what, const wchar_t *root, const char *args_json, char *out, int cap) {
    (void)root;
    const std::string w = what ? what : "";
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
    } catch (const UpdateError &e) {
        int status = give(e.message, out, cap);
        return status < 0 ? status : 1;
    } catch (const OsError &e) {
        int status = give(e.message, out, cap);
        return status < 0 ? status : 3;
    }
    return 2;
}

// Step of a running operation (kept for the launcher's progress display; no operation reports steps any more).
UN_API int un_game_update_step(char *out, int cap) {
    std::lock_guard<std::mutex> guard(g_lock);
    return give(g_step, out, cap);
}

}  // extern "C"
