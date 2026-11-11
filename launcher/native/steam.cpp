// steam.cpp - implements the launcher's "Add to Steam" action (Advanced -> STEAM) inside uncharted_native.dll.
//
// Non-Steam shortcuts live in <Steam>/userdata/<account>/config/shortcuts.vdf, a binary key/value file, with their
// artwork beside it as config/grid/<app id><suffix>.png. Because Steam loads the file on startup and overwrites it
// when it quits, the launcher first makes sure Steam is closed (un_steam_running).
//
// Design rules:
//   * Lossless round trip: shortcuts created by anyone else are written back byte for byte, in their original order.
//   * Existing launcher shortcuts are recognised (matching program and options, or a known name pointing at a
//     launcher file in a different folder) and edited in place, so Steam keeps their play time, collections and
//     hidden flag.
//   * All accounts' files are parsed before any of them is touched. Every write first saves a timestamped backup
//     (at most five are kept) and goes through a temporary file.
//   * Artwork is generated only from the player's own game files (sce_sys) and never replaces an image that exists.
//
// The exported functions exchange UTF-8 text: one record per line, tab-separated fields (documented in native.h).

#include "native.h"

#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <windows.h>
#include <tlhelp32.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

// ------------------------------------------------------------------------------------------------ text helpers

std::wstring widen(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// lowercases with Unicode rules; names, paths and options are compared case-insensitively like Windows paths
std::string folded(const std::string &s) {
    std::wstring w = widen(s);
    if (!w.empty()) CharLowerBuffW(w.data(), (DWORD)w.size());
    return narrow(w);
}

std::string trimmed(const std::string &s, const char *chars = " \t\r\n") {
    size_t a = s.find_first_not_of(chars);
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(chars);
    return s.substr(a, b - a + 1);
}

bool starts_with(const std::string &s, const std::string &prefix) {
    return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin());
}

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t at = s.find(sep, start);
        out.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) return out;
        start = at + 1;
    }
}

// ------------------------------------------------------------------------------------------------ files

bool read_file(const std::wstring &path, std::string &data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1LL << 30);
    if (ok) {
        data.assign((size_t)size.QuadPart, '\0');
        DWORD got = 0;
        ok = data.empty() || (ReadFile(h, data.data(), (DWORD)data.size(), &got, nullptr) && got == data.size());
    }
    CloseHandle(h);
    return ok;
}

bool write_file(const std::wstring &path, const std::string &data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size());
    ok = CloseHandle(h) && ok;
    return ok;
}

