// play.cpp - everything behind the PLAY button, inside uncharted_native.dll and driven by core.cpp. It turns the
// launcher's settings into environment variables, runs the game starter as a child process (its output is copied to
// launcher-last-run.log and passed up to the launcher one line at a time; STOP kills it), and afterwards reads the
// newest boot.log to explain why the game ended.

#include "native.h"
#include "json.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <windows.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
UN_API int un_game_sizes(const char *resolution, const char *aspect, const char *upscale, const char *upscaling, char *out,
                         int cap);
UN_API int un_upscaling_choice(const char *upscaler, const char *fsr1, char *out, int cap);
}

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
std::wstring widen(const std::string &s) {   // invalid UTF-8 becomes U+FFFD (Python's errors="replace")
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// a settings value printed the way Python's str() prints it, plus a truth test with a default for missing keys
std::string str_of(const Value *v, const std::string &missing) {
    if (!v) return missing;
    switch (v->kind) {
    case Value::Null: return "None";
    case Value::Bool: return v->b ? "True" : "False";
    case Value::Int: return std::to_string(v->i);
    case Value::Float: return json::real(v->f);
    case Value::String: return v->s;
    default: return json::dump(*v);
    }
}
bool truthy_or(const Value *v, bool missing) { return v ? v->truthy() : missing; }

std::string call_text(int (*f)(const char *, const char *, const char *, const char *, char *, int), const std::string &a,
                      const std::string &b, const std::string &c, const std::string &d) {
    std::vector<char> buf(1 << 12);
    int status = f(a.c_str(), b.c_str(), c.c_str(), d.c_str(), buf.data(), (int)buf.size());
    return status == 0 ? std::string(buf.data()) : std::string();
}

// The variables PLAY passes to the game, in the same order the old play() set them. Each entry is [name, value], or
// [name, null] when the name must be dropped. Not included here: the update folder, the multiplayer variables and the
// window title suffix, which the launcher adds itself.
Value play_env(const Value &config, const std::wstring &user_dir) {
    Value out;
    out.kind = Value::Array;
    auto set = [&](const std::string &k, const std::string &v) {
        Value p;
        p.kind = Value::Array;
        p.items = {Value::text(k), Value::text(v)};
        out.items.push_back(p);
    };
    auto remove = [&](const std::string &k) {
        Value p;
        p.kind = Value::Array;
        p.items = {Value::text(k), Value::null()};
        out.items.push_back(p);
    };
    auto get = [&](const char *k) { return config.get(k); };
    set("UNCHARTED_DIAGNOSTICS", truthy_or(get("diagnostics"), true) ? "1" : "0");
    set("UNCHARTED_TORCH_FORWARD", truthy_or(get("torch_forward"), true) ? "1" : "0");
    const std::string job = str_of(get("job_sleep"), "auto");
    set("UNCHARTED_JOB_SLEEP", job == "off" ? "0" : job == "on" ? "1" : "auto");
    // read by renderer_vulkan/vk_instance.cpp: "1" forces it, "auto" enables it on GPUs with at most 4.5 GiB, other
    // values disable it
    const std::string vram = str_of(get("low_vram"), "off");
    set("GOW3_LOW_VRAM", vram == "on" ? "1" : vram == "auto" ? "auto" : "0");
    set("UNCHARTED_LANGUAGE", str_of(get("language"), "1"));
    set("UNCHARTED_SPEAKERS", str_of(get("speakers"), "auto"));
    set("UNCHARTED_U3_EXPOSURE", str_of(get("u3_exposure"), "0"));
    set("UNCHARTED_ANISOTROPY", str_of(get("anisotropy"), "0"));
    set("UNCHARTED_HIRES_SHADOWS", truthy_or(get("hires_shadows"), false) ? "1" : "0");
    set("UNCHARTED_FULL_RES_TEXTURES", truthy_or(get("full_res_textures"), false) ? "1" : "0");
    set("UNCHARTED_NO_DOF", truthy_or(get("no_dof"), false) ? "1" : "0");
    set("GOW3_MOVIE_STRETCH", truthy_or(get("movie_stretch"), false) ? "1" : "0");
    set("UNCHARTED_UNLOCK", truthy_or(get("unlock"), false) ? "1" : "0");
    set("UNCHARTED_UNLOCK_REWARDS", truthy_or(get("unlock_rewards"), false) ? "1" : "0");
    // enabled cheat switches as a comma list (names from system.cpp's CHEATS table)
    std::string cheats;
    for (const char *name : {"invincible", "ammo", "noreload", "grenades", "onehit"})
        if (truthy_or(config.get(std::string("cheat_") + name), false)) cheats += (cheats.empty() ? "" : ",") + std::string(name);
    set("UNCHARTED_CHEATS", cheats);
    // Per-game values go out for all three titles, because a game can switch to another one from the Collection
    // menu without the launcher. GOW3_VBLANK_HZ is cleared: the starter sets it for whichever game it launches,
    // from that game's settings file, when the frame rate is above 60.
    remove("GOW3_VBLANK_HZ");
    const Value *games = config.get("games");
    for (const char *key : {"u1", "u2", "u3"}) {
        const Value *settings = games ? games->get(key) : nullptr;
        if (!settings) throw std::string("KeyError: '") + key + "'";
        std::string upper = key;
        upper[0] = 'U';
        // An upscaler makes the render size smaller than the picked resolution, which then becomes the output size.
        std::vector<char> buf(1 << 15);
        std::string upscaler, fsr1 = "0";
        bool have_upscaler = false;
        if (un_ini_read(user_dir.c_str(), key, buf.data(), (int)buf.size()) == 0) {
            const std::string text = buf.data();
            for (size_t a = 0; a < text.size();) {
                size_t b = text.find('\n', a);
                if (b == std::string::npos) b = text.size();
                const std::string line = text.substr(a, b - a);
                size_t tab = line.find('\t');
                if (tab != std::string::npos) {
                    const std::string k = line.substr(0, tab), v = line.substr(tab + 1);
                    if (k == "upscaler") { upscaler = v; have_upscaler = true; }
                    if (k == "fsr1") fsr1 = v;
                }
                a = b + 1;
            }
        }
        if (!have_upscaler) {
            std::vector<char> d(1 << 15);
            if (un_settings_table("ini_defaults", d.data(), (int)d.size()) == 0) {
                const std::string text = d.data();
                size_t at = text.find("upscaler\t");
                while (at != std::string::npos && at != 0 && text[at - 1] != '\n') at = text.find("upscaler\t", at + 1);
                if (at != std::string::npos) upscaler = text.substr(at + 9, text.find('\n', at) - at - 9);
            }
        }
        std::vector<char> choice(256);
        un_upscaling_choice(upscaler.c_str(), fsr1.c_str(), choice.data(), (int)choice.size());
        const Value *resolution = settings->get("resolution");
        if (!resolution) throw std::string("KeyError: 'resolution'");
        const std::string sizes = call_text(un_game_sizes, str_of(resolution, ""), str_of(settings->get("aspect"), "16:9"),
                                            str_of(settings->get("upscale"), "off"), choice.data());
        const size_t tab = sizes.find('\t');
        const std::string render = sizes.substr(0, tab), output = tab == std::string::npos ? "" : sizes.substr(tab + 1);
        set("UNCHARTED_RESOLUTION_" + upper, render);
        if (!output.empty()) set("UNCHARTED_OUTPUT_RES_" + upper, output); else remove("UNCHARTED_OUTPUT_RES_" + upper);
        const Value *mt = settings->get("multithreaded");
        if (!mt) throw std::string("KeyError: 'multithreaded'");
        set("UNCHARTED_MULTITHREADED_" + upper, mt->truthy() ? "1" : "0");
        set("UNCHARTED_SAVE_PROFILE_" + upper, str_of(settings->get("save_profile"), "1"));
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ the running game

struct Session {
    std::mutex lock;
    HANDLE process = nullptr;
    DWORD pid = 0;
    std::deque<std::string> lines;   // the output lines not yet handed over (trailing whitespace removed)
    bool reading = false;            // the reader thread is still at work
    bool ended = false;
    DWORD exit_code = 0;
};
Session g_session;

// subprocess.list2cmdline
std::wstring command_line(const std::vector<std::wstring> &args) {
    std::wstring result;
    for (const std::wstring &arg : args) {
        std::wstring bs;
        if (!result.empty()) result += L' ';
        const bool quote = arg.find(L' ') != std::wstring::npos || arg.find(L'\t') != std::wstring::npos || arg.empty();
        if (quote) result += L'"';
        for (wchar_t c : arg) {
            if (c == L'\\') {
                bs += c;
            } else if (c == L'"') {
                result += std::wstring(bs.size() * 2, L'\\');
                bs.clear();
                result += L"\\\"";
            } else {
                result += bs;
                bs.clear();
                result += c;
            }
        }
        result += bs;
        if (quote) {
            result += bs;
            result += L'"';
        }
    }
    return result;
}

std::string os_error(DWORD code) {
    wchar_t *text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                   0, (LPWSTR)&text, 0, nullptr);
    std::wstring t = text ? text : L"error";
    if (text) LocalFree(text);
    while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r' || t.back() == L' ' || t.back() == L'.')) t.pop_back();
    return "[WinError " + std::to_string(code) + "] " + utf8(t);
}

// str.rstrip(): without trailing whitespace (Python's whitespace)
std::wstring rstrip(std::wstring s) {
    while (!s.empty() && (iswspace(s.back()) || (s.back() >= 0x1c && s.back() <= 0x1f) || s.back() == 0x85)) s.pop_back();
    return s;
}

// Reader thread for the starter's pipe. Bytes are decoded as UTF-8 with bad sequences replaced, and a line ends at
// \r\n, a lone \r or \n (the same rule as Python text mode). Every line is written to the run log ending in \r\n and
// queued for the launcher.
void read_output(HANDLE pipe, HANDLE log) {
    std::string pending;   // bytes of the line being read
    bool cr = false;       // the last byte read was \r (a \n right after it belongs to the same line end)
    auto emit = [&](const std::string &bytes, bool newline) {
        const std::wstring line = widen(bytes);
        if (log) {
            const std::string text = utf8(line) + (newline ? "\r\n" : "");
            DWORD put = 0;
            if (!WriteFile(log, text.data(), (DWORD)text.size(), &put, nullptr)) {
                CloseHandle(log);
                log = nullptr;
            }
        }
        std::lock_guard<std::mutex> guard(g_session.lock);
        g_session.lines.push_back(utf8(rstrip(line)));
    };
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(pipe, buf, sizeof buf, &got, nullptr) && got) {
        for (DWORD i = 0; i < got; ++i) {
            const char c = buf[i];
            if (cr && c == '\n') { cr = false; continue; }
            cr = false;
            if (c == '\n' || c == '\r') {
                emit(pending, true);
                pending.clear();
                cr = c == '\r';
            } else {
                pending += c;
            }
        }
    }
    if (!pending.empty()) emit(pending, false);
    CloseHandle(pipe);
    WaitForSingleObject(g_session.process, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(g_session.process, &code);
    if (log) {
        const std::string text = "exit code " + std::to_string(code) + "\r\n";
        DWORD put = 0;
        WriteFile(log, text.data(), (DWORD)text.size(), &put, nullptr);
        CloseHandle(log);
    }
    std::lock_guard<std::mutex> guard(g_session.lock);
    g_session.exit_code = code;
    g_session.reading = false;
}

// Launches the game starter. The request carries command (argv), env (the complete environment as [name, value]
// pairs), cwd and log (path of the run log, which gets a timestamp-and-command header and then the output). pid
// receives the process id; failures are thrown as text.
std::string play_start(const Value &request, DWORD &pid) {
    {
        std::lock_guard<std::mutex> guard(g_session.lock);
        if (g_session.process && (g_session.reading || !g_session.ended)) throw std::string("a game is already running");
    }
    std::vector<std::wstring> command;
    if (const Value *c = request.get("command")) for (auto &a : c->items) command.push_back(widen(a.s));
    if (command.empty()) throw std::string("no command");
    std::vector<std::pair<std::wstring, std::wstring>> vars;
    if (const Value *e = request.get("env"))
        for (auto &p : e->items) if (p.items.size() == 2) vars.push_back({widen(p.items[0].s), widen(p.items[1].s)});
    std::stable_sort(vars.begin(), vars.end(), [](auto &a, auto &b) {
        return CompareStringOrdinal(a.first.c_str(), (int)a.first.size(), b.first.c_str(), (int)b.first.size(), TRUE) ==
               CSTR_LESS_THAN;
    });
    std::vector<wchar_t> block;
    for (auto &kv : vars) {
        const std::wstring line = kv.first + L"=" + kv.second;
        block.insert(block.end(), line.begin(), line.end());
        block.push_back(0);
    }
    block.push_back(0);
    if (block.size() == 1) block.push_back(0);
    const std::wstring cwd = request.get("cwd") ? widen(request.get("cwd")->s) : L"";
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) throw os_error(GetLastError());
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> attr_buf(size);
    auto attr = (LPPROC_THREAD_ATTRIBUTE_LIST)attr_buf.data();
    InitializeProcThreadAttributeList(attr, 1, 0, &size);
    HANDLE handles[2] = {null_in, write_end};
    UpdateProcThreadAttribute(attr, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof handles, nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = null_in;
    si.StartupInfo.hStdOutput = write_end;
    si.StartupInfo.hStdError = write_end;
    si.lpAttributeList = attr;
    std::wstring cmd = command_line(command);
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(0);
    // Let any process grab the foreground. Windows reserves that for the current foreground process, and the game
    // window belongs to a grandchild, yet it needs focus for fullscreen and for keyboard input.
    AllowSetForegroundWindow(ASFW_ANY);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                                   EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, block.data(),
                                   cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    const DWORD error = GetLastError();
    DeleteProcThreadAttributeList(attr);
    CloseHandle(write_end);
    if (null_in != INVALID_HANDLE_VALUE) CloseHandle(null_in);
    if (!ok) {
        CloseHandle(read_end);
        throw os_error(error);
    }
    CloseHandle(pi.hThread);
    // Run log, for bug reports where the game never comes up: a timestamp and the command line, then the output.
    HANDLE log = nullptr;
    if (const Value *l = request.get("log")) {
        log = CreateFileW(widen(l->s).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE) {
            log = nullptr;
        } else {
            SYSTEMTIME t;
            GetLocalTime(&t);
            char stamp[32];
            std::snprintf(stamp, sizeof stamp, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                          t.wSecond);
            std::string joined;
            for (size_t k = 0; k < command.size(); ++k) joined += (k ? " " : "") + utf8(command[k]);
            const std::string header = std::string(stamp) + " " + joined + "\r\n";
            DWORD put = 0;
            WriteFile(log, header.data(), (DWORD)header.size(), &put, nullptr);
        }
    }
    {
        std::lock_guard<std::mutex> guard(g_session.lock);
        if (g_session.process) CloseHandle(g_session.process);
        g_session.process = pi.hProcess;
        g_session.pid = pi.dwProcessId;
        g_session.lines.clear();
        g_session.reading = true;
        g_session.ended = false;
        g_session.exit_code = 0;
    }
    std::thread(read_output, read_end, log).detach();
    pid = pi.dwProcessId;
    return "";
}

// ------------------------------------------------------------------------------------------------ why a game stopped

std::wstring strip(const std::wstring &s) {
    size_t a = 0, b = s.size();
    auto space = [](wchar_t c) { return iswspace(c) || (c >= 0x1c && c <= 0x1f) || c == 0x85; };
    while (a < b && space(s[a])) ++a;
    while (b > a && space(s[b - 1])) --b;
    return s.substr(a, b - a);
}
bool starts(const std::wstring &s, const wchar_t *prefix) { return s.compare(0, wcslen(prefix), prefix) == 0; }

// str.splitlines()
std::vector<std::wstring> splitlines(const std::wstring &t) {
    std::vector<std::wstring> lines;
    size_t a = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        const wchar_t c = t[i];
        if (c == L'\n' || c == L'\r' || c == 0x0b || c == 0x0c || c == 0x1c || c == 0x1d || c == 0x1e || c == 0x85 ||
            c == 0x2028 || c == 0x2029) {
            lines.push_back(t.substr(a, i - a));
            if (c == L'\r' && i + 1 < t.size() && t[i + 1] == L'\n') ++i;
            a = i + 1;
        }
    }
    if (a < t.size()) lines.push_back(t.substr(a));
    return lines;
}

double mtime_of(const std::wstring &path, bool &ok) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    ok = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) != 0 && !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    if (!ok) return 0;
    const unsigned long long t = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    return (double)(long long)(t - 116444736000000000ull) / 1e7;
}

