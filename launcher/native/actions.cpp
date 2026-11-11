// actions.cpp - TOOLS' actions, part of uncharted_native.dll: each one step by step, with the dialogs it needs. The
// launcher shows the dialog an answer asks for and calls again with the answer; a long install runs in a thread here
// while the launcher polls its step. The messages are all here; the work is in gameupdate.cpp, gamefiles.cpp,
// steam.cpp and system.cpp.
//
//   un_action(request_json, out, cap)
//     request: {"name": open_logs | open_saves | clear_cache | choose_game_dir | choose_update_folder |
//               install_game_update | install_game_pkg | check_game_files | add_to_steam, "value": the action's
//               argument (add_to_steam: one entry per game), "step": 0 to start, else the step the last answer said,
//               "data": what the last answer said to keep, "answer": the dialog's answer (true / false, a path, "" for
//               cancelled), "state": {"root", "user_dir", "game_dir", "game_running", "wine", "python", "script",
//               "games": [{"key", "name"}]}}
//     out: what the launcher does next, and any of "config" (launcher settings to save), "progress" (TOOLS' lines
//          to show: {"update" | "game_dir": text}), "result" (choose_game_dir: true when a folder was chosen):
//       {"dialog": {"kind": info | warning | error | yesno | okcancel | retrycancel | directory | open, "text",
//                   "title", "initialdir", "filetypes": [[name, pattern]]}, "step", "data"}   ask, then call again
//       {"work": true, "step", "data"}   an install runs: poll un_action_poll until it stops, then call again
//       {"done": true}
//   un_action_poll(out, cap)   {"running": bool, "text": the install's current step}
#include "native.h"
#include "json.h"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
UN_API int un_game_update(const char *what, const wchar_t *root, const char *args_json, char *out, int cap);
UN_API int un_game_update_step(char *out, int cap);
UN_API int un_check_game_files(const wchar_t *root, const wchar_t *game_dir, char *out, int cap);
}

