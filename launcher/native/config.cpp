// config.cpp - settings handling inside uncharted_native.dll. It owns one runtime settings file per game
// (analysis/uncharted-user/probe-<game>.ini, which the in-game menu reads and writes too), the default values, the
// list of frame-rate caps and the graphics presets. The launcher pages read and update all of it through the exports
// further down.
//
// File format: one "key=value" per line. An update rewrites matching keys on the line they already occupy and adds
// keys that are not there yet at the end; anything else in the file (comment lines, keys only the in-game menu
// knows) is left untouched. Output uses CR LF line breaks.

#include "native.h"
#include "json.h"

#include <cstdlib>
#include <windows.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using Pairs = std::vector<std::pair<std::string, std::string>>;

// ------------------------------------------------------------------------------------------------ tables

const Pairs &ini_defaults() {
    static const Pairs d = {
        {"display_mode", "windowed"}, {"vsync", "1"}, {"fps_limit", "60"}, {"frames_queued", "1"},
        {"async_shaders", "1"}, {"fsr1", "0"}, {"rcas", "1"}, {"rcas_strength", "75"}, {"show_fps", "1"},
        {"upscaler", "off"},
        // FPS overlay start values (which readouts, screen corner, arrangement, text scale, backdrop opacity), matching
        // what the runtime uses when nothing is set
        {"overlay_fps", "1"}, {"overlay_frametime", "1"}, {"overlay_gpu", "0"}, {"overlay_cpu", "0"},
        {"overlay_ram", "0"}, {"overlay_vram", "0"}, {"overlay_upscaler", "1"}, {"overlay_corner", "1"},
        {"overlay_layout", "0"}, {"overlay_scale", "100"}, {"overlay_opacity", "50"},
    };
    return d;
}

const int MAX_FPS = 120;
const int FPS_LIMITS[] = {30, 40, 45, 50, 55, 60, 75, 90, 100, MAX_FPS};

struct Preset {
    const char *name, *title, *summary, *detail, *resolution, *upscale;
    Pairs values;
};

Pairs common(const char *fps, const char *fsr1, const char *sharpen) {
    return {{"fps_limit", fps}, {"frames_queued", "1"}, {"vsync", "1"}, {"async_shaders", "1"},
            {"fsr1", fsr1},     {"rcas", "1"},          {"rcas_strength", sharpen}};
}

const std::vector<Preset> &presets() {
    static const std::vector<Preset> list = [] {
        std::vector<Preset> p;
        // Credit: the IGPU preset (its card, the values it sets and the integrated-GPU detection) is based on
        // PR #223 by memeguyoffical.
        Pairs igpu = common("60", "1", "75");
        igpu.push_back({"upscaler", "off"});
        p.push_back({"igpu", "IGPU", "30\xE2\x80\x93" "60 FPS  \xE2\x80\xA2  540\xE2\x80\x93" "720p, FSR 1",
                     "Made for Intel and AMD integrated GPUs. Rendering happens at 540p (720p on the fastest iGPUs, "
                     "360p on the slowest), then FSR 1 brings it up to 1080p. Expect around 30 FPS on Vega and 2-CU "
                     "Radeon and 60 on Radeon 680M/760M/780M or Iris Xe. Anti-aliasing is off and memory use is "
                     "the smallest of all presets.",
                     "1920x1080", "performance", igpu});
        p.push_back({"low", "LOW", "60 FPS  \xE2\x80\xA2  1080p, FSR 1",
                     "Made for older GPUs. Rendering happens at half of 1080p and FSR 1 scales it back up, while "
                     "the HUD is kept at full sharpness.",
                     "1920x1080", "performance", common("60", "1", "75")});
        p.push_back({"medium", "MEDIUM", "60 FPS  \xE2\x80\xA2  as on PS4",
                     "Matches the PS4 version: 1080p at 60 FPS. A good fit for most PCs.", "1920x1080", "off",
                     common("60", "0", "75")});
        p.push_back({"high", "HIGH", "60 FPS  \xE2\x80\xA2  1440p", "A crisper picture, rendered at 1440p.",
                     "2560x1440", "off", common("60", "0", "50")});
        p.push_back({"ultra", "ULTRA", "4K  \xE2\x80\xA2  up to 120 FPS",
                     "Made for fast GPUs: renders at native 4K and allows up to 120 FPS (experimental). In "
                     "Drake's Fortune rendering stops at 1620p and is upscaled from there.",
                     "3840x2160", "off", common("120", "0", "50")});
        return p;
    }();
    return list;
}