// Most recently written boot.log among the three games' folders, or an empty path.
std::wstring latest_log(const std::wstring &root) {
    std::wstring best;
    double best_time = 0;
    for (const wchar_t *folder : {L"uncharted", L"big2-ps4_Shipping", L"big3-ps4_Shipping"}) {
        const std::wstring log = root + L"\\analysis\\" + folder + L"\\boot.log";
        bool ok = false;
        const double t = mtime_of(log, ok);
        if (ok && (best.empty() || t > best_time)) {
            best = log;
            best_time = t;
        }
    }
    return best;
}

// Looks at the last lines of the newest boot.log (only if it changed after started_at) for, in this order: a hang
// watchdog report, a runtime fault with its location, or a STOP / assertion message. found stays false otherwise.
std::string crash_reason(const std::wstring &root, double started_at, bool &found) {
    found = false;
    const std::wstring log = latest_log(root);
    bool ok = false;
    if (log.empty() || mtime_of(log, ok) < started_at) return "";
    std::string data;
    {
        HANDLE h = CreateFileW(log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return "";
        char buf[1 << 16];
        DWORD got = 0;
        while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) data.append(buf, got);
        CloseHandle(h);
    }
    std::vector<std::wstring> lines = splitlines(widen(data));
    std::vector<std::wstring> tail(lines.size() > 80 ? lines.end() - 80 : lines.begin(), lines.end());
    for (size_t k = tail.size(); k-- > 0;) {
        const std::wstring &line = tail[k];
        const size_t at = line.find(L" report ");
        if (starts(line, L"Hang watchdog:") && at != std::wstring::npos) {
            found = true;
            // play_end tells a freeze by this text's first 14 characters ("The game froze")
            return "The game froze; a report showing where it got stuck is here:\n" + utf8(line.substr(at + 8)) +
                   "\n\nWhen you open a GitHub issue, include that report and boot.log from the log folder.";
        }
    }
    // Exit code 3 is a crash; the runtime then logs a "Fault:" line followed by a backtrace whose #0 frame is the spot.
    for (size_t i = 0; i < tail.size(); ++i) {
        if (!starts(tail[i], L"Fault:")) continue;
        std::wstring where;
        for (size_t k = i + 1; k < tail.size() && k < i + 6; ++k) {
            std::wstring left = tail[k];
            size_t a = 0;
            while (a < left.size() && (iswspace(left[a]) || (left[a] >= 0x1c && left[a] <= 0x1f) || left[a] == 0x85)) ++a;
            left = left.substr(a);
            if (starts(left, L"host #0") || starts(left, L"#0")) {
                where = strip(tail[k]);
                break;
            }
        }
        found = true;
        return utf8(strip(tail[i])) + (where.empty() ? "" : "\n" + utf8(where)) +
               "\n\nTo report it, open a GitHub issue with boot.log from the log folder and name your graphics card, "
               "game version and settings.";
    }
    for (size_t i = 0; i < tail.size(); ++i) {
        const std::wstring &line = tail[i];
        if (starts(line, L"STOP:") || line.find(L"Unreachable code") != std::wstring::npos ||
            line.find(L"Assertion") != std::wstring::npos) {
            std::wstring joined;
            bool first = true;
            for (size_t k = i >= 2 ? i - 2 : 0; k <= i; ++k) {
                if (strip(tail[k]).empty()) continue;
                joined += (first ? L"" : L"\n") + tail[k];
                first = false;
            }
            found = true;
            return utf8(joined.size() > 600 ? joined.substr(joined.size() - 600) : joined);
        }
    }
    return "";
}

}  // namespace

