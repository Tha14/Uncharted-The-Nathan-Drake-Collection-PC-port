// starter.cpp - the game starter's settings (tools/probe_uncharted.py), part of uncharted_native.dll: the CPU threads
// the game may use, the job workers and whether idle ones sleep, Uncharted 3's exposure, a second instance's folder
// suffix and the Uncharted 3 multiplayer image the launcher's import made. Everything comes from the environment the
// launcher sets (UNCHARTED_*); see probe_uncharted.py's notes for the measurements behind each choice.

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

// an environment value (UTF-8), `fallback` when it is not set
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

// Python's int(): optional sign, digits (underscores between them); false when it isn't one
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
    long long n = digits.size() > 18 ? (1LL << 62) : std::strtoll(digits.c_str(), nullptr, 10);   // big: only its sign counts
    value = negative ? -n : n;
    return true;
}

// Python's float() for plain decimal text, inf and nan; false when it isn't one
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

// logical processors this process may run on (its affinity mask; the game inherits it)
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

// JOB_SLEEP_PATCHES: idle job workers sleep (UNCHARTED_JOB_SLEEP: 0/off/no/spin spin; auto: spin in Uncharted 1 from
// 8 hardware threads, sleep below and in the other games)
bool job_sleep(const std::string &executable) {
    std::string value = lower(strip(env("UNCHARTED_JOB_SLEEP", "auto")));
    if (value == "0" || value == "off" || value == "no" || value == "spin") return false;
    if (value == "auto") return std::string(game_key(executable)) != "u1" || hardware_threads() < 8;
    return true;
}

// UNCHARTED_JOB_WORKERS=<1..6> | auto (hardware threads minus two, 2..6) | 0/off (the engine's own 6): 0 for none
int job_workers() {
    std::string value = lower(strip(env("UNCHARTED_JOB_WORKERS", "0")));
    if (value.empty() || value == "0" || value == "off" || value == "no") return 0;
    if (value == "auto") return std::max(2, std::min(6, hardware_threads() - 2));
    long long n = 0;
    if (!python_int(value, n)) return 0;
    return (int)std::max(1LL, std::min(6LL, n));
}

// UNCHARTED_U3_EXPOSURE=<EV>: -2..1, 0 below 0.05 either way
double u3_exposure() {
    std::string text = strip(env("UNCHARTED_U3_EXPOSURE", "0"));
    double bias = 0;
    if (!text.empty() && !python_float(text, bias)) return 0.0;
    return std::fabs(bias) >= 0.05 ? std::max(-2.0, std::min(1.0, bias)) : 0.0;
}

// UNCHARTED_INSTANCE=<n> (n >= 2): "-i<n>", the folders of another game on this PC
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

// Uncharted 3 multiplayer as the launcher set it: UNCHARTED_U3MP_IMAGE (the linked image), UNCHARTED_U3MP_IMAGE_SHA256,
// UNCHARTED_U3MP_OVERLAYS (folders, ';' between them, the first wins), UNCHARTED_U3MP_VERSION. A message for the
// log (or an empty line), then the image as JSON {image, overlays, version} (nothing: start Uncharted 3 as usual).
std::string u3mp_image(const std::string &executable) {
    std::string image = env("UNCHARTED_U3MP_IMAGE", "");
    if (executable != "big3-ps4_Shipping.elf" || image.empty()) return "\n";
    std::string want = lower(env("UNCHARTED_U3MP_IMAGE_SHA256", "")), digest, error;
    if (!sha256_file(widen(image), digest, error))
        return "Uncharted 3 multiplayer image not readable (" + error + "): starting Uncharted 3 without it\n";
    if (!want.empty() && digest != want)
        return "Uncharted 3 multiplayer image " + image + " changed (sha256 " + digest.substr(0, 12) + "..., expected " +
               want.substr(0, 12) + "...): starting Uncharted 3 without it; import it again in the launcher\n";
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
        return "Uncharted 3 multiplayer overlays missing (" + missing + "): starting Uncharted 3 without them\n";
    Value info = Value::object();
    info.set("image", Value::text(image));
    info.set("overlays", overlays);
    info.set("version", Value::text(env("UNCHARTED_U3MP_VERSION", "?")));
    return "\n" + json::dump(info);
}

}  // namespace

extern "C" {

// what: hardware_threads, job_sleep (arg: the executable; 1 or 0), job_workers (0 none), u3_exposure (EV, repr),
// instance_suffix, u3mp_image (arg: the executable; a message line, then JSON). 0 ok, 1 unknown what.
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
