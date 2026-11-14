// updater.cpp (in uncharted_native.dll) keeps the launcher itself up to date from the project's releases on GitHub;
// it took over from launcher/updater.py.
//
// The installed version is read from the VERSION file that tools/package_windows.sh puts in every package. Running
// straight from a source checkout means there is no VERSION file, and then no update is ever offered. The update runs
// in four stages:
//   1. latest       pick the highest release above the installed version that carries an Uncharted-windows.zip asset
//                   with a published SHA-256 (GitHub's "digest" field on the asset)
//   2. download     fetch that zip into update\ and verify its size and SHA-256; when the connection hangs or drops,
//                   reconnect and resume from the last byte received (HTTP Range)
//   3. stage        unzip it into update\staged and verify it (required files present, VERSION matches)
//   4. start_apply  launch the installer that ships inside the new version (tools\apply_update.exe),
//                   which holds off until this launcher has exited, exchanges the program folders with rollback on failure,
//                   and starts the new launcher
// analysis\ (saves, settings, logs, shader cache) is never modified. Setting UNCHARTED_UPDATE_API makes the check read
// a different releases list, for tests; plain http is then accepted for 127.0.0.1 only.

#include "native.h"
#include "json.h"
#include "inflate.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using json::Value;

const char *const REPOSITORY = "fidalgojoao/Uncharted-The-Nathan-Drake-Collection-PC-port";
const char *const ASSET = "Uncharted-windows.zip";
// Files a downloaded package needs before it may replace this install: the launcher exe, its native DLL, the runtime
// and the package's own update installer. None of these is Python, so packages that no longer ship Python qualify.
const char *const REQUIRED[] = {"Uncharted Launcher.exe", "VERSION", "launcher/native/uncharted_native.dll",
                                "tools/apply_update.exe", "gow3_pc/out/gow3-probe.exe"};

