// updates.cpp - the launcher's self-update as the player sees it, part of uncharted_native.dll: the check, the UPDATE
// button, the automatic mode (it waits while a game or an import runs), the download's progress, and every message.
// The steps themselves are updater.cpp's (un_update_*); this keeps the state between them and runs the slow ones in a
// thread. The launcher calls un_updates with an "op" and does what the answer says.
//
//   un_updates(request_json, out, cap)
//     request: {"op": "check" | "install" | "confirmed" | "mode" | "tick", "manual" (check: the player asked),
//               "auto" (install: no question), "answer" (confirmed: the player's yes / no), "state": {"root",
//               "mode": the launcher's update_mode, "game_running", "import_running"}}
//     out: any of
//       "status": [text, detail]         the launcher's status line
//       "button": text                   the UPDATE button: "" hides it, a text shows it with that text
//       "dialog": {"kind": info | error | yesno, "text"}   yesno: the answer goes back with op "confirmed"
//       "exit": true                     the installer runs: the launcher saves its window place and closes
//       "updating": [done, total] | null the window's banner: busy with a release found (installing, or checking again)
//       "stop_staged_imports": true      Automatic mode, a found release waits for an import: the launcher stops an
//                                        Uncharted 3 import update (it builds in its own copy) and the install follows
//       "hold_imports": bool             in every answer: true while the check runs, and in Automatic mode while a found
//                                        release waits to install or installs, so the launcher holds back its own
//                                        automatic import updates (the new launcher brings its own recipe)
//   "tick": about every 200 ms; it reports what the threads did and starts the automatic install when it is due.
#include "native.h"
#include "json.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {   // updater.cpp
UN_API int un_update_installed(const wchar_t *root, char *out, int cap);
UN_API int un_update_latest(const char *current, int timeout, char *out, int cap);
UN_API int un_update_download(const char *release_json, const wchar_t *root, char *out, int cap);
UN_API void un_update_progress(long long *done, long long *total);
UN_API int un_update_stage(const wchar_t *archive, const wchar_t *root, const char *version, char *out, int cap);
UN_API int un_update_start_apply(const wchar_t *root, const wchar_t *staged, char *out, int cap);
}

