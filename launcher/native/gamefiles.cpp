// gamefiles.cpp - Check game files (launcher: Advanced -> FOLDERS), part of uncharted_native.dll: the player's game
// folder against the list of every file of the Collection's base game (tools/game_files_102.json.gz: relative path ->
// size, or -1 where regions differ or update 1.02 replaces the file; names and sizes only, no game data; sce_sys left
// out). Why: Uncharted 3 chapter 8 stopped for players whose dump lacked one script file (#247, #296).

#include "native.h"
#include "json.h"
#include "inflate.h"

#include <cstdint>
#include <cstring>
#include <windows.h>

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
// str(Path(p)): / as \, no doubled or trailing separators
std::wstring path_str(std::wstring p) {
    for (auto &c : p) if (c == L'/') c = L'\\';
    std::wstring o;
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == L'\\' && !o.empty() && o.back() == L'\\' && i > 1) continue;
        o += p[i];
    }
    while (o.size() > 3 && o.back() == L'\\') o.pop_back();
    return o;
}

bool read_file(const std::wstring &path, std::string &data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    data.assign((size_t)size.QuadPart, '\0');
    DWORD got = 0;
    const bool ok = size.QuadPart == 0 || (ReadFile(h, &data[0], (DWORD)data.size(), &got, nullptr) && got == data.size());
    CloseHandle(h);
    return ok;
}

// The file's size with links followed (Path.stat); false when it can't be read (missing).
bool size_of(const std::wstring &path, long long &size) {
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILE_STANDARD_INFO info{};
    const bool ok = GetFileInformationByHandleEx(h, FileStandardInfo, &info, sizeof info) != 0;
    CloseHandle(h);
    size = info.Directory ? 0 : info.EndOfFile.QuadPart;
    return ok;
}

}  // namespace

extern "C" {

// The game folder checked against the list: out is the report (first line "<folder>: N files checked, M missing,
// K with a wrong size", then "missing <file>" and "size    <file>" lines). The folder is the game's uroot when it has
// one. 0 every file is there with its size; 1 some are missing or wrong; 2 the list could not be read (out: why).
UN_API int un_check_game_files(const wchar_t *root, const wchar_t *game, char *out, int cap) {
    const std::wstring list_path = std::wstring(root ? root : L".") + L"\\tools\\game_files_102.json.gz";
    std::string gz, text;
    Value wanted;
    if (!read_file(list_path, gz)) return give("The game file list is missing: " + utf8(list_path), out, cap) < 0 ? -1 : 2;
    try {
        text = inflate::gunzip(gz);
    } catch (const inflate::InflateError &e) {
        return give("The game file list can't be read: " + e.message, out, cap) < 0 ? -1 : 2;
    }
    if (!json::parse(text, wanted) || wanted.kind != Value::Object)
        return give("The game file list can't be read.", out, cap) < 0 ? -1 : 2;
    std::wstring folder = path_str(game ? game : L"");
    long long ignored = 0;
    if (size_of(folder + L"\\uroot\\eboot.bin", ignored)) folder += L"\\uroot";
    std::string missing, wrong;
    size_t missing_count = 0, wrong_count = 0;
    for (const auto &entry : wanted.fields) {
        std::wstring rel = widen(entry.first);
        for (auto &c : rel) if (c == L'/') c = L'\\';
        long long actual = 0;
        if (!size_of(folder + L"\\" + rel, actual)) {
            missing += "missing " + entry.first + "\n";
            ++missing_count;
            continue;
        }
        const long long size = entry.second.kind == Value::Int ? entry.second.i : -1;
        if (size >= 0 && actual != size) {
            wrong += "size    " + entry.first + "\n";
            ++wrong_count;
        }
    }
    const std::string report = utf8(folder) + ": " + std::to_string(wanted.fields.size()) + " files checked, " +
                               std::to_string(missing_count) + " missing, " + std::to_string(wrong_count) +
                               " with a wrong size\n" + missing + wrong;
    const int status = give(report, out, cap);
    if (status < 0) return status;
    return missing_count || wrong_count ? 1 : 0;
}

}  // extern "C"
