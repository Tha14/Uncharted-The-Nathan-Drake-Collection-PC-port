// core.cpp - the launcher itself, part of uncharted_native.dll: its settings, the window's model (game cards, tabs,
// status line, PLAY, the update banner), every action, PLAY and the running game, the launcher's own updates, the
// online section and the multiplayer imports. The Qt window (launcher-qt) runs it in its own process and talks to it
// with JSON messages:
//
//   un_core_open(root, args_json)        once; args: {"direct": "u1" | "u2" | "u3" | null} (--play: the game starts
//                                        at once and the launcher ends with it)
//   un_core_send(message_json, out, cap) one request ({"op": ...}: model, config, ini, game_setting, ini_all, call, act,
//                                        game, playgame, tab, play, update, answer, close); out: a JSON array of
//                                        the replies and events it caused ({"re": "model", ...}, {"ev": "dialog", ...},
//                                        {"ev": "closed"})
//   un_core_tick(out, cap)               about every 50 ms: the events the timers caused (the game's output, the
//                                        imports, the update, the online section)
//   un_core_close()                      the window is gone: the online program is told to close
// Dialogs: an {"ev": "dialog", "dialog": id, "kind", "title", "message", ...} event; the window answers with
// {"op": "answer", "dialog": id, "value"} and the action goes on from there. Slow work (Python with numpy for the
// Uncharted 2 import, the Uncharted 2 menu overlay, the recipe fingerprints, the graphics card) runs in threads and goes
// on at the next tick. The multiplayer importers stay Python programs (the package's python\python.exe runs them).
#include "native.h"
#include "json.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
UN_API int un_controls(const char *what, const char *in_json, char *out, int cap);
UN_API int un_gpu_recommendation(const wchar_t *logs, char *out, int cap);
UN_API int un_gpu_query(const char *what, const char *name, int integrated, char *out, int cap);
UN_API int un_play_env(const char *config_json, const wchar_t *user_dir, char *out, int cap);
UN_API int un_play_start(const char *request_json, char *out, int cap);
UN_API int un_play_poll(char *out, int cap);
UN_API int un_play_stop(void);
UN_API int un_play_questions(const char *state_json, char *out, int cap);
UN_API int un_play_end(const char *state_json, char *out, int cap);
UN_API int un_action(const char *request_json, char *out, int cap);
UN_API int un_action_poll(char *out, int cap);
UN_API int un_updates(const char *request_json, char *out, int cap);
UN_API void un_update_cleanup(const wchar_t *root);
UN_API int un_shortcuts(const wchar_t *root, const char *request_json, char *out, int cap);
}

