// starter.cpp - small settings queries for the game starter, in uncharted_native.dll. It answers: how many CPU threads
// the game can run on, how many job workers to use and whether idle workers sleep or spin, the Uncharted 3 exposure
// bias, the folder suffix for an extra instance, and which Uncharted 3 multiplayer image (made by the launcher's
// import) to boot. All answers are derived from the UNCHARTED_* variables the launcher exports. The measurements that
// justified the defaults were recorded alongside tools/probe_uncharted.py, which this replaces.

#include "native.h"
#include "json.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cmath>
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

// Reads an environment variable as UTF-8; returns `fallback` only if the variable is not defined at all.
std::string env(const char *name, const char *fallback = "") {
    std::wstring wname = widen(name);
    SetLastError(0);
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (!n) return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? fallback : "";
    std::wstring v(n, L'\0');
    n = GetEnvironmentVariableW(wname.c_str(), v.data(), n);
    v.resize(n);
    return utf8(v);
}

std::string strip(const std::string &s) {
    const char *ws = " \t\r\n\v\f";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Integer parsing with Python int() rules: optional sign, then digits that may be grouped by single underscores.
// Returns false for anything else.
bool python_int(const std::string &text, long long &value) {
    std::string t = strip(text), digits;
    size_t i = 0;
    bool negative = false;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) negative = t[i++] == '-';
    for (; i < t.size(); ++i) {
        if (std::isdigit((unsigned char)t[i])) digits += t[i];
        else if (t[i] == '_' && !digits.empty() && i + 1 < t.size() && std::isdigit((unsigned char)t[i + 1])) continue;
        else return false;
    }
    if (digits.empty()) return false;
    // Huge values become a large stand-in: callers clamp the result, so only its sign matters.
    long long n = digits.size() > 18 ? (1LL << 62) : std::strtoll(digits.c_str(), nullptr, 10);
    value = negative ? -n : n;
    return true;
}

// Float parsing with Python float() rules for ordinary decimal numbers plus inf/infinity/nan. Returns false otherwise.
bool python_float(const std::string &text, double &value) {
    std::string t = lower(strip(text));
    if (t.empty()) return false;
    std::string body = (t[0] == '+' || t[0] == '-') ? t.substr(1) : t;
    if (body == "inf" || body == "infinity" || body == "nan") {
        value = body == "nan" ? NAN : (t[0] == '-' ? -INFINITY : INFINITY);
        return true;
    }
    for (char c : body)
        if (!(std::isdigit((unsigned char)c) || c == '.' || c == 'e' || c == '+' || c == '-')) return false;
    char *end = nullptr;
    value = std::strtod(t.c_str(), &end);
    return end == t.c_str() + t.size();
}

// Number of logical CPUs in this process's affinity mask. The game is started as a child, so it gets the same mask.
int hardware_threads() {
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        int n = 0;
        for (DWORD_PTR m = process; m; m &= m - 1) ++n;
        return n;
    }
    DWORD all = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return all ? (int)all : 8;
}

const char *game_key(const std::string &executable) {
    if (executable == "eboot.bin") return "u1";
    if (executable == "big2-ps4_Shipping.elf") return "u2";
    if (executable == "big3-ps4_Shipping.elf") return "u3";
    return "";
}

// Decides whether the JOB_SLEEP_PATCHES go in, i.e. whether idle job workers sleep instead of spinning.
// UNCHARTED_JOB_SLEEP = 0, off, no or spin -> spin. auto (the default) -> spin only for Uncharted 1 on a CPU with at
// least 8 hardware threads; sleep on smaller CPUs and always in Uncharted 2 and 3. Any other value -> sleep.
bool job_sleep(const std::string &executable) {
    std::string value = lower(strip(env("UNCHARTED_JOB_SLEEP", "auto")));
    if (value == "0" || value == "off" || value == "no" || value == "spin") return false;
    if (value == "auto") return std::string(game_key(executable)) != "u1" || hardware_threads() < 8;
    return true;
}

// Job worker count override from UNCHARTED_JOB_WORKERS: a number (clamped to 1..6), auto (hardware threads minus
// two, kept within 2..6), or 0/off to leave the engine at its built-in 6. Returns 0 when no override applies.
int job_workers() {
    std::string value = lower(strip(env("UNCHARTED_JOB_WORKERS", "0")));
    if (value.empty() || value == "0" || value == "off" || value == "no") return 0;
    if (value == "auto") return std::max(2, std::min(6, hardware_threads() - 2));
    long long n = 0;
    if (!python_int(value, n)) return 0;
    return (int)std::max(1LL, std::min(6LL, n));
}

// Exposure bias in EV from UNCHARTED_U3_EXPOSURE, limited to -2..1. Magnitudes under 0.05 count as 0.
double u3_exposure() {
    std::string text = strip(env("UNCHARTED_U3_EXPOSURE", "0"));
    double bias = 0;
    if (!text.empty() && !python_float(text, bias)) return 0.0;
    return std::fabs(bias) >= 0.05 ? std::max(-2.0, std::min(1.0, bias)) : 0.0;
}