struct UpdateError { std::string message; };

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
std::wstring widen(const std::string &s, UINT page = CP_UTF8) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(page, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(page, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::wstring env(const wchar_t *name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return L"";
    std::wstring v(n, L'\0');
    n = GetEnvironmentVariableW(name, &v[0], n);
    v.resize(n);
    return v;
}
std::wstring join(const std::wstring &a, const std::wstring &b) {
    if (a.empty()) return b;
    return (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b;
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

// ------------------------------------------------------------------------------------------------ version numbers

// Parses "v0.1.10" or "0.1.10" into {0, 1, 10}. Anything that is not exactly three numbers gives an empty vector.
std::vector<long long> parse_version(const std::string &text) {
    std::string t = text;
    while (!t.empty() && isspace((unsigned char)t.front())) t.erase(t.begin());
    while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back();
    if (!t.empty() && t[0] == 'v') t.erase(t.begin());
    std::vector<long long> parts;
    size_t at = 0;
    for (int k = 0; k < 3; ++k) {
        size_t end = at;
        while (end < t.size() && isdigit((unsigned char)t[end])) ++end;
        if (end == at || end - at > 18) return {};
        parts.push_back(std::stoll(t.substr(at, end - at)));
        if (k < 2) {
            if (end >= t.size() || t[end] != '.') return {};
            at = end + 1;
        } else if (end != t.size()) {
            return {};
        }
    }
    return parts;
}
std::string version_text(const std::vector<long long> &v) {
    std::string s;
    for (size_t k = 0; k < v.size(); ++k) s += (k ? "." : "") + std::to_string(v[k]);
    return s;
}

// Version from the root's VERSION file; empty when the file is absent (a source checkout).
std::vector<long long> installed_version(const std::wstring &root) {
    HANDLE h = CreateFileW(join(root, L"VERSION").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    char buf[256];
    DWORD got = 0;
    ReadFile(h, buf, sizeof buf - 1, &got, nullptr);
    CloseHandle(h);
    return parse_version(std::string(buf, got));
}

// ------------------------------------------------------------------------------------------------ HTTPS (WinHTTP)

bool allowed_url(const std::wstring &url) {
    URL_COMPONENTS u{};
    u.dwStructSize = sizeof u;
    wchar_t host[256];
    u.lpszHostName = host;
    u.dwHostNameLength = 256;
    u.dwSchemeLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) return false;
    if (u.nScheme == INTERNET_SCHEME_HTTPS) return true;
    return u.nScheme == INTERNET_SCHEME_HTTP && std::wstring(host, u.dwHostNameLength) == L"127.0.0.1" &&
           !env(L"UNCHARTED_UPDATE_API").empty();
}

std::string http_error_text(DWORD code) {   // connection failures, mostly phrased as urllib reported them
    if (code == ERROR_WINHTTP_TIMEOUT) return "No data arrived in time (timeout)";
    if (code == ERROR_WINHTTP_NAME_NOT_RESOLVED) return "<urlopen error [Errno 11001] getaddrinfo failed>";
    if (code == ERROR_WINHTTP_CANNOT_CONNECT) return "<urlopen error [WinError 10061] No connection could be made>";
    if (code == ERROR_WINHTTP_CONNECTION_ERROR) return "[WinError 10054] An existing connection was forcibly closed by the remote host";
    return "<urlopen error " + win_error(code) + ">";
}

struct Http {
    HINTERNET session = nullptr, connect = nullptr, request = nullptr;
    DWORD status = 0;
    ~Http() {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
    }
    // Sends a GET for `url` with the given extra headers and leaves the final HTTP status in `status`. Failures are
    // thrown as a std::string describing them.
    void open(const std::wstring &url, const std::wstring &headers, int timeout_s) {
        URL_COMPONENTS u{};
        u.dwStructSize = sizeof u;
        u.dwHostNameLength = u.dwUrlPathLength = u.dwExtraInfoLength = (DWORD)-1;
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) throw std::string("unknown url type: '" + utf8(url) + "'");
        const std::wstring host(u.lpszHostName, u.dwHostNameLength);
        const std::wstring path = std::wstring(u.lpszUrlPath, u.dwUrlPathLength) + std::wstring(u.lpszExtraInfo, u.dwExtraInfoLength);
        session = WinHttpOpen(L"Uncharted-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            session = WinHttpOpen(L"Uncharted-Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) throw http_error_text(GetLastError());
        const int ms = timeout_s * 1000;
        WinHttpSetTimeouts(session, ms, ms, ms, ms);
        connect = WinHttpConnect(session, host.c_str(), u.nPort, 0);
        if (!connect) throw http_error_text(GetLastError());
        request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!request) throw http_error_text(GetLastError());
        if (!WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                headers.empty() ? 0 : (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr))
            throw http_error_text(GetLastError());
        DWORD size = sizeof status;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status >= 400) {
            wchar_t reason[256] = L"";
            DWORD rsize = sizeof reason;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_TEXT, WINHTTP_HEADER_NAME_BY_INDEX, reason, &rsize,
                                WINHTTP_NO_HEADER_INDEX);
            throw "HTTP Error " + std::to_string(status) + ": " + utf8(reason);
        }
    }
    // Reads the next chunk of the response body, returning 0 once it is exhausted; failures are thrown as text
    DWORD read(char *buf, DWORD cap) {
        DWORD got = 0;
        if (!WinHttpReadData(request, buf, cap, &got)) throw http_error_text(GetLastError());
        return got;
    }
};

std::wstring api_url() {
    const std::wstring custom = env(L"UNCHARTED_UPDATE_API");
    if (!custom.empty()) return custom;
    return widen(std::string("https://api.github.com/repos/") + REPOSITORY + "/releases?per_page=20");
}

Value fetch_json(const std::wstring &url, int timeout) {
    if (!allowed_url(url)) throw UpdateError{"Not an HTTPS address, so it was refused: " + utf8(url)};
    Http http;
    std::string body;
    try {
        http.open(url, L"Accept: application/vnd.github+json\r\n", timeout);
        char buf[1 << 16];
        while (DWORD got = http.read(buf, sizeof buf)) body.append(buf, got);
    } catch (const std::string &error) {
        throw UpdateError{error};
    }
    Value v;
    if (!json::parse(body, v)) throw UpdateError{"Expecting value: line 1 column 1 (char 0)"};
    return v;
}