namespace {

using json::Value;

const char *const APP_TITLE = "Uncharted: The Nathan Drake Collection";

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

// a text call of the DLL's convention (0 ok; a negative status: the size it needs); (status, text)
template <typename Call> std::pair<int, std::string> text_call(Call call) {
    std::string out(1 << 16, '\0');
    int status = call(&out[0], (int)out.size());
    if (status < 0) {
        out.assign(-status, '\0');
        status = call(&out[0], (int)out.size());
    }
    out.resize(std::strlen(out.c_str()));
    return {status, out};
}

std::string system_answer(const char *what, const std::string &arg) {
    const std::wstring a = wide(arg);
    auto r = text_call([&](char *o, int c) { return un_system(what, a.c_str(), o, c); });
    return r.first == 0 ? r.second : "";
}

bool exists(const std::string &path) { return GetFileAttributesW(wide(path).c_str()) != INVALID_FILE_ATTRIBUTES; }
bool is_game_dir(const std::string &folder) { return !folder.empty() && system_answer("is_game_dir", folder) == "1"; }

std::string parent_of(std::string path) {
    while (path.size() > 3 && (path.back() == '\\' || path.back() == '/')) path.pop_back();
    const size_t cut = path.find_last_of("\\/");
    if (cut == std::string::npos) return path;
    return cut <= 2 && path.size() > 1 && path[1] == ':' ? path.substr(0, cut + 1) : path.substr(0, cut);
}
std::string name_of(std::string path) {
    while (path.size() > 1 && (path.back() == '\\' || path.back() == '/')) path.pop_back();
    const size_t cut = path.find_last_of("\\/");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

void open_folder(const std::string &path) {
    ShellExecuteW(nullptr, L"open", wide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool delete_tree(const std::wstring &path) {
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = f.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = path + L"\\" + name;
            if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                delete_tree(full);
            } else {
                if (f.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(full.c_str());
                else DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    return RemoveDirectoryW(path.c_str()) != 0;
}

// gameupdate.cpp: (status, text); 1 a reason for the player, 3 a file system failure ("[WinError n] ...")
std::pair<int, std::string> game_update(const char *what, const std::string &root, std::vector<std::string> args) {
    Value a;
    a.kind = Value::Array;
    for (auto &s : args) a.items.push_back(Value::text(s));
    const std::string j = json::dump(a, false);
    const std::wstring r = wide(root);
    return text_call([&](char *o, int c) { return un_game_update(what, r.c_str(), j.c_str(), o, c); });
}

// a file system failure as the launcher's Python worded it: "<its OSError class>: <message>"
std::string os_error(const std::string &text) {
    int code = 0;
    if (text.compare(0, 10, "[WinError ") == 0) code = std::atoi(text.c_str() + 10);
    const char *kind = code == 2 || code == 3 ? "FileNotFoundError" : code == 5 || code == 32 ? "PermissionError"
                     : code == 80 || code == 183 ? "FileExistsError" : "OSError";
    return std::string(kind) + ": " + text;
}

// ------------------------------------------------------------------------------------------------ the install thread

struct Work {
    std::mutex m;
    std::thread thread;
    std::atomic<bool> running{false};
    int status = 0;
    std::string text;
} g_work;

void start_work(const char *what, const std::string &root, std::vector<std::string> args) {
    if (g_work.thread.joinable()) g_work.thread.join();
    g_work.running = true;
    g_work.thread = std::thread([what, root, args]() {
        auto r = game_update(what, root, args);
        {
            std::lock_guard<std::mutex> l(g_work.m);
            g_work.status = r.first;
            g_work.text = r.second;
        }
        g_work.running = false;
    });
}

std::pair<int, std::string> work_result() {
    if (g_work.thread.joinable()) g_work.thread.join();
    std::lock_guard<std::mutex> l(g_work.m);
    return {g_work.status, g_work.text};
}

// ------------------------------------------------------------------------------------------------ the answers

struct Request {
    std::string name, root, user_dir, game_dir, python, script;
    bool game_running = false, wine = false;
    Value games, value, data, answer, steam_accounts;
    long long step = 0;
};

std::string field(const Value &v, const char *key) {
    const Value *f = v.get(key);
    return f && f->kind == Value::String ? f->s : "";
}

Value reply() { return Value::object(); }

Value dialog(const char *kind, const std::string &text, long long step, Value data = Value::object()) {
    Value d = Value::object();
    d.set("kind", Value::text(kind));
    d.set("title", Value::text(APP_TITLE));
    d.set("text", Value::text(text));
    Value r = reply();
    r.set("dialog", std::move(d));
    r.set("step", Value::integer(step));
    r.set("data", std::move(data));
    return r;
}

// a file or folder dialog: filetypes [[name, pattern]]
Value file_dialog(const char *kind, const std::string &title, const std::string &initialdir, long long step,
                  Value data = Value::object(), bool packages = false) {
    Value d = Value::object();
    d.set("kind", Value::text(kind));
    d.set("title", Value::text(title));
    d.set("text", Value::text(""));
    d.set("initialdir", Value::text(initialdir));
    Value types;
    types.kind = Value::Array;
    if (packages)
        for (auto &t : {std::make_pair("PS4 package", "*.pkg"), std::make_pair("All files", "*.*")}) {
            Value pair;
            pair.kind = Value::Array;
            pair.items = {Value::text(t.first), Value::text(t.second)};
            types.items.push_back(std::move(pair));
        }
    d.set("filetypes", std::move(types));
    Value r = reply();
    r.set("dialog", std::move(d));
    r.set("step", Value::integer(step));
    r.set("data", std::move(data));
    return r;
}

Value done() {
    Value r = reply();
    r.set("done", Value::boolean(true));
    return r;
}

// a last message, then done: the launcher shows it and calls again with the step -1
Value finally(const char *kind, const std::string &text) { return dialog(kind, text, -1); }

Value with(Value r, const char *key, Value v) {
    r.set(key, std::move(v));
    return r;
}

Value config_change(std::initializer_list<std::pair<const char *, Value>> changes) {
    Value c = Value::object();
    for (auto &x : changes) c.set(x.first, x.second);
    return c;
}

bool answered_yes(const Request &q) { return q.answer.truthy(); }
std::string answered_path(const Request &q) { return q.answer.kind == Value::String ? q.answer.s : ""; }

// the newest of the games' boot.log files' folder, else the analysis folder
Value open_logs(const Request &q) {
    std::string best;
    FILETIME best_time{};
    for (const char *folder : {"uncharted", "big2-ps4_Shipping", "big3-ps4_Shipping"}) {
        const std::string log = q.root + "\\analysis\\" + folder + "\\boot.log";
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExW(wide(log).c_str(), GetFileExInfoStandard, &a)) continue;
        if (best.empty() || CompareFileTime(&a.ftLastWriteTime, &best_time) > 0) {
            best = log;
            best_time = a.ftLastWriteTime;
        }
    }
    open_folder(best.empty() ? q.root + "\\analysis" : parent_of(best));
    return done();
}

Value open_saves(const Request &q) {
    const std::string saves = q.user_dir + "\\savedata";
    open_folder(exists(saves) ? saves : q.user_dir);
    return done();
}

Value clear_cache(const Request &q) {
    const std::string cache = q.user_dir + "\\cache";
    if (q.step == 0) {
        if (!exists(cache)) return finally("info", "No shader cache has been built yet.");
        if (q.game_running) return finally("warning", "Please close the game first.");
        return dialog("yesno", "Clear the shader cache?\n\nThe shaders get rebuilt the next time you play, so "
                               "expect some stutter until that is done.", 1);
    }
    if (!answered_yes(q)) return done();
    delete_tree(wide(cache));
    return finally("info", "The shader cache is cleared.");
}

Value game_folder_change(const Request &q, const std::string &folder) {
    Value c = config_change({{"game_dir", Value::text(folder)}});
    if (q.wine) c.set("game_dir_unix", system_answer("unix_path", folder).empty() ? Value::null()
                                                                                  : Value::text(system_answer("unix_path", folder)));
    return c;
}

Value choose_game_dir(const Request &q) {
    if (q.step == 0)
        return file_dialog("directory", "Pick the game folder (it contains eboot.bin and sce_sys)",
                           exists(q.game_dir) ? q.game_dir : q.root, 1);
    if (q.step == 1) {
        const std::string folder = answered_path(q);
        if (folder.empty()) return with(done(), "result", Value::boolean(false));
        if (!is_game_dir(folder))
            return with(dialog("error", "eboot.bin and sce_sys/param.sfo are not in that folder.\n\nPick the top "
                                        "folder of the game (CUSA02344).", 2), "result", Value::boolean(false));
        return with(with(done(), "config", game_folder_change(q, folder)), "result", Value::boolean(true));
    }
    return with(done(), "result", Value::boolean(false));
}

Value choose_update_folder(const Request &q) {
    if (q.step == 0) {
        if (q.game_running) return finally("warning", "Please close the game first.");
        if (!is_game_dir(q.game_dir)) return finally("warning", "Set the game folder first (Change game folder).");
        Value d = file_dialog("directory", "Pick the unpacked update 1.02 folder", "", 1);
        return d;
    }
    const std::string folder = answered_path(q);
    if (folder.empty()) return done();
    auto r = game_update("check_folder", q.root, {folder, q.game_dir});
    if (r.first == 1) return finally("error", "That folder doesn't work as the update:\n" + r.second);
    if (r.first != 0) return finally("error", "That folder doesn't work as the update:\n" + (r.first == 3 ? os_error(r.second) : r.second));
    const std::string version = system_answer("app_version", r.second);
    return with(finally("info", "Update " + (version.empty() ? std::string("?") : version) + " will be used from now on:\n" +
                                    r.second),
                "config", config_change({{"update_dir", Value::text(r.second)}}));
}

std::string install_error(const std::pair<int, std::string> &r, const char *what) {
    if (r.first == 1) return r.second;
    if (r.first == 3) return os_error(r.second);
    return std::string("ValueError: ") + what;
}

Value install_game_update(const Request &q) {
    if (q.step == 0) {
        if (q.game_running) return finally("warning", "Please close the game first.");
        if (!is_game_dir(q.game_dir)) return finally("warning", "Set the game folder first (Change game folder).");
        return file_dialog("open", "Pick the update package (.pkg)", "", 1, Value::object(), true);
    }
    auto begin = [&](const std::string &pkg) {
        start_work("install", q.root, {pkg, q.game_dir});
        Value r = reply();
        r.set("work", Value::boolean(true));
        r.set("step", Value::integer(3));
        r.set("data", Value::object());
        r.set("progress", config_change({{"update", Value::text("Checking the update package...")}}));
        return r;
    };
    if (q.step == 1) {
        const std::string pkg = answered_path(q);
        if (pkg.empty()) return done();
        auto existing = game_update("existing_update", q.root, {q.game_dir});
        if (existing.first == 0 && !existing.second.empty()) {
            Value data = Value::object();
            data.set("pkg", Value::text(pkg));
            return dialog("yesno", "An update folder exists already:\n" + existing.second + "\n\nReplace it with this package?",
                          2, std::move(data));
        }
        return begin(pkg);
    }
    if (q.step == 2) {
        if (!answered_yes(q)) return done();
        return begin(field(q.data, "pkg"));
    }
    if (q.step == 3) {
        auto r = work_result();
        Value out;
        if (r.first == 0) {   // the installed one is used, also over a folder chosen before
            out = finally("info", "The update is installed:\n" + r.second + "\n\nThe games pick it up when they "
                                                                            "next start.");
            out.set("config", config_change({{"update_dir", Value::text(r.second)}}));
        } else {
            out = finally("error", "The update could not be installed:\n" + install_error(r, "install"));
        }
        out.set("progress", config_change({{"update", Value::text("")}}));
        return out;
    }
    return done();
}

Value install_game_pkg(const Request &q) {
    if (q.step == 0) {
        if (q.game_running) return finally("warning", "Please close the game first.");
        return file_dialog("open", "Pick the game package (.pkg), not the update", "", 1, Value::object(), true);
    }
    if (q.step == 1) {
        const std::string pkg = answered_path(q);
        if (pkg.empty()) return done();
        Value data = Value::object();
        data.set("pkg", Value::text(pkg));
        return file_dialog("directory", "Pick a folder for the game (it needs about 60 GB)", parent_of(pkg), 2, std::move(data));
    }
    if (q.step == 2) {
        const std::string parent = answered_path(q);
        if (parent.empty()) return done();
        start_work("install_game", q.root, {field(q.data, "pkg"), parent});
        Value r = reply();
        r.set("work", Value::boolean(true));
        r.set("step", Value::integer(3));
        r.set("data", Value::object());
        r.set("progress", config_change({{"game_dir", Value::text("Checking the game package first...")}}));
        return r;
    }
    if (q.step == 3) {
        auto r = work_result();
        Value out;
        if (r.first == 0) {
            out = finally("info", "The game is installed:\n" + r.second + "\n\nThe launcher uses it as the game "
                                                                          "folder from now on. Next step: add update "
                                                                          "1.02 with GAME UPDATE -> Install from "
                                                                          ".pkg.");
            out.set("config", game_folder_change(q, r.second));
        } else {
            out = finally("error", "The game could not be installed:\n" + install_error(r, "install_game"));
        }
        out.set("progress", config_change({{"game_dir", Value::text("")}}));
        return out;
    }
    return done();
}

std::vector<std::string> lines_of(const std::string &text) {   // str.splitlines()
    std::vector<std::string> lines;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find_first_of("\r\n", at);
        if (end == std::string::npos) end = text.size();
        lines.push_back(text.substr(at, end - at));
        at = end + (text.compare(end, 2, "\r\n") == 0 ? 2 : 1);
    }
    return lines;
}

Value check_game_files(const Request &q) {
    const std::wstring r = wide(q.root), g = wide(q.game_dir);
    std::string out(1 << 20, '\0');
    int status = un_check_game_files(r.c_str(), g.c_str(), &out[0], (int)out.size());
    if (status < 0) {
        out.assign(-status, '\0');
        status = un_check_game_files(r.c_str(), g.c_str(), &out[0], (int)out.size());
    }
    out.resize(std::strlen(out.c_str()));
    const std::vector<std::string> lines = lines_of(out);
    auto joined = [](const std::vector<std::string> &l, size_t from, size_t count) {
        std::string s;
        for (size_t i = from; i < l.size() && i < from + count; ++i) s += (i > from ? "\n" : "") + l[i];
        return s;
    };
    if (status == 2) return finally("error", "The file check could not be done:\n\n" + joined(lines, 0, lines.size()));
    if (status == 0) return finally("info", "All game files are present.\n\n" + (lines.empty() ? std::string() : lines[0]));
    const size_t problems = lines.size() > 1 ? lines.size() - 1 : 0;
    std::string shown = joined(lines, 1, 15);
    if (problems > 15) shown += "\n... and " + std::to_string(problems - 15) + " more";
    return finally("error", "Some game files are missing or wrong, and the game will stop when it reaches them. "
                            "Unpack or dump the base game again.\n\n" + (lines.empty() ? std::string() : lines[0]) + "\n\n" + shown);
}

// Steam's text requests (steam.cpp): rows of tab-separated fields
std::pair<int, std::string> steam_request(int (*fn)(const char *, char *, int), const std::vector<std::vector<std::string>> &rows) {
    std::string text;
    for (auto &row : rows) {
        for (size_t i = 0; i < row.size(); ++i) text += (i ? "\t" : "") + row[i];
        text += "\n";
    }
    return text_call([&](char *o, int c) { return fn(text.c_str(), o, c); });
}

std::vector<std::string> split_tabs(const std::string &line) {
    std::vector<std::string> f;
    size_t at = 0;
    while (true) {
        size_t end = line.find('\t', at);
        f.push_back(line.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return f;
}

Value add_to_steam(const Request &q) {
    const bool per_game = q.value.truthy();
    if (q.step == 1 && !answered_yes(q)) return done();   // Retry / Cancel on "Steam is open"
    if (q.step <= 1) {
        if (un_steam_running())
            return dialog("retrycancel", "Steam is running. It rewrites its game list when it quits, which would "
                                         "undo anything added now.\n\nQuit Steam fully (Steam menu -> Exit, or "
                                         "right-click its tray icon -> Exit) and then press Retry.", 1);
    }
    if (q.step > 1) return done();
    std::vector<std::vector<std::string>> rows = {{"root", q.root}, {"starter", q.root + "\\Uncharted Launcher.exe"},
                                                  {"python", q.python}, {"script", q.script}};
    std::vector<std::string> names = {APP_TITLE};
    for (auto &g : q.games.items) names.push_back(field(g, "name"));
    if (per_game) {
        rows.push_back({"per_game"});
        for (auto &g : q.games.items) rows.push_back({"game", field(g, "key"), field(g, "name")});
    }
    auto targets = steam_request(un_steam_targets, rows);
    std::vector<std::vector<std::string>> add;
    for (auto &line : lines_of(targets.second))
        if (!line.empty()) {
            std::vector<std::string> row = {"target"};
            for (auto &f : split_tabs(line)) row.push_back(f);
            add.push_back(row);
        }
    for (auto &n : names) add.push_back({"name", n});
    add.push_back({"icon", q.root + "\\launcher\\assets\\uncharted.ico"});
    const bool pictures = is_game_dir(q.game_dir);
    if (pictures) add.push_back({"game_dir", q.game_dir});
    for (auto &a : q.steam_accounts.items)
        if (a.items.size() == 2) add.push_back({"account", a.items[0].s, a.items[1].s});
    auto result = steam_request(un_steam_add, add);
    if (result.first == 1) {
        std::string text = "Steam isn't set up on this PC (Steam's userdata has no account folder). Install Steam, "
                           "sign in once, and try again.";
        if (q.wine) text += "\n\nOn Linux, add the launcher from inside Steam: Games -> Add a Non-Steam Game to My "
                            "Library.";
        return finally("error", text);
    }
    if (result.first == 2)
        return finally("error", "Steam's non-Steam game list couldn't be read, so it was not changed:\n" + result.second);
    if (result.first != 0) return finally("error", "Steam's game list could not be saved:\n" + result.second);
    std::string lines, backups;
    for (auto &line : lines_of(result.second)) {
        if (line.empty()) continue;
        const std::vector<std::string> f = split_tabs(line);   // account, folder, name, file, backup, added, updated, removed
        if (f.size() < 10) continue;
        const std::string who = !f[2].empty() ? f[2] + " (" + name_of(f[1]) + ")" : "Steam account " + name_of(f[1]);
        std::string changes;
        for (auto &c : {std::make_pair(f[5], "added"), std::make_pair(f[6], "updated"), std::make_pair(f[7], "replaced")})
            if (std::atoi(c.first.c_str())) changes += (changes.empty() ? "" : ", ") + std::to_string(std::atoi(c.first.c_str())) + " " + c.second;
        lines += (lines.empty() ? "" : "\n") + who + ": " + changes;
        if (!f[4].empty()) backups += (backups.empty() ? "" : "\n") + f[4];
    }
    const std::string added = per_game ? "the three games" : std::string("\"") + APP_TITLE + "\"";
    const std::string pictures_text = pictures ? "The library artwork is taken from your game files."
                                               : "No library artwork yet: set the game folder, then add again.";
    return finally("info", "Added " + added + " to Steam:\n" + lines + "\n\n" + pictures_text +
                               (backups.empty() ? "" : "\n\nA copy of the old list is at:\n" + backups) +
                               "\n\nOpen Steam again: " + (per_game ? "they are" : "it is") + " in your Library now.");
}

Value run(const Request &q) {
    if (q.step == -1) return done();   // after a last message
    if (q.name == "open_logs") return open_logs(q);
    if (q.name == "open_saves") return open_saves(q);
    if (q.name == "clear_cache") return clear_cache(q);
    if (q.name == "choose_game_dir") return choose_game_dir(q);
    if (q.name == "choose_update_folder") return choose_update_folder(q);
    if (q.name == "install_game_update") return install_game_update(q);
    if (q.name == "install_game_pkg") return install_game_pkg(q);
    if (q.name == "check_game_files") return check_game_files(q);
    if (q.name == "add_to_steam") return add_to_steam(q);
    return done();
}

}  // namespace

extern "C" {

UN_API int un_action(const char *request_json, char *out, int cap) {
    Value v;
    if (!json::parse(request_json ? request_json : "{}", v) || !v.is_object()) return 3;
    Request q;
    q.name = field(v, "name");
    if (const Value *s = v.get("step")) q.step = s->i;
    if (const Value *d = v.get("data")) q.data = *d;
    if (const Value *a = v.get("answer")) q.answer = *a;
    if (const Value *x = v.get("value")) q.value = *x;
    const Value none = Value::object();
    const Value &st = v.get("state") && v.get("state")->is_object() ? *v.get("state") : none;
    q.root = field(st, "root");
    q.user_dir = field(st, "user_dir");
    q.game_dir = field(st, "game_dir");
    q.python = field(st, "python");
    q.script = field(st, "script");
    q.game_running = st.get("game_running") && st.get("game_running")->truthy();
    q.wine = st.get("wine") && st.get("wine")->truthy();
    if (const Value *g = st.get("games")) q.games = *g;
    if (const Value *a = st.get("steam_accounts")) q.steam_accounts = *a;   // tests: made-up accounts [[folder, name]]
    return give(json::dump(run(q), false), out, cap);
}

UN_API int un_action_poll(char *out, int cap) {
    Value r = Value::object();
    r.set("running", Value::boolean(g_work.running.load()));
    std::string step(1024, '\0');
    if (un_game_update_step(&step[0], (int)step.size()) == 0) step.resize(std::strlen(step.c_str()));
    else step.clear();
    r.set("text", Value::text(step));
    return give(json::dump(r, false), out, cap);
}

}  // extern "C"