namespace {

using json::Value;

const char *const IMPORT_BUSY =
    "The Multiplayer tab is still importing or updating in the background, and the update would overwrite files that "
    "work needs. Let it finish, then press UPDATE again.";

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

long long now_ms() { return (long long)GetTickCount64(); }

// a text call of the DLL's convention; a step with side effects runs once (its buffer is big enough)
template <typename Call> std::pair<int, std::string> text_call(Call call, int room = 1 << 16, bool repeatable = true) {
    std::string out(room, '\0');
    int status = call(&out[0], (int)out.size());
    if (status < 0 && repeatable) {
        out.assign(-status, '\0');
        status = call(&out[0], (int)out.size());
    }
    out.resize(std::strlen(out.c_str()));
    return {status, out};
}

// the installed version as dotted numbers without leading zeros; empty when running from a git checkout
std::string installed(const std::string &root) {
    const std::wstring r = wide(root);
    auto t = text_call([&](char *o, int c) { return un_update_installed(r.c_str(), o, c); });
    if (t.first != 0) return "";
    std::string out;
    size_t at = 0;
    while (at <= t.second.size()) {
        size_t dot = t.second.find('.', at);
        if (dot == std::string::npos) dot = t.second.size();
        out += (out.empty() ? "" : ".") + std::to_string(std::atoll(t.second.substr(at, dot - at).c_str()));
        at = dot + 1;
    }
    return out;
}

std::string version_text(const Value &release) {
    std::string out;
    if (const Value *v = release.get("version"))
        for (auto &p : v->items) out += (out.empty() ? "" : ".") + std::to_string(p.i);
    return out;
}

std::string fixed0(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.0f", v);
    return b;
}

long long size_of(const Value &release) {
    const Value *s = release.get("size");
    return s && s->kind == Value::Int ? s->i : 0;
}

struct Host { std::string root, mode; bool game_running = false, import_running = false; };

struct Updates {
    std::mutex m;
    bool busy = false;
    Value release;               // the newer release found (null: none)
    std::string button = "UPDATE";
    bool button_shown = false;
    long long auto_at = 0;       // when to try the automatic install (0: not due)
    bool checking = false, check_done = false, manual = false;
    Value found;
    std::string check_error, current;
    bool installing = false, install_done = false;
    std::string staged, install_error;
    std::thread worker;
    ~Updates() {   // the launcher may close while a check runs: never end the program over a joinable thread
        if (worker.joinable()) worker.detach();
    }
} g;

Value status(const std::string &text, const std::string &detail) {
    Value s;
    s.kind = Value::Array;
    s.items = {Value::text(text), Value::text(detail)};
    return s;
}

Value dialog(const char *kind, const std::string &text) {
    Value d = Value::object();
    d.set("kind", Value::text(kind));
    d.set("text", Value::text(text));
    return d;
}

void join() {
    if (g.worker.joinable()) g.worker.join();
}

void show_button(Value &out, bool shown) {
    g.button_shown = shown;
    out.set("button", Value::text(shown ? g.button : ""));
}

void check(const Host &h, bool manual, Value &out) {
    const std::string current = installed(h.root);
    if (current.empty()) {
        if (manual) out.set("dialog", dialog("info", "This is a source checkout: update it with git."));
        return;
    }
    if (g.busy) return;
    g.busy = true;
    g.checking = true;
    g.check_done = false;
    g.manual = manual;
    g.current = current;
    if (manual) out.set("status", status("Looking for a new version...", ""));
    join();
    g.worker = std::thread([current]() {   // no network or a GitHub rate limit ends up in check_error
        auto t = text_call([&](char *o, int c) { return un_update_latest(current.c_str(), 15, o, c); }, 1 << 22);
        std::lock_guard<std::mutex> l(g.m);
        g.found = Value::null();
        g.check_error.clear();
        if (t.first != 0) g.check_error = t.second.empty() ? "UpdateError" : t.second;
        else json::parse(t.second, g.found);
        g.check_done = true;
    });
}

void install(const Host &h, bool automatic, Value &out) {
    if (!g.release.is_object() || g.busy) return;
    if (h.game_running) {
        out.set("dialog", dialog("info", "Please close the game first, since the update swaps out the runtime it is using."));
        return;
    }
    if (h.import_running) {
        out.set("dialog", dialog("info", IMPORT_BUSY));
        return;
    }
    if (!automatic) {
        const Value *page = g.release.get("page");
        out.set("dialog", dialog("yesno", "Update to version " + version_text(g.release) + " (" +
                                              fixed0((double)size_of(g.release) / 1e6) + " MB to download)?\n\nYour saves, "
                                                                                         "settings and shader cache stay. The "
                                                                                         "launcher will close, install the update "
                                                                                         "and reopen.\n\nRelease notes: " + (page && page->kind == Value::String ? page->s : "")));
        return;
    }
    g.busy = true;
    g.installing = true;
    g.install_done = false;
    g.staged.clear();
    g.install_error.clear();
    join();
    const Value release = g.release;
    const std::string root = h.root;
    g.worker = std::thread([release, root]() {
        const std::wstring r = wide(root);
        const std::string request = json::dump(release, false);
        std::string staged, error;
        auto d = text_call([&](char *o, int c) { return un_update_download(request.c_str(), r.c_str(), o, c); }, 1 << 16, false);
        if (d.first != 0) {
            error = d.second;
        } else {
            const std::wstring archive = wide(d.second);
            const std::string version = version_text(release);
            auto s = text_call([&](char *o, int c) { return un_update_stage(archive.c_str(), r.c_str(), version.c_str(), o, c); },
                               1 << 16, false);
            if (s.first != 0) error = s.second;
            else staged = s.second;
        }
        std::lock_guard<std::mutex> l(g.m);
        g.staged = staged;
        g.install_error = error.empty() && staged.empty() ? "UpdateError" : error;
        g.install_done = true;
    });
}

void tick(const Host &h, Value &out) {
    std::lock_guard<std::mutex> l(g.m);
    if (g.checking && g.check_done) {
        g.checking = false;
        g.busy = false;
        if (g.worker.joinable()) g.worker.join();   // it has just finished
        if (g.found.is_object()) {
            g.release = g.found;
            const std::string version = version_text(g.release);
            if (h.mode == "auto") {
                if (!h.game_running) out.set("status", status("Version " + version + " is out.",
                                                              "It will install automatically shortly."));
                g.auto_at = now_ms() + 1500;
            } else {
                g.button = "UPDATE TO " + version;
                show_button(out, true);
                if (!h.game_running)
                    out.set("status", status("Version " + version + " is out.",
                                             "Press UPDATE to install it; saves and settings stay, and the launcher "
                                             "restarts."));
            }
        } else if (g.manual) {
            if (!g.check_error.empty()) out.set("status", status("The update check failed.", g.check_error));
            else out.set("status", status("You're up to date.", "Version " + g.current + " is the latest release."));
        }
    }
    if (g.auto_at && now_ms() >= g.auto_at) {   // auto mode: install once neither a game nor an import is running
        g.auto_at = 0;
        if (h.mode == "auto" && g.release.is_object() && !g.busy) {
            if (h.game_running) g.auto_at = now_ms() + 60000;
            else if (h.import_running) {   // the launcher goes first: a staged import update is stopped, a first import finishes
                out.set("stop_staged_imports", Value::boolean(true));
                g.auto_at = now_ms() + 5000;
            } else install(h, true, out);
        }
    }
    if (g.installing) {
        if (!g.install_done) {
            long long done = 0, total = 0;
            un_update_progress(&done, &total);
            const std::string amount = total ? fixed0(100.0 * (double)done / (double)total) + "%"
                                             : fixed0((double)done / 1e6) + " MB";
            out.set("status", status("Downloading the new version... " + amount, "Please keep the launcher open."));
        } else {
            g.installing = false;
            join();
            if (!g.install_error.empty()) {
                g.busy = false;
                out.set("status", status("The update didn't go through.", "Your current version is still installed."));
                out.set("dialog", dialog("error", "The update couldn't be installed:\n\n" + g.install_error));
            } else if (h.import_running) {
                // a multiplayer import began during the download and now holds files the update would swap out;
                // Windows won't move a file that is open, so stop here and let the next UPDATE press do the install
                g.busy = false;
                g.staged.clear();
                out.set("status", status("The update is downloaded.", "Once the multiplayer import is done, press UPDATE again."));
                out.set("dialog", dialog("info", IMPORT_BUSY));
            } else {
                out.set("status", status("Installing the new version...", "The launcher will reopen shortly."));
                const std::wstring r = wide(h.root), s = wide(g.staged);
                auto a = text_call([&](char *o, int c) { return un_update_start_apply(r.c_str(), s.c_str(), o, c); }, 1 << 16,
                                   false);
                if (a.first != 0) {
                    g.busy = false;
                    out.set("dialog", dialog("error", "The installer couldn't be started:\n\n" + a.second));
                } else {
                    out.set("exit", Value::boolean(true));   // close now: the installer only starts its work once we are gone
                }
            }
        }
    }
    if (g.busy && g.release.is_object()) {
        long long done = 0, total = 0;
        if (g.installing) un_update_progress(&done, &total);
        Value u;
        u.kind = Value::Array;
        u.items = {Value::integer(done), Value::integer(total)};
        out.set("updating", u);
    } else {
        out.set("updating", Value::null());
    }
}

}  // namespace