namespace {

using json::Value;

const char *const APP_TITLE = "Uncharted: The Nathan Drake Collection";
const char *const GAME_KEYS[] = {"u1", "u2", "u3"};

// ------------------------------------------------------------------------------------------------ helpers

int give(const std::string &s, char *out, int cap) {
    if ((int)s.size() + 1 > cap) return -(int)(s.size() + 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

std::wstring wide(const std::string &s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string utf8(const std::wstring &w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

long long now_ms() { return (long long)GetTickCount64(); }
double unix_now() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (double)(long long)(t - 116444736000000000ull) / 1e7;
}

bool exists(const std::string &p) { return GetFileAttributesW(wide(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
bool is_file(const std::string &p) {
    const DWORD a = GetFileAttributesW(wide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool is_dir(const std::string &p) {
    const DWORD a = GetFileAttributesW(wide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
long long mtime_of(const std::string &p) {   // -1: none
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(wide(p).c_str(), GetFileExInfoStandard, &a)) return -1;
    return (long long)(((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime);
}
long long size_of_file(const std::string &p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(wide(p).c_str(), GetFileExInfoStandard, &a)) return -1;
    return (long long)(((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow);
}
std::string read_file(const std::string &p, long long from = 0) {
    HANDLE h = CreateFileW(wide(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER at;
    at.QuadPart = from;
    SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
    std::string data;
    char buf[1 << 16];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) data.append(buf, got);
    CloseHandle(h);
    return data;
}
std::string join(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    return a + (a.back() == '\\' || a.back() == '/' ? "" : "\\") + b;
}
std::string parent_of(std::string p) {
    while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    const size_t cut = p.find_last_of("\\/");
    if (cut == std::string::npos) return p;
    return cut <= 2 && p.size() > 1 && p[1] == ':' ? p.substr(0, cut + 1) : p.substr(0, cut);
}
// Path(text) as Python shows it: forward slashes become backslashes, no trailing separator
std::string as_path(std::string p) {
    for (auto &c : p) if (c == '/') c = '\\';
    while (p.size() > 3 && p.back() == '\\') p.pop_back();
    return p;
}
std::string lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string strip(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
std::vector<std::string> lines_of(const std::string &t) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < t.size()) {
        size_t end = t.find_first_of("\r\n", at);
        if (end == std::string::npos) end = t.size();
        out.push_back(t.substr(at, end - at));
        at = end + (t.compare(end, 2, "\r\n") == 0 ? 2 : 1);
    }
    return out;
}

std::string text(const Value *v) { return v && v->kind == Value::String ? v->s : ""; }
std::string text(const Value &v, const char *key) { return text(v.get(key)); }
bool truthy(const Value *v) { return v && v->truthy(); }
Value array() { Value a; a.kind = Value::Array; return a; }
Value pair(const std::string &a, const std::string &b) { Value p = array(); p.items = {Value::text(a), Value::text(b)}; return p; }

// a call of the DLL's text convention: (status, text)
template <typename Call> std::pair<int, std::string> text_call(Call call, int room = 1 << 16) {
    std::string out(room, '\0');
    int status = call(&out[0], (int)out.size());
    if (status < 0) {
        out.assign(-status, '\0');
        status = call(&out[0], (int)out.size());
    }
    out.resize(std::strlen(out.c_str()));
    return {status, out};
}
Value json_call(int (*fn)(const char *, char *, int), const Value &request) {
    const std::string r = json::dump(request, false);
    auto t = text_call([&](char *o, int c) { return fn(r.c_str(), o, c); });
    Value v;
    if (t.first != 0 || !json::parse(t.second, v)) return Value::null();
    return v;
}

std::string sys(const char *what, const std::string &arg = "") {
    const std::wstring a = wide(arg);
    auto t = text_call([&](char *o, int c) { return un_system(what, a.c_str(), o, c); }, 1 << 12);
    return t.first == 0 ? t.second : "";
}
bool is_game_dir(const std::string &folder) { return !folder.empty() && sys("is_game_dir", folder) == "1"; }

Value controls(const char *what, const Value &in) {
    const std::string r = json::dump(in, false);
    auto t = text_call([&](char *o, int c) { return un_controls(what, r.c_str(), o, c); });
    Value v;
    json::parse(t.second, v);
    return v;
}

std::string fixed(double v, int digits) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", digits, v);
    return b;
}

// ------------------------------------------------------------------------------------------------ processes

std::wstring quote_arg(const std::wstring &a) {   // the C runtime's command line rules
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') { q.append(slashes * 2 + 1, L'\\'); q += c; slashes = 0; continue; }
        q.append(slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
}
std::wstring command_line(const std::vector<std::string> &args) {
    std::wstring line;
    for (auto &a : args) line += (line.empty() ? L"" : L" ") + quote_arg(wide(a));
    return line;
}

// the process's environment with these changes (value null: removed)
std::vector<wchar_t> environment(const std::vector<std::pair<std::string, Value>> &changes) {
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> vars;   // upper name -> (name, value)
    wchar_t *block = GetEnvironmentStringsW();
    for (wchar_t *p = block; p && *p; p += wcslen(p) + 1) {
        std::wstring entry = p;
        const size_t eq = entry.find(L'=', 1);
        if (eq == std::wstring::npos) continue;
        std::wstring name = entry.substr(0, eq), upper = name;
        for (auto &c : upper) c = (wchar_t)towupper(c);
        vars[upper] = {name, entry.substr(eq + 1)};
    }
    if (block) FreeEnvironmentStringsW(block);
    for (auto &c : changes) {
        std::wstring name = wide(c.first), upper = name;
        for (auto &ch : upper) ch = (wchar_t)towupper(ch);
        if (c.second.kind == Value::Null) vars.erase(upper);
        else vars[upper] = {name, wide(c.second.kind == Value::String ? c.second.s : json::dump(c.second, false))};
    }
    std::vector<wchar_t> out;
    for (auto &v : vars) {
        const std::wstring e = v.second.first + L"=" + v.second.second;
        out.insert(out.end(), e.begin(), e.end());
        out.push_back(0);
    }
    out.push_back(0);
    return out;
}

// runs a program without a window and returns (exit code, its output); -1 when it could not start or timed out
std::pair<int, std::string> run(const std::vector<std::string> &args, int timeout_ms, const std::string &cwd = "",
                                const std::vector<std::pair<std::string, Value>> &env_changes = {}) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return {-1, ""};
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring line = command_line(args);
    std::vector<wchar_t> buf(line.begin(), line.end());
    buf.push_back(0);
    std::vector<wchar_t> env = environment(env_changes);
    const std::wstring dir = wide(cwd);
    const BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                   env.data(), cwd.empty() ? nullptr : dir.c_str(), &si, &pi);
    CloseHandle(write_end);
    if (!ok) {
        CloseHandle(read_end);
        return {-1, ""};
    }
    std::string output;
    std::thread reader([&]() {
        char chunk[4096];
        DWORD got = 0;
        while (ReadFile(read_end, chunk, sizeof chunk, &got, nullptr) && got) output.append(chunk, got);
    });
    const bool done = WaitForSingleObject(pi.hProcess, (DWORD)timeout_ms) == WAIT_OBJECT_0;
    if (!done) TerminateProcess(pi.hProcess, 1);
    reader.join();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read_end);
    return {done ? (int)code : -1, output};
}

// a background program whose output goes to a file (the imports); its process handle, or null with the reason
HANDLE spawn_logged(const std::vector<std::string> &args, const std::string &cwd, const std::string &log, bool append,
                    std::string &error) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE file = CreateFileW(wide(log).c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "[WinError " + std::to_string(GetLastError()) + "] could not open " + log;
        return nullptr;
    }
    HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = file;
    si.hStdError = file;
    si.hStdInput = null_in;
    PROCESS_INFORMATION pi{};
    std::wstring line = command_line(args);
    std::vector<wchar_t> buf(line.begin(), line.end());
    buf.push_back(0);
    std::vector<wchar_t> env = environment({{"PYTHONIOENCODING", Value::text("utf-8")}});
    const std::wstring dir = wide(cwd);
    // below normal priority: an import never slows a running game down
    const BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | BELOW_NORMAL_PRIORITY_CLASS, env.data(),
                                   dir.c_str(), &si, &pi);
    const DWORD why = GetLastError();
    CloseHandle(file);
    if (null_in != INVALID_HANDLE_VALUE) CloseHandle(null_in);
    if (!ok) {
        error = "[WinError " + std::to_string(why) + "] could not start " + args[0];
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

bool process_running(HANDLE h) { return h && WaitForSingleObject(h, 0) == WAIT_TIMEOUT; }
bool pid_alive(const Value *pid) {
    if (!pid) return false;
    long long n = pid->kind == Value::Int ? pid->i : pid->kind == Value::String ? std::atoll(pid->s.c_str()) : 0;
    return n > 0 && un_pid_alive((unsigned long)n);
}
void kill_tree(long long pid) {
    run({"taskkill", "/PID", std::to_string(pid), "/T", "/F"}, 15000);
}

}  // namespace

namespace {

// ------------------------------------------------------------------------------------------------ the launcher's state

struct Import {                       // one multiplayer import (Uncharted 3's or 2's)
    HANDLE proc = nullptr;            // started here
    std::string automatic;            // why the launcher started it (an update), else ""
    long long log_pos = 0;
    double n = 0, total = 0;          // the progress bar
    std::string step;                 // the current step's line
    long long steps_n = 1, steps_total = 1;
    bool polled = false;
};

using Fn = std::function<void()>;
using Answer = std::function<void(const Value &)>;

struct Core {
    bool open = false, closing = false;
    std::string root, user_dir, u3mp_dir, u2mp_dir, config_file, prefix_file;
    bool wine = false;
    Value config = Value::object(), ini = Value::object(), games = array(), ini_defaults = Value::object();
    std::string direct;
    std::string tab = "setup";
    std::string status, detail;
    std::string gpu_name, recommended;
    long long ini_seen = -2;
    // the game
    bool playing = false, stopping = false, game_started = false;
    double started_at = 0;
    long long last_output = 0;
    int poll_count = 0;
    // the launcher's update, TOOLS' lines
    std::string update_button;
    bool update_shown = false;
    bool hold_imports = false;   // the launcher's own update goes first (updates.cpp "hold_imports")
    Value update_view;
    std::string progress_game_dir, progress_update;
    // the imports
    Import u3, u2;
    std::string u3_block, u2_block;
    bool banner = false;
    std::string u3_shipped, u2_shipped;
    bool u3_shipped_known = false, u2_shipped_known = false;
    Value release;
    bool release_read = false;
    // the online program (online\uncharted_online.exe, when the package has it): a separate process, started hidden,
    // that the launcher talks to over its stdin/stdout, one JSON object per line each way
    bool online = false;
    HANDLE online_process = nullptr, online_in = nullptr, online_out = nullptr;
    std::string online_pending;   // what the program wrote past the last full line
    long long online_next = 0;
    // dialogs, timers, background work
    int next_dialog = 1;
    std::map<int, Answer> waiting;
    std::vector<Value> out;
    std::string last_model;
    struct Timer { long long at; Fn fn; };
    std::vector<Timer> timers;
    std::mutex done_lock;
    std::deque<Fn> done;
};
Core g;

void emit(Value v) { g.out.push_back(std::move(v)); }
void after(long long ms, Fn fn) { g.timers.push_back({now_ms() + ms, std::move(fn)}); }

// work in a thread; `then` runs on the launcher's own thread at the next tick
template <typename Work> void background(Work work, std::function<void()> then) {
    std::thread([work, then]() mutable {
        work();
        std::lock_guard<std::mutex> l(g.done_lock);
        g.done.push_back(then);
    }).detach();
}

// a message box: info | warning | error (answer ignored) or yesno | okcancel | retrycancel (answer: true / false)
void ask(const char *kind, const std::string &message, Answer then = nullptr) {
    const int id = g.next_dialog++;
    Value e = Value::object();
    e.set("ev", Value::text("dialog"));
    e.set("dialog", Value::integer(id));
    e.set("kind", Value::text(kind));
    e.set("title", Value::text(APP_TITLE));
    e.set("message", Value::text(message));
    e.set("detail", Value::text(""));
    emit(std::move(e));
    g.waiting[id] = then ? then : [](const Value &) {};
}
void tell(const char *kind, const std::string &message) { ask(kind, message); }

// a file or folder dialog (kind: directory | open); the answer is the path, "" when cancelled
void ask_path(const char *kind, const std::string &title, const std::string &initialdir, const Value &filetypes, Answer then) {
    const int id = g.next_dialog++;
    std::string filter;
    for (auto &t : filetypes.items)
        if (t.items.size() == 2) filter += (filter.empty() ? "" : ";;") + t.items[0].s + " (" + t.items[1].s + ")";
    Value e = Value::object();
    e.set("ev", Value::text("dialog"));
    e.set("dialog", Value::integer(id));
    e.set("kind", Value::text(kind));
    e.set("title", Value::text(title));
    e.set("initialdir", Value::text(initialdir));
    e.set("initialfile", Value::text(""));
    e.set("filetypes", Value::text(filter));
    emit(std::move(e));
    g.waiting[id] = then;
}

void set_status(const std::string &text, const std::string *detail = nullptr) {
    g.status = text;
    if (detail) g.detail = *detail;
}
void set_status(const std::string &text, const std::string &detail) { set_status(text, &detail); }

// ------------------------------------------------------------------------------------------------ settings

std::string appdata_file() {
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, 32768);
    std::string base = n && n < 32768 ? utf8(std::wstring(buf, n)) : "";
    if (base.empty()) {
        n = GetEnvironmentVariableW(L"USERPROFILE", buf, 32768);
        base = n && n < 32768 ? utf8(std::wstring(buf, n)) : ".";
    }
    return join(join(base, "Uncharted Launcher"), "launcher.json");
}

void write_config() {
    const std::string j = json::dump(g.config, false);
    un_config_write(wide(g.config_file).c_str(), j.c_str());
}

Value read_ini(const std::string &game) {
    const std::wstring u = wide(g.user_dir);
    auto t = text_call([&](char *o, int c) { return un_ini_read(u.c_str(), game.c_str(), o, c); });
    Value v = Value::object();
    for (auto &line : lines_of(t.second)) {
        const size_t tab = line.find('\t');
        if (tab != std::string::npos) v.set(line.substr(0, tab), Value::text(line.substr(tab + 1)));
    }
    return v;
}
std::string ini_file(const std::string &game) { return join(g.user_dir, "probe-" + game + ".ini"); }
void write_ini(const std::string &game, const Value &updates) {
    std::string lines;
    for (auto &f : updates.fields)
        lines += f.first + "\t" + (f.second.kind == Value::String ? f.second.s : json::dump(f.second, false)) + "\n";
    un_ini_write(wide(g.user_dir).c_str(), game.c_str(), lines.c_str());
}

std::string game_key() { return text(g.config, "game"); }
Value &game_settings() {
    Value *games = g.config.get("games");
    if (!games) games = &g.config.set("games", Value::object());
    Value *s = games->get(game_key());
    if (!s) s = &games->set(game_key(), Value::object());
    return *s;
}
const Value *game_entry(const std::string &key) {
    for (auto &game : g.games.items)
        if (text(game, "key") == key) return &game;
    return nullptr;
}
std::string game_name() {
    const Value *e = game_entry(game_key());
    return e ? text(*e, "name") : "";
}

// copies the bindings stored in a game's ini into launcher.json's game block; returns false if the ini has no
// bindings written yet
bool controls_from_ini(const Value &values, Value &settings) {
    Value in = Value::object();
    in.set("values", values);
    in.set("settings", settings);
    const Value r = controls("from_ini", in);
    if (const Value *s = r.get("settings")) settings = *s;
    return truthy(r.get("found"));
}

void save_ini(const Value &updates) {
    write_ini(game_key(), updates);
    g.ini_seen = mtime_of(ini_file(game_key()));
}
void reload_ini() {
    g.ini = read_ini(game_key());
    g.ini_seen = mtime_of(ini_file(game_key()));
    if (controls_from_ini(g.ini, game_settings())) write_config();
}
void save_controls() {
    write_config();
    save_ini(controls("to_ini", game_settings()));
}
void follow_ini() {   // picks up the ini again after the in-game (Insert) menu was closed and wrote to it
    if (mtime_of(ini_file(game_key())) != g.ini_seen) reload_ini();
}

const char *const CONTROL_KEYS[] = {"mouse", "mouse_sens", "mouse_invert", "prompts", "mouse_aim_sens", "mouse_vertical",
                                    "mouse_invert_x", "vibration", "controls", "pad_controls"};

void change_ini(const std::string &key, const Value &value) {   // edits from a page switch the preset to Custom
    g.ini.set(key, value);
    Value u = Value::object();
    u.set(key, value);
    save_ini(u);
    if (text(game_settings(), "preset") != "custom") {
        game_settings().set("preset", Value::text("custom"));
        write_config();
    }
}
void change_game_setting(const std::string &key, const Value &value) {
    game_settings().set(key, value);
    for (const char *k : CONTROL_KEYS)
        if (key == k) { save_controls(); return; }
    write_config();
}
void change_config(const std::string &key, const Value &value) {
    g.config.set(key, value);
    write_config();
}
void change_all_games(const std::string &key, const Value &value) {   // written to all three inis, preset untouched
    g.ini.set(key, value);
    Value u = Value::object();
    u.set(key, value);
    for (const char *game : GAME_KEYS)
        if (game != game_key()) write_ini(game, u);
    save_ini(u);
}

// ------------------------------------------------------------------------------------------------ folders

std::string game_dir() {
    const std::string folder = text(g.config, "game_dir");
    if (folder.empty()) return join(join(join(g.root, "game"), "CUSA02344"), "uroot");
    if (g.wine && !exists(folder)) {   // the path came from a different Wine prefix (e.g. a second Steam shortcut):
                                       // try the stored Linux path mapped into this prefix
        const std::string here = sys("windows_path", text(g.config, "game_dir_unix"));
        if (!here.empty() && exists(here)) return as_path(here);
    }
    return as_path(folder);
}
std::string chosen_update() {   // update_dir from TOOLS (or its uroot), "" once it no longer exists
    const std::string folder = text(g.config, "update_dir");
    if (folder.empty()) return "";
    for (const std::string &candidate : {as_path(folder), join(as_path(folder), "uroot")})
        if (is_game_dir(candidate)) return candidate;
    return "";
}
std::string update_dir() {
    const std::string chosen = chosen_update();
    if (!chosen.empty()) return chosen;
    const std::string found = sys("update_dir", game_dir());
    return found.empty() ? "" : as_path(found);
}

// TOOLS' lines: the game folder, the game update and the settings file (an install shows its step instead)
std::string update_text() {
    const std::string update = update_dir();
    if (!update.empty()) {
        const std::string v = sys("app_version", update);
        return "Using game update " + (v.empty() ? std::string("?") : v) + ": " + update;
    }
    const std::string version = sys("app_version", game_dir());
    if (!version.empty() && version >= "01.02")
        return "The game folder is version " + version + " and already includes the update.";
    return "The game folder is version " + (version.empty() ? std::string("?") : version) +
           ", and no update was found. To get the 1.02 fixes, add your update 1.02 folder or its .pkg in "
           "step 2 on the Setup tab.";
}
// SETUP's first two steps: a game folder the starter accepts, and update 1.02 (a chosen or found update folder, or a
// game folder that is already 01.02 or later)
Value setup_steps() {
    Value s = Value::object();
    const std::string dir = game_dir();
    s.set("game", Value::boolean(!dir.empty() && is_game_dir(dir)));
    const std::string version = dir.empty() ? "" : sys("app_version", dir);
    s.set("update", Value::boolean(!update_dir().empty() || (!version.empty() && version >= "01.02")));
    return s;
}
Value tools_lines() {
    Value t = Value::object();
    t.set("game_dir", Value::text(!g.progress_game_dir.empty() ? g.progress_game_dir : "Game folder: " + game_dir()));
    t.set("update", Value::text(!g.progress_update.empty() ? g.progress_update : update_text()));
    t.set("ini", Value::text("Settings file: " + ini_file(game_key())));
    t.set("steps", setup_steps());
    return t;
}

// ------------------------------------------------------------------------------------------------ titles

std::string version_suffix(const std::string &imported) {
    static std::string launcher, recipe;
    static bool read = false;
    if (!read) {
        launcher = sys("launcher_version", g.root);
        recipe = sys("recipe_version", g.u3mp_dir);
        read = true;
    }
    return " - Unofficial Port - Launcher Version: " + launcher + " - Recipe Version: " + recipe +
           (imported.empty() ? "" : " - Imported v" + imported);
}

}  // namespace

namespace {

// ------------------------------------------------------------------------------------------------ Python for the importers

// the package's python\python.exe (the importers are Python programs), else one on PATH
std::string python_exe() {
    const std::string own = join(join(g.root, "python"), "python.exe");
    if (is_file(own)) return own;
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, found, nullptr)) return utf8(found);
    return "python.exe";
}

// a Python expression printed by the package's Python with `folder` on its path (the recipes' fingerprints, ...)
std::string python_print(const std::string &folder, const std::string &code) {
    auto r = run({python_exe(), "-c", "import sys; sys.path.insert(0, sys.argv[1]); " + code, folder}, 120000, folder);
    return r.first == 0 ? strip(r.second) : "";
}

// ------------------------------------------------------------------------------------------------ Uncharted 3 multiplayer

std::string u3_data() {
    const std::string chosen = text(g.config, "u3mp_data");
    return as_path(!chosen.empty() ? chosen : sys("u3mp_default_data"));
}
Value read_json(const std::string &path) {
    Value v;
    if (!json::parse(read_file(path), v) || !v.is_object()) return Value::object();
    return v;
}
Value u3_state() { return read_json(join(u3_data(), "state.json")); }
bool u3_running() {
    if (process_running(g.u3.proc)) return true;
    const Value s = u3_state();
    return text(s, "status") == "running" && pid_alive(s.get("pid"));
}
const Value &u3_release() {
    if (!g.release_read) {
        g.release_read = true;
        g.release = Value::null();
        const std::string t = sys("u3mp_release", g.u3mp_dir);
        if (!t.empty()) json::parse(t, g.release);
    }
    return g.release;
}
bool u3_on() {
    const std::string s = text(u3_state(), "status");
    return s == "imported" || s == "partial";
}
std::string u3_imported_version() {
    const Value s = u3_state();
    const std::string status = text(s, "status");
    if (status != "imported" && status != "partial") return "";
    const Value *image = s.get("image");
    const Value *v = image ? image->get("version") : nullptr;
    if (!v || !v->truthy()) return "";
    return v->kind == Value::String ? v->s : json::dump(*v, false);
}
std::string title() { return std::string(APP_TITLE) + version_suffix(u3_imported_version()); }

// (ready, why not): an imported multiplayer image of the shipped build, and its overlays
std::pair<bool, std::string> u3_ready(const Value &s) {
    const std::string status = text(s, "status");
    if (status != "imported" && status != "partial") return {false, "not imported"};
    std::string broken;   // dc1 modules without their modules.bin stop Uncharted 3 at boot
    if (const Value *failed = s.get("failed"))
        for (const char *step : {"convert-dc1", "dc-recipes"})
            for (auto &f : failed->items)
                if (f.kind == Value::String && f.s == step) broken += (broken.empty() ? "" : ", ") + std::string(step);
    if (!broken.empty()) return {false, "the import step " + broken + " failed: press RE-IMPORT / UPDATE"};
    const Value &release = u3_release();
    const Value *image = s.get("image");
    const std::string sha = image ? text(*image, "sha256") : "";
    if (!release.is_object() || sha != text(release, "image_sha256")) return {false, "imported for another multiplayer build: import again"};
    if (!is_file(image ? text(*image, "path") : "")) return {false, "the multiplayer image is missing: import again"};
    bool any = false;
    if (const Value *o = s.get("overlays"))
        for (auto &p : o->items) any = any || (p.kind == Value::String && is_dir(p.s));
    if (!any) return {false, "the overlays are missing: import again"};
    return {true, ""};
}

std::string u3_shipped_id() {   // the shipped recipe's fingerprint (u3mp_version.py), worked out once
    if (!g.u3_shipped_known) {
        g.u3_shipped = python_print(g.u3mp_dir, "import u3mp_version; print(u3mp_version.shipped_id(sys.argv[1]))");
        g.u3_shipped_known = true;
    }
    return g.u3_shipped;
}
std::string u3_recipe_version() { return strip(read_file(join(join(g.u3mp_dir, "recipe"), "VERSION"))); }

// what a launcher update changed in an existing import: "image", "recipe", "image+recipe" or ""
std::string u3_pending(const Value &s) {
    const std::string status = text(s, "status");
    if (status != "imported" && status != "partial") return "";
    std::vector<std::string> parts;
    const Value &release = u3_release();
    const Value *image = s.get("image");
    if (release.is_object() && (image ? text(*image, "sha256") : "") != text(release, "image_sha256")) parts.push_back("image");
    const std::string shipped = u3_shipped_id();
    const std::string recorded = text(s, "recipe_id");
    if (!shipped.empty() && (!recorded.empty() ? recorded != shipped : text(s, "recipe_version") != u3_recipe_version())) {
        const Value *next = s.get("next");
        if (text(s, "update_failed_for") != shipped || (next && text(*next, "recipe_id") == shipped)) parts.push_back("recipe");
    }
    std::string out;
    for (auto &p : parts) out += (out.empty() ? "" : "+") + p;
    return out;
}

std::string u3_dns() {   // the Multiplayer tab's DNS, else the in-game RPCN settings' dns= line
    const std::string v = strip(text(g.config, "u3mp_dns"));
    if (!v.empty()) return v;
    for (auto &line : lines_of(read_file(join(g.user_dir, "u3-rpcn.cfg"))))
        if (line.compare(0, 4, "dns=") == 0) return strip(line.substr(4));
    return "";
}

void u3_import(bool silent, bool image_only = false, const std::string &reason = "", const std::string &keep = "");

// the gold banner and PLAY follow the multiplayer updates (every second, and when the game changes)
std::string u3_block_reason();
std::string u2_block_reason();
bool blocked() {
    const std::string game = game_key();
    return !g.playing && ((game == "u3" && !g.u3_block.empty()) || (game == "u2" && !g.u2_block.empty()));
}
void gate() {
    const std::string r3 = u3_block_reason(), r2 = u2_block_reason();
    const bool changed = r3 != g.u3_block || r2 != g.u2_block;
    g.u3_block = r3;
    g.u2_block = r2;
    g.banner = !r3.empty() || !r2.empty();
    if (changed && !g.playing) {
        if (blocked())
            set_status("Uncharted " + std::string(game_key() == "u2" ? "2" : "3") + " is updating its multiplayer files.",
                       "PLAY unlocks when the update finishes.");
        else
            set_status(game_name() + " is ready to play.");
    }
}

std::string u3_block_reason() {
    const Value s = u3_state();
    if (u3_running()) return s.get("image") && s.get("image")->truthy() ? "running" : "";
    const std::string status = text(s, "status");
    if ((status == "imported" || status == "partial") && !u3_pending(s).empty()) return "pending";
    return "";
}

void u2_auto_update();
void u3_auto_update();

// The imports' automatic updates wait for the launcher's own update: while its check runs, or (Automatic mode) while a
// newer launcher waits to install or installs, nothing starts; that launcher restarts with its own recipe and runs the
// import once, instead of an import for the old recipe and then another one. Checked again every second (at most
// about 2 minutes, in case the check never answers).
void auto_updates_after_launcher() {
    static int waited = 0;
    if (g.hold_imports && waited < 120) {
        ++waited;
        after(1000, auto_updates_after_launcher);
        return;
    }
    u3_auto_update();
    after(500, u2_auto_update);
}

void u3_auto_update() {   // after a launcher update: an existing import brought up to date in the background
    if (u3_running()) return;
    const std::string pending = u3_pending(u3_state());
    if (!pending.empty())
        u3_import(true, pending == "image",
                  "the launcher update brought a new multiplayer " + std::string(pending == "image" ? "build" : "version"));
}

void u3_import(bool silent, bool image_only, const std::string &reason, const std::string &keep) {
    if (u3_running()) {
        if (!silent) tell("info", "The import is already running.");
        return;
    }
    const std::string game = game_dir();
    if (!is_game_dir(game)) {
        if (!silent) tell("info", "Choose your PS4 Uncharted: The Nathan Drake Collection folder first (SETUP -> Change game folder).");
        return;
    }
    std::vector<std::string> folders;
    if (const Value *f = g.config.get("u3mp_ps3_u3"))
        for (auto &x : f->items) if (x.kind == Value::String && is_dir(x.s)) folders.push_back(x.s);
    const Value s = u3_state();
    const Value *kept_v = s.get("ps3_kept");
    const bool kept = kept_v && truthy(kept_v->get("complete"));
    if (folders.empty() && !image_only && !kept && keep != "off") {
        if (!silent) tell("info", "Add your PS3 Uncharted 3 folder(s) first: the game disc's folder and RPCS3's dev_hdd0/game.");
        return;
    }
    const std::string data = u3_data();
    SHCreateDirectoryExW(nullptr, wide(data).c_str(), nullptr);
    ULARGE_INTEGER free_bytes{};
    if (!GetDiskFreeSpaceExW(wide(data).c_str(), &free_bytes, nullptr, nullptr)) {
        if (!silent) tell("error", "The multiplayer data folder can't be used:\n" + data);
        return;
    }
    auto start = [silent, image_only, reason, keep, game, folders, data]() {
        std::vector<std::string> cmd = {python_exe(), "-u", join(g.u3mp_dir, "u3mp_import.py"), "--ps4", game, "--data", data,
                                        "--port", g.root};
        const std::string update = update_dir();
        if (!update.empty()) { cmd.push_back("--ps4-update"); cmd.push_back(update); }
        for (auto &f : folders) { cmd.push_back("--ps3-u3"); cmd.push_back(f); }
        if (!text(g.config, "u3mp_ps3_u2").empty()) { cmd.push_back("--ps3-u2"); cmd.push_back(text(g.config, "u3mp_ps3_u2")); }
        if (!text(g.config, "u3mp_ps3_elf").empty()) { cmd.push_back("--ps3-elf"); cmd.push_back(text(g.config, "u3mp_ps3_elf")); }
        if (!u3_dns().empty()) { cmd.push_back("--dns"); cmd.push_back(u3_dns()); }
        if (image_only) cmd.push_back("--image-only");
        if (!keep.empty()) cmd.push_back("--keep-ps3-" + keep);
        else if (truthy(g.config.get("u3mp_keep_ps3"))) cmd.push_back("--keep-ps3");
        const std::string log = join(data, "import.log");
        const long long log_start = !keep.empty() && is_file(log) ? size_of_file(log) : 0;
        std::string error;
        HANDLE proc = spawn_logged(cmd, g.u3mp_dir, log, !keep.empty(), error);
        if (!proc) {
            if (!silent) tell("error", "Could not start the import:\n" + error);
            return;
        }
        if (g.u3.proc) CloseHandle(g.u3.proc);
        g.u3.proc = proc;
        g.u3.automatic = silent ? reason : "";
        g.u3.log_pos = log_start;   // a keep run appends to the import log: only its lines
        g.u3.n = g.u3.total = 0;
        g.u3.step = silent ? "Updating the multiplayer files: " + reason + "..."
                           : keep == "only" ? "Keeping the PS3 files..." : !keep.empty() ? "Removing the kept PS3 files..."
                                                                                       : "Starting the import...";
    };
    const double free_gb = (double)free_bytes.QuadPart / (1024.0 * 1024.0 * 1024.0);
    if (free_gb < 12 && !image_only && keep.empty()) {
        const std::string anchor = data.size() >= 3 && data[1] == ':' ? data.substr(0, 3) : data;
        if (silent) return;   // an update never starts without the room
        ask("yesno", anchor + " has " + fixed(free_gb, 0) + " GB free; the import needs about 12 GB. Start anyway?",
            [start](const Value &yes) { if (yes.truthy()) start(); });
        return;
    }
    start();
}

// Automatic updates: a newer launcher goes first (updates.cpp asks with "stop_staged_imports"). An Uncharted 3 import
// update builds in its own copy (<data>/recipe-next, state "staging") while the current files stay as they are, so it
// is stopped and marked; the importer then deletes that copy (u3mp_import.py) and the new launcher runs the update again.
// A first import, or an update made in place (too little disk space), writes the import itself: it is left to finish.
void u3_stop_staged() {
    if (!u3_running()) return;
    const Value s = u3_state();
    if (!truthy(s.get("staging"))) return;
    long long pid = 0;
    if (process_running(g.u3.proc)) pid = (long long)GetProcessId(g.u3.proc);
    else if (const Value *p = s.get("pid")) pid = p->kind == Value::Int ? p->i : 0;
    if (!pid) return;
    kill_tree(pid);
    const std::string marker = join(join(u3_data(), "recipe-next"), ".stopped");
    HANDLE f = CreateFileW(wide(marker).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD n = 0;
        WriteFile(f, "launcher update\n", 16, &n, nullptr);
        CloseHandle(f);
    }
    g.u3.step = "Import update stopped: the launcher updates first, then runs it again.";
}

void u3_cancel() {
    long long pid = 0;
    if (process_running(g.u3.proc)) pid = (long long)GetProcessId(g.u3.proc);
    else if (const Value *p = u3_state().get("pid")) pid = p->kind == Value::Int ? p->i : 0;
    if (!pid) return;
    ask("yesno", "Stop the import? Finished steps are kept; Import continues it.", [pid](const Value &yes) {
        if (!yes.truthy()) return;
        kill_tree(pid);
        g.u3.step = "Import stopped.";
    });
}

void u3_keep_ps3(bool on) {   // keep only the PS3 files updates need (ps3keep.py does the work)
    const Value s = u3_state();
    const Value *kept_v = s.get("ps3_kept");
    const bool kept = kept_v && truthy(kept_v->get("complete"));
    bool any_folder = false;
    if (const Value *f = g.config.get("u3mp_ps3_u3"))
        for (auto &x : f->items) any_folder = any_folder || (x.kind == Value::String && is_dir(x.s));
    auto apply = [on]() {
        if (u3_running()) {
            tell("info", "The import is running. Change this when it has finished.");
            return;
        }
        change_config("u3mp_keep_ps3", Value::boolean(on));
        const std::string status = text(u3_state(), "status");
        if (on && (status == "imported" || status == "partial")) u3_import(false, false, "", "only");
        else if (!on && exists(join(u3_data(), "ps3-kept"))) u3_import(false, false, "", "off");
    };
    if (!on && kept && !any_folder) {
        ask("yesno", "Your PS3 Uncharted 3 folders are not there any more, so the kept PS3 files are the only copy the "
                     "import can use. Turning this off deletes them, and updates then need your PS3 game again.\n\n"
                     "Turn it off anyway?",
            [apply](const Value &yes) { if (yes.truthy()) apply(); });
        return;
    }
    apply();
}

// the import's output: its progress events (@U3MP / @U2MP lines) since the last read
template <typename OnEvent> void read_events(Import &imp, const std::string &log, const char *marker, OnEvent on_event) {
    if (!is_file(log)) return;
    const long long size = size_of_file(log);
    if (size < imp.log_pos) imp.log_pos = 0;   // a new run rewrote the file
    const std::string chunk = read_file(log, imp.log_pos);
    const size_t cut = chunk.rfind('\n');
    if (cut == std::string::npos) return;
    imp.log_pos += (long long)cut + 1;
    for (auto &line : lines_of(chunk.substr(0, cut + 1))) {
        if (line.compare(0, std::strlen(marker), marker) != 0) continue;
        Value ev;
        if (json::parse(line.substr(std::strlen(marker)), ev) && ev.is_object()) on_event(ev);
    }
}
double num(const Value *v) { return !v ? 0 : v->kind == Value::Float ? v->f : (double)v->i; }

void u3_poll() {
    read_events(g.u3, join(u3_data(), "import.log"), "@U3MP ", [](const Value &ev) {
        const std::string phase = text(ev, "phase");
        if (phase == "image") {
            g.u3.step = "Multiplayer image: " + text(ev, "status");
        } else if (phase == "recipe" && ev.get("step")) {
            g.u3.n = num(ev.get("step")) - (text(ev, "status") != "running" ? 0 : 1);
            g.u3.total = num(ev.get("total"));
            g.u3.step = "Step " + std::to_string((long long)num(ev.get("step"))) + "/" + std::to_string((long long)num(ev.get("total"))) +
                        ": " + text(ev, "title").substr(0, 90);
        } else if (phase == "ps3keep") {
            g.u3.step = text(ev, "note");
        } else if (phase == "done") {
            g.u3.n = g.u3.total;
            const std::string status = text(ev, "status");
            g.u3.step = status == "imported" || status == "partial" ? "Import finished." : "Import failed: " + text(ev, "error");
        }
    });
    if (g.u3.proc && !process_running(g.u3.proc)) {
        CloseHandle(g.u3.proc);
        g.u3.proc = nullptr;
        g.u3.automatic.clear();
    }
    gate();
}

bool u3_confirm_play();
bool u2_confirm_play();

// ------------------------------------------------------------------------------------------------ Uncharted 2 multiplayer

std::string u2_dir() { return join(join(g.user_dir, "u2mp"), "import"); }
Value u2_state() { return read_json(join(u2_dir(), "state.json")); }
bool u2_running() {
    if (process_running(g.u2.proc)) return true;
    const Value s = u2_state();
    return text(s, "status") == "running" && pid_alive(s.get("pid"));
}
std::string u2_shipped_id() {
    if (!g.u2_shipped_known) {
        g.u2_shipped = python_print(join(join(g.root, "u2mp"), "patcher"), "import u2mp_version; print(u2mp_version.shipped_id())");
        g.u2_shipped_known = true;
    }
    return g.u2_shipped;
}
bool u2_pending(const Value &s) {
    const std::string status = text(s, "status");
    if (status != "imported" && status != "partial") return false;
    if (!is_file(join(u2_dir(), "import.log"))) return false;   // a state.json copied in is not an import of this PC
    const std::string shipped = u2_shipped_id();
    return !shipped.empty() && text(s, "recipe_id") != shipped && text(s, "update_failed_for") != shipped;
}
std::string u2_block_reason() {
    const Value s = u2_state();
    if (u2_running()) return truthy(s.get("recipe_id")) || truthy(s.get("updating")) ? "running" : "";
    if (u2_pending(s)) return "pending";
    return "";
}

// the PS3 game-data folder: the RPCS3 row, else a folder holding <id>DATA2 folders above the chosen ones, else ""
std::string u2_data_folder() {
    const std::string chosen = text(g.config, "u2mp_ps3_data");
    if (!chosen.empty() && is_dir(chosen)) return chosen;
    std::vector<std::string> candidates = {text(g.config, "u2mp_ps3")};
    if (const Value *f = g.config.get("u3mp_ps3_u3")) for (auto &x : f->items) candidates.push_back(text(&x));
    for (auto &folder : candidates) {
        if (folder.empty()) continue;
        std::string q = as_path(folder);
        for (int up = 0; up <= 5; ++up) {
            if (is_dir(q)) {
                WIN32_FIND_DATAW fd;
                HANDLE h = FindFirstFileW(wide(join(q, "*")).c_str(), &fd);
                bool found = false;
                if (h != INVALID_HANDLE_VALUE) {
                    do {
                        const std::string n = utf8(fd.cFileName);
                        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n.size() >= 5 &&
                            lower(n.substr(n.size() - 5)) == "data2") found = true;
                    } while (!found && FindNextFileW(h, &fd));
                    FindClose(h);
                }
                if (found) return q;
            }
            const std::string p = parent_of(q);
            if (p == q) break;
            q = p;
        }
    }
    return "";
}

// a Python with numpy for the Uncharted 2 import: the package's, else one the py launcher lists ("" none)
std::vector<std::string> python_candidates() {
    std::vector<std::string> list = {python_exe()};
    auto r = run({"py", "-0p"}, 10000);
    if (r.first == 0)
        for (auto &line : lines_of(r.second)) {
            const size_t sp = line.find_last_of(" \t");
            const std::string path = strip(sp == std::string::npos ? line : line.substr(sp + 1));
            if (lower(path).size() >= 10 && lower(path).compare(lower(path).size() - 10, 10, "python.exe") == 0) list.push_back(path);
        }
    return list;
}
std::string python_with_numpy(const std::vector<std::string> &candidates) {
    for (auto &p : candidates)
        if (is_file(p) && run({p, "-c", "import numpy"}, 60000).first == 0) return p;
    return "";
}

void u2_import(bool silent, const std::string &reason = "") {
    if (u2_running()) {
        if (!silent) tell("info", "The import is already running.");
        return;
    }
    const std::string ps3 = text(g.config, "u2mp_ps3");
    if (ps3.empty()) {
        if (!silent) tell("info", "Choose your PS3 Uncharted 2 folder first.");
        return;
    }
    const std::string out = u2_dir();
    SHCreateDirectoryExW(nullptr, wide(out).c_str(), nullptr);
    auto launch = [silent, reason, ps3, out](const std::string &python) {
        std::vector<std::string> cmd = {python, "-u", join(join(join(g.root, "u2mp"), "patcher"), "u2mp_import.py"), "--ps4",
                                        game_dir(), "--ps3", ps3, "--out", out};
        const std::string data = u2_data_folder();   // GOTY data only: the game data folder, no map pack folder
        if (!data.empty()) { cmd.push_back("--ps3-data"); cmd.push_back(data); }
        if (silent) cmd.push_back("--update");
        std::string error;
        HANDLE proc = spawn_logged(cmd, join(join(g.root, "u2mp"), "patcher"), join(out, "launcher-run.log"), false, error);
        if (!proc) {
            if (!silent) tell("error", "Could not start the import:\n" + error);
            return;
        }
        if (g.u2.proc) CloseHandle(g.u2.proc);
        g.u2.proc = proc;
        g.u2.automatic = silent ? reason : "";
        g.u2.log_pos = 0;
        g.u2.n = g.u2.total = 0;
        g.u2.step = silent ? "Updating the multiplayer files: " + reason + "..." : "Starting the import...";
    };
    auto found = std::make_shared<std::string>();
    auto candidates = std::make_shared<std::vector<std::string>>();
    g.u2.step = "Looking for Python with numpy...";
    background([found, candidates]() {
        *candidates = python_candidates();
        *found = python_with_numpy(*candidates);
    }, [found, candidates, silent, launch]() {
        g.u2.step.clear();
        if (!found->empty()) { launch(*found); return; }
        auto none = [silent]() {
            if (!silent)
                tell("error", "The Uncharted 2 import needs Python's numpy (for the characters, maps and menu pictures), and no "
                              "Python on this PC has it.\n\nInstall it with:  py -m pip install numpy\n(the launcher's release "
                              "package already carries it).");
        };
        if (silent) return;
        ask("yesno", "The Uncharted 2 import needs Python's numpy package (for the characters, maps and menu pictures), and no "
                     "Python on this PC has it.\n\nInstall numpy now? (pip downloads it from the Python Package Index into your "
                     "user packages; this takes a minute.)",
            [candidates, launch, none](const Value &yes) {
                if (!yes.truthy()) { none(); return; }
                auto installed = std::make_shared<std::string>();
                g.u2.step = "Installing numpy...";
                background([candidates, installed]() {
                    for (auto &p : *candidates) {
                        if (!is_file(p)) continue;
                        auto r = run({p, "-m", "pip", "install", "--user", "--only-binary=:all:", "--disable-pip-version-check",
                                      "numpy"}, 900000);
                        if (r.first == 0 && run({p, "-c", "import numpy"}, 60000).first == 0) { *installed = p; return; }
                    }
                }, [installed, launch, none]() {
                    g.u2.step.clear();
                    if (installed->empty()) none();
                    else launch(*installed);
                });
            });
    });
}

void u2_cancel() {
    long long pid = 0;
    if (process_running(g.u2.proc)) pid = (long long)GetProcessId(g.u2.proc);
    else if (const Value *p = u2_state().get("pid")) pid = p->kind == Value::Int ? p->i : 0;
    if (!pid) return;
    ask("yesno", "Stop the import? Finished files are kept; Import continues it.", [pid](const Value &yes) {
        if (!yes.truthy()) return;
        kill_tree(pid);
        g.u2.step = "Import stopped.";
    });
}

void u2_auto_update() {
    if (!u2_running() && u2_pending(u2_state())) u2_import(true, "the launcher update changed the Uncharted 2 multiplayer files");
}

void u2_poll() {
    read_events(g.u2, join(u2_dir(), "launcher-run.log"), "@U2MP ", [](const Value &ev) {
        const std::string phase = text(ev, "phase");
        if (phase == "recipe" && ev.get("step")) {
            g.u2.steps_n = (long long)num(ev.get("step"));
            g.u2.steps_total = (long long)num(ev.get("total"));
            g.u2.n = num(ev.get("step")) - 1;
            g.u2.total = num(ev.get("total"));
            g.u2.step = "Step " + std::to_string(g.u2.steps_n) + "/" + std::to_string(g.u2.steps_total) + ": " +
                        text(ev, "title").substr(0, 90);
        } else if (phase == "item" && num(ev.get("total"))) {
            g.u2.n = (double)(g.u2.steps_n - 1) + num(ev.get("n")) / num(ev.get("total"));
            g.u2.total = (double)g.u2.steps_total;
            g.u2.step = "Step " + std::to_string(g.u2.steps_n) + "/" + std::to_string(g.u2.steps_total) + ": file " +
                        std::to_string((long long)num(ev.get("n"))) + "/" + std::to_string((long long)num(ev.get("total"))) +
                        ": " + text(ev, "key").substr(0, 80);
        } else if (phase == "done") {
            g.u2.n = g.u2.total = 1;
            const std::string status = text(ev, "status");
            g.u2.step = status == "imported" || status == "partial" ? "Import finished." : "Import failed: " + text(ev, "error");
        }
    });
    if (g.u2.proc && !process_running(g.u2.proc)) {
        DWORD code = 0;
        GetExitCodeProcess(g.u2.proc, &code);
        CloseHandle(g.u2.proc);
        g.u2.proc = nullptr;
        g.u2.automatic.clear();
        if (code != 0 && g.u2.step.compare(0, 14, "Import stopped") != 0 && g.u2.step.compare(0, 13, "Import failed") != 0)
            g.u2.step = "Import failed: see the import log.";
    }
}

// PLAY: Uncharted 3 / 2 do not start while their multiplayer update is pending or running (a pending one starts now)
bool u3_confirm_play() {
    if (game_key() == "u2") return u2_confirm_play();
    if (game_key() != "u3" || u3_block_reason().empty()) return true;
    const std::string pending = u3_pending(u3_state());
    if (!pending.empty() && !u3_running())
        u3_import(true, pending == "image",
                  "the launcher update brought a new multiplayer " + std::string(pending == "image" ? "build" : "version"));
    gate();
    tell("info", "Uncharted 3 is updating its multiplayer files after the launcher update.\n\nIt can be played as soon as the "
                 "update finishes (progress at the top of the launcher). Uncharted 1 and 2 can be played meanwhile.");
    return false;
}
bool u2_confirm_play() {
    if (u2_block_reason().empty()) return true;
    if (!u2_running() && u2_pending(u2_state())) u2_import(true, "the launcher update changed the Uncharted 2 multiplayer files");
    gate();
    tell("info", "Uncharted 2 is updating its multiplayer files after the launcher update.\n\nIt can be played as soon as the "
                 "update finishes (progress at the top of the launcher). Uncharted 1 and 3 can be played meanwhile.");
    return false;
}

// ------------------------------------------------------------------------------------------------ the online section

Value online_host() {
    Value h = Value::object();
    h.set("game_alive", Value::boolean(g.playing));
    h.set("game", Value::text(game_key()));
    return h;
}
void online_apply(const Value &r) {
    if (const Value *c = r.get("config"))
        for (auto &f : c->fields)
            if (!g.config.get(f.first) || json::dump(*g.config.get(f.first), false) != json::dump(f.second, false))
                change_config(f.first, f.second);
}
// The online program is a separate process. It gets its requests on stdin and answers each with one line on stdout:
// a JSON object written as one line (json.h lays objects out over several lines, but strings never hold a raw line
// break, so turning the layout's breaks into spaces keeps it valid). Replies to a hand-off carry secrets, so every
// buffer that held a line is wiped.
void wipe(std::string &s) {
    if (!s.empty()) SecureZeroMemory(&s[0], s.size());
    s.clear();
}

// Ending the conversation: closing its input tells the program to wind down. It may stay alive a while for a game
// still using its session (it exits by itself), so it is not waited for.
void online_stop() {
    for (HANDLE *h : {&g.online_in, &g.online_out, &g.online_process})
        if (*h) { CloseHandle(*h); *h = nullptr; }
    wipe(g.online_pending);
    g.online = false;
}

bool online_spawn(const std::string &exe) {
    SECURITY_ATTRIBUTES pass_on{sizeof pass_on, nullptr, TRUE};
    HANDLE its_in = nullptr, our_in = nullptr, our_out = nullptr, its_out = nullptr;
    if (!CreatePipe(&its_in, &our_in, &pass_on, 0)) return false;
    if (!CreatePipe(&our_out, &its_out, &pass_on, 0)) {
        CloseHandle(its_in);
        CloseHandle(our_in);
        return false;
    }
    SetHandleInformation(our_in, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(our_out, HANDLE_FLAG_INHERIT, 0);
    // only the two pipe ends go to the program, not every inheritable handle the launcher happens to hold
    HANDLE handed[2] = {its_in, its_out};
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> room(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(room.data());
    const bool listed = InitializeProcThreadAttributeList(attributes, 1, 0, &size);
    bool started = listed && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handed,
                                                       sizeof handed, nullptr, nullptr);
    STARTUPINFOEXW start{};
    start.StartupInfo.cb = sizeof start;
    start.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    start.StartupInfo.hStdInput = its_in;
    start.StartupInfo.hStdOutput = its_out;
    start.lpAttributeList = attributes;
    PROCESS_INFORMATION child{};
    std::wstring command = L"\"" + wide(exe) + L"\"";
    const std::wstring folder = wide(join(g.u3mp_dir, "online"));
    started = started && CreateProcessW(nullptr, &command[0], nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, folder.c_str(),
                                        &start.StartupInfo, &child);
    if (listed) DeleteProcThreadAttributeList(attributes);
    CloseHandle(its_in);
    CloseHandle(its_out);
    if (!started) {
        CloseHandle(our_in);
        CloseHandle(our_out);
        return false;
    }
    CloseHandle(child.hThread);
    g.online_process = child.hProcess;
    g.online_in = our_in;
    g.online_out = our_out;
    return true;
}

// The next full line from the program, within wait_ms. The program answers in well under a millisecond, so this
// first yields the thread for a moment before it starts sleeping between looks.
bool online_line(std::string &line, DWORD wait_ms) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        const size_t end = g.online_pending.find('\n');
        if (end != std::string::npos) {
            line.assign(g.online_pending, 0, end);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            SecureZeroMemory(&g.online_pending[0], end + 1);
            g.online_pending.erase(0, end + 1);
            return true;
        }
        DWORD waiting = 0;
        if (!PeekNamedPipe(g.online_out, nullptr, 0, nullptr, &waiting, nullptr)) return false;   // it has ended
        if (waiting) {
            std::string chunk(waiting, '\0');
            DWORD got = 0;
            const bool read = ReadFile(g.online_out, &chunk[0], waiting, &got, nullptr) && got;
            g.online_pending.append(chunk, 0, got);
            wipe(chunk);
            if (!read) return false;
            continue;
        }
        const ULONGLONG spent = GetTickCount64() - start;
        if (spent >= wait_ms) return false;
        if (spent < 5) SwitchToThread();
        else Sleep(1);
    }
}

// One request and its reply. A program that stops answering (or has ended) is let go, and the online section is
// simply gone for the rest of this launcher run.
Value online_request(const Value &request, DWORD wait_ms = 15000) {
    if (!g.online_in) return Value::null();
    std::string out = json::dump(request);
    for (char &c : out) if (c == '\n') c = ' ';
    out += '\n';
    DWORD put = 0;
    const bool sent = WriteFile(g.online_in, out.data(), (DWORD)out.size(), &put, nullptr) && put == out.size();
    wipe(out);
    std::string line;
    if (!sent || !online_line(line, wait_ms)) {
        wipe(line);
        online_stop();
        return Value::null();
    }
    Value reply;
    json::parse(line, reply);
    wipe(line);
    return reply;
}
Value online_op(const char *op, const std::string &game = std::string()) {
    Value r = Value::object();
    r.set("op", Value::text(op));
    if (!game.empty()) r.set("game", Value::text(game));
    return r;
}
// what an "out" reply holds (null when there is none)
Value online_out(const Value &reply) {
    const Value *out = reply.get("out");
    return out ? *out : Value::null();
}

void online_open() {
    const std::string exe = join(join(g.u3mp_dir, "online"), "uncharted_online.exe");
    if (!is_file(exe) || !online_spawn(exe)) return;
    if (num(online_request(online_op("hello"), 5000).get("abi")) < 1) { online_stop(); return; }
    Value open = online_op("open"), state = Value::object();
    open.set("user_dir", Value::text(g.user_dir));
    state.set("dns", Value::text(text(g.config, "u3mp_dns")));
    state.set("direct", Value::boolean(!g.direct.empty()));
    open.set("state", state);
    if (!truthy(online_request(open, 5000).get("ok"))) { online_stop(); return; }
    g.online = true;
}
void online_close() {
    if (!g.online) return;
    online_request(online_op("close"), 3000);
    online_stop();
}
void online_tick() {
    if (!g.online) return;
    Value tick = online_op("tick");
    tick.set("host", online_host());
    online_apply(online_out(online_request(tick)));
}
Value online_page(const std::string &game) {
    if (!g.online) return array();
    Value v = online_out(online_request(online_op("page", game)));
    return v.kind == Value::Array ? v : array();
}
// an online control on a MULTIPLAYER page: the page says which game's account and server it belongs to
void online_act(const std::string &id, const std::string &kind, const Value &value) {
    if (!g.online) return;
    Value act = online_op("act", g.tab == "multiplayer" ? "u2" : "u3");
    act.set("id", Value::text(id));
    act.set("kind", Value::text(kind));
    act.set("value", value);
    act.set("host", online_host());
    online_apply(online_out(online_request(act)));
}
void online_env(std::vector<std::pair<std::string, Value>> &env, const char *game) {
    if (!g.online) return;
    Value handoff = online_op("handoff", game);
    handoff.set("host", online_host());
    Value r = online_out(online_request(handoff));
    if (const Value *unset = r.get("unset")) for (auto &n : unset->items) env.push_back({text(&n), Value::null()});
    if (const Value *set = r.get("set")) for (auto &f : set->fields) env.push_back({f.first, f.second});
    for (auto &f : r.fields)   // the reply held the hand-off's secrets
        if (f.second.kind == Value::Object)
            for (auto &v : f.second.fields) if (v.second.kind == Value::String) wipe(v.second.s);
}

}  // namespace

namespace {

// ------------------------------------------------------------------------------------------------ presets and resets

Value page_act(const char *page, const std::string &id, const Value &state) {
    const std::string s = json::dump(state, false);
    auto t = text_call([&](char *o, int c) { return un_page_act(page, id.c_str(), "click", "null", s.c_str(), o, c); });
    Value v;
    json::parse(t.second, v);
    return v;
}

Value gpu_state() {
    Value gpu = Value::object();
    gpu.set("name", Value::text(g.gpu_name));
    gpu.set("recommended", Value::text(g.recommended));
    return gpu;
}

// a Quick Setup card's changes, saved the way a click on the card saves them
void apply_preset(const std::string &name) {
    Value state = Value::object();
    state.set("config", g.config);
    state.set("ini", g.ini);
    state.set("root", Value::text(g.root));
    state.set("gpu", gpu_state());
    const Value changes = page_act("setup", "native:setup:preset:" + name, state);
    if (const Value *ini = changes.get("ini")) for (auto &f : ini->fields) change_ini(f.first, f.second);
    if (const Value *game = changes.get("game")) for (auto &f : game->fields) change_game_setting(f.first, f.second);
}

Value default_game_settings() {
    Value d = Value::object();
    d.set("preset", Value::text("medium"));
    d.set("resolution", Value::text("1920x1080"));
    d.set("aspect", Value::text("16:9"));
    d.set("multithreaded", Value::boolean(false));
    d.set("save_profile", Value::text("1"));
    d.set("async_readback", Value::boolean(false));
    return d;
}
Value default_controls() {
    Value d = Value::object();
    d.set("controls", Value::null());
    d.set("mouse", Value::boolean(true));
    d.set("mouse_sens", Value::text("1.0"));
    d.set("mouse_invert", Value::boolean(false));
    d.set("prompts", Value::text("auto"));
    d.set("pad_controls", Value::null());
    d.set("mouse_aim_sens", Value::text("1.0"));
    d.set("mouse_vertical", Value::text("1.0"));
    d.set("mouse_invert_x", Value::boolean(false));
    d.set("vibration", Value::boolean(true));
    return d;
}

void reset_settings() {
    ask("yesno", "Put " + game_name() + "'s settings back to the Medium preset?", [](const Value &yes) {
        if (!yes.truthy()) return;
        for (auto &f : default_game_settings().fields) game_settings().set(f.first, f.second);
        for (auto &f : default_controls().fields) game_settings().set(f.first, f.second);
        for (auto &f : g.ini_defaults.fields) g.ini.set(f.first, f.second);
        apply_preset("medium");
    });
}
void reset_controls() {
    ask("yesno", "Restore " + game_name() + "'s default keyboard, mouse and controller bindings?", [](const Value &yes) {
        if (!yes.truthy()) return;
        for (auto &f : default_controls().fields) game_settings().set(f.first, f.second);
        save_controls();
    });
}

// ------------------------------------------------------------------------------------------------ TOOLS' actions

std::string launcher_script() { return join(join(g.root, "launcher"), "uncharted_launcher.pyw"); }

Value games_list() {
    Value list = array();
    for (auto &game : g.games.items) {
        Value e = Value::object();
        e.set("key", Value::text(text(game, "key")));
        e.set("name", Value::text(text(game, "name")));
        list.items.push_back(std::move(e));
    }
    return list;
}

// one action step after another (actions.cpp): its dialogs, settings, TOOLS' lines and its install's progress
void run_action(const std::string &name, const Value &value, long long step, const Value &data, const Value &answer,
                std::function<void(const Value &)> finished) {
    Value request = Value::object();
    request.set("name", Value::text(name));
    request.set("value", value);
    request.set("step", Value::integer(step));
    request.set("data", data);
    request.set("answer", answer);
    Value state = Value::object();
    state.set("root", Value::text(g.root));
    state.set("user_dir", Value::text(g.user_dir));
    state.set("game_dir", Value::text(game_dir()));
    state.set("game_running", Value::boolean(g.playing));
    state.set("wine", Value::boolean(g.wine));
    state.set("python", Value::text(python_exe()));
    state.set("script", Value::text(launcher_script()));
    state.set("games", games_list());
    request.set("state", state);
    const Value reply = json_call(un_action, request);
    if (const Value *c = reply.get("config")) {
        for (auto &f : c->fields) g.config.set(f.first, f.second);
        if (!c->fields.empty()) write_config();
    }
    if (const Value *p = reply.get("progress")) {
        if (const Value *t = p->get("game_dir")) g.progress_game_dir = text(t);
        if (const Value *t = p->get("update")) g.progress_update = text(t);
    }
    const Value result = reply.get("result") ? *reply.get("result") : Value::null();
    const long long next = reply.get("step") ? reply.get("step")->i : 0;
    const Value next_data = reply.get("data") ? *reply.get("data") : Value::object();
    if (truthy(reply.get("work"))) {   // an install: its step on TOOLS' line until it ends
        const bool update_line = reply.get("progress") && reply.get("progress")->get("update");
        std::shared_ptr<std::function<void()>> watch = std::make_shared<std::function<void()>>();
        *watch = [name, value, next, next_data, finished, update_line, watch]() {
            Value state;
            json::parse(text_call([](char *o, int c) { return un_action_poll(o, c); }).second, state);
            if (truthy(state.get("running"))) {
                const std::string step_text = text(state, "text");
                if (!step_text.empty()) (update_line ? g.progress_update : g.progress_game_dir) = step_text;
                after(200, *watch);
            } else {
                run_action(name, value, next, next_data, Value::null(), finished);
            }
        };
        after(200, *watch);
        return;
    }
    if (const Value *d = reply.get("dialog")) {
        const std::string kind = text(*d, "kind");
        Answer go_on = [name, value, next, next_data, finished](const Value &a) { run_action(name, value, next, next_data, a, finished); };
        if (kind == "directory" || kind == "open")
            ask_path(kind.c_str(), text(*d, "title"), text(*d, "initialdir"), d->get("filetypes") ? *d->get("filetypes") : array(),
                     [go_on](const Value &a) { go_on(Value::text(text(&a))); });
        else
            ask(kind.c_str(), text(*d, "text"), [go_on, kind](const Value &a) {
                go_on(kind == "yesno" || kind == "okcancel" || kind == "retrycancel" ? Value::boolean(a.truthy()) : Value::null());
            });
        return;
    }
    if (finished) finished(result);
}
void action(const std::string &name, const Value &value = Value::null(), std::function<void(const Value &)> finished = nullptr) {
    run_action(name, value, 0, Value::object(), Value::null(), finished);
}

void make_shortcuts(const char *which) {
    Value request = Value::object();
    request.set("which", Value::text(which));
    request.set("games", games_list());
    request.set("python", Value::text(python_exe()));
    request.set("script", Value::text(launcher_script()));
    const std::string r = json::dump(request, false);
    auto t = text_call([&](char *o, int c) { return un_shortcuts(wide(g.root).c_str(), r.c_str(), o, c); });
    Value reply;
    json::parse(t.second, reply);
    tell(text(reply, "kind") == "error" ? "error" : "info", text(reply, "text"));
}

// ------------------------------------------------------------------------------------------------ the launcher's update

bool import_running() { return u3_running() || u2_running(); }

void close_launcher();

void update_op(const char *op, Value fields = Value::object()) {
    fields.set("op", Value::text(op));
    Value state = Value::object();
    state.set("root", Value::text(g.root));
    state.set("mode", g.config.get("update_mode") ? *g.config.get("update_mode") : Value::null());
    state.set("game_running", Value::boolean(g.playing));
    state.set("import_running", Value::boolean(import_running()));
    fields.set("state", state);
    const Value reply = json_call(un_updates, fields);
    if (const Value *s = reply.get("status"))
        if (s->items.size() == 2) set_status(text(&s->items[0]), text(&s->items[1]));
    if (const Value *b = reply.get("button")) {
        g.update_button = text(b);
        g.update_shown = !g.update_button.empty();
    }
    if (const Value *u = reply.get("updating")) g.update_view = *u;
    if (const Value *h = reply.get("hold_imports")) g.hold_imports = h->truthy();
    if (truthy(reply.get("stop_staged_imports"))) u3_stop_staged();
    if (const Value *d = reply.get("dialog")) {
        const std::string kind = text(*d, "kind");
        if (kind == "yesno")
            ask("yesno", text(*d, "text"), [](const Value &yes) {
                Value f = Value::object();
                f.set("answer", Value::boolean(yes.truthy()));
                update_op("confirmed", f);
            });
        else
            tell(kind == "error" ? "error" : "info", text(*d, "text"));
    }
    if (truthy(reply.get("exit"))) close_launcher();   // the installer holds off until we have quit
}

// ------------------------------------------------------------------------------------------------ the game

const Value *game_by_exe(const std::string &exe) {
    for (auto &game : g.games.items)
        if (text(game, "exe") == exe) return &game;
    return nullptr;
}

// the game's environment beyond the launcher's settings: Uncharted 3's multiplayer image and overlays
void u3_env(std::vector<std::pair<std::string, Value>> &env) {
    for (const char *k : {"UNCHARTED_U3MP_IMAGE", "UNCHARTED_U3MP_IMAGE_SHA256", "UNCHARTED_U3MP_OVERLAYS", "UNCHARTED_U3MP_VERSION"})
        env.push_back({k, Value::null()});
    if (!u3_on()) return;
    const Value s = u3_state();
    if (u3_running() && !truthy(s.get("staging"))) return;   // the files are being made: Uncharted 3 starts without them
    if (!u3_ready(s).first) return;
    const Value &image = *s.get("image");
    env.push_back({"UNCHARTED_U3MP_IMAGE", Value::text(text(image, "path"))});
    const std::string file_sha = text(image, "file_sha256");
    env.push_back({"UNCHARTED_U3MP_IMAGE_SHA256", Value::text(!file_sha.empty() ? file_sha : text(image, "sha256"))});
    const Value *version = image.get("version");
    env.push_back({"UNCHARTED_U3MP_VERSION", Value::text(version && version->kind == Value::String ? version->s : "")});
    std::string overlays;
    if (const Value *o = s.get("overlays"))
        for (auto &p : o->items)
            if (p.kind == Value::String && is_dir(p.s)) overlays += (overlays.empty() ? "" : ";") + p.s;
    env.push_back({"UNCHARTED_U3MP_OVERLAYS", Value::text(overlays)});
    online_env(env, "u3");
}

// Uncharted 2's multiplayer: its menu overlay (made by the import's u2mp_overlay.py) and the imported files; set for
// every start, since the Collection menu can switch to Uncharted 2 from any game. `overlay`: the overlay folder made
// in the background before ("" none).
void u2_env(std::vector<std::pair<std::string, Value>> &env, const std::string &overlay) {
    for (const char *k : {"UNCHARTED_U2MP_OVERLAY", "UNCHARTED_U2MP", "UNCHARTED_U2MP_NAME", "UNCHARTED_U2MP_JOIN",
                          "UNCHARTED_U2MP_RELAY", "UNCHARTED_U2MP_ONLINE_DLL"})
        env.push_back({k, Value::null()});
    if (overlay.empty()) return;
    env.push_back({"UNCHARTED_U2MP_OVERLAY", Value::text(overlay + ";" + u2_dir())});
    env.push_back({"UNCHARTED_U2MP", Value::text("1")});
    env.push_back({"UNCHARTED_U2MP_NAME", Value::text("Drake")});
    const std::string online_dll = join(join(g.u3mp_dir, "online"), "u2_online.dll");
    if (is_file(online_dll)) env.push_back({"UNCHARTED_U2MP_ONLINE_DLL", Value::text(online_dll)});
    online_env(env, "u2");
}

// whether Uncharted 2's multiplayer is ready to go with the game: a finished import that is not running
bool u2_usable() {
    const Value s = u2_state();
    const std::string status = text(s, "status");
    return (status == "imported" || status == "partial") && is_dir(join(u2_dir(), "u2data")) && !u2_running();
}

// the overlay folder for Uncharted 2's menu (u2mp_overlay.py builds it from the player's game; "" when it can't)
std::string build_u2_overlay(const std::string &game, const std::string &update) {
    const std::string patcher = join(join(g.root, "u2mp"), "patcher");
    auto r = run({python_exe(), "-c",
                  "import sys; sys.path.insert(0, sys.argv[1]); from pathlib import Path; import u2mp_overlay; "
                  "print(u2mp_overlay.build(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]) if sys.argv[4] else None, "
                  "log=lambda *a, **k: None))",
                  patcher, game, join(join(g.user_dir, "u2mp"), "overlay"), update},
                 600000, patcher);
    if (r.first != 0) return "";
    const std::vector<std::string> lines = lines_of(r.second);
    return lines.empty() ? "" : strip(lines.back());
}

void finished_game(long long code, bool known);

void start_game(std::function<void(bool)> done, const std::string &overlay) {
    const Value *entry = game_entry(game_key());
    const std::string starter = join(join(join(g.root, "launcher"), "native"), "game_starter.exe");
    std::vector<std::pair<std::string, Value>> env;
    env.push_back({"GOW3_TITLE_SUFFIX", Value::text(version_suffix(u3_imported_version()))});
    {
        const std::string c = json::dump(g.config, false);
        auto t = text_call([&](char *o, int cap) { return un_play_env(c.c_str(), wide(g.user_dir).c_str(), o, cap); });
        Value list;
        if (t.first == 0 && json::parse(t.second, list))
            for (auto &p : list.items)
                if (p.items.size() == 2) env.push_back({text(&p.items[0]), p.items[1]});
    }
    u3_env(env);
    u2_env(env, overlay);
    if (!chosen_update().empty()) env.push_back({"UNCHARTED_UPDATE", Value::text(chosen_update())});
    // the full environment the starter gets
    Value env_list = array();
    {
        std::vector<wchar_t> block = environment(env);
        for (const wchar_t *p = block.data(); *p; p += wcslen(p) + 1) {
            const std::wstring e = p;
            const size_t eq = e.find(L'=', 1);
            if (eq != std::wstring::npos) env_list.items.push_back(pair(utf8(e.substr(0, eq)), utf8(e.substr(eq + 1))));
        }
    }
    Value command = array();
    for (const std::string &a : {starter, std::string("--boot"), std::string("--seconds"), std::string("0"),
                                 std::string("--executable"), text(*entry, "exe"), std::string("--game"), game_dir()})
        command.items.push_back(Value::text(a));
    Value request = Value::object();
    request.set("command", command);
    request.set("env", env_list);
    request.set("cwd", Value::text(g.root));
    request.set("log", Value::text(join(g.user_dir, "launcher-last-run.log")));
    const std::string r = json::dump(request, false);
    auto t = text_call([&](char *o, int c) { return un_play_start(r.c_str(), o, c); });
    if (t.first != 0) {
        tell("error", "The game couldn't be started:\n" + t.second);
        if (done) done(false);
        return;
    }
    g.playing = true;
    g.stopping = false;
    g.game_started = false;
    g.started_at = unix_now();
    g.last_output = now_ms();
    set_status("Launching " + game_name() + "...", "Getting the game files and code patches ready.");
    if (done) done(true);
}

// PLAY in order: make sure a game folder is set, run through the pre-launch warnings, wait for pending multiplayer
// updates, then launch the starter with the built environment
void play(std::function<void(bool)> done = nullptr) {
    auto fail = [done]() { if (done) done(false); };
    if (!is_file(join(join(join(g.root, "launcher"), "native"), "game_starter.exe"))) {
        tell("error", "The game starter is missing:\n" + join(join(join(g.root, "launcher"), "native"), "game_starter.exe"));
        fail();
        return;
    }
    auto after_folder = [done, fail]() {
        Value state = Value::object();
        state.set("root", Value::text(g.root));
        state.set("config", g.config);
        state.set("game_dir", Value::text(game_dir()));
        state.set("update_dir", Value::text(update_dir()));
        const Value questions = json_call(un_play_questions, state);
        auto list = std::make_shared<std::vector<Value>>(questions.kind == Value::Array ? questions.items : std::vector<Value>{});
        auto next = std::make_shared<std::function<void(size_t)>>();
        *next = [list, next, done, fail](size_t i) {
            if (i < list->size()) {
                const Value q = (*list)[i];
                ask("okcancel", text(q, "text"), [q, i, next, fail](const Value &ok) {
                    if (!ok.truthy()) { fail(); return; }
                    if (const Value *remember = q.get("remember"))
                        if (!remember->fields.empty()) {
                            for (auto &f : remember->fields) g.config.set(f.first, f.second);
                            write_config();
                        }
                    (*next)(i + 1);
                });
                return;
            }
            if (!u3_confirm_play()) { fail(); return; }
            follow_ini();   // reload first so a stale copy can't overwrite what the in-game menu saved
            Value all = Value::object();
            for (auto &f : g.ini_defaults.fields) all.set(f.first, g.ini.get(f.first) ? *g.ini.get(f.first) : f.second);
            save_ini(all);
            write_config();
            if (!u2_usable()) { start_game(done, ""); return; }
            // Uncharted 2's menu overlay can take a moment: made in the background, then the game starts
            set_status("Preparing " + game_name() + "...", "Building the Uncharted 2 multiplayer menu.");
            auto overlay = std::make_shared<std::string>();
            const std::string game = game_dir(), update = chosen_update();
            background([overlay, game, update]() { *overlay = build_u2_overlay(game, update); },
                       [overlay, done]() { start_game(done, *overlay); });
        };
        (*next)(0);
    };
    if (is_game_dir(game_dir())) {
        after_folder();
        return;
    }
    ask("info", "First, show the launcher where your Uncharted: The Nathan Drake Collection game is: the CUSA02344 "
                "folder with eboot.bin and sce_sys in it.", [after_folder, fail](const Value &) {
        action("choose_game_dir", Value::null(), [after_folder, fail](const Value &chosen) {
            if (chosen.truthy()) after_folder();
            else fail();
        });
    });
}

void stop_game() {
    if (!g.playing) return;
    g.stopping = true;
    un_play_stop();
    set_status("Closing the game...");
}

void poll_game() {
    if (!g.playing) return;
    Value state;
    json::parse(text_call([](char *o, int c) { return un_play_poll(o, c); }).second, state);
    if (const Value *lines = state.get("lines"))
        for (auto &l : lines->items) {
            g.last_output = now_ms();
            const std::string line = text(&l);
            const std::string marker = "Collection: switching to";
            if (line.compare(0, marker.size(), marker) == 0) {   // player picked a different title in the Collection menu
                std::string exe = strip(line.substr(marker.size()));
                exe = exe.substr(0, exe.find(' '));
                const Value *other = game_by_exe(exe);
                set_status("Changing to " + (other ? text(*other, "name") : exe) + "...", "The Collection is launching another game.");
                g.game_started = false;
            }
        }
    const Value *exit_code = state.get("exit");
    if (exit_code && exit_code->kind != Value::Null) {
        finished_game(exit_code->i, true);
        return;
    }
    if (!g.game_started && now_ms() - g.last_output > 2500) {   // 2.5 s without output: loading is over
        g.game_started = true;
        set_status("Now playing " + game_name(), "In-game menu: Insert or R3 + L2. Press STOP to quit the game.");
    }
}

void finished_game(long long code, bool known) {
    g.playing = false;
    Value state = Value::object();
    state.set("root", Value::text(g.root));
    state.set("game_dir", Value::text(game_dir()));
    state.set("update_dir", Value::text(update_dir()));
    state.set("code", known ? Value::integer(code) : Value::null());
    state.set("stopping", Value::boolean(g.stopping));
    Value started;
    started.kind = Value::Float;
    started.f = g.started_at;
    state.set("started_at", started);
    const Value end = json_call(un_play_end, state);
    g.stopping = false;
    set_status(text(end, "status"), text(end, "detail"));
    const std::string question = text(end, "ask");
    if (!question.empty())
        ask("yesno", question, [](const Value &yes) { if (yes.truthy()) action("open_logs"); });
    if (!g.direct.empty()) after(200, close_launcher);
}

// ------------------------------------------------------------------------------------------------ games and tabs

void show_tab(std::string name, bool save) {
    if (name == "multiplayer" && game_key() != "u2") name = "setup";
    if (name == "u3mp" && game_key() == "u2") name = "multiplayer";
    g.tab = name;
    if (save && text(g.config, "tab") != name) change_config("tab", Value::text(name));
}

void select_game(std::string key, bool save) {
    if (!game_entry(key)) key = "u1";
    const bool changed = key != game_key();
    g.config.set("game", Value::text(key));
    if (save) write_config();
    if (changed) reload_ini();   // settings are per game, so load the new one's ini
    if (!g.playing) set_status(game_name() + " is ready to play.");
    // the Multiplayer tab: Uncharted 2's while it is chosen, Uncharted 3's otherwise
    if (key == "u2" && g.tab == "u3mp") show_tab("multiplayer", false);
    if (key != "u2" && g.tab == "multiplayer") show_tab("u3mp", false);
    gate();
}

// ------------------------------------------------------------------------------------------------ the model

const char *const TABS[][2] = {{"setup", "SETUP"}, {"graphics", "GRAPHICS"}, {"controls", "CONTROLS"}, {"game", "GAME"},
                               {"cheats", "CHEATS"}, {"experimental", "EXPERIMENTAL"}, {"tools", "TOOLS"},
                               {"u3mp", "MULTIPLAYER"}, {"multiplayer", "MULTIPLAYER"}, {"launcher", "LAUNCHER"}};

Value chrome() {
    Value c = Value::object();
    c.set("title", Value::text(title()));
    Value cards = array();
    for (auto &game : g.games.items) {
        Value e = Value::object();
        for (const char *k : {"key", "number", "title", "subtitle"}) e.set(k, Value::text(text(game, k)));
        e.set("selected", Value::boolean(text(game, "key") == game_key()));
        cards.items.push_back(std::move(e));
    }
    c.set("games", cards);
    Value tabs = array();
    for (auto &t : TABS) {
        Value e = Value::object();
        e.set("name", Value::text(t[0]));
        e.set("label", Value::text(t[1]));
        const std::string name = t[0];
        e.set("visible", Value::boolean(name == "u3mp" ? game_key() != "u2" : name == "multiplayer" ? game_key() == "u2" : true));
        e.set("selected", Value::boolean(name == g.tab));
        tabs.items.push_back(std::move(e));
    }
    c.set("tabs", tabs);
    Value status = Value::object();
    status.set("text", Value::text(g.status));
    status.set("detail", Value::text(g.detail));
    c.set("status", status);
    c.set("play", Value::text(g.playing ? "STOP" : blocked() ? "UPDATING..." : "PLAY"));
    c.set("running", Value::boolean(g.playing));
    c.set("update", Value::text(g.update_shown ? g.update_button : ""));
    // the bottom bar's update line: a multiplayer update (its steps), or the launcher's own update
    Value banner;
    if (g.banner) {
        const bool three = !g.u3_block.empty();
        const std::string reason = three ? g.u3_block : g.u2_block;
        const Import &imp = three ? g.u3 : g.u2;
        if (reason == "running" && imp.total > 0) {
            const long long total = (long long)imp.total, done = std::max(0LL, std::min((long long)imp.n, total));
            banner = Value::object();
            banner.set("title", Value::text("UPDATE IN PROGRESS - " + std::to_string(done) + "/" + std::to_string(total) + " (" +
                                            std::to_string(100 * done / total) + "%)"));
            Value p;
            p.kind = Value::Float;
            p.f = (double)done / (double)total;
            banner.set("progress", p);
        } else if (!reason.empty()) {
            banner = Value::object();
            banner.set("title", Value::text("UPDATE STARTING"));
            banner.set("progress", Value::integer(0));
        }
    }
    if (g.update_view.kind == Value::Array && g.update_view.items.size() == 2) {
        const long long done = g.update_view.items[0].i, total = g.update_view.items[1].i;
        banner = Value::object();
        banner.set("title", Value::text("UPDATING THE LAUNCHER" + (total ? " (" + std::to_string(100 * done / total) + "%)" : std::string())));
        Value p;
        p.kind = Value::Float;
        p.f = total ? (double)done / (double)total : 0.0;
        banner.set("progress", p);
    }
    c.set("banner", banner);
    std::string hero;
    for (const char *pic : {"pic1.png", "pic0.png"}) {
        const std::string path = join(join(game_dir(), "sce_sys"), pic);
        if (is_file(path)) { hero = path; break; }
    }
    c.set("hero", Value::text(hero));
    return c;
}

const char *const MP_KEYS[] = {"status", "finished", "failed", "failed_at", "error", "recipe_version", "ps3_elf", "ps3_kept", "updating"};

Value progress_pair(const Import &imp) {
    Value p = array();
    Value n, t;
    n.kind = t.kind = Value::Float;
    n.f = imp.n;
    t.f = imp.total;
    p.items = {n, t};
    return p;
}

// what the Multiplayer tab's native page shows (pages.cpp page_u3mp / page_u2mp)
Value mp_state(const std::string &tab) {
    Value mp = Value::object();
    if (tab == "u3mp") {
        const Value s = u3_state();
        Value trimmed = Value::object();
        for (const char *k : MP_KEYS) if (const Value *v = s.get(k)) trimmed.set(k, *v);
        if (const Value *image = s.get("image"))
            if (image->truthy()) {
                Value i = Value::object();
                i.set("version", image->get("version") ? *image->get("version") : Value::text("?"));
                trimmed.set("image", i);
            }
        const auto ready = u3_ready(s);
        Value u3 = Value::object();
        u3.set("state", trimmed);
        u3.set("running", Value::boolean(u3_running()));
        u3.set("auto", Value::text(g.u3.automatic));
        u3.set("step", Value::text(g.u3.step));
        u3.set("progress", progress_pair(g.u3));
        u3.set("ready", Value::boolean(ready.first));
        u3.set("why", Value::text(ready.second));
        u3.set("data", Value::text(u3_data()));
        mp.set("u3", u3);
    } else {
        const Value s = u2_state();
        Value trimmed = Value::object();
        for (const char *k : MP_KEYS) if (const Value *v = s.get(k)) trimmed.set(k, *v);
        Value u2 = Value::object();
        u2.set("state", trimmed);
        u2.set("running", Value::boolean(u2_running()));
        u2.set("auto", Value::text(g.u2.automatic));
        u2.set("step", Value::text(g.u2.step));
        u2.set("progress", progress_pair(g.u2));
        u2.set("found", Value::text(u2_data_folder()));
        u2.set("dir", Value::text(u2_dir()));
        mp.set("u2", u2);
    }
    return mp;
}

Value model() {
    Value native = Value::object();
    native.set("config", g.config);
    native.set("ini", g.ini);
    native.set("gpu", gpu_state());
    native.set("game_dir", Value::text(game_dir()));
    native.set("tools", tools_lines());
    if (g.tab == "u3mp" || g.tab == "multiplayer") native.set("mp", mp_state(g.tab));
    Value m = Value::object();
    m.set("chrome", chrome());
    m.set("tab", Value::text(g.tab));
    m.set("page", g.tab == "u3mp" ? online_page("u3") : g.tab == "multiplayer" ? online_page("u2") : array());
    m.set("native", native);
    return m;
}

// ------------------------------------------------------------------------------------------------ requests

void close_launcher() {
    if (g.closing) return;
    g.closing = true;
    online_close();
    Value e = Value::object();
    e.set("ev", Value::text("closed"));
    emit(std::move(e));
}

// a native page's button or choice that runs a launcher action (pages.cpp says its name)
void call(const std::string &name, const Value *value) {
    const Value v = value ? *value : Value::null();
    if (name == "desktop_shortcut") make_shortcuts("launcher");
    else if (name == "game_shortcuts") make_shortcuts("games");
    else if (name == "add_to_steam") action("add_to_steam", Value::boolean(false));
    else if (name == "add_games_to_steam") action("add_to_steam", Value::boolean(true));
    else if (name == "open_logs" || name == "open_saves" || name == "choose_game_dir" || name == "install_game_pkg" ||
             name == "check_game_files" || name == "choose_update_folder" || name == "install_game_update" ||
             name == "clear_cache")
        action(name);
    else if (name == "check_updates") { Value f = Value::object(); f.set("manual", Value::boolean(true)); update_op("check", f); }
    else if (name == "reset") reset_settings();
    else if (name == "update_mode") { change_config("update_mode", v); update_op("mode"); }
    else if (name == "reset_controls") reset_controls();
    // the Multiplayer tabs
    else if (name == "u3mp_ps3_u3_add")
        ask_path("directory", "Pick a PS3 Uncharted 3 folder (game disc or RPCS3 dev_hdd0/game)", "", array(), [](const Value &a) {
            const std::string folder = text(&a);
            if (folder.empty()) return;
            Value list = array();
            if (const Value *old = g.config.get("u3mp_ps3_u3"))
                for (auto &f : old->items) if (text(&f) != folder) list.items.push_back(f);
            list.items.push_back(Value::text(folder));
            change_config("u3mp_ps3_u3", list);
        });
    else if (name == "u3mp_ps3_u3_clear") change_config("u3mp_ps3_u3", array());
    else if (name == "u3mp_ps3_elf_choose") {
        Value types = array();
        types.items = {pair("Decrypted PS3 executable", "*.elf"), pair("All files", "*.*")};
        ask_path("open", "Pick the decrypted EBOOT.elf (Uncharted 3 update 1.19)", "", types, [](const Value &a) {
            if (!text(&a).empty()) change_config("u3mp_ps3_elf", a);
        });
    } else if (name == "u3mp_ps3_elf_clear") change_config("u3mp_ps3_elf", Value::text(""));
    else if (name == "u3mp_data_change")
        ask_path("directory", "Pick the folder for the multiplayer files (about 9 GB)", "", array(), [](const Value &a) {
            if (!text(&a).empty()) change_config("u3mp_data", Value::text(join(as_path(text(&a)), "U3 Multiplayer")));
        });
    else if (name == "u3mp_data_open") action("open_folder", Value::text(u3_data()));
    else if (name == "u3mp_import") u3_import(false);
    else if (name == "u3mp_cancel") u3_cancel();
    else if (name == "u3mp_log") action("open_folder", Value::text(join(u3_data(), "import.log")));
    else if (name == "u3mp_keep_ps3") u3_keep_ps3(v.truthy());
    else if (name == "u2mp_ps3_choose")
        ask_path("directory", "Pick the PS3 Uncharted 2 folder", "", array(), [](const Value &a) {
            if (!text(&a).empty()) change_config("u2mp_ps3", a);
        });
    else if (name == "u2mp_ps3_clear") change_config("u2mp_ps3", Value::text(""));
    else if (name == "u2mp_data_choose")
        ask_path("directory", "Pick RPCS3's dev_hdd0/game folder", "", array(), [](const Value &a) {
            if (!text(&a).empty()) change_config("u2mp_ps3_data", a);
        });
    else if (name == "u2mp_data_clear") change_config("u2mp_ps3_data", Value::text(""));
    else if (name == "u2mp_dir_open") action("open_folder", Value::text(u2_dir()));
    else if (name == "u2mp_import") u2_import(false);
    else if (name == "u2mp_cancel") u2_cancel();
    else if (name == "u2mp_log") action("open_folder", Value::text(join(u2_dir(), "import.log")));
}

void handle(const Value &m) {
    const std::string op = text(m, "op");
    const Value *key = m.get("key");
    const Value value = m.get("value") ? *m.get("value") : Value::null();
    if (op == "model") {
        const std::string data = json::dump(model(), false);
        Value r = Value::object();
        r.set("re", Value::text("model"));
        if (data == g.last_model && !truthy(m.get("force"))) {
            r.set("same", Value::boolean(true));
        } else {
            g.last_model = data;
            Value parsed;
            json::parse(data, parsed);
            r.set("data", parsed);
        }
        emit(std::move(r));
    } else if (op == "config") change_config(text(key), value);
    else if (op == "ini") change_ini(text(key), value);
    else if (op == "game_setting") change_game_setting(text(key), value);
    else if (op == "ini_all") change_all_games(text(key), value);
    else if (op == "call") call(text(m, "name"), m.get("value"));
    else if (op == "act") {
        const std::string id = text(m, "id");
        if (id.compare(0, 7, "online:") == 0) online_act(id, text(m, "kind"), value);
    } else if (op == "game") select_game(text(key), true);
    else if (op == "playgame") { select_game(text(key), true); if (!g.playing) play(); }
    else if (op == "tab") show_tab(text(m, "name"), true);
    else if (op == "play") { if (g.playing) stop_game(); else play(); }
    else if (op == "update") { Value f = Value::object(); f.set("auto", Value::boolean(false)); update_op("install", f); }
    else if (op == "answer") {
        const int id = (int)(m.get("dialog") ? m.get("dialog")->i : 0);
        auto it = g.waiting.find(id);
        if (it != g.waiting.end()) {
            Answer then = it->second;
            g.waiting.erase(it);
            then(value);
        }
    } else if (op == "close") {
        if (g.playing)
            ask("yesno", "Your game is still open. Quit the launcher and leave the game running?",
                [](const Value &yes) { if (yes.truthy()) close_launcher(); });
        else
            close_launcher();
    }
}

// the timers: due ones, and what threads finished
void tick() {
    const long long now = now_ms();
    std::vector<Core::Timer> due;
    for (auto it = g.timers.begin(); it != g.timers.end();) {
        if (it->at <= now) { due.push_back(std::move(*it)); it = g.timers.erase(it); }
        else ++it;
    }
    for (auto &t : due) t.fn();
    std::deque<Fn> done;
    {
        std::lock_guard<std::mutex> l(g.done_lock);
        done.swap(g.done);
    }
    for (auto &f : done) f();
}

void every(long long ms, Fn fn) {   // a repeating timer
    auto repeat = std::make_shared<Fn>();
    *repeat = [ms, fn, repeat]() {
        if (g.closing) return;
        fn();
        after(ms, *repeat);
    };
    after(ms, *repeat);
}

void start(const std::string &root, const Value &args) {
    g.root = root;
    g.user_dir = join(join(root, "analysis"), "uncharted-user");
    g.u3mp_dir = join(join(root, "u3mp"), "patcher");
    g.u2mp_dir = join(g.user_dir, "u2mp");
    g.wine = un_wine() != 0;
    g.prefix_file = appdata_file();
    const std::string shared = g.wine ? sys("shared_config") : "";
    g.config_file = !shared.empty() ? shared : g.prefix_file;
    g.direct = text(args, "direct");
    {
        auto t = text_call([](char *o, int c) { return un_system_tables(o, c); });
        Value tables;
        if (json::parse(t.second, tables) && tables.get("GAMES")) g.games = *tables.get("GAMES");
    }
    {
        auto t = text_call([](char *o, int c) { return un_settings_table("ini_defaults", o, c); });
        for (auto &line : lines_of(t.second)) {
            const size_t tab = line.find('\t');
            if (tab != std::string::npos) g.ini_defaults.set(line.substr(0, tab), Value::text(line.substr(tab + 1)));
        }
    }
    {
        const std::wstring cf = wide(g.config_file), pf = wide(g.prefix_file), ud = wide(g.user_dir);
        auto t = text_call([&](char *o, int c) { return un_config_read(cf.c_str(), pf.c_str(), ud.c_str(), g.wine ? 1 : 0, o, c); });
        Value cfg;
        if (json::parse(t.second, cfg) && cfg.is_object()) g.config = cfg;
    }
    if (!game_entry(game_key())) g.config.set("game", Value::text("u1"));
    for (const char *game : GAME_KEYS) {   // the ini is the source of truth for bindings, since the in-game menu edits it;
                                           // an ini with none yet gets the launcher's copy
        Value *games = g.config.get("games");
        if (!games) continue;
        Value *settings = games->get(game);
        if (!settings) continue;
        if (!controls_from_ini(read_ini(game), *settings)) write_ini(game, controls("to_ini", *settings));
    }
    reload_ini();
    g.gpu_name = text(g.config, "gpu");
    if (!g.gpu_name.empty())
        g.recommended = text_call([](char *o, int c) { return un_gpu_query("recommend", g.gpu_name.c_str(), -1, o, c); }).second;
    write_config();   // persist any migrations un_config_read applied, even if nothing else changes
    g.detail = "During play, Insert or R3 + L2 shows the in-game menu.";
    select_game(game_key(), false);
    std::string saved = text(g.config, "tab");
    if (saved == "display" || saved == "performance") saved = "graphics";   // the tabs before 0.10.78
    if (saved == "advanced") saved = "game";
    bool known = false;
    for (auto &t : TABS) known = known || saved == t[0];
    // until the game folder and update 1.02 are set up, the launcher opens on SETUP's steps
    const Value steps = setup_steps();
    const bool set_up = truthy(steps.get("game")) && truthy(steps.get("update"));
    show_tab(known && set_up ? saved : "setup", false);
    online_open();
    every(150, []() { poll_game(); if (++g.poll_count % 7 == 0) follow_ini(); });
    every(200, []() { update_op("tick"); });
    every(200, []() { online_tick(); });
    every(1000, []() { u3_poll(); u2_poll(); });
    after(4000, auto_updates_after_launcher);
    after(3000, []() { un_update_cleanup(wide(g.root).c_str()); });
    if (truthy(g.config.get("check_updates")) || !g.config.get("check_updates"))
        if (g.direct.empty()) after(1500, []() { update_op("check"); });
    if (g.direct.empty()) {   // detect the GPU off-thread (Windows is slow to enumerate) for the Quick Setup suggestion
        auto found = std::make_shared<std::string>();
        background([found]() {
            std::wstring logs;
            for (const char *folder : {"uncharted", "big2-ps4_Shipping", "big3-ps4_Shipping"})
                logs += (logs.empty() ? L"" : L"\n") + wide(join(join(join(g.root, "analysis"), folder), "boot.log"));
            *found = text_call([&](char *o, int c) { return un_gpu_recommendation(logs.c_str(), o, c); }).second;
        }, [found]() {
            std::vector<std::string> f;
            size_t at = 0;
            while (at <= found->size()) {
                size_t end = found->find('\t', at);
                if (end == std::string::npos) end = found->size();
                f.push_back(found->substr(at, end - at));
                at = end + 1;
            }
            if (f.size() < 3 || f[0].empty()) return;
            g.gpu_name = f[0];
            g.recommended = f[2];
            if (text(g.config, "gpu") != f[0]) change_config("gpu", Value::text(f[0]));
        });
    } else {   // --play mode: launch right away and quit together with the game
        select_game(g.direct, false);
        after(100, []() { play([](bool started) { if (!started) close_launcher(); }); });
    }
    g.open = true;
    Value ready = Value::object();
    ready.set("ev", Value::text("ready"));
    emit(std::move(ready));
}

std::string take_out() {
    Value list = array();
    list.items.swap(g.out);
    return json::dump(list, false);
}

}  // namespace

extern "C" {

UN_API int un_core_open(const wchar_t *root, const char *args_json) {
    if (g.open) return 0;
    Value args;
    json::parse(args_json ? args_json : "{}", args);
    start(utf8(root ? root : L"."), args);
    return 0;
}

UN_API int un_core_send(const char *message_json, char *out, int cap) {
    static std::string pending;   // what did not fit last time goes first
    if (message_json) {
        Value m;
        if (json::parse(message_json, m) && m.is_object()) handle(m);
    }
    if (pending.empty()) pending = take_out();
    if ((int)pending.size() + 1 > cap) return -(int)(pending.size() + 1);
    const int r = give(pending, out, cap);
    pending.clear();
    return r;
}

UN_API int un_core_tick(char *out, int cap) {
    static std::string pending;
    if (pending.empty()) {
        tick();
        pending = take_out();
    }
    if ((int)pending.size() + 1 > cap) return -(int)(pending.size() + 1);
    const int r = give(pending, out, cap);
    pending.clear();
    return r;
}

UN_API void un_core_close(void) {
    if (!g.closing) online_close();
    g.closing = true;
}

}  // extern "C"