// ------------------------------------------------------------------------------------------------ helpers

std::wstring widen(const char *s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    w.resize(n - 1);
    return w;
}

std::string trim(const std::string &s) {
    const char *ws = " \t\r\n\v\f";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return std::string();
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

bool read_text(const std::wstring &path, std::string &text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (16 << 20);
    if (ok) {
        text.assign((size_t)size.QuadPart, '\0');
        DWORD got = 0;
        ok = text.empty() || (ReadFile(h, text.data(), (DWORD)text.size(), &got, nullptr) && got == text.size());
    }
    CloseHandle(h);
    return ok;
}

// splits text at CR LF, a lone LF or a lone CR and drops the break characters (same result as splitlines() on
// these files)
std::vector<std::string> lines_of(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r' || c == '\n') {
            out.push_back(cur);
            cur.clear();
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::wstring settings_file(const std::wstring &user_dir, const char *game) {
    return user_dir + L"\\probe-" + widen(game) + L".ini";
}

// starts from the per-game file; failing that, from probe.ini (the single file older versions shared); failing
// both, from just a header comment
std::vector<std::string> settings_lines(const std::wstring &user_dir, const char *game) {
    std::string text;
    if (read_text(settings_file(user_dir, game), text) || read_text(user_dir + L"\\probe.ini", text)) return lines_of(text);
    return {"# Uncharted settings (launcher and in-game menu: Insert / R3+L2)"};
}

bool is_setting(const std::string &line) {
    size_t i = line.find_first_not_of(" \t\r\n\v\f");
    return line.find('=') != std::string::npos && !(i != std::string::npos && line[i] == '#');
}

std::string key_of(const std::string &line) { return trim(line.substr(0, line.find('='))); }

// keeps a value from FPS_LIMITS as is; 0 (no cap) or anything above MAX_FPS (older builds let you pick 144-240) is
// clamped to MAX_FPS, and everything else turns into 60
std::string fps_limit(const std::string &value) {
    for (int v : FPS_LIMITS)
        if (value == std::to_string(v)) return value;
    std::string t = trim(value);
    bool number = !t.empty() && t.find_first_not_of("+-0123456789") == std::string::npos &&
                  t.find_first_of("0123456789") != std::string::npos;
    if (!number) return "60";
    long long n = _strtoi64(t.c_str(), nullptr, 10);
    return (n == 0 || n > MAX_FPS) ? std::to_string(MAX_FPS) : "60";
}

int give(const std::string &text, char *out, int cap) {
    if ((int)text.size() + 1 > cap) return -(int)(text.size() + 1);
    std::memcpy(out, text.c_str(), text.size() + 1);
    return 0;
}

bool write_bytes(const std::wstring &path, const std::string &data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size());
    ok = CloseHandle(h) && ok;
    return ok;
}

Pairs ini_values(const std::wstring &user_dir, const char *game);
bool ini_update(const std::wstring &user_dir, const char *game, const Pairs &changes);
json::Value read_config(const std::wstring &config_file, const std::wstring &prefix_file, const std::wstring &user_dir,
                        bool wine);
bool write_config(const std::wstring &path, const json::Value &config);

}  // namespace

