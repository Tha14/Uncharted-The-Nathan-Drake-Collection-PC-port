// gamefiles.cpp (in uncharted_native.dll) backs the "Check game files" button under Advanced -> FOLDERS. It compares
// what is in the player's game folder with tools/game_files_102.json.gz, a listing of the base game of the Collection
// that maps each relative path to its byte size. A size of -1 means "only check that it exists": the regions disagree
// on it, or update 1.02 ships its own copy. The listing carries file names and sizes, never game data, and skips
// sce_sys. Background: players whose dump was short one script file got stuck in chapter 8 of Uncharted 3 (#247, #296).

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
// Normalises a path the way str(Path(p)) does: forward slashes turned into backslashes, runs of separators collapsed
// (a leading UNC pair survives), no separator at the end
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

// Size of the file behind `path`, following links like Path.stat. Returns false if it can't be opened (e.g. absent).
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

// Runs the comparison and writes a report to out. Its first line reads "<folder>: N files compared, M not found, K with
// a different size"; after it come one "missing <file>" line per absent file and one "size    <file>" line per size
// mismatch. If the game folder has a uroot subfolder, that subfolder is what gets checked. Returns 0 when everything
// matches, 1 when something is absent or the wrong size, and 2 when the listing itself is unusable (out says why).
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
    const std::string report = utf8(folder) + ": " + std::to_string(wanted.fields.size()) + " files compared, " +
                               std::to_string(missing_count) + " not found, " + std::to_string(wrong_count) +
                               " with a different size\n" + missing + wrong;
    const int status = give(report, out, cap);
    if (status < 0) return status;
    return missing_count || wrong_count ? 1 : 0;
}

}  // extern "C"