extern "C" {

// config_json is the launcher's in-memory launcher.json, user_dir the folder with the games' settings files. Writes
// a JSON list of [name, value] pairs (null value = unset that name) in the old play() order. Returns 0, or 1 with
// the reason in out when a game's block is missing from the settings.
UN_API int un_play_env(const char *config_json, const wchar_t *user_dir, char *out, int cap) {
    Value config;
    if (!json::parse(config_json ? config_json : "{}", config)) return give("invalid settings", out, cap) < 0 ? -1 : 1;
    try {
        return give(json::dump(play_env(config, user_dir ? user_dir : L"")), out, cap);
    } catch (const std::string &error) {
        int status = give(error, out, cap);
        return status < 0 ? status : 1;
    }
}

// request_json = {command: [...], env: [[name, value], ...], cwd, log}. Returns 0 with the new process id in out,
// or 1 with the failure text in out (in the "[WinError n] message" form Python's OSError used).
UN_API int un_play_start(const char *request_json, char *out, int cap) {
    Value request;
    if (!json::parse(request_json ? request_json : "{}", request)) return give("invalid request", out, cap) < 0 ? -1 : 1;
    try {
        DWORD pid = 0;
        play_start(request, pid);
        return give(std::to_string(pid), out, cap);
    } catch (const std::string &error) {
        int status = give(error, out, cap);
        return status < 0 ? status : 1;
    }
}

// Polled by the launcher: {"lines": new output since the previous poll, "exit": null while running, the exit code
// once the process is gone and its last line has been delivered}. Lines are delivered only once; if out is too small
// nothing is consumed and the needed size comes back negative.
UN_API int un_play_poll(char *out, int cap) {
    std::lock_guard<std::mutex> guard(g_session.lock);
    Value state = Value::object(), lines;
    lines.kind = Value::Array;
    for (auto &l : g_session.lines) lines.items.push_back(Value::text(l));
    const bool done = g_session.process && !g_session.reading;
    state.set("lines", lines);
    state.set("exit", done ? Value::integer((long long)g_session.exit_code) : Value::null());
    const std::string text = json::dump(state, false);
    if ((int)text.size() + 1 > cap) return -(int)(text.size() + 1);
    g_session.lines.clear();
    if (done) {
        CloseHandle(g_session.process);
        g_session.process = nullptr;
        g_session.ended = true;
    }
    return give(text, out, cap);
}

// The STOP button: kills the starter's whole process tree with taskkill /T /F, the same command the Python used.
UN_API int un_play_stop(void) {
    DWORD pid;
    {
        std::lock_guard<std::mutex> guard(g_session.lock);
        if (!g_session.process) return 1;
        pid = g_session.pid;
    }
    std::wstring cmd = L"taskkill /PID " + std::to_wstring(pid) + L" /T /F";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE null_io = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
                                 nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = si.hStdOutput = si.hStdError = null_io;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (null_io != INVALID_HANDLE_VALUE) CloseHandle(null_io);
    return 0;
}

// root is the launcher folder, started_at the PLAY time in Unix seconds. Returns 0 with an explanation in out, or 2
// if the newest boot.log gives no reason.
UN_API int un_play_crash_reason(const wchar_t *root, double started_at, char *out, int cap) {
    bool found = false;
    const std::string reason = crash_reason(root ? root : L"", started_at, found);
    if (!found) return give("", out, cap) < 0 ? -1 : 2;
    return give(reason, out, cap);
}

}  // extern "C"