extern "C" {

// Returns one of the fixed tables as tab-separated lines, picked by name:
//   "ini_defaults" -> key\tvalue
//   "fps_limits"   -> value\tlabel
//   "presets"      -> name\ttitle\tsummary\tdetail\tresolution\tupscale\tkey=value;key=value...
UN_API int un_settings_table(const char *name, char *out, int cap) {
    std::string text, n = name ? name : "";
    if (n == "ini_defaults") {
        for (auto &kv : ini_defaults()) text += kv.first + "\t" + kv.second + "\n";
    } else if (n == "fps_limits") {
        for (int v : FPS_LIMITS) text += std::to_string(v) + "\t" + std::to_string(v) + "\n";
    } else if (n == "presets") {
        for (const Preset &p : presets()) {
            std::string values;
            for (size_t i = 0; i < p.values.size(); ++i) values += (i ? ";" : "") + p.values[i].first + "=" + p.values[i].second;
            text += std::string(p.name) + "\t" + p.title + "\t" + p.summary + "\t" + p.detail + "\t" + p.resolution + "\t" +
                    p.upscale + "\t" + values + "\n";
        }
    } else {
        return give("", out, cap) < 0 ? -1 : 1;
    }
    return give(text, out, cap);
}

// Effective runtime settings for one game as key\tvalue lines: every default key comes first (overridden by the
// file where present), followed by keys only the file has, in the order they appear there. fps_limit is passed
// through fps_limit() before it is returned.
UN_API int un_ini_read(const wchar_t *user_dir, const char *game, char *out, int cap) {
    std::string text;
    for (auto &kv : ini_values(user_dir, game)) text += kv.first + "\t" + kv.second + "\n";
    return give(text, out, cap);
}

// `updates` holds key\tvalue lines. Each key is rewritten on every line where it already occurs, keys that occur
// nowhere are added at the end. Result: 0 on success, 1 if writing the file failed.
UN_API int un_ini_write(const wchar_t *user_dir, const char *game, const char *updates) {
    Pairs changes;
    std::string u = updates ? updates : "";
    for (const std::string &line : lines_of(u)) {
        size_t tab = line.find('\t');
        if (tab != std::string::npos) changes.push_back({line.substr(0, tab), line.substr(tab + 1)});
    }
    return ini_update(user_dir, game, changes) ? 0 : 1;
}

UN_API int un_config_read(const wchar_t *config_file, const wchar_t *prefix_file, const wchar_t *user_dir, int wine,
                          char *out, int cap) {
    return give(json::dump(read_config(config_file, prefix_file, user_dir, wine != 0)), out, cap);
}

UN_API int un_config_write(const wchar_t *config_file, const char *config_json) {
    json::Value v;
    if (!json::parse(config_json ? config_json : "", v) || !v.is_object()) return 2;
    return write_config(config_file, v) ? 0 : 1;
}

}  // extern "C"

namespace {

Pairs ini_values(const std::wstring &user_dir, const char *game) {
    Pairs values = ini_defaults();
    for (const std::string &line : settings_lines(user_dir, game)) {
        if (!is_setting(line)) continue;
        std::string key = key_of(line), value = trim(line.substr(line.find('=') + 1));
        bool found = false;
        for (auto &kv : values)
            if (kv.first == key) { kv.second = value; found = true; }
        if (!found) values.push_back({key, value});
    }
    for (auto &kv : values)
        if (kv.first == "fps_limit") kv.second = fps_limit(kv.second);
    return values;
}

bool ini_update(const std::wstring &user_dir, const char *game, const Pairs &changes) {
    std::vector<std::string> lines = settings_lines(user_dir, game);
    std::vector<bool> done(changes.size(), false);
    for (std::string &line : lines) {
        if (!is_setting(line)) continue;
        std::string key = key_of(line);
        for (size_t i = 0; i < changes.size(); ++i)
            if (changes[i].first == key) { line = key + "=" + changes[i].second; done[i] = true; }
    }
    for (size_t i = 0; i < changes.size(); ++i)
        if (!done[i]) {
            bool earlier = false;   // duplicate keys in `changes`: only the final occurrence gets appended (dict-like)
            for (size_t j = i + 1; j < changes.size(); ++j) earlier |= changes[j].first == changes[i].first;
            if (!earlier) lines.push_back(changes[i].first + "=" + changes[i].second);
        }
    std::string text;
    for (const std::string &line : lines) text += line + "\r\n";
    std::wstring dir = user_dir;
    CreateDirectoryW(dir.c_str(), nullptr);
    return write_bytes(settings_file(dir, game), text);
}

// ------------------------------------------------------------------------------------------------ launcher.json

using json::Value;

const char *const GAME_KEYS[] = {"u1", "u2", "u3"};
const char *const RESOLUTION_IDS[] = {"960x540", "1280x720", "1600x900", "1920x1080", "2560x1440", "2880x1620", "3840x2160"};
const char *const ASPECT_IDS[] = {"16:10", "16:9", "21:9", "32:9"};

Value launcher_defaults() {
    Value d = Value::object();
    d.set("game", Value::text("u1"));
    d.set("diagnostics", Value::boolean(true));
    d.set("game_dir", Value::text(""));
    d.set("check_updates", Value::boolean(true));
    d.set("language", Value::text("1"));
    d.set("unlock", Value::boolean(false));
    return d;
}

// settings stored separately for every game, and the value each starts with
std::vector<std::pair<std::string, Value>> game_setting_defaults() {
    return {{"preset", Value::text("medium")},       {"resolution", Value::text("1920x1080")},
            {"aspect", Value::text("16:9")},         {"multithreaded", Value::boolean(false)},
            {"save_profile", Value::text("1")},      {"async_readback", Value::boolean(false)},
            {"controls", Value::null()},             {"mouse", Value::boolean(true)},
            {"mouse_sens", Value::text("1.0")},      {"mouse_invert", Value::boolean(false)},
            {"prompts", Value::text("auto")},        {"mouse_aim_sens", Value::text("1.0")},
            {"mouse_vertical", Value::text("1.0")},  {"mouse_invert_x", Value::boolean(false)},
            {"vibration", Value::boolean(true)}};
}

const Preset *preset_named(const Value &v) {
    if (v.kind != Value::String) return nullptr;
    for (const Preset &p : presets())
        if (v.s == p.name) return &p;
    return nullptr;
}

bool valid_utf8(const std::string &s) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0) > 0 || s.empty();
}