extern "C" {

UN_API int un_updates(const char *request_json, char *out, int cap) {
    Value q;
    if (!json::parse(request_json ? request_json : "{}", q) || !q.is_object()) return 3;
    const Value none = Value::object();
    const Value &st = q.get("state") && q.get("state")->is_object() ? *q.get("state") : none;
    Host h;
    auto text = [](const Value &v, const char *k) { const Value *f = v.get(k); return f && f->kind == Value::String ? f->s : ""; };
    h.root = text(st, "root");
    h.mode = text(st, "mode");
    h.game_running = st.get("game_running") && st.get("game_running")->truthy();
    h.import_running = st.get("import_running") && st.get("import_running")->truthy();
    const std::string op = text(q, "op");
    Value result = Value::object();
    if (op == "check") {
        check(h, q.get("manual") && q.get("manual")->truthy(), result);
    } else if (op == "install") {
        install(h, q.get("auto") && q.get("auto")->truthy(), result);
    } else if (op == "confirmed") {
        if (q.get("answer") && q.get("answer")->truthy()) {
            // the question was asked: install without asking again (the same checks as before it)
            install(h, true, result);
        }
    } else if (op == "mode") {
        if (g.release.is_object() && !g.busy) {
            if (h.mode == "auto") {
                show_button(result, false);
                g.auto_at = now_ms();   // the automatic install, at the next tick
            } else if (h.mode == "manual") {
                show_button(result, true);
            }
        }
    } else if (op == "tick") {
        tick(h, result);
    }
    {
        std::lock_guard<std::mutex> l(g.m);
        result.set("hold_imports", Value::boolean(g.checking || (h.mode == "auto" && g.release.is_object() &&
                                                                 (g.busy || g.auto_at != 0))));
    }
    return give(json::dump(result, false), out, cap);
}

}  // extern "C"
