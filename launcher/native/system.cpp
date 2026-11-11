// system.cpp - environment and install queries for the launcher, compiled into uncharted_native.dll. Covers: the
// static game / language / cheat tables; validating a game folder and locating its update; reading the game version
// from param.sfo and cross-checking it with the actual executable; detecting which speech languages are installed;
// Wine detection and path conversion; spotting drives that cause stutter; process liveness; the Desktop location;
// and the launcher and recipe version files.

#include "native.h"
#include "json.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <windows.h>
#include <shlobj.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
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

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

bool is_file(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool is_dir(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring join(const std::wstring &a, const std::wstring &b) {
    if (a.empty()) return b;
    wchar_t last = a.back();
    return (last == L'\\' || last == L'/') ? a + b : a + L"\\" + b;
}

bool read_all(const std::wstring &path, std::string &data, size_t limit = (size_t)1 << 31) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && (size_t)size.QuadPart <= limit;
    if (ok) {
        data.assign((size_t)size.QuadPart, '\0');
        size_t done = 0;
        while (ok && done < data.size()) {
            DWORD got = 0;
            DWORD want = (DWORD)std::min<size_t>(data.size() - done, 1 << 30);
            ok = ReadFile(h, data.data() + done, want, &got, nullptr) && got > 0;
            done += got;
        }
    }
    CloseHandle(h);
    return ok;
}

std::vector<std::wstring> entries(const std::wstring &folder, const std::wstring &pattern, int dirs) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(join(folder, pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        bool d = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (dirs < 0 || d == dirs) out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// ------------------------------------------------------------------------------------------------ tables

Value tables() {
    Value t = Value::object();
    Value games;
    games.kind = Value::Array;
    const char *rows[][7] = {
        {"u1", "1", "DRAKE'S FORTUNE", "Uncharted: Drake's Fortune", "Uncharted 1  \xE2\x80\xA2  double-click to start", "eboot.bin", "uncharted"},
        {"u2", "2", "AMONG THIEVES", "Uncharted 2: Among Thieves", "Uncharted 2  \xE2\x80\xA2  double-click to start", "big2-ps4_Shipping.elf", "big2-ps4_Shipping"},
        {"u3", "3", "DRAKE'S DECEPTION", "Uncharted 3: Drake's Deception", "Uncharted 3  \xE2\x80\xA2  double-click to start", "big3-ps4_Shipping.elf", "big3-ps4_Shipping"}};
    const char *keys[] = {"key", "number", "title", "name", "subtitle", "exe", "log"};
    for (auto &r : rows) {
        Value g = Value::object();
        for (int k = 0; k < 7; ++k) g.set(keys[k], Value::text(r[k]));
        games.items.push_back(g);
    }
    t.set("GAMES", games);
    // PS4 system-language ids, one per text language the Collection ships (22 in total)
    const char *langs[][2] = {
        {"1", "English (US)"}, {"18", "English (UK)"}, {"2", "Fran\xC3\xA7" "ais"}, {"4", "Deutsch"}, {"5", "Italiano"},
        {"3", "Espa\xC3\xB1ol (Espa\xC3\xB1" "a)"}, {"20", "Espa\xC3\xB1ol (Latinoam\xC3\xA9rica)"},
        {"7", "Portugu\xC3\xAAs (Portugal)"}, {"17", "Portugu\xC3\xAAs (Brasil)"}, {"6", "Nederlands"}, {"14", "Dansk"},
        {"15", "Norsk"}, {"13", "Svenska"}, {"12", "Suomi"}, {"16", "Polski"}, {"23", "\xC4\x8C" "e\xC5\xA1tina"},
        {"25", "\xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC"},
        {"8", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9"}, {"19", "T\xC3\xBCrk\xC3\xA7" "e"},
        {"0", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"}, {"9", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4"},
        {"10", "\xE4\xB8\xAD\xE6\x96\x87 (\xE7\xB9\x81\xE9\xAB\x94)"}};
    Value l;
    l.kind = Value::Array;
    for (auto &r : langs) {
        Value p;
        p.kind = Value::Array;
        p.items = {Value::text(r[0]), Value::text(r[1])};
        l.items.push_back(p);
    }
    t.set("LANGUAGES", l);
    const char *cheats[][3] = {{"invincible", "Invincible", "Nate cannot be hurt. Works in all three games."},
                               {"ammo", "Infinite ammo", "Unlimited ammunition. Works in all three games."},
                               {"noreload", "No reload", "The current magazine stays full. Uncharted 2 and 3 only."},
                               {"grenades", "Infinite grenades", "Unlimited grenades. Uncharted 3 only."},
                               {"onehit", "One-hit kills", "A single hit is enough to drop an enemy. Uncharted 3 only."}};
    Value c;
    c.kind = Value::Array;
    for (auto &r : cheats) {
        Value p;
        p.kind = Value::Array;
        p.items = {Value::text(r[0]), Value::text(r[1]), Value::text(r[2])};
        c.items.push_back(p);
    }
    t.set("CHEATS", c);
    return t;
}

// speech archive base name (sound1/.../<language>_ps4.psarc) -> language ids that archive provides audio for
const std::map<std::string, std::vector<const char *>> &speech_packs() {
    static const std::map<std::string, std::vector<const char *>> m = {
        {"english", {"1", "18"}}, {"french", {"2"}},     {"german", {"4"}},     {"italian", {"5"}},
        {"spanish", {"3", "20"}}, {"portuguese", {"7", "17"}}, {"dutch", {"6"}}, {"danish", {"14"}},
        {"norwegian", {"15"}},    {"swedish", {"13"}},   {"finnish", {"12"}},   {"polish", {"16"}},
        {"czech", {"23"}},        {"greek", {"25"}},     {"russian", {"8"}},    {"turkish", {"19"}},
        {"japanese", {"0"}},      {"korean", {"9"}},     {"chinese", {"10"}}};
    return m;
}

// ------------------------------------------------------------------------------------------------ game folder

bool game_folder(const std::wstring &f) {
    return !f.empty() && is_file(join(f, L"eboot.bin")) && is_file(join(f, L"sce_sys\\param.sfo"));
}

std::wstring update_folder(std::wstring folder) {
    std::replace(folder.begin(), folder.end(), L'/', L'\\');
    while (folder.size() > 3 && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();
    size_t cut = folder.find_last_of(L"\\/");
    std::wstring name = cut == std::wstring::npos ? folder : folder.substr(cut + 1);
    std::wstring title = (lower(name) == L"uroot" && cut != std::wstring::npos) ? folder.substr(0, cut) : folder;
    size_t tcut = title.find_last_of(L"\\/");
    if (tcut == std::wstring::npos) return std::wstring();
    std::wstring parent = title.substr(0, tcut), title_name = lower(title.substr(tcut + 1));
    if (parent.size() == 2 && parent[1] == L':') parent += L"\\";
    std::set<std::wstring> names;
    for (const wchar_t *suffix : {L"-patch", L"-update", L"_patch", L"_update"}) names.insert(title_name + suffix);
    for (const std::wstring &e : entries(parent, L"*", -1)) {
        if (!names.count(lower(e))) continue;
        std::wstring entry = join(parent, e);
        for (const std::wstring &candidate : {join(entry, L"uroot"), entry})
            if (game_folder(candidate)) return candidate;
    }
    return std::wstring();
}

// Two byte signatures inside Uncharted 2's executable: the torch routine in its 1.02 build, and the
// PlayerBase::UpdateFocus assert in its 1.00 build. Both are copied verbatim from the shipped binaries; the probe
// (tools/probe_uncharted.py) patches these same spots.
std::string hex(const char *h) {
    std::string out;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) out += (char)std::strtol(std::string(h + i, 2).c_str(), nullptr, 16);
    return out;
}
const std::string &code_102() { static const std::string c = hex("488d05cc9fd5008b485084c974408a50"); return c; }
const std::string &code_100() {
    static const std::string c = hex("c4e37904c900c5f0c2c900c5f850c183f80f7438e8a8d71a004885c07427488d3d4c9c8300"
                                     "488d354b998300488d155b9c8300488d0d7e99830041b84d0400004531c9ffd0cd41");
    return c;
}

// scans the folder's big2-ps4_Shipping.elf for both signatures -> {1.02 found, 1.00 found}; results are memoized
// per path and last-write time
std::pair<bool, bool> executable_codes(const std::wstring &folder) {
    static std::map<std::wstring, std::pair<bool, bool>> cache;
    std::wstring path = join(folder, L"big2-ps4_Shipping.elf");
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return {false, false};
    std::wstring key = path + L"|" + std::to_wstring(((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) |
                                                     a.ftLastWriteTime.dwLowDateTime);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::string data;
    std::pair<bool, bool> r{false, false};
    if (read_all(path, data)) r = {data.find(code_102()) != std::string::npos, data.find(code_100()) != std::string::npos};
    cache[key] = r;
    return r;
}

uint32_t u32(const std::string &d, size_t at) {
    return (uint32_t)(uint8_t)d[at] | ((uint32_t)(uint8_t)d[at + 1] << 8) | ((uint32_t)(uint8_t)d[at + 2] << 16) |
           ((uint32_t)(uint8_t)d[at + 3] << 24);
}

// Reads APP_VER from sce_sys/param.sfo. The executable overrides it when they disagree: a 1.02 binary under an older
// APP_VER reports "01.02", and a 1.00 binary under APP_VER 01.02 or later reports "01.00". Returns "" if unreadable.
std::string version_of(const std::wstring &folder, bool &found) {
    std::string d;
    found = false;
    if (!read_all(join(folder, L"sce_sys\\param.sfo"), d, 1 << 20) || d.size() < 20) return "";
    uint32_t keys = u32(d, 8), values = u32(d, 12), count = u32(d, 16);
    for (uint32_t i = 0; i < count; ++i) {
        size_t e = 20 + (size_t)i * 16;
        if (e + 16 > d.size()) return "";
        uint32_t key_at = (uint8_t)d[e] | ((uint8_t)d[e + 1] << 8), length = u32(d, e + 4), value_at = u32(d, e + 12);
        size_t k = (size_t)keys + key_at, end = d.find('\0', k);
        if (k >= d.size() || end == std::string::npos) return "";
        if (d.compare(k, end - k, "APP_VER") != 0) continue;
        size_t v = (size_t)values + value_at;
        std::string version = v >= d.size() ? "" : d.substr(v, std::min<size_t>(length, d.size() - v));
        while (!version.empty() && version.back() == '\0') version.pop_back();
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, version.data(), (int)version.size(), nullptr, 0) == 0 &&
            !version.empty())
            return "";
        found = true;
        if (version < "01.02" && executable_codes(folder).first) return "01.02";
        if (version >= "01.02" && executable_codes(folder).second) return "01.00";
        return version;
    }
    return "";
}

// language ids whose voice-over is installed, judged by the *_ps4.psarc files under
// u*data/build/main/sound1/[streams/]speech
std::vector<std::string> voices(const std::wstring &game_dir) {
    std::set<std::string> ids;
    if (game_dir.empty()) return {};
    for (const std::wstring &data : entries(game_dir, L"u*data", 1))
        for (const wchar_t *sub : {L"build\\main\\sound1\\speech", L"build\\main\\sound1\\streams\\speech"})
            for (const std::wstring &pack : entries(join(join(game_dir, data), sub), L"*_ps4.psarc", 0)) {
                std::string name = utf8(lower(pack));
                name = name.substr(0, name.size() - std::strlen("_ps4.psarc"));
                auto it = speech_packs().find(name);
                if (it != speech_packs().end()) for (const char *id : it->second) ids.insert(id);
            }
    return std::vector<std::string>(ids.begin(), ids.end());
}

// ------------------------------------------------------------------------------------------------ system

bool under_wine() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

std::string wine_unix(const std::wstring &path) {
    typedef char *(__cdecl * F)(LPCWSTR);
    F f = (F)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    if (!f) return "";
    char *p = f(path.c_str());
    if (!p) return "";
    std::string s = p;
    HeapFree(GetProcessHeap(), 0, p);
    return s;
}

std::wstring wine_windows(const std::string &path) {
    typedef wchar_t *(__cdecl * F)(LPCSTR);
    F f = (F)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name");
    if (!f || path.empty()) return L"";
    wchar_t *p = f(path.c_str());
    if (!p) return L"";
    std::wstring s = p;
    HeapFree(GetProcessHeap(), 0, p);
    return s;
}

std::wstring env(const wchar_t *name) {
    wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    return (n && n < 2048) ? std::wstring(buf, n) : std::wstring();
}

// Wine only: path of a launcher.json kept in the Linux user's XDG data directory, so every Steam shortcut prefix
// sees one common file
std::wstring shared_config() {
    std::wstring home = env(L"HOME"), data = env(L"XDG_DATA_HOME");
    if (data.empty()) data = (!home.empty() && home[0] == L'/') ? home + L"/.local/share" : L"";
    std::wstring folder = (!data.empty() && data[0] == L'/') ? wine_windows(utf8(data)) : L"";
    std::wstring winehome = env(L"WINEHOMEDIR");
    if (folder.empty() && winehome.rfind(L"\\??\\", 0) == 0) folder = join(winehome.substr(4), L".local\\share");
    return folder.empty() ? L"" : join(join(folder, L"uncharted-launcher"), L"launcher.json");
}

// Explains why the volume containing `path` is likely to stall the games (attached over USB, SD, MMC or FireWire;
// flagged removable; FAT or exFAT file system). Empty when none applies, and always empty under Wine.
std::string slow_drive(const std::wstring &path) {
    if (under_wine()) return "";
    wchar_t full[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(path.c_str(), MAX_PATH * 4, full, nullptr);
    std::wstring p = (n && n < MAX_PATH * 4) ? std::wstring(full, n) : path;
    HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {   // resolve junctions and SUBST drives to the volume that really holds the data
        wchar_t real[MAX_PATH * 4];
        DWORD r = GetFinalPathNameByHandleW(h, real, MAX_PATH * 4, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        CloseHandle(h);
        if (r && r < MAX_PATH * 4) {
            p = std::wstring(real, r);
            if (p.rfind(L"\\\\?\\", 0) == 0 && p.rfind(L"\\\\?\\UNC\\", 0) != 0) p = p.substr(4);
        }
    }
    if (p.size() < 2 || p[1] != L':') return "";
    std::wstring drive = p.substr(0, 2);
    std::vector<std::string> reasons;
    wchar_t fs[32];
    if (GetVolumeInformationW((drive + L"\\").c_str(), nullptr, 0, nullptr, nullptr, nullptr, fs, 32)) {
        std::wstring upper = fs;
        CharUpperBuffW(upper.data(), (DWORD)upper.size());
        if (upper == L"EXFAT" || upper == L"FAT32" || upper == L"FAT") reasons.push_back(utf8(fs) + "-formatted");
    }
    if (GetDriveTypeW((drive + L"\\").c_str()) == DRIVE_REMOVABLE) reasons.push_back("removable");
    HANDLE dev = CreateFileW((L"\\\\.\\" + drive).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                             nullptr);
    if (dev != INVALID_HANDLE_VALUE) {
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        char buf[1024];
        DWORD got = 0;
        if (DeviceIoControl(dev, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &got, nullptr) && got >= 32) {
            uint32_t bus;
            std::memcpy(&bus, buf + 28, 4);
            const char *name = bus == 7 ? "USB" : bus == 12 ? "SD card" : bus == 13 ? "MMC card" : bus == 4 ? "FireWire" : nullptr;
            if (name) reasons.insert(reasons.begin(), name);
        }
        CloseHandle(dev);
    }
    if (reasons.empty()) return "";
    std::string text;
    std::vector<std::string> seen;
    for (auto &r : reasons)
        if (std::find(seen.begin(), seen.end(), r) == seen.end()) {
            text += (seen.empty() ? "" : ", ") + r;
            seen.push_back(r);
        }
    return "drive " + utf8(drive) + " (" + text + ")";
}

std::string first_line_text(const std::wstring &path) {
    std::string d;
    if (!read_all(path, d, 1 << 16)) return "";
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, d.data(), (int)d.size(), nullptr, 0) == 0 && !d.empty()) return "";
    const char *ws = " \t\r\n\v\f";
    size_t a = d.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    return d.substr(a, d.find_last_not_of(ws) - a + 1);
}

std::wstring desktop() {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)) && p) {
        std::wstring s = p;
        CoTaskMemFree(p);
        if (is_dir(s)) return s;
    } else if (p) {
        CoTaskMemFree(p);
    }
    std::wstring profile = env(L"USERPROFILE");
    return join(profile.empty() ? L"." : profile, L"Desktop");
}