// Loads a launcher.json and returns its top-level object (empty if the file does not exist). If the content does
// not parse, for example because a write was interrupted, a copy is saved next to it with a .bad suffix and an
// empty object is returned so the defaults take over.
Value load_config_file(const std::wstring &path) {
    std::string text;
    if (!read_text(path, text)) return Value::object();
    Value v;
    if (!valid_utf8(text) || !json::parse(text, v)) {
        CopyFileW(path.c_str(), (path + L".bad").c_str(), FALSE);
        return Value::object();
    }
    return v.is_object() ? v : Value::object();
}

double version_of(const Value &config) {
    const Value *v = config.get("version");
    if (!v) return 1;
    if (v->kind == Value::Int) return (double)v->i;
    if (v->kind == Value::Float) return v->f;
    if (v->kind == Value::Bool) return v->b ? 1 : 0;
    return 1e18;   // non-numeric version: treat it as newer than everything so no migration step runs
}

std::string unix_path(const std::wstring &path) {
    typedef char *(__cdecl * ToUnix)(LPCWSTR);
    ToUnix to_unix = (ToUnix)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    if (!to_unix) return std::string();
    char *p = to_unix(path.c_str());
    if (!p) return std::string();
    std::string s = p;
    HeapFree(GetProcessHeap(), 0, p);
    return s;
}