bool is_file(const std::wstring &path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool is_dir(const std::wstring &path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void make_dirs(const std::wstring &path) {
    if (path.empty() || is_dir(path)) return;
    size_t cut = path.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 0) make_dirs(path.substr(0, cut));
    CreateDirectoryW(path.c_str(), nullptr);
}

std::wstring join(const std::wstring &a, const std::wstring &b) {
    if (a.empty()) return b;
    wchar_t last = a.back();
    return (last == L'\\' || last == L'/') ? a + b : a + L"\\" + b;
}

std::wstring last_error_text(const wchar_t *what, const std::wstring &path) {
    return std::wstring(what) + L" " + path + L" (Windows error " + std::to_wstring(GetLastError()) + L")";
}

struct FoundFile {
    std::wstring name;
    FILETIME written;
};

std::vector<FoundFile> list_files(const std::wstring &folder, const std::wstring &pattern, bool dirs) {
    std::vector<FoundFile> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(join(folder, pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (dir != dirs || !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        out.push_back({fd.cFileName, fd.ftLastWriteTime});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// ------------------------------------------------------------------------------------------------ binary key/value

enum Kind : uint8_t { K_MAP = 0, K_STRING = 1, K_INT32 = 2, K_FLOAT = 3, K_POINTER = 4, K_WSTRING = 5, K_COLOR = 6,
                      K_UINT64 = 7, K_END = 8, K_INT64 = 10 };

int fixed_size(uint8_t kind) {
    switch (kind) {
    case K_INT32: case K_FLOAT: case K_POINTER: case K_COLOR: return 4;
    case K_UINT64: case K_INT64: return 8;
    default: return -1;
    }
}

// A parsed node. `raw` keeps the value's bytes untouched (strings minus the NUL, wide strings minus the double NUL,
// numbers as stored); for a map the children sit in `items` instead.
struct Item {
    uint8_t kind = K_STRING;
    std::string key;
    std::string raw;
    std::vector<Item> items;

    std::string text() const { return kind == K_STRING ? raw : std::string(); }
};

struct Reader {
    const std::string &d;
    size_t pos = 0;
    std::string error;

    explicit Reader(const std::string &data) : d(data) {}

    bool cstring(std::string &out) {
        size_t end = d.find('\0', pos);
        if (end == std::string::npos) { error = "a string has no terminating zero byte"; return false; }
        out.assign(d, pos, end - pos);
        pos = end + 1;
        return true;
    }

    bool entries(std::vector<Item> &out) {
        for (;;) {
            if (pos >= d.size()) { error = "a map is cut off by the end of the file"; return false; }
            uint8_t kind = (uint8_t)d[pos++];
            if (kind == K_END) return true;
            Item it;
            it.kind = kind;
            if (!cstring(it.key)) return false;
            if (kind == K_MAP) {
                if (!entries(it.items)) return false;
            } else if (kind == K_STRING) {
                if (!cstring(it.raw)) return false;
            } else if (kind == K_WSTRING) {
                size_t end = pos;
                while (true) {
                    if (end + 1 >= d.size()) { error = "a wide string has no terminator"; return false; }
                    if (d[end] == '\0' && d[end + 1] == '\0') break;
                    end += 2;
                }
                it.raw.assign(d, pos, end - pos);
                pos = end + 2;
            } else if (fixed_size(kind) > 0) {
                size_t n = (size_t)fixed_size(kind);
                if (pos + n > d.size()) { error = "a number is cut off by the end of the file"; return false; }
                it.raw.assign(d, pos, n);
                pos += n;
            } else {
                error = "byte " + std::to_string(pos - 1) + " holds an unsupported node type " + std::to_string(kind);
                return false;
            }
            out.push_back(std::move(it));
        }
    }
};

bool parse_file(const std::string &data, std::vector<Item> &top, std::string &error) {
    Reader r(data);
    if (!r.entries(top)) { error = r.error; return false; }
    for (size_t i = r.pos; i < data.size(); ++i)
        if (data[i] != '\0') { error = "extra bytes follow the end of the data at byte " + std::to_string(r.pos); return false; }
    return true;
}

void write_entries(const std::vector<Item> &items, std::string &out) {
    for (const Item &it : items) {
        out += (char)it.kind;
        out += it.key;
        out += '\0';
        if (it.kind == K_MAP) {
            write_entries(it.items, out);
        } else if (it.kind == K_STRING) {
            out += it.raw;
            out += '\0';
        } else if (it.kind == K_WSTRING) {
            out += it.raw;
            out.append(2, '\0');
        } else {
            out += it.raw;
        }
    }
    out += (char)K_END;
}

bool only_zeros(const std::string &data) {
    return std::all_of(data.begin(), data.end(), [](char c) { return c == '\0'; });
}

std::string u32_bytes(uint32_t v) {
    std::string s(4, '\0');
    for (int i = 0; i < 4; ++i) s[i] = (char)(v >> (8 * i));
    return s;
}

Item make_text(const char *key, const std::string &value) { Item it; it.kind = K_STRING; it.key = key; it.raw = value; return it; }
Item make_number(const char *key, uint32_t value) { Item it; it.kind = K_INT32; it.key = key; it.raw = u32_bytes(value); return it; }

const Item *find_key(const std::vector<Item> &items, const char *key) {
    std::string want = folded(key);
    for (const Item &it : items)
        if (folded(it.key) == want) return &it;
    return nullptr;
}

std::string text_of(const std::vector<Item> &entry, const char *key) {
    const Item *it = find_key(entry, key);
    return it ? it->text() : std::string();
}

void put(std::vector<Item> &entry, Item value) {
    std::string want = folded(value.key);
    for (Item &it : entry)
        if (folded(it.key) == want) { it.kind = value.kind; it.raw = value.raw; it.items.clear(); return; }
    entry.push_back(std::move(value));
}

// ------------------------------------------------------------------------------------------------ the launcher's entries

const char *const COLLECTION = "Uncharted: The Nathan Drake Collection";

uint32_t crc32_of(const std::string &s) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Steam's app id for a shortcut (also the file name stem of its artwork): CRC-32 over exe + name with bit 31 forced on.
uint32_t game_id(const std::string &exe, const std::string &name) { return crc32_of(exe + name) | 0x80000000u; }

struct Target {
    std::string name, exe, start, options;
};

std::vector<Item> fresh_entry(uint32_t id, const Target &t, const std::string &icon) {
    std::vector<Item> e;
    e.push_back(make_number("appid", id));
    e.push_back(make_text("AppName", t.name));
    e.push_back(make_text("Exe", t.exe));
    e.push_back(make_text("StartDir", t.start));
    e.push_back(make_text("icon", icon));
    e.push_back(make_text("ShortcutPath", ""));
    e.push_back(make_text("LaunchOptions", t.options));
    e.push_back(make_number("IsHidden", 0));
    e.push_back(make_number("AllowDesktopConfig", 1));
    e.push_back(make_number("AllowOverlay", 1));
    e.push_back(make_number("OpenVR", 0));
    e.push_back(make_number("Devkit", 0));
    e.push_back(make_text("DevkitGameID", ""));
    e.push_back(make_number("DevkitOverrideAppID", 0));
    e.push_back(make_number("LastPlayTime", 0));
    e.push_back(make_text("FlatpakAppID", ""));
    Item tags;
    tags.kind = K_MAP;
    tags.key = "tags";
    e.push_back(tags);
    return e;
}

struct MergeResult {
    std::vector<uint32_t> ids;
    int added = 0, updated = 0, removed = 0;
};

// True for a shortcut this launcher made: same executable and, when running from source, launch options that begin
// with the same script path.
bool is_ours(const std::vector<Item> &entry, const std::string &exe, const std::string &base) {
    std::string its = folded(trimmed(trimmed(text_of(entry, "Exe")), "\""));
    if (its != folded(trimmed(exe, "\""))) return false;
    std::string options = folded(trimmed(text_of(entry, "LaunchOptions")));
    if (!base.empty()) return starts_with(options, folded(base));
    return options.empty() || starts_with(options, "--play");
}

// True for one of our shortcuts that points at a different install location, e.g. after the port folder was moved
// or after switching between the release package and a source checkout.
bool is_ours_elsewhere(const std::vector<Item> &entry, const std::set<std::string> &names) {
    if (!names.count(text_of(entry, "AppName"))) return false;
    std::string command = folded(text_of(entry, "Exe") + " " + text_of(entry, "LaunchOptions"));
    return command.find("uncharted launcher.exe") != std::string::npos ||
           command.find("uncharted_launcher.pyw") != std::string::npos;
}

MergeResult merge(std::vector<Item> &top, const std::vector<Target> &targets, const std::string &icon,
                  const std::vector<std::string> &extra_names) {
    std::vector<Item> *list = nullptr;
    std::string shortcuts = "shortcuts";
    for (Item &it : top)
        if (it.kind == K_MAP && folded(it.key) == shortcuts) { list = &it.items; break; }
    if (!list) {
        Item m;
        m.kind = K_MAP;
        m.key = "shortcuts";
        top.push_back(m);
        list = &top.back().items;
    }
    const std::string &exe = targets[0].exe, &start = targets[0].start;
    std::string base = trimmed(targets[0].options.substr(0, targets[0].options.find("--play")));
    std::set<std::string> names(extra_names.begin(), extra_names.end());
    for (const Target &t : targets) names.insert(t.name);

    MergeResult r;
    std::vector<Item> kept;
    std::set<std::string> done;
    auto wanted = [&](const std::string &options) -> const Target * {
        for (const Target &t : targets)
            if (folded(t.options) == options) return &t;
        return nullptr;
    };
    for (Item &node : *list) {
        std::string options;
        bool have_options = false;
        if (node.kind == K_MAP && is_ours(node.items, exe, base)) {
            options = folded(trimmed(text_of(node.items, "LaunchOptions")));
            have_options = true;
        } else if (node.kind == K_MAP && is_ours_elsewhere(node.items, names)) {
            std::string name = text_of(node.items, "AppName");
            for (const Target &t : targets)
                if (t.name == name) { options = folded(t.options); have_options = true; }
        } else {
            kept.push_back(std::move(node));
            continue;
        }
        const Target *t = have_options ? wanted(options) : nullptr;
        if (!t || done.count(options)) { r.removed++; continue; }
        put(node.items, make_number("appid", game_id(exe, t->name)));
        put(node.items, make_text("AppName", t->name));
        put(node.items, make_text("Exe", exe));
        put(node.items, make_text("StartDir", start));
        put(node.items, make_text("icon", icon));
        put(node.items, make_text("LaunchOptions", t->options));
        kept.push_back(std::move(node));
        done.insert(options);
        r.updated++;
    }
    for (const Target &t : targets) {
        if (done.count(folded(t.options))) continue;
        Item m;
        m.kind = K_MAP;
        m.items = fresh_entry(game_id(exe, t.name), t, icon);
        kept.push_back(std::move(m));
        r.added++;
    }
    for (size_t i = 0; i < kept.size(); ++i) kept[i].key = std::to_string(i);
    *list = std::move(kept);
    for (const Target &t : targets) r.ids.push_back(game_id(exe, t.name));
    return r;
}

// ------------------------------------------------------------------------------------------------ Steam's folders

std::wstring registry_text(HKEY hive, const wchar_t *path, const wchar_t *value) {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(hive, path, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return std::wstring();
    return buf;
}

std::wstring full_path(const std::wstring &p) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 4, buf, nullptr);
    std::wstring s = (n && n < MAX_PATH * 4) ? std::wstring(buf, n) : p;
    for (wchar_t &c : s) if (c == L'/') c = L'\\';
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

std::wstring real_path(const std::wstring &p) {
    HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return full_path(p);
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFinalPathNameByHandleW(h, buf, MAX_PATH * 4, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    return (n && n < MAX_PATH * 4) ? std::wstring(buf, n) : full_path(p);
}

std::wstring lower_w(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

std::vector<std::wstring> steam_folders() {
    std::vector<std::wstring> found;
    found.push_back(registry_text(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"));
    found.push_back(registry_text(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"));
    found.push_back(registry_text(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"));
    for (const wchar_t *var : {L"ProgramFiles(x86)", L"ProgramFiles"}) {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH);
        if (n && n < MAX_PATH) found.push_back(join(buf, L"Steam"));
    }
    std::vector<std::wstring> out;
    std::set<std::wstring> seen;
    for (const std::wstring &f : found) {
        if (f.empty()) continue;
        if (seen.insert(lower_w(full_path(f))).second) out.push_back(f);
    }
    return out;
}

// pairs of (userdata folder number, persona name), taken from the text-format config/loginusers.vdf
std::vector<std::pair<std::string, std::string>> persona_names(const std::wstring &root) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string text;
    if (!read_file(join(root, L"config\\loginusers.vdf"), text)) return out;
    size_t pos = 0;
    while ((pos = text.find('"', pos)) != std::string::npos) {
        size_t end = text.find('"', pos + 1);
        if (end == std::string::npos) break;
        std::string token = text.substr(pos + 1, end - pos - 1);
        pos = end + 1;
        if (token.size() != 17 || !std::all_of(token.begin(), token.end(), ::isdigit)) continue;
        size_t open = text.find_first_not_of(" \t\r\n", pos);
        if (open == std::string::npos || text[open] != '{') continue;
        size_t close = text.find_first_of("{}", open + 1);
        if (close == std::string::npos || text[close] != '}') continue;
        std::string body = text.substr(open + 1, close - open - 1);
        std::string persona;
        size_t key = body.find("\"PersonaName\"");
        if (key != std::string::npos) {
            size_t a = body.find('"', key + 13);
            size_t b = a == std::string::npos ? a : body.find('"', a + 1);
            if (b != std::string::npos) persona = body.substr(a + 1, b - a - 1);
        }
        unsigned long long id = _strtoui64(token.c_str(), nullptr, 10);
        out.push_back({std::to_string((unsigned long)(id & 0xFFFFFFFFull)), persona});
        pos = close + 1;
    }
    return out;
}

struct Account {
    std::wstring folder;
    std::string name;
};

std::vector<Account> accounts_on_this_pc() {
    std::vector<Account> out;
    std::set<std::wstring> seen;
    for (const std::wstring &root : steam_folders()) {
        std::wstring userdata = join(root, L"userdata");
        if (!is_dir(userdata)) continue;
        auto names = persona_names(root);
        std::vector<FoundFile> dirs = list_files(userdata, L"*", true);
        std::sort(dirs.begin(), dirs.end(), [](const FoundFile &a, const FoundFile &b) { return a.name < b.name; });
        for (const FoundFile &d : dirs) {
            if (d.name == L"0" || !std::all_of(d.name.begin(), d.name.end(), iswdigit)) continue;
            std::wstring folder = join(userdata, d.name);
            if (!seen.insert(lower_w(real_path(folder))).second) continue;
            std::string persona;
            for (auto &n : names)
                if (n.first == narrow(d.name)) persona = n.second;
            out.push_back({folder, persona});
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ pictures (WIC)

struct Picture {
    int w = 0, h = 0;
    std::vector<uint8_t> px;   // 8-bit RGBA, rows stored top to bottom
    bool has_alpha = false;

    uint8_t *at(int x, int y) { return &px[((size_t)y * w + x) * 4]; }
    const uint8_t *at(int x, int y) const { return &px[((size_t)y * w + x) * 4]; }
};

struct Com {
    IWICImagingFactory *factory = nullptr;
    bool owns = false;
    Com() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owns = SUCCEEDED(hr);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    }
    ~Com() {
        if (factory) factory->Release();
        if (owns) CoUninitialize();
    }
};

template <class T> struct Ref {
    T *p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T **out() { return &p; }
    T *operator->() const { return p; }
};

bool load_picture(Com &com, const std::wstring &path, Picture &pic) {
    if (!com.factory || !is_file(path)) return false;
    Ref<IWICBitmapDecoder> dec;
    Ref<IWICBitmapFrameDecode> frame;
    Ref<IWICFormatConverter> conv;
    if (FAILED(com.factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                      dec.out())) ||
        FAILED(dec->GetFrame(0, frame.out())) || FAILED(com.factory->CreateFormatConverter(conv.out())) ||
        FAILED(conv->Initialize(frame.p, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || !h || w > 16384 || h > 16384) return false;
    pic.w = (int)w;
    pic.h = (int)h;
    pic.px.assign((size_t)w * h * 4, 0);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)pic.px.size(), pic.px.data()))) return false;
    for (size_t i = 3; i < pic.px.size(); i += 4) pic.px[i] = 255;   // source art is treated as fully opaque
    return true;
}

bool save_picture(Com &com, const Picture &pic, const std::wstring &path) {
    if (!com.factory) return false;
    Ref<IWICStream> stream;
    Ref<IWICBitmapEncoder> enc;
    Ref<IWICBitmapFrameEncode> frame;
    if (FAILED(com.factory->CreateStream(stream.out())) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(com.factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.out())) ||
        FAILED(enc->Initialize(stream.p, WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(frame.out(), nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize((UINT)pic.w, (UINT)pic.h)))
        return false;
    // opaque images are saved as 24-bit RGB; only the logo (which has has_alpha set) is saved with transparency
    std::vector<uint8_t> rows;
    WICPixelFormatGUID fmt = pic.has_alpha ? GUID_WICPixelFormat32bppRGBA : GUID_WICPixelFormat24bppRGB;
    WICPixelFormatGUID want = fmt;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;
    UINT stride;
    if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppRGBA)) {
        stride = (UINT)pic.w * 4;
        rows = pic.px;
        if (!pic.has_alpha) for (size_t i = 3; i < rows.size(); i += 4) rows[i] = 255;
    } else if (IsEqualGUID(fmt, GUID_WICPixelFormat24bppRGB) || IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR)) {
        bool bgr = IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR);
        stride = (UINT)pic.w * 3;
        rows.resize((size_t)stride * pic.h);
        for (size_t i = 0, j = 0; i < pic.px.size(); i += 4, j += 3) {
            rows[j] = pic.px[bgr ? i + 2 : i];
            rows[j + 1] = pic.px[i + 1];
            rows[j + 2] = pic.px[bgr ? i : i + 2];
        }
    } else if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) || IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGR)) {
        stride = (UINT)pic.w * 4;
        rows = pic.px;
        for (size_t i = 0; i < rows.size(); i += 4) {
            std::swap(rows[i], rows[i + 2]);
            if (!pic.has_alpha) rows[i + 3] = 255;
        }
    } else {
        (void)want;
        return false;
    }
    return SUCCEEDED(frame->WritePixels((UINT)pic.h, stride, (UINT)rows.size(), rows.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

// Lanczos-3 kernel; resized() applies it in two separable passes on float channels, premultiplying by alpha when the
// image carries transparency
double lanczos(double x) {
    const double a = 3.0;
    x = std::fabs(x);
    if (x < 1e-9) return 1.0;
    if (x >= a) return 0.0;
    const double pi = 3.14159265358979323846;
    return a * std::sin(pi * x) * std::sin(pi * x / a) / (pi * pi * x * x);
}

Picture resized(const Picture &src, int w, int h) {
    auto pass = [](const std::vector<float> &in, int iw, int ih, int ow, bool horizontal) {
        int in_len = horizontal ? iw : ih, out_len = horizontal ? ow : ih;
        int lines = horizontal ? ih : iw;
        if (!horizontal) out_len = ow;   // vertical pass: `ow` is reused as the target height
        std::vector<float> out((size_t)(horizontal ? ow * ih : iw * ow) * 4, 0.f);
        double scale = (double)in_len / out_len;
        double support = 3.0 * std::max(1.0, scale);
        double filter_scale = std::max(1.0, scale);
        for (int o = 0; o < out_len; ++o) {
            double center = (o + 0.5) * scale;
            int lo = std::max(0, (int)std::floor(center - support));
            int hi = std::min(in_len - 1, (int)std::ceil(center + support));
            std::vector<double> wts;
            double sum = 0;
            for (int i = lo; i <= hi; ++i) {
                double wv = lanczos((i + 0.5 - center) / filter_scale);
                wts.push_back(wv);
                sum += wv;
            }
            if (sum == 0) sum = 1;
            for (int l = 0; l < lines; ++l) {
                double acc[4] = {0, 0, 0, 0};
                for (int i = lo; i <= hi; ++i) {
                    size_t s = horizontal ? ((size_t)l * iw + i) * 4 : ((size_t)i * iw + l) * 4;
                    double wv = wts[i - lo] / sum;
                    for (int c = 0; c < 4; ++c) acc[c] += in[s + c] * wv;
                }
                size_t d = horizontal ? ((size_t)l * ow + o) * 4 : ((size_t)o * iw + l) * 4;
                for (int c = 0; c < 4; ++c) out[d + c] = (float)acc[c];
            }
        }
        return out;
    };
    std::vector<float> f(src.px.size());
    for (size_t i = 0; i < src.px.size(); i += 4) {
        float a = src.has_alpha ? src.px[i + 3] / 255.f : 1.f;
        for (int c = 0; c < 3; ++c) f[i + c] = src.px[i + c] * a;
        f[i + 3] = src.px[i + 3];
    }
    std::vector<float> horiz = pass(f, src.w, src.h, w, true);
    std::vector<float> both = pass(horiz, w, src.h, h, false);
    Picture out;
    out.w = w;
    out.h = h;
    out.has_alpha = src.has_alpha;
    out.px.resize((size_t)w * h * 4);
    for (size_t i = 0; i < out.px.size(); i += 4) {
        float a = std::min(255.f, std::max(0.f, both[i + 3]));
        float div = src.has_alpha ? (a > 0.f ? a / 255.f : 1.f) : 1.f;
        for (int c = 0; c < 3; ++c)
            out.px[i + c] = (uint8_t)std::lround(std::min(255.f, std::max(0.f, both[i + c] / div)));
        out.px[i + 3] = (uint8_t)std::lround(a);
    }
    return out;
}

Picture cropped(const Picture &src, int x0, int y0, int x1, int y1) {
    Picture out;
    out.w = std::max(1, x1 - x0);
    out.h = std::max(1, y1 - y0);
    out.has_alpha = src.has_alpha;
    out.px.assign((size_t)out.w * out.h * 4, 0);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            int sx = std::min(src.w - 1, std::max(0, x0 + x)), sy = std::min(src.h - 1, std::max(0, y0 + y));
            std::memcpy(out.at(x, y), src.at(sx, sy), 4);
        }
    return out;
}

int round_half_even(double v) { return (int)std::nearbyint(v); }

// "cover" fit: scale until w x h is completely filled, then crop, positioning the cut by the focus (fx, fy in 0..1)
Picture covering(const Picture &src, int w, int h, double fx = 0.5, double fy = 0.5) {
    double scale = std::max((double)w / src.w, (double)h / src.h);
    int sw = std::max(w, round_half_even(src.w * scale)), sh = std::max(h, round_half_even(src.h * scale));
    Picture big = resized(src, sw, sh);
    int left = round_half_even((sw - w) * fx), top = round_half_even((sh - h) * fy);
    return cropped(big, left, top, left + w, top + h);
}

// paste `pic` onto `base` at (x, y) with a soft border: opacity ramps from 0 to full across the outer `fade` pixels
void blend_in(Picture &base, const Picture &pic, int x, int y, int fade) {
    for (int py = 0; py < pic.h; ++py)
        for (int px = 0; px < pic.w; ++px) {
            int bx = x + px, by = y + py;
            if (bx < 0 || by < 0 || bx >= base.w || by >= base.h) continue;
            int edge = std::min(std::min(px, py), std::min(pic.w - 1 - px, pic.h - 1 - py));
            int m = edge >= fade ? 255 : round_half_even(255.0 * edge / fade);
            uint8_t *b = base.at(bx, by);
            const uint8_t *s = pic.at(px, py);
            for (int c = 0; c < 3; ++c) b[c] = (uint8_t)((s[c] * m + b[c] * (255 - m) + 127) / 255);
        }
}

// Extracts the Collection's title text from the cover art (icon0.png) for the logo: dark pixels turn opaque and the
// light parchment background turns transparent; the result is trimmed and scaled to 1280 px wide.
Picture title_lettering(const Picture &cover) {
    const int w = cover.w, h = cover.h;
    Picture area = cropped(cover, round_half_even(w * 0.06), round_half_even(h * 0.47), round_half_even(w * 0.94),
                           round_half_even(h * 0.74));
    area.has_alpha = true;
    int x0 = area.w, y0 = area.h, x1 = -1, y1 = -1;
    for (int y = 0; y < area.h; ++y)
        for (int x = 0; x < area.w; ++x) {
            uint8_t *p = area.at(x, y);
            int grey = (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000;
            int a = grey <= 60 ? 255 : grey >= 110 ? 0 : round_half_even(255.0 * (110 - grey) / 50);
            p[3] = (uint8_t)a;
            if (a) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
        }
    if (x1 >= 0) area = cropped(area, x0, y0, x1 + 1, y1 + 1);
    const int width = 1280;
    return resized(area, width, round_half_even(area.h * (double)width / area.w));
}

struct Made {
    std::string suffix;
    Picture pic;
};

std::vector<Made> pictures_from_game(Com &com, const std::wstring &game_dir) {
    std::vector<Made> out;
    if (game_dir.empty()) return out;
    std::wstring sce = join(game_dir, L"sce_sys");
    Picture art, cover;
    bool have_art = load_picture(com, join(sce, L"pic1.png"), art) || load_picture(com, join(sce, L"pic0.png"), art);
    bool have_cover = load_picture(com, join(sce, L"icon0.png"), cover);
    if (have_art) out.push_back({"_hero", covering(art, 1920, 620, 0.5, 0.25)});
    if (have_cover) {
        // Backdrop for the capsules: the blank left strip of the background art (just parchment), so Drake does not
        // appear twice next to the cover
        Picture paper = have_art ? cropped(art, 0, 0, round_half_even(art.w * 0.3), art.h) : cropped(cover, 0, 0, 8, 8);
        Picture tall = covering(paper, 600, 900);
        blend_in(tall, resized(cover, 600, 600), 0, 210, 130);
        out.push_back({"p", tall});
        Picture wide = covering(paper, 920, 430);
        blend_in(wide, resized(cover, 430, 430), (920 - 430) / 2, 0, 100);
        out.push_back({"", wide});
        out.push_back({"_logo", title_lettering(cover)});
    }
    return out;
}

// saves every generated image for every app id into grid\, skipping any stem that already has a file in any format;
// returns how many files were created
int write_pictures(Com &com, const std::wstring &grid, const std::vector<uint32_t> &ids, const std::vector<Made> &made) {
    if (made.empty()) return 0;
    make_dirs(grid);
    int written = 0;
    for (uint32_t id : ids)
        for (const Made &m : made) {
            std::wstring stem = std::to_wstring(id) + widen(m.suffix);
            if (!list_files(grid, stem + L".*", false).empty()) continue;
            if (save_picture(com, m.pic, join(grid, stem + L".png"))) written++;
        }
    return written;
}

// ------------------------------------------------------------------------------------------------ writing one account

const wchar_t *const BACKUP_END = L".uncharted-backup";

std::wstring keep_backup(const std::wstring &file) {
    if (!is_file(file)) return std::wstring();
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    wchar_t stamp[32];
    wcsftime(stamp, 32, L"%Y%m%d-%H%M%S", &local);
    std::wstring copy = file + L"." + stamp + BACKUP_END;
    for (int n = 2; is_file(copy); ++n) copy = file + L"." + stamp + L"-" + std::to_wstring(n) + BACKUP_END;
    if (!CopyFileW(file.c_str(), copy.c_str(), TRUE)) return std::wstring();
    size_t cut = file.find_last_of(L"\\/");
    std::wstring folder = file.substr(0, cut), name = file.substr(cut + 1);
    std::vector<FoundFile> old = list_files(folder, name + L".*" + BACKUP_END, false);
    std::sort(old.begin(), old.end(), [](const FoundFile &a, const FoundFile &b) {
        return CompareFileTime(&a.written, &b.written) < 0;
    });
    for (size_t i = 0; i + 5 < old.size(); ++i) DeleteFileW(join(folder, old[i].name).c_str());
    return copy;
}

struct AccountResult {
    std::wstring file, backup;
    MergeResult merged;
    int pictures = 0;
};

bool read_shortcuts(const std::wstring &file, std::vector<Item> &top, std::string &error) {
    std::string data;
    if (!is_file(file)) return true;
    if (!read_file(file, data)) { error = "could not be read"; return false; }
    if (only_zeros(data)) return true;
    return parse_file(data, top, error);
}

// ------------------------------------------------------------------------------------------------ the C interface

int give(const std::string &text, char *out, int cap) {
    if ((int)text.size() + 1 > cap) return -(int)(text.size() + 1);
    std::memcpy(out, text.c_str(), text.size() + 1);
    return 0;
}

std::vector<std::vector<std::string>> records(const char *text) {
    std::vector<std::vector<std::string>> out;
    for (std::string line : split(text ? text : "", '\n')) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(split(line, '\t'));
    }
    return out;
}

}  // namespace

extern "C" {

UN_API int un_version(void) { return UN_VERSION; }

UN_API int un_steam_running(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    int running = 0;
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, L"steam.exe")) { running = 1; break; }
    CloseHandle(snap);
    return running;
}

UN_API int un_steam_targets(const char *request, char *out, int cap) {
    std::string root, starter, python, script;
    std::vector<std::pair<std::string, std::string>> games;
    bool per_game = false;
    for (auto &r : records(request)) {
        const std::string &tag = r[0];
        if (tag == "root" && r.size() > 1) root = r[1];
        else if (tag == "starter" && r.size() > 1) starter = r[1];
        else if (tag == "python" && r.size() > 1) python = r[1];
        else if (tag == "script" && r.size() > 1) script = r[1];
        else if (tag == "per_game") per_game = true;
        else if (tag == "game" && r.size() > 2) games.push_back({r[1], r[2]});
    }
    std::string exe, base;
    if (!starter.empty() && is_file(widen(starter))) {
        exe = "\"" + starter + "\"";
    } else {
        size_t cut = python.find_last_of("\\/");
        std::string pythonw = (cut == std::string::npos ? std::string() : python.substr(0, cut + 1)) + "pythonw.exe";
        exe = "\"" + (is_file(widen(pythonw)) ? pythonw : python) + "\"";
        base = "\"" + script + "\"";
    }
    std::string start = "\"" + root + "\"";
    std::string text;
    if (!per_game) {
        text = std::string(COLLECTION) + "\t" + exe + "\t" + start + "\t" + base + "\n";
    } else {
        for (auto &g : games) text += g.second + "\t" + exe + "\t" + start + "\t" + trimmed(base + " --play " + g.first) + "\n";
    }
    return give(text, out, cap);
}

UN_API int un_steam_add(const char *request, char *out, int cap) {
    std::vector<Target> targets;
    std::vector<std::string> names;
    std::string icon, game_dir;
    std::vector<Account> accounts;
    bool given_accounts = false;
    for (auto &r : records(request)) {
        const std::string &tag = r[0];
        if (tag == "target" && r.size() > 4) targets.push_back({r[1], r[2], r[3], r[4]});
        else if (tag == "name" && r.size() > 1) names.push_back(r[1]);
        else if (tag == "icon" && r.size() > 1) icon = r[1];
        else if (tag == "game_dir" && r.size() > 1) game_dir = r[1];
        else if (tag == "account" && r.size() > 2) { given_accounts = true; accounts.push_back({widen(r[1]), r[2]}); }
    }
    if (targets.empty()) { int r = give("no shortcut targets were given", out, cap); return r < 0 ? r : UN_ERR_OS; }
    if (!given_accounts) accounts = accounts_on_this_pc();
    if (accounts.empty()) { int r = give("", out, cap); return r < 0 ? r : UN_ERR_NO_STEAM; }
    // validate all accounts up front so a broken file aborts before anything has been modified
    for (const Account &a : accounts) {
        std::vector<Item> probe;
        std::string error;
        std::wstring file = join(a.folder, L"config\\shortcuts.vdf");
        if (!read_shortcuts(file, probe, error)) {
            int r = give(narrow(file) + ": " + error, out, cap);
            return r < 0 ? r : UN_ERR_BAD_FILE;
        }
    }
    Com com;
    std::vector<Made> made = pictures_from_game(com, widen(game_dir));
    std::string text;
    for (const Account &a : accounts) {
        std::wstring config = join(a.folder, L"config"), file = join(config, L"shortcuts.vdf");
        std::vector<Item> top;
        std::string error;
        if (!read_shortcuts(file, top, error)) {
            int r = give(narrow(file) + ": " + error, out, cap);
            return r < 0 ? r : UN_ERR_BAD_FILE;
        }
        AccountResult res;
        res.merged = merge(top, targets, icon, names);
        make_dirs(config);
        res.file = file;
        res.backup = keep_backup(file);
        std::string data;
        write_entries(top, data);
        std::wstring writing = file + L".writing";
        if (!write_file(writing, data) || !MoveFileExW(writing.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            int r = give(narrow(last_error_text(L"failed to write", file)), out, cap);
            return r < 0 ? r : UN_ERR_OS;
        }
        res.pictures = write_pictures(com, join(config, L"grid"), res.merged.ids, made);
        std::string ids;
        for (size_t i = 0; i < res.merged.ids.size(); ++i) ids += (i ? "," : "") + std::to_string(res.merged.ids[i]);
        text += "account\t" + narrow(a.folder) + "\t" + a.name + "\t" + narrow(res.file) + "\t" + narrow(res.backup) + "\t" +
                std::to_string(res.merged.added) + "\t" + std::to_string(res.merged.updated) + "\t" +
                std::to_string(res.merged.removed) + "\t" + std::to_string(res.pictures) + "\t" + ids + "\n";
    }
    return give(text, out, cap);
}

UN_API int un_steam_roundtrip(const char *path) {
    // for tests: parse a shortcuts file and re-serialize it to <path>.roundtrip. 0 = done, 1 = I/O problem,
    // 2 = parse failure
    std::wstring file = widen(path ? path : "");
    std::vector<Item> top;
    std::string error;
    if (!is_file(file)) return 1;
    if (!read_shortcuts(file, top, error)) return 2;
    std::string data;
    write_entries(top, data);
    return write_file(file + L".roundtrip", data) ? 0 : 1;
}

}  // extern "C"