// JSON of the highest-versioned multiplayer build found as u3mp/patcher/build/v<version>/build.json; "" if there is none
std::string newest_build(const std::wstring &u3mp_dir) {
    std::wstring builds = join(u3mp_dir, L"build");
    std::vector<long long> best_key;
    std::string best;
    for (const std::wstring &d : entries(builds, L"v*", 1)) {
        std::string text;
        Value info;
        if (!read_all(join(join(builds, d), L"build.json"), text, 1 << 20) || !json::parse(text, info) || !info.is_object())
            continue;
        const Value *v = info.get("version");
        if (!v) continue;
        std::string version = v->kind == Value::String ? v->s : v->kind == Value::Int ? std::to_string(v->i) : json::dump(*v);
        std::vector<long long> key;
        bool ok = !version.empty();
        size_t start = 0;
        while (ok) {
            size_t dot = version.find('.', start);
            std::string part = version.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            char *end = nullptr;
            const char *ws = " \t";
            size_t a = part.find_first_not_of(ws), b = part.find_last_not_of(ws);
            part = a == std::string::npos ? "" : part.substr(a, b - a + 1);
            long long n = std::strtoll(part.c_str(), &end, 10);
            ok = !part.empty() && end == part.c_str() + part.size();
            key.push_back(n);
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        if (!ok) continue;
        if (best.empty() || key > best_key) { best_key = key; best = json::dump(info); }
    }
    return best;
}

// ------------------------------------------------------------------------------------------------ game starter

// returns the input with every byte that is not part of a valid UTF-8 sequence replaced by U+FFFD
std::string utf8_replace(const std::string &t) {
    std::string out;
    for (size_t i = 0; i < t.size();) {
        unsigned char c = (unsigned char)t[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = n > 0 && i + n <= t.size();
        for (int k = 1; ok && k < n; ++k) ok = ((unsigned char)t[i + k] >> 6) == 2;
        if (ok && n > 1 &&
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, t.data() + i, n, nullptr, 0) == 0)
            ok = false;
        if (ok) { out.append(t, i, n); i += n; }
        else { out += "\xEF\xBF\xBD"; ++i; }
    }
    return out;
}

// every param.sfo entry as a JSON object (integer-format entries become numbers, the rest strings), matching what
// gow3_pc/scripts/prepare.py extracts; false if the file is missing or malformed
bool sfo_values(const std::wstring &folder, Value &out) {
    std::string d;
    if (!read_all(join(folder, L"sce_sys\\param.sfo"), d, 1 << 24) || d.size() < 20 || d.compare(0, 4, std::string("\0PSF", 4)))
        return false;
    uint32_t keys = u32(d, 8), values = u32(d, 12), count = u32(d, 16);
    out = Value::object();
    for (uint32_t i = 0; i < count; ++i) {
        size_t e = 20 + (size_t)i * 16;
        if (e + 16 > d.size()) return false;
        uint32_t key_at = (uint8_t)d[e] | ((uint8_t)d[e + 1] << 8), fmt = (uint8_t)d[e + 2] | ((uint8_t)d[e + 3] << 8);
        uint32_t size = u32(d, e + 4), at = u32(d, e + 12);
        size_t k = (size_t)keys + key_at;
        if (k > d.size()) return false;
        size_t end = d.find('\0', k);
        if (end == std::string::npos) return false;
        std::string key = d.substr(k, end - k);
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, key.data(), (int)key.size(), nullptr, 0) == 0 && !key.empty())
            return false;
        size_t v = (size_t)values + at;
        if (v + size > d.size()) return false;
        std::string value = d.substr(v, size);
        if (fmt == 0x404) {
            if (size != 4) return false;
            out.set(key, Value::integer(u32(value, 0)));
        } else {
            while (!value.empty() && value.back() == '\0') value.pop_back();
            out.set(key, Value::text(utf8_replace(value)));
        }
    }
    return true;
}