Value read_config(const std::wstring &config_file, const std::wstring &prefix_file, const std::wstring &user_dir, bool wine) {
    Value config = launcher_defaults();
    Value stored = load_config_file(config_file);
    if (lstrcmpiW(config_file.c_str(), prefix_file.c_str()) != 0) {
        // Wine case: values in the shared file take priority; gaps are filled per game from the copy stored in this
        // Wine prefix
        Value older = load_config_file(prefix_file);
        if (!older.fields.empty()) {
            Value games = Value::object();
            for (const Value *source : {&older, &stored}) {
                const Value *g = source->get("games");
                if (!g || !g->is_object()) continue;
                for (auto &kv : g->fields) {
                    if (!kv.second.is_object()) continue;
                    Value &mine = games.setdefault(kv.first, Value::object());
                    for (auto &f : kv.second.fields) mine.set(f.first, f.second);
                }
            }
            Value merged = older;
            for (auto &f : stored.fields) merged.set(f.first, f.second);
            merged.set("games", games);
            stored = merged;
        }
    }
    for (auto &f : stored.fields) config.set(f.first, f.second);
    // migration to 2: older presets could pick a render resolution above 1080p, which crashed during play
    if (version_of(config) < 2) config.set("resolution", Value::text("1920x1080"));
    // migration to 3: preset, resolution and multithreading become per-game; each game is seeded from the old
    // top-level values
    if (!config.get("games") || !config.get("games")->is_object()) config.set("games", Value::object());
    Value &games = *config.get("games");
    for (const char *game : GAME_KEYS) {
        if (!games.get(game) || !games.get(game)->is_object()) games.set(game, Value::object());
        Value &entry = *games.get(game);
        for (auto &d : game_setting_defaults()) {
            const Value *shared = config.get(d.first);
            entry.setdefault(d.first, shared ? *shared : d.second);
        }
        if (entry.get("preset")->equals_text("balanced")) entry.set("preset", Value::text("medium"));
        // an unknown value (from a different build or edited by hand) is reset on its own, the rest is kept
        bool ok = false;
        for (const char *r : RESOLUTION_IDS) ok |= entry.get("resolution")->equals_text(r);
        if (!ok) entry.set("resolution", Value::text("1920x1080"));
        ok = false;
        for (const char *a : ASPECT_IDS) ok |= entry.get("aspect") && entry.get("aspect")->equals_text(a);
        if (!ok) entry.set("aspect", Value::text("16:9"));
    }
    auto each_game = [&](auto body) {
        for (auto &kv : games.fields)
            if (kv.second.is_object()) body(kv.first, kv.second);
    };
    // migration to 4: presets now render at 720p, 1080p, 1440p or 4K instead of a fixed 1080p
    if (version_of(config) < 4)
        each_game([&](const std::string &game, Value &entry) {
            const Value *name = entry.get("preset");
            if (const Preset *p = name ? preset_named(*name) : nullptr) {
                entry.set("resolution", Value::text(p->resolution));
                ini_update(user_dir, game.c_str(), p->values);
            }
        });
    // migration to 5: preset users got frames_queued=2, except where multithreaded rendering was on
    if (version_of(config) < 5)
        each_game([&](const std::string &game, Value &entry) {
            const Value *name = entry.get("preset"), *mt = entry.get("multithreaded");
            if (name && preset_named(*name) && !(mt && mt->truthy())) ini_update(user_dir, game.c_str(), {{"frames_queued", "2"}});
        });
    // migration to 6: back to frames_queued=1 for preset users, because a depth of 2 delivered GPU results one frame
    // too late; hand-tuned settings are not touched
    if (version_of(config) < 6)
        each_game([&](const std::string &game, Value &entry) {
            const Value *name = entry.get("preset");
            if (name && preset_named(*name)) ini_update(user_dir, game.c_str(), {{"frames_queued", "1"}});
        });
    // migration to 7: FSR 1 now works as an upscaler, so the game renders below the chosen resolution. The Low preset
    // switches to half-1080p rendering with FSR 1; any other setup that had FSR 1 enabled gets it turned off and
    // keeps its resolution
    if (version_of(config) < 7)
        each_game([&](const std::string &game, Value &entry) {
            const Value *name = entry.get("preset");
            if (name && name->equals_text("low")) {
                const Preset *low = preset_named(*name);
                entry.set("resolution", Value::text(low->resolution));
                entry.set("upscale", Value::text(low->upscale));
                ini_update(user_dir, game.c_str(), {{"fsr1", "1"}});
            } else {
                std::string fsr1;
                for (auto &kv : ini_values(user_dir, game.c_str()))
                    if (kv.first == "fsr1") fsr1 = kv.second;
                const Value *up = entry.get("upscale");
                bool off = !up || up->equals_text("off");
                if (fsr1 == "1" && off) ini_update(user_dir, game.c_str(), {{"fsr1", "0"}});
            }
        });
    // migration to 8: switch multithreaded rendering on for Drake's Deception. This runs a single time, so turning
    // it off afterwards sticks
    if (version_of(config) < 8) games.get("u3")->set("multithreaded", Value::boolean(true));
    for (auto &d : game_setting_defaults()) config.remove(d.first);
    config.set("version", Value::integer(8));
    // on Wine, store the game folder's Linux path as well, since a different prefix can map that folder to a
    // different drive letter
    const Value *folder = config.get("game_dir");
    const Value *known = config.get("game_dir_unix");
    if (wine && folder && folder->kind == Value::String && !folder->s.empty() && !(known && known->truthy())) {
        int n = MultiByteToWideChar(CP_UTF8, 0, folder->s.c_str(), -1, nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, folder->s.c_str(), -1, w.data(), n);
        w.resize(n - 1);
        if (GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::string unix = unix_path(w);
            if (!unix.empty()) config.set("game_dir_unix", Value::text(unix));
        }
    }
    return config;
}

// Saves via <file>.<pid>.tmp and a rename, because the launcher and a shortcut-started game can save at the same
// moment and an interrupted save must never leave a truncated file. If the rename is refused (the file held open
// by an antivirus or another launcher instance), the file is written directly instead.
bool write_config(const std::wstring &path, const Value &config) {
    std::string text, raw = json::dump(config);
    for (char c : raw) {
        if (c == '\n') text += '\r';
        text += c;
    }
    size_t cut = path.find_last_of(L"\\/");
    if (cut != std::wstring::npos) {
        std::wstring dir = path.substr(0, cut);
        for (size_t k = 3; k <= dir.size(); ++k)
            if (k == dir.size() || dir[k] == L'\\' || dir[k] == L'/') CreateDirectoryW(dir.substr(0, k).c_str(), nullptr);
    }
    std::wstring temp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    if (write_bytes(temp, text) && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(temp.c_str());
    return write_bytes(path, text);
}

}  // namespace