// ------------------------------------------------------------------------------------------------ PLAY's questions

extern "C" UN_API int un_game_update(const char *what, const wchar_t *root, const char *args_json, char *out, int cap);

namespace {

// a system.cpp answer ("" for none)
std::string system_answer(const char *what, const std::string &arg) {
    const std::wstring a = widen(arg);
    std::string out(1 << 12, '\0');
    int status = un_system(what, a.c_str(), &out[0], (int)out.size());
    if (status < 0) {
        out.assign(-status, '\0');
        status = un_system(what, a.c_str(), &out[0], (int)out.size());
    }
    if (status != 0) return "";
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::string str_field(const json::Value &v, const char *key) {
    const json::Value *f = v.get(key);
    return f && f->kind == json::Value::String ? f->s : "";
}

std::string lstrip_zeros(const std::string &s) {
    size_t i = 0;
    while (i < s.size() && s[i] == '0') ++i;
    return s.substr(i);
}

// a folder's last part (Path.name)
std::string name_of(std::string path) {
    while (path.size() > 1 && (path.back() == '\\' || path.back() == '/')) path.pop_back();
    const size_t cut = path.find_last_of("\\/");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}
std::string parent_of(std::string path) {
    while (path.size() > 1 && (path.back() == '\\' || path.back() == '/')) path.pop_back();
    const size_t cut = path.find_last_of("\\/");
    return cut == std::string::npos ? path : path.substr(0, cut);
}

std::string lower_ascii(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// What the update folder lacks (gameupdate.cpp check_update): its lines, none when the check itself fails.
std::vector<std::string> update_problems(const std::string &root, const std::string &update) {
    const std::wstring r = widen(root);
    json::Value args;
    args.kind = json::Value::Array;
    args.items.push_back(json::Value::text(update));
    const std::string a = json::dump(args, false);
    std::string out(1 << 16, '\0');
    int status = un_game_update("check_update", r.c_str(), a.c_str(), &out[0], (int)out.size());
    if (status < 0) {
        out.assign(-status, '\0');
        status = un_game_update("check_update", r.c_str(), a.c_str(), &out[0], (int)out.size());
    }
    std::vector<std::string> lines;
    if (status != 0) return lines;   // a failing check stays silent rather than blocking PLAY
    out.resize(std::strlen(out.c_str()));
    size_t at = 0;
    while (at < out.size()) {   // str.splitlines()
        size_t end = out.find_first_of("\r\n", at);
        if (end == std::string::npos) end = out.size();
        lines.push_back(out.substr(at, end - at));
        at = end + (out.compare(end, 2, "\r\n") == 0 ? 2 : 1);
    }
    return lines;
}

json::Value question(const char *id, const std::string &text) {
    json::Value q = json::Value::object();
    q.set("id", json::Value::text(id));
    q.set("text", json::Value::text(text));
    q.set("remember", json::Value::object());
    return q;
}

// Warnings shown before launch, each as an OK / Cancel box in this order. Cancel aborts PLAY; OK stores the entry's
// "remember" values so the same warning does not come back:
//  - without_update: base game lacks update 1.02, which the bulk of the port's fixes target (asked once per game
//    folder)
//  - update_files: files of the update folder missing or broken (#43 was an unreadable bin.psarc)
//  - slow_drive: port or game folder on USB, SD or a FAT/exFAT volume, where file writes stall the games (asked once
//    per port + game folder combination)
json::Value play_questions(const json::Value &state) {
    json::Value list;
    list.kind = json::Value::Array;
    json::Value config = state.get("config") && state.get("config")->is_object() ? *state.get("config") : json::Value::object();
    const std::string root = str_field(state, "root"), game = str_field(state, "game_dir"), update = str_field(state, "update_dir");

    const std::string version = system_answer("app_version", game);
    if (update.empty() && !version.empty() && version < "01.02" && str_field(config, "warned_no_update") != game) {
        const std::string title = lower_ascii(name_of(game)) == "uroot" ? name_of(parent_of(game)) : name_of(game);
        json::Value q = question("without_update",
            "Update 1.02 is missing: this game folder holds version " + lstrip_zeros(version) +
                " only.\n\nThe port's fixes, crash fixes included, were built mainly around update 1.02, so expect "
                "more crashes without it.\n\nTo add it, dump update 1.02 from your PS4 and unpack it next to the "
                "game folder, named " + title + "-patch. TOOLS will then show it.\n\nStart the game anyway?");
        q.get("remember")->set("warned_no_update", json::Value::text(game));
        list.items.push_back(std::move(q));
    }

    if (!update.empty()) {
        const std::vector<std::string> problems = update_problems(root, update);
        if (!problems.empty()) {
            std::string named;
            for (size_t i = 0; i < problems.size() && i < 6; ++i) named += (i ? "\n" : "") + problems[i];
            list.items.push_back(question("update_files",
                "Some files of the game update are missing or damaged:\n\n" + named + "\n\n" + update +
                    "\n\nWith these files broken, the games will probably not get past startup. Unpack the update "
                    "once more and choose it with SETUP -> step 2 -> Choose update folder. Start the game anyway?"));
        }
    }

    std::vector<std::string> places;
    for (auto &place : {std::make_pair(std::string("This port"), root), std::make_pair(std::string("The game"), game)}) {
        const std::string reason = system_answer("slow_drive", place.second);
        if (!reason.empty()) places.push_back(place.first + " (" + place.second + ") is on " + reason + ".");
    }
    const std::string key = root + "|" + game;
    if (!places.empty() && str_field(config, "warned_slow_drive") != key) {
        std::string text;
        for (size_t i = 0; i < places.size(); ++i) text += (i ? "\n" : "") + places[i];
        json::Value q = question("slow_drive",
            text + "\n\nWindows doesn't cache writes to drives like this, so whenever the port stores its shader "
                   "cache, logs or saves there, the games can hang for up to a second, over and over.\n\nTo avoid "
                   "that, put both this port folder and the game folder on an internal drive (an SSD such as C:) and "
                   "run the launcher from its new place.\n\nStart the game anyway?");
        q.get("remember")->set("warned_slow_drive", json::Value::text(key));
        list.items.push_back(std::move(q));
    }
    return list;
}

// The launcher's words after the game exits: {"status", "detail", "ask"}, where "ask" is a yes / no error box that
// offers the log folder, or "" when there is nothing to show. A hang report wins even when the player ended the game
// with STOP or from Task Manager.
json::Value play_end(const json::Value &state) {
    const std::string root = str_field(state, "root"), game = str_field(state, "game_dir"), update = str_field(state, "update_dir");
    const json::Value *code = state.get("code");
    const bool stopping = state.get("stopping") && state.get("stopping")->truthy();
    const json::Value *started = state.get("started_at");
    const double started_at = !started ? 0.0 : started->kind == json::Value::Float ? started->f : (double)started->i;
    bool found = false;
    std::string reason = crash_reason(widen(root), started_at, found);
    if (!found) reason.clear();
    const bool froze = reason.compare(0, 14, "The game froze") == 0;
    const bool clean = !code || code->kind == json::Value::Null || (code->kind == json::Value::Int && code->i == 0);
    json::Value end = json::Value::object();
    if ((clean || stopping) && !froze) {
        end.set("status", json::Value::text("The game was closed."));
        end.set("detail", json::Value::text("You can play again."));
        end.set("ask", json::Value::text(""));
        return end;
    }
    end.set("status", json::Value::text(froze ? "The game stopped responding." : "The game ended unexpectedly."));
    end.set("detail", json::Value::text(froze ? "A report is in the log folder."
                                              : "Exit code " + (code && code->kind == json::Value::Int ? std::to_string(code->i)
                                                                                                       : std::string("None")) +
                                                    ". The log has the details."));
    if (!reason.empty()) {
        // 24 Oct 2026 (#148, #163, #172): players on an unpatched 1.00 game filed its crashes as port bugs, while the
        // port's fixes are largely made for update 1.02; say so next to the crash
        const std::string version = system_answer("app_version", game);
        if (update.empty() && !version.empty() && version < "01.02")
            reason += "\n\nNote: this game folder is version " + lstrip_zeros(version) +
                      " without update 1.02, and the bulk of this port's crash fixes rely on that update. Add it first "
                      "(SETUP, step 2).";
        end.set("ask", json::Value::text("Something went wrong in the game:\n\n" + reason + "\n\nShow the log folder?"));
    } else {
        end.set("ask", json::Value::text(""));
    }
    return end;
}

}  // namespace

extern "C" {

// PLAY's questions before the game starts: state {"root", "config", "game_dir", "update_dir" ("" for none)}; out: a
// JSON array of {"id", "text", "remember": settings to save after OK}.
UN_API int un_play_questions(const char *state_json, char *out, int cap) {
    json::Value state;
    if (!json::parse(state_json ? state_json : "{}", state) || !state.is_object()) return 3;
    return give(json::dump(play_questions(state), false), out, cap);
}

// What to say when the game ended: state {"root", "game_dir", "update_dir", "code" (exit code or null), "stopping",
// "started_at"}; out: {"status", "detail", "ask"}.
UN_API int un_play_end(const char *state_json, char *out, int cap) {
    json::Value state;
    if (!json::parse(state_json ? state_json : "{}", state) || !state.is_object()) return 3;
    return give(json::dump(play_end(state), false), out, cap);
}

}  // extern "C"