// version as the game starter sees it: APP_VER, corrected by the Uncharted 2 executable signatures in the same way
// as version_of(); `found` stays false if param.sfo lacks APP_VER
std::string starter_version(const std::wstring &folder, bool &found) {
    Value sfo;
    found = false;
    if (!sfo_values(folder, sfo)) return "";
    const Value *v = sfo.get("APP_VER");
    if (!v || v->kind != Value::String) return "";
    found = true;
    std::string version = v->s;
    if (version.empty()) return version;
    std::pair<bool, bool> codes = executable_codes(folder);
    if (version < "01.02" && codes.first) return "01.02";
    if (version >= "01.02" && codes.second) return "01.00";
    return version;
}

// canonical path of an existing file or directory after following links and SUBST drives (the \\?\ prefix is
// stripped); the absolute input path if it cannot be opened
std::wstring final_path(const std::wstring &path) {
    wchar_t full[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(path.c_str(), MAX_PATH * 4, full, nullptr);
    std::wstring p = (n && n < MAX_PATH * 4) ? std::wstring(full, n) : path;
    HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return p;
    wchar_t real[MAX_PATH * 4];
    DWORD r = GetFinalPathNameByHandleW(h, real, MAX_PATH * 4, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (!r || r >= MAX_PATH * 4) return p;
    std::wstring f(real, r);
    if (f.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + f.substr(8);
    if (f.rfind(L"\\\\?\\", 0) == 0) return f.substr(4);
    return f;
}

// Locates the update for a game folder. UNCHARTED_UPDATE wins if set ("0" disables updates); otherwise a sibling
// folder whose name is the game folder's name plus -patch/-update/_patch/_update (case-insensitive) is used, either
// directly or through its uroot subfolder. Result is two lines: update path (may be empty), then a notice (may be
// empty).
std::string starter_update(std::wstring game) {
    std::wstring chosen = env(L"UNCHARTED_UPDATE");
    if (chosen == L"0") return "\n";
    std::vector<std::wstring> candidates;
    if (!chosen.empty()) {
        candidates = {chosen, join(chosen, L"uroot")};
    } else {
        std::replace(game.begin(), game.end(), L'/', L'\\');
        while (game.size() > 3 && game.back() == L'\\') game.pop_back();
        size_t cut = game.find_last_of(L'\\');
        std::wstring name = cut == std::wstring::npos ? game : game.substr(cut + 1);
        std::wstring title = (lower(name) == L"uroot" && cut != std::wstring::npos) ? game.substr(0, cut) : game;
        size_t tcut = title.find_last_of(L'\\');
        if (tcut != std::wstring::npos) {
            std::wstring parent = title.substr(0, tcut), title_name = lower(title.substr(tcut + 1));
            if (parent.size() == 2 && parent[1] == L':') parent += L"\\";
            std::set<std::wstring> names;
            for (const wchar_t *suffix : {L"-patch", L"-update", L"_patch", L"_update"}) names.insert(title_name + suffix);
            for (const std::wstring &e : entries(parent, L"*", -1))
                if (names.count(lower(e))) {
                    candidates.push_back(join(join(parent, e), L"uroot"));
                    candidates.push_back(join(parent, e));
                }
        }
    }
    for (const std::wstring &c : candidates)
        if (game_folder(c)) return utf8(final_path(c)) + "\n";
    if (!chosen.empty())
        return "\nUNCHARTED_UPDATE=" + utf8(chosen) + " is missing eboot.bin and sce_sys/param.sfo, so no update is used";
    return "\n";
}

}  // namespace

extern "C" {

UN_API int un_system_tables(char *out, int cap) { return give(json::dump(tables()), out, cap); }
UN_API int un_wine(void) { return under_wine() ? 1 : 0; }

// Dispatcher for small system lookups. `what` is one of unix_path, windows_path, shared_config, slow_drive,
// is_game_dir, update_dir, app_version, voices (one id per line), launcher_version (arg = install root),
// recipe_version (arg = u3mp dir), u3mp_release (arg = u3mp dir; build.json text or ""), desktop, sfo,
// starter_version, find_update, u3mp_default_data. An empty answer means "nothing".
UN_API int un_system(const char *what, const wchar_t *arg, char *out, int cap) {
    std::string w = what ? what : "", r;
    bool found = true;
    std::wstring a = arg ? arg : L"";
    if (w == "unix_path") r = under_wine() ? wine_unix(a) : "";
    else if (w == "windows_path") r = under_wine() ? utf8(wine_windows(utf8(a))) : "";
    else if (w == "shared_config") r = under_wine() ? utf8(shared_config()) : "";
    else if (w == "slow_drive") r = slow_drive(a);
    else if (w == "is_game_dir") r = game_folder(a) ? "1" : "";
    else if (w == "update_dir") r = utf8(update_folder(a));
    else if (w == "app_version") r = version_of(a, found);
    else if (w == "voices") { for (auto &id : voices(a)) r += id + "\n"; }
    else if (w == "launcher_version") {
        for (const wchar_t *f : {L"VERSION", L"tools\\VERSION"})
            if (r.empty()) r = first_line_text(join(a, f));
        if (r.empty()) r = "?";
    } else if (w == "recipe_version") {
        r = first_line_text(join(a, L"recipe\\VERSION"));
        if (r.empty()) r = "?";
    } else if (w == "u3mp_release") r = newest_build(a);
    else if (w == "desktop") r = utf8(desktop());
    else if (w == "sfo") {
        Value sfo;
        found = sfo_values(a, sfo);
        if (found) r = json::dump(sfo);
    } else if (w == "starter_version") r = starter_version(a, found);
    else if (w == "find_update") r = starter_update(a);
    else if (w == "u3mp_default_data") {
        std::wstring base = env(L"LOCALAPPDATA");
        if (base.empty()) base = env(L"APPDATA");
        if (base.empty()) base = env(L"USERPROFILE");
        r = utf8(join(join(base, L"Uncharted Launcher"), L"U3 Multiplayer"));
    }
    else return 1;
    if (!found) return 2;
    return give(r, out, cap);
}

UN_API int un_pid_alive(unsigned long pid) {
    if (!pid) return 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return ok && code == STILL_ACTIVE ? 1 : 0;
}

}  // extern "C"
