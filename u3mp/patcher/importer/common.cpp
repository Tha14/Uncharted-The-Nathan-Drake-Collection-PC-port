// importer/common.cpp: see importer.hpp.
#include "importer.hpp"

#include "inflate.h"   // launcher/native/inflate.h

#include <windows.h>
#include <bcrypt.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace u3imp {

// ------------------------------------------------------------------------------------------------ text and paths
fs::path P(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string S(const fs::path &p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
std::string fwd(const fs::path &p) {
    std::string s = S(p.lexically_normal());
    for (char &c : s)
        if (c == '\\') c = '/';
    while (s.size() > 1 && s.back() == '/' && !(s.size() == 3 && s[1] == ':')) s.pop_back();
    return s;
}
std::string lower(std::string s) {
    for (char &c : s) c = char(std::tolower((unsigned char)c));
    return s;
}
bool starts(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }
bool ends(const std::string &s, const std::string &p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
std::string strip(const std::string &s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::vector<std::string> split(const std::string &s, char c) {
    std::vector<std::string> out;
    size_t a = 0;
    while (true) {
        const size_t b = s.find(c, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) return out;
        a = b + 1;
    }
}
std::string now_text() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}
std::string fmt(const char *f, ...) {
    va_list a, b;
    va_start(a, f);
    va_copy(b, a);
    const int n = std::vsnprintf(nullptr, 0, f, a);
    va_end(a);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    std::vsnprintf(s.data(), s.size() + 1, f, b);
    va_end(b);
    return s;
}

// ------------------------------------------------------------------------------------------------ output
namespace {
std::mutex g_out;
}
void log(const std::string &line) {
    std::lock_guard<std::mutex> l(g_out);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
const char *g_event_prefix = "@U3MP ";
void event(const Value &kw) { log(g_event_prefix + dumps(kw)); }
Value obj(std::initializer_list<std::pair<std::string, Value>> kv) {
    Value v = Value::object();
    for (const auto &[k, x] : kv) v.set(k, x);
    return v;
}
Value str(const std::string &s) { return Value::text(s); }
Value num(long long v) { return Value::integer(v); }

// ------------------------------------------------------------------------------------------------ JSON
namespace {
void dumps1(std::string &o, const Value &v, bool sort_keys) {
    switch (v.kind) {
    case Value::Null: o += "null"; return;
    case Value::Bool: o += v.b ? "true" : "false"; return;
    case Value::Int: o += std::to_string(v.i); return;
    case Value::Float: o += json::real(v.f); return;
    case Value::String: json::quote(o, v.s); return;
    case Value::Array:
        o += '[';
        for (size_t k = 0; k < v.items.size(); k++) {
            if (k) o += ", ";
            dumps1(o, v.items[k], sort_keys);
        }
        o += ']';
        return;
    case Value::Object: {
        std::vector<const std::pair<std::string, Value> *> f;
        for (const auto &x : v.fields) f.push_back(&x);
        if (sort_keys)   // Python sorts by code point: for UTF-8 text the byte order is the same
            std::stable_sort(f.begin(), f.end(), [](auto *a, auto *b) { return a->first < b->first; });
        o += '{';
        for (size_t k = 0; k < f.size(); k++) {
            if (k) o += ", ";
            json::quote(o, f[k]->first);
            o += ": ";
            dumps1(o, f[k]->second, sort_keys);
        }
        o += '}';
        return;
    }
    }
}
void dump1(std::string &o, const Value &v, int depth) {
    const std::string pad(size_t(depth + 1), ' '), end(size_t(depth), ' ');
    if (v.kind == Value::Array && !v.items.empty()) {
        o += "[\n";
        for (size_t k = 0; k < v.items.size(); k++) {
            o += pad;
            dump1(o, v.items[k], depth + 1);
            o += k + 1 < v.items.size() ? ",\n" : "\n";
        }
        o += end + "]";
    } else if (v.kind == Value::Object && !v.fields.empty()) {
        o += "{\n";
        for (size_t k = 0; k < v.fields.size(); k++) {
            o += pad;
            json::quote(o, v.fields[k].first);
            o += ": ";
            dump1(o, v.fields[k].second, depth + 1);
            o += k + 1 < v.fields.size() ? ",\n" : "\n";
        }
        o += end + "}";
    } else
        dumps1(o, v, false);
}
}  // namespace
std::string dumps(const Value &v, bool sort_keys) {
    std::string o;
    dumps1(o, v, sort_keys);
    return o;
}
std::string dump_indent1(const Value &v) {
    std::string o;
    dump1(o, v, 0);
    return o;
}
Value load_json(const fs::path &p) {
    Value v;
    if (!json::parse(read_text(p), v)) throw u3::Stop("not JSON: " + S(p));
    return v;
}
std::optional<Value> try_json(const fs::path &p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return std::nullopt;
    try {
        Value v;
        if (json::parse(read_text(p), v)) return v;
    } catch (const std::exception &) {
    }
    return std::nullopt;
}

// ------------------------------------------------------------------------------------------------ files
Bytes read_bytes(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw u3::Stop("[Errno 2] No such file or directory: '" + S(p) + "'");
    f.seekg(0, std::ios::end);
    Bytes b(size_t(f.tellg()));
    f.seekg(0);
    if (!b.empty()) f.read(reinterpret_cast<char *>(b.data()), std::streamsize(b.size()));
    return b;
}
std::string read_text(const fs::path &p) {
    const Bytes b = read_bytes(p);
    return std::string(b.begin(), b.end());
}
void write_bytes(const fs::path &p, const Bytes &b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) throw u3::Stop("cannot write " + S(p));
    if (!b.empty()) f.write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size()));
    if (!f) throw u3::Stop("cannot write " + S(p));
}
void write_text(const fs::path &p, const std::string &t) { write_bytes(p, Bytes(t.begin(), t.end())); }

Sha256::Sha256() {
    BCryptOpenAlgorithmProvider(reinterpret_cast<BCRYPT_ALG_HANDLE *>(&alg), BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(static_cast<BCRYPT_ALG_HANDLE>(alg), reinterpret_cast<BCRYPT_HASH_HANDLE *>(&hash), nullptr, 0, nullptr, 0, 0);
}
Sha256::~Sha256() {
    if (hash) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash));
    if (alg) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(alg), 0);
}
void Sha256::update(const void *data, size_t n) {
    const auto *p = static_cast<const unsigned char *>(data);
    while (n) {
        const ULONG k = ULONG(std::min<size_t>(n, 1u << 30));
        BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash), const_cast<PUCHAR>(p), k, 0);
        p += k;
        n -= k;
    }
}
Bytes Sha256::digest() {
    Bytes d(32);
    BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash), d.data(), 32, 0);
    return d;
}
std::string Sha256::hex() {
    static const char *H = "0123456789abcdef";
    std::string s;
    for (uint8_t c : digest()) {
        s += H[c >> 4];
        s += H[c & 15];
    }
    return s;
}
std::string sha256_hex(const void *data, size_t n) {
    Sha256 h;
    h.update(data, n);
    return h.hex();
}
Bytes sha256_raw(const void *data, size_t n) {
    Sha256 h;
    h.update(data, n);
    return h.digest();
}
std::string sha256_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw u3::Stop("[Errno 2] No such file or directory: '" + S(p) + "'");
    Sha256 h;
    std::vector<char> buf(8 << 20);
    while (f) {
        f.read(buf.data(), std::streamsize(buf.size()));
        if (f.gcount() > 0) h.update(buf.data(), size_t(f.gcount()));
    }
    return h.hex();
}
std::string sha256_cached(const fs::path &p) {
    static std::mutex m;
    static std::map<std::string, std::pair<std::pair<uint64_t, int64_t>, std::string>> cache;
    std::error_code ec;
    const uint64_t size = fs::file_size(p, ec);
    if (ec) return sha256_file(p);   // reports the missing file
    const int64_t mt = int64_t(fs::last_write_time(p, ec).time_since_epoch().count());
    const std::string key = S(p);
    {
        std::lock_guard<std::mutex> l(m);
        auto it = cache.find(key);
        if (it != cache.end() && it->second.first == std::make_pair(size, mt)) return it->second.second;
    }
    const std::string h = sha256_file(p);
    std::lock_guard<std::mutex> l(m);
    cache[key] = {{size, mt}, h};
    return h;
}
uint64_t tree_size(const fs::path &folder) {
    uint64_t n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) n += it->file_size(e2);
    }
    return n;
}
void rmtree(const fs::path &p) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        SetFileAttributesW(it->path().c_str(), FILE_ATTRIBUTE_NORMAL);
    fs::remove_all(p, ec);
}
bool rmtree_retry(const fs::path &p, double seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(seconds * 1000));
    std::error_code ec;
    while (fs::exists(p, ec)) {
        rmtree(p);
        if (!fs::exists(p, ec) || std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return !fs::exists(p, ec);
}
std::string zlib_inflate(const std::string &z) {
    if (z.size() < 6) throw u3::Stop("zlib data too short");
    std::string out;
    try {
        inflate::Inflater(reinterpret_cast<const unsigned char *>(z.data()) + 2, z.size() - 6, out).run();
    } catch (const inflate::InflateError &e) {
        throw u3::Stop("zlib data damaged: " + e.message);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ processes
namespace {
std::wstring wide(const std::string &s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::wstring quote_arg(const std::wstring &a) {   // the rules CommandLineToArgvW reads back
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring o = L"\"";
    for (size_t i = 0;; i++) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') {
            i++;
            bs++;
        }
        if (i == a.size()) {
            o.append(bs * 2, L'\\');
            break;
        }
        if (a[i] == L'"') o.append(bs * 2 + 1, L'\\');
        else o.append(bs, L'\\');
        o += a[i];
    }
    return o + L"\"";
}
}  // namespace

namespace {
std::wstring env_block(const std::vector<std::pair<std::string, std::string>> &env);
}

int run_split(const std::vector<std::string> &argv, const fs::path &cwd, const std::vector<std::pair<std::string, std::string>> &env, std::string &out,
              std::string &err) {
    std::wstring cmd;
    for (const auto &a : argv) cmd += (cmd.empty() ? L"" : L" ") + quote_arg(wide(a));
    std::wstring block = env_block(env);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE ord = nullptr, owr = nullptr, erd = nullptr, ewr = nullptr;
    if (!CreatePipe(&ord, &owr, &sa, 0) || !CreatePipe(&erd, &ewr, &sa, 0)) throw u3::Stop("cannot create a pipe");
    SetHandleInformation(ord, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(erd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = owr;
    si.hStdError = ewr;
    PROCESS_INFORMATION pi{};
    std::wstring cwdw = cwd.wstring();
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, block.data(),
                                   cwdw.empty() ? nullptr : cwdw.c_str(), &si, &pi);
    const DWORD e = GetLastError();
    CloseHandle(owr);
    CloseHandle(ewr);
    CloseHandle(nul);
    if (!ok) {
        CloseHandle(ord);
        CloseHandle(erd);
        throw u3::Stop(fmt("[WinError %lu] cannot start %s", e, argv.empty() ? "" : argv[0].c_str()));
    }
    auto drain = [](HANDLE h, std::string *s) {
        char buf[65536];
        DWORD got = 0;
        while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) s->append(buf, got);
    };
    std::thread t(drain, erd, &err);
    drain(ord, &out);
    t.join();
    CloseHandle(ord);
    CloseHandle(erd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return int(code);
}

namespace {
std::wstring env_block(const std::vector<std::pair<std::string, std::string>> &env) {
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> vars;
    if (wchar_t *block = GetEnvironmentStringsW()) {
        for (wchar_t *p = block; *p; p += wcslen(p) + 1) {
            std::wstring kv(p);
            const size_t eq = kv.find(L'=', 1);
            if (eq == std::wstring::npos) continue;
            std::wstring k = kv.substr(0, eq), up = k;
            for (auto &c : up) c = wchar_t(towupper(c));
            vars[up] = {k, kv.substr(eq + 1)};
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto &[k, v] : env) {
        std::wstring wk = wide(k), up = wk;
        for (auto &c : up) c = wchar_t(towupper(c));
        vars[up] = {wk, wide(v)};
    }
    std::wstring block;
    for (const auto &[up, kv] : vars) block += kv.first + L"=" + kv.second + L'\0';
    block += L'\0';
    return block;
}
}  // namespace

int run(const std::vector<std::string> &argv, const fs::path &cwd, const std::vector<std::pair<std::string, std::string>> &env,
        const std::function<void(const std::string &)> &on_line) {
    std::wstring cmd;
    for (const auto &a : argv) cmd += (cmd.empty() ? L"" : L" ") + quote_arg(wide(a));
    // environment: ours plus the extra variables (replacing same-named ones, case-insensitive like Windows)
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> vars;
    if (wchar_t *block = GetEnvironmentStringsW()) {
        for (wchar_t *p = block; *p; p += wcslen(p) + 1) {
            std::wstring kv(p);
            const size_t eq = kv.find(L'=', 1);
            if (eq == std::wstring::npos) continue;
            std::wstring k = kv.substr(0, eq), up = k;
            for (auto &c : up) c = wchar_t(towupper(c));
            vars[up] = {k, kv.substr(eq + 1)};
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto &[k, v] : env) {
        std::wstring wk = wide(k), up = wk;
        for (auto &c : up) c = wchar_t(towupper(c));
        vars[up] = {wk, wide(v)};
    }
    std::wstring block;
    for (const auto &[up, kv] : vars) block += kv.first + L"=" + kv.second + L'\0';
    block += L'\0';
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) throw u3::Stop("cannot create a pipe");
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cwdw = cwd.wstring();
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, block.data(),
                                   cwdw.empty() ? nullptr : cwdw.c_str(), &si, &pi);
    const DWORD err = GetLastError();
    CloseHandle(wr);
    CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        throw u3::Stop(fmt("[WinError %lu] cannot start %s", err, argv.empty() ? "" : argv[0].c_str()));
    }
    std::string pending;
    char buf[65536];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got) {
        pending.append(buf, got);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            on_line(line);
            pending.erase(0, nl + 1);
        }
    }
    if (!pending.empty()) {
        if (pending.back() == '\r') pending.pop_back();
        on_line(pending);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return int(code);
}

fs::path exe_dir() {
    wchar_t b[32768];
    const DWORD n = GetModuleFileNameW(nullptr, b, 32768);
    return fs::path(std::wstring(b, n)).parent_path();
}

std::string find_program(const std::vector<fs::path> &dirs, const std::string &name, const std::vector<std::string> &extra) {
    std::error_code ec;
    for (const fs::path &d : dirs)
        if (fs::is_regular_file(d / P(name + ".exe"), ec)) return S(d / P(name + ".exe"));
    wchar_t b[32768];
    if (SearchPathW(nullptr, wide(name).c_str(), L".exe", 32768, b, nullptr)) return S(fs::path(b));
    for (const std::string &e : extra)
        if (fs::is_regular_file(P(e), ec)) return e;
    return "";
}

}  // namespace u3imp