// Returns the highest release above `current` that has a usable package, as an object with version, tag, page, notes,
// url, size and sha256; null if there is none. Pre-releases are included, since all releases to date are betas.
Value latest_release(const std::vector<long long> &current, int timeout) {
    Value best;
    std::vector<long long> best_version;
    const Value list = fetch_json(api_url(), timeout);
    for (const Value &release : list.items) {
        const Value *tag = release.get("tag_name");
        const std::vector<long long> version = parse_version(tag && tag->kind == Value::String ? tag->s : "");
        const Value *draft = release.get("draft");
        if ((draft && draft->truthy()) || version.empty() || version <= current) continue;
        if (!best_version.empty() && version <= best_version) continue;
        const Value *asset = nullptr;
        if (const Value *assets = release.get("assets"))
            for (const Value &a : assets->items) {
                const Value *name = a.get("name");
                if (name && name->kind == Value::String && name->s == ASSET) { asset = &a; break; }
            }
        const Value *digest = asset ? asset->get("digest") : nullptr;
        const std::string digest_text = digest && digest->kind == Value::String ? digest->s : "";
        if (!asset || digest_text.compare(0, 7, "sha256:") != 0) continue;
        best = Value::object();
        Value v;
        v.kind = Value::Array;
        for (long long p : version) v.items.push_back(Value::integer(p));
        best.set("version", v);
        best.set("tag", *tag);
        const Value *page = release.get("html_url");
        best.set("page", page ? *page : Value::text(std::string("https://github.com/") + REPOSITORY + "/releases"));
        const Value *notes = release.get("body");
        best.set("notes", notes && notes->truthy() ? *notes : Value::text(""));
        const Value *url = asset->get("browser_download_url");
        best.set("url", url ? *url : Value::null());
        const Value *size = asset->get("size");
        best.set("size", Value::integer(size && size->kind == Value::Int ? size->i : 0));
        std::string sha = digest_text.substr(7);
        std::transform(sha.begin(), sha.end(), sha.begin(), [](unsigned char c) { return (char)tolower(c); });
        best.set("sha256", Value::text(sha));
        best_version = version;
    }
    return best;
}

// ------------------------------------------------------------------------------------------------ downloading

std::atomic<long long> g_done{0}, g_total{0};

struct Sha256 {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    Sha256() { reset(); }
    ~Sha256() { close(); }
    void close() {
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        hash = nullptr;
        alg = nullptr;
    }
    void reset() {
        close();
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    }
    void update(const char *data, size_t size) { BCryptHashData(hash, (PUCHAR)data, (ULONG)size, 0); }
    std::string hex() {
        unsigned char d[32];
        BCryptFinishHash(hash, d, 32, 0);
        static const char *x = "0123456789abcdef";
        std::string o;
        for (unsigned char c : d) { o += x[c >> 4]; o += x[c & 15]; }
        return o;
    }
};