// For UNCHARTED_INSTANCE=<n> with n >= 2 this returns "-i<n>", which gives a second game running on the same PC its
// own folders. Instance 1 (the default) gets an empty suffix.
std::string instance_suffix() {
    std::string value = strip(env("UNCHARTED_INSTANCE", "1"));
    bool digits = !value.empty();
    for (char c : value) digits = digits && std::isdigit((unsigned char)c);
    if (!digits || value.size() > 9) return "";
    long n = std::strtol(value.c_str(), nullptr, 10);
    return n >= 2 ? "-i" + std::to_string(n) : "";
}

bool sha256_file(const std::wstring &path, std::string &hex, std::string &error) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        wchar_t *text = nullptr;
        DWORD code = GetLastError();
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                       code, 0, (LPWSTR)&text, 0, nullptr);
        error = "[WinError " + std::to_string(code) + "] " + (text ? strip(utf8(text)) : std::string("error")) + ": '" +
                utf8(path) + "'";
        if (text) LocalFree(text);
        return false;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    std::vector<unsigned char> buf(1 << 20);
    DWORD got = 0;
    while (ok && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got)
        ok = BCryptHashData(hash, buf.data(), got, 0) == 0;
    unsigned char digest[32];
    ok = ok && BCryptFinishHash(hash, digest, 32, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (!ok) { error = "reading failed: '" + utf8(path) + "'"; return false; }
    static const char *digits = "0123456789abcdef";
    hex.clear();
    for (unsigned char b : digest) { hex += digits[b >> 4]; hex += digits[b & 15]; }
    return true;
}

// Checks the Uncharted 3 multiplayer setup the launcher exported: UNCHARTED_U3MP_IMAGE (path of the linked image),
// UNCHARTED_U3MP_IMAGE_SHA256 (its expected hash), UNCHARTED_U3MP_OVERLAYS (';'-separated folders, earliest has
// priority) and UNCHARTED_U3MP_VERSION.
// Result: first line = a log message, possibly empty. If the image is usable, a second part follows with the JSON
// {image, overlays, version}; if that part is missing, Uncharted 3 boots normally.
std::string u3mp_image(const std::string &executable) {
    std::string image = env("UNCHARTED_U3MP_IMAGE", "");
    if (executable != "big3-ps4_Shipping.elf" || image.empty()) return "\n";
    std::string want = lower(env("UNCHARTED_U3MP_IMAGE_SHA256", "")), digest, error;
    if (!sha256_file(widen(image), digest, error))
        return "Uncharted 3 multiplayer image could not be read (" + error + "): Uncharted 3 starts without it\n";
    if (!want.empty() && digest != want)
        return "Uncharted 3 multiplayer image " + image + " no longer matches (sha256 " + digest.substr(0, 12) +
               "..., expected " + want.substr(0, 12) +
               "...): Uncharted 3 starts without it. Redo the multiplayer import in the launcher\n";
    Value overlays;
    overlays.kind = Value::Array;
    std::string missing, list = env("UNCHARTED_U3MP_OVERLAYS", "");
    size_t start = 0;
    for (;;) {
        size_t cut = list.find(';', start);
        std::string p = list.substr(start, cut == std::string::npos ? std::string::npos : cut - start);
        if (!p.empty()) {
            overlays.items.push_back(Value::text(p));
            DWORD a = GetFileAttributesW(widen(p).c_str());
            if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) missing += (missing.empty() ? "" : ", ") + p;
        }
        if (cut == std::string::npos) break;
        start = cut + 1;
    }
    if (!missing.empty())
        return "Uncharted 3 multiplayer overlay folders not found (" + missing + "): Uncharted 3 starts without them\n";
    Value info = Value::object();
    info.set("image", Value::text(image));
    info.set("overlays", overlays);
    info.set("version", Value::text(env("UNCHARTED_U3MP_VERSION", "?")));
    return "\n" + json::dump(info);
}

}  // namespace

extern "C" {

// Settings query. `what` selects the answer:
//   hardware_threads, job_workers (0 = no override), u3_exposure (EV, printed with full precision), instance_suffix,
//   job_sleep (arg = executable; "1" or "0"), u3mp_image (arg = executable; message line, then optional JSON).
// Returns 0 on success, 1 when `what` is not recognised.
UN_API int un_starter(const char *what, const char *arg, char *out, int cap) {
    std::string w = what ? what : "", a = arg ? arg : "", r;
    if (w == "hardware_threads") r = std::to_string(hardware_threads());
    else if (w == "job_sleep") r = job_sleep(a) ? "1" : "0";
    else if (w == "job_workers") r = std::to_string(job_workers());
    else if (w == "u3_exposure") {
        char b[64];
        std::snprintf(b, sizeof b, "%.17g", u3_exposure());
        r = b;
    } else if (w == "instance_suffix") r = instance_suffix();
    else if (w == "u3mp_image") r = u3mp_image(a);
    else return 1;
    return give(r, out, cap);
}

}  // extern "C"