// Saves the release's zip in update\ and verifies its size and SHA-256. If the connection hangs or drops, a new one is
// made and the transfer resumes at the byte where it stopped (HTTP Range). It gives up after `attempts` consecutive
// tries that each moved less than 1 MiB.
std::wstring download(const Value &release, const std::wstring &root, int attempts = 8, int timeout = 60, double pause = 2.0) {
    const Value *url_v = release.get("url");
    const std::wstring url = widen(url_v && url_v->kind == Value::String ? url_v->s : "");
    if (!allowed_url(url)) throw UpdateError{"Not an HTTPS download, so it was refused: " + utf8(url)};
    const std::wstring folder = join(root, L"update");
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        throw UpdateError{win_error(GetLastError()) + ": '" + utf8(folder) + "'"};
    const std::wstring target = join(folder, widen(ASSET)), partial = target + L".part";
    const Value *size_v = release.get("size");
    const long long size = size_v && size_v->kind == Value::Int ? size_v->i : 0;
    const Value *sha_v = release.get("sha256");
    const std::string want = sha_v && sha_v->kind == Value::String ? sha_v->s : "";
    HANDLE out = CreateFileW(partial.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) throw UpdateError{win_error(GetLastError()) + ": '" + utf8(partial) + "'"};
    Sha256 digest;
    long long done = 0;
    int failures = 0;
    g_done = 0;
    g_total = size;
    std::vector<char> buf(1 << 16);
    try {
        while (!size || done < size) {
            std::wstring headers;
            if (done) headers = L"Range: bytes=" + std::to_wstring(done) + L"-\r\n";
            const long long start = done;
            try {
                // Start every try at github.com again, because the signed redirect it hands out to the file host only
                // stays valid for a limited time.
                Http http;
                http.open(url, headers, timeout);
                if (done && http.status != 206) {   // server sent the whole file instead of the range: start over
                    SetFilePointer(out, 0, nullptr, FILE_BEGIN);
                    SetEndOfFile(out);
                    digest.reset();
                    done = 0;
                }
                while (DWORD got = http.read(buf.data(), (DWORD)buf.size())) {
                    DWORD put = 0;
                    if (!WriteFile(out, buf.data(), got, &put, nullptr) || put != got)
                        throw UpdateError{win_error(GetLastError()) + ": '" + utf8(partial) + "'"};
                    digest.update(buf.data(), got);
                    done += got;
                    g_done = done;
                }
                if (!size) break;
                if (done < size)
                    throw std::string("the connection ended after " + std::to_string(done) + " of " + std::to_string(size) +
                                      " bytes");
            } catch (const std::string &error) {   // time-outs, dropped connections and HTTP error statuses land here
                failures = done - start >= (1 << 20) ? 0 : failures + 1;
                if (failures >= attempts)
                    throw UpdateError{"The download from GitHub keeps breaking off (" + error + ").\n\nPress UPDATE again "
                                      "later, or get Uncharted-windows.zip from the releases page yourself and unpack it "
                                      "over this folder (the saves and settings in analysis\\ stay)."};
                Sleep((DWORD)(std::min(pause * std::max(failures, 1), 15.0) * 1000));
            }
        }
    } catch (...) {
        CloseHandle(out);
        throw;
    }
    CloseHandle(out);
    if (size && done != size) {
        DeleteFileW(partial.c_str());
        throw UpdateError{"Only part of the download arrived (" + std::to_string(done) + " of " + std::to_string(size) + " bytes)."};
    }
    if (digest.hex() != want) {
        DeleteFileW(partial.c_str());
        throw UpdateError{"The downloaded file's SHA-256 differs from the release's, so it was not installed."};
    }
    if (!MoveFileExW(partial.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
        throw UpdateError{win_error(GetLastError()) + ": '" + utf8(partial) + "' -> '" + utf8(target) + "'"};
    return target;
}

// ------------------------------------------------------------------------------------------------ zip reading (inflate)

using inflate::crc32;
using inflate::Inflater;

struct Entry {
    std::string name;   // decoded name, '/' between folders (what Python's zipfile reports)
    std::string raw;    // undecoded name bytes as stored in the central directory
    uint16_t flags = 0, method = 0;
    uint32_t crc = 0;
    uint64_t csize = 0, usize = 0, offset = 0;
};

uint64_t le(const std::string &d, size_t at, int n) {
    if (at + n > d.size()) throw UpdateError{"File is not a zip file"};
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= (uint64_t)(uint8_t)d[at + i] << (8 * i);
    return v;
}

std::vector<Entry> central_directory(const std::string &zip) {
    // search backwards for the end-of-directory record; a trailing comment can be up to 64 KiB long
    size_t end = std::string::npos;
    for (size_t k = zip.size() >= 22 ? zip.size() - 22 : 0; zip.size() >= 22; --k) {
        if (le(zip, k, 4) == 0x06054b50) { end = k; break; }
        if (k == 0 || zip.size() - k > 22 + 65535) break;
    }
    if (end == std::string::npos) throw UpdateError{"File is not a zip file"};
    uint64_t count = le(zip, end + 10, 2), dir_size = le(zip, end + 12, 4), dir_at = le(zip, end + 16, 4);
    if ((count == 0xffff || dir_at == 0xffffffff || dir_size == 0xffffffff) && end >= 20 && le(zip, end - 20, 4) == 0x07064b50) {
        const uint64_t at64 = le(zip, end - 20 + 8, 8);
        if (le(zip, (size_t)at64, 4) != 0x06064b50) throw UpdateError{"File is not a zip file"};
        count = le(zip, (size_t)at64 + 32, 8);
        dir_size = le(zip, (size_t)at64 + 40, 8);
        dir_at = le(zip, (size_t)at64 + 48, 8);
    }
    std::vector<Entry> entries;
    size_t at = (size_t)dir_at;
    for (uint64_t n = 0; n < count; ++n) {
        if (le(zip, at, 4) != 0x02014b50) throw UpdateError{"Bad magic number for central directory"};
        Entry e;
        e.flags = (uint16_t)le(zip, at + 8, 2);
        e.method = (uint16_t)le(zip, at + 10, 2);
        e.crc = (uint32_t)le(zip, at + 16, 4);
        e.csize = le(zip, at + 20, 4);
        e.usize = le(zip, at + 24, 4);
        const size_t nlen = (size_t)le(zip, at + 28, 2), xlen = (size_t)le(zip, at + 30, 2), clen = (size_t)le(zip, at + 32, 2);
        e.offset = le(zip, at + 42, 4);
        if (at + 46 + nlen > zip.size()) throw UpdateError{"File is not a zip file"};
        std::string raw = zip.substr(at + 46, nlen);
        e.raw = raw;
        // decode like zipfile: UTF-8 if bit 11 is set, code page 437 otherwise; cut at a NUL; backslashes become '/'
        std::wstring name = (e.flags & 0x800) ? widen(raw) : widen(raw, 437);
        const size_t nul = name.find(L'\0');
        if (nul != std::wstring::npos) name.resize(nul);
        std::replace(name.begin(), name.end(), L'\\', L'/');
        e.name = utf8(name);
        // a ZIP64 extra field (id 1) supplies the sizes and offset that did not fit in 32 bits
        for (size_t x = at + 46 + nlen; x + 4 <= at + 46 + nlen + xlen;) {
            const uint64_t id = le(zip, x, 2), len = le(zip, x + 2, 2);
            if (id == 1) {
                size_t p = x + 4;
                if (e.usize == 0xffffffff) { e.usize = le(zip, p, 8); p += 8; }
                if (e.csize == 0xffffffff) { e.csize = le(zip, p, 8); p += 8; }
                if (e.offset == 0xffffffff) { e.offset = le(zip, p, 8); }
            }
            x += 4 + (size_t)len;
        }
        entries.push_back(e);
        at += 46 + nlen + xlen + clen;
    }
    return entries;
}

std::string member(const std::string &zip, const Entry &e) {
    if (e.flags & 1) throw UpdateError{"File '" + e.name + "' is encrypted, password required for extraction"};
    if (le(zip, (size_t)e.offset, 4) != 0x04034b50) throw UpdateError{"Bad magic number for file header"};
    const size_t local_name = (size_t)le(zip, (size_t)e.offset + 26, 2);
    const size_t start = (size_t)e.offset + 30 + local_name + (size_t)le(zip, (size_t)e.offset + 28, 2);
    if ((size_t)e.offset + 30 + local_name > zip.size()) throw UpdateError{"File is not a zip file"};
    // the local header's name must equal the central directory's, a check zipfile makes as well
    const std::string header_name = zip.substr((size_t)e.offset + 30, local_name);
    if (header_name != e.raw)
        throw UpdateError{"File name in directory '" + e.name + "' and header b'" + header_name + "' differ."};
    if (start + e.csize > zip.size()) throw UpdateError{"File is not a zip file"};
    std::string data;
    if (e.method == 0) {
        data = zip.substr(start, (size_t)e.csize);
    } else if (e.method == 8) {
        data.reserve((size_t)e.usize);
        try {
            Inflater(reinterpret_cast<const unsigned char *>(zip.data()) + start, (size_t)e.csize, data).run();
        } catch (const inflate::InflateError &error) {
            throw UpdateError{error.message};
        }
    } else {
        throw UpdateError{"That compression method is not supported"};
    }
    if (data.size() != e.usize || crc32(0, reinterpret_cast<const unsigned char *>(data.data()), data.size()) != e.crc)
        throw UpdateError{"Bad CRC-32 for file '" + e.name + "'"};
    return data;
}

bool read_all(const std::wstring &path, std::string &data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    data.assign((size_t)size.QuadPart, '\0');
    size_t done = 0;
    bool ok = true;
    while (ok && done < data.size()) {
        DWORD got = 0;
        ok = ReadFile(h, &data[done], (DWORD)std::min<size_t>(data.size() - done, 1 << 30), &got, nullptr) && got;
        done += got;
    }
    CloseHandle(h);
    return ok;
}

void make_dirs(const std::wstring &p) {
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    const size_t cut = p.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 2) make_dirs(p.substr(0, cut));
    CreateDirectoryW(p.c_str(), nullptr);
}

std::wstring full_path(const std::wstring &p) {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetFullPathNameW(p.c_str(), 32768, buf.data(), nullptr);
    std::wstring f = n && n < 32768 ? std::wstring(buf.data(), n) : p;
    while (f.size() > 3 && (f.back() == L'\\' || f.back() == L'/')) f.pop_back();
    return f;
}

// Deletes a tree, ignoring failures (as shutil.rmtree(ignore_errors=True)); links go, their targets are not entered
void remove_tree(const std::wstring &path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if ((a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(join(path, L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring n = fd.cFileName;
                if (n != L"." && n != L"..") remove_tree(join(path, n));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (a & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(path.c_str());
    else DeleteFileW(path.c_str());
}

// Extracts the downloaded package into update\staged, verifies the result and returns the folder's path.
std::wstring stage(const std::wstring &archive, const std::wstring &root, const std::vector<long long> &version) {
    const std::wstring staged = join(join(root, L"update"), L"staged");
    remove_tree(staged);
    make_dirs(staged);
    std::string zip;
    if (!read_all(archive, zip)) throw UpdateError{win_error(GetLastError()) + ": '" + utf8(archive) + "'"};
    const std::vector<Entry> entries = central_directory(zip);
    // Zips made with Compress-Archive wrap all files in a single top folder (Uncharted-windows/); strip it if present.
    std::vector<std::string> names;
    for (auto &e : entries) if (!e.name.empty() && e.name.back() != '/') names.push_back(e.name);
    std::vector<std::string> tops;
    bool all_nested = true;
    for (auto &n : names) {
        const std::string top = n.substr(0, n.find('/'));
        if (std::find(tops.begin(), tops.end(), top) == tops.end()) tops.push_back(top);
        all_nested = all_nested && n.find('/') != std::string::npos;
    }
    const std::string prefix = tops.size() == 1 && all_nested ? tops[0] + "/" : "";
    const std::wstring base = full_path(staged);
    for (const Entry &e : entries) {
        const std::string &name = e.name;
        if ((!name.empty() && name.back() == '/') || name.compare(0, prefix.size(), prefix) != 0) continue;
        std::wstring rel = widen(name.substr(prefix.size()));
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        std::wstring joined;
        if (rel.size() >= 2 && rel[1] == L':') joined = rel;                                  // names a drive itself
        else if (!rel.empty() && rel[0] == L'\\') joined = base.substr(0, 2) + rel;          // rooted on base's drive
        else joined = base + L"\\" + rel;
        const std::wstring target = full_path(joined);
        if (!(target.size() > base.size() + 1 && CompareStringOrdinal(target.c_str(), (int)base.size(), base.c_str(),
                                                                         (int)base.size(), TRUE) == CSTR_EQUAL &&
              target[base.size()] == L'\\'))
            throw UpdateError{"The package contains a path that leads outside its folder: " + name};
        const size_t cut = target.find_last_of(L'\\');
        make_dirs(target.substr(0, cut));
        const std::string data = member(zip, e);
        HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) throw UpdateError{win_error(GetLastError()) + ": '" + utf8(target) + "'"};
        size_t done = 0;
        bool ok = true;
        while (ok && done < data.size()) {
            DWORD put = 0;
            ok = WriteFile(h, data.data() + done, (DWORD)std::min<size_t>(data.size() - done, 1 << 30), &put, nullptr) && put;
            done += put;
        }
        CloseHandle(h);
        if (!ok) throw UpdateError{win_error(GetLastError()) + ": '" + utf8(target) + "'"};
    }
    std::string missing;
    for (const char *path : REQUIRED) {
        std::wstring p = widen(path);
        std::replace(p.begin(), p.end(), L'/', L'\\');
        const DWORD a = GetFileAttributesW(join(staged, p).c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) missing += (missing.empty() ? "" : ", ") + std::string(path);
    }
    if (!missing.empty()) throw UpdateError{"Files are missing from the package: " + missing};
    if (installed_version(staged) != version) throw UpdateError{"The VERSION in the package differs from the release's."};
    return staged;
}

// Joins arguments into one command line with the quoting rules of subprocess.list2cmdline
std::wstring command_line(const std::vector<std::wstring> &args) {
    std::wstring result;
    for (const std::wstring &arg : args) {
        std::wstring bs;
        if (!result.empty()) result += L' ';
        const bool quote = arg.find(L' ') != std::wstring::npos || arg.find(L'\t') != std::wstring::npos || arg.empty();
        if (quote) result += L'"';
        for (wchar_t c : arg) {
            if (c == L'\\') bs += c;
            else if (c == L'"') { result += std::wstring(bs.size() * 2, L'\\'); bs.clear(); result += L"\\\""; }
            else { result += bs; bs.clear(); result += c; }
        }
        result += bs;
        if (quote) { result += bs; result += L'"'; }
    }
    return result;
}

// Launches the installer of the staged version, its tools\apply_update.exe (stage checks that it is there: REQUIRED),
// which waits for this process to end.
void start_apply(const std::wstring &root, const std::wstring &staged) {
    const std::wstring installer = join(join(staged, L"tools"), L"apply_update.exe");
    const std::wstring cmd = command_line({installer, L"--root", root, L"--wait-pid", std::to_wstring(GetCurrentProcessId())});
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr,
                        staged.c_str(), &si, &pi))
        throw UpdateError{win_error(GetLastError())};
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

// Deletes the leftovers of a finished or aborted update. Failures are ignored, since some files may still be open.
void cleanup(const std::wstring &root) {
    const std::wstring folder = join(root, L"update");
    if (GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    for (const std::wstring &name : {std::wstring(L"old"), std::wstring(L"staged"), widen(ASSET), widen(ASSET) + L".part"})
        remove_tree(join(folder, name));
    RemoveDirectoryW(folder.c_str());
}

std::vector<long long> version_of(const char *text) { return parse_version(text ? text : ""); }

}  // namespace

extern "C" {

// Writes the installed version, such as "0.10.62", to out. Returns 2 when there is no VERSION (a source checkout).
UN_API int un_update_installed(const wchar_t *root, char *out, int cap) {
    const std::vector<long long> v = installed_version(root ? root : L".");
    if (v.empty()) return give("", out, cap) < 0 ? -1 : 2;
    return give(version_text(v), out, cap);
}

// Looks up the best release above `current` (e.g. "0.10.62") and writes it to out as JSON with version (a list), tag,
// page, notes, url, size and sha256, or writes "null" when there is nothing newer. Returns 1 with the reason in out
// if the lookup failed.
UN_API int un_update_latest(const char *current, int timeout, char *out, int cap) {
    try {
        return give(json::dump(latest_release(version_of(current), timeout > 0 ? timeout : 15), false), out, cap);
    } catch (const UpdateError &e) {
        int status = give(e.message, out, cap);
        return status < 0 ? status : 1;
    }
}

// Fetches the release described by release_json (the object un_update_latest produced) into root\update and writes
// the zip's path to out. The call blocks until done; poll un_update_progress meanwhile. Returns 1 with the reason in
// out on failure.
UN_API int un_update_download(const char *release_json, const wchar_t *root, char *out, int cap) {
    Value release;
    json::parse(release_json ? release_json : "{}", release);
    try {
        return give(utf8(download(release, root ? root : L".")), out, cap);
    } catch (const UpdateError &e) {
        int status = give(e.message, out, cap);
        return status < 0 ? status : 1;
    }
}

// Progress of the current download: bytes received so far and the expected total (0 when not known).
UN_API void un_update_progress(long long *done, long long *total) {
    if (done) *done = g_done;
    if (total) *total = g_total;
}

// Extracts the downloaded zip into root\update\staged, checks it against `version` (e.g. "0.10.63") and writes the
// folder to out. Returns 1 with the reason in out when the package is unusable.
UN_API int un_update_stage(const wchar_t *archive, const wchar_t *root, const char *version, char *out, int cap) {
    try {
        return give(utf8(stage(archive ? archive : L"", root ? root : L".", version_of(version))), out, cap);
    } catch (const UpdateError &e) {
        int status = give(e.message, out, cap);
        return status < 0 ? status : 1;
    } catch (const std::bad_alloc &) {
        int status = give("out of memory", out, cap);
        return status < 0 ? status : 1;
    }
}

// Launches the staged version's installer, which then waits for this process to end. Returns 1 with the reason in
// out if it would not start.
UN_API int un_update_start_apply(const wchar_t *root, const wchar_t *staged, char *out, int cap) {
    try {
        start_apply(root ? root : L".", staged ? staged : L".");
        return give("", out, cap);
    } catch (const UpdateError &e) {
        int status = give(e.message, out, cap);
        return status < 0 ? status : 1;
    }
}

// Clears out update\ after an update finished or was given up.
UN_API void un_update_cleanup(const wchar_t *root) { cleanup(root ? root : L"."); }

}  // extern "C"
