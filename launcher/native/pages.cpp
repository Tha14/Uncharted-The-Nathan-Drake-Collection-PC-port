// pages.cpp - the launcher's pages built natively, part of uncharted_native.dll. A page is the tree the Qt window
// (launcher-qt, NodeView.qml / PageView.qml) draws: sections, rows, separators, labels and controls, as JSON. A click
// on one of its controls comes back to un_page_act, which says what changes: settings keys of launcher.json for now
// (the launcher's Python side still writes that file, so there is one writer).
//
//   un_page(name, state)                          the page, or status 2 when the page is not a native one
//   un_page_act(name, id, kind, value, state)     {"config", "ini", "game"}: the settings the click changes
// state: {"config": launcher.json, "ini": the selected game's settings file (analysis/uncharted-user/probe-*.ini)}.
//
// Pages: cheats (the games' developer switches), performance (frame rate, processor, video memory), display (resolution,
// upscaling, picture, performance counter), setup (Quick Setup's preset cards), advanced (game, extra quality, unlocks,
// launcher switches), tools (the launcher's actions: a click returns {"call": {"name", "value"}}, which the Python side
// runs), launcher (its updates), controls (mouse, controller and the key / controller bindings; a click on a slot
// returns {"ui": ...}, the window's new state for the page: which slot waits for an input, the hint line). state also has "root": the launcher's
// folder (which upscalers it has), and "gpu": {"name", "recommended"}: the graphics card the launcher found and its preset.

#include "native.h"
#include "json.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" UN_API int un_monitor_refresh_hz(void);   // display.cpp
extern "C" {   // display.cpp
UN_API int un_display_table(const char *name, char *out, int cap);
UN_API int un_resolution_options(const char *aspect, char *out, int cap);
UN_API int un_game_sizes(const char *resolution, const char *aspect, const char *upscale, const char *upscaling, char *out,
                         int cap);
UN_API int un_upscaling_choice(const char *upscaler, const char *fsr1, char *out, int cap);
UN_API int un_anti_aliasing_choice(const char *upscaler, const char *launcher_aa, char *out, int cap);
UN_API int un_upscaling_options(const wchar_t *root, char *out, int cap);
UN_API int un_gpu_query(const char *what, const char *name, int integrated, char *out, int cap);
}
extern "C" {   // controls.cpp, gamepads.cpp
UN_API int un_controls_tables(char *out, int cap);
UN_API int un_controls(const char *what, const char *in_json, char *out, int cap);
UN_API int un_pad_init(const wchar_t *root);
UN_API int un_pad_state(char *out, int cap);
}

namespace {

using json::Value;

// The launcher's state a page is built from, and what a click changes.
struct State {
    Value config = Value::object();
    Value ini = Value::object();
    std::string root = ".";   // the launcher's folder
    Value gpu = Value::object();   // {"name", "recommended"}
    Value game_dir;                // the game folder (the launcher's game_dir())
    Value tools = Value::object(); // TOOLS' lines: {"game_dir", "update", "ini"}
    Value ui = Value::object();    // what the window keeps for the page (CONTROLS: the slot waiting for an input)
};
struct Changes {
    Value config = Value::object();   // launcher.json keys
    Value ini = Value::object();      // the selected game's settings file
    Value game = Value::object();     // the selected game's launcher settings (config["games"][game])
    Value ini_all = Value::object();  // all three games' settings files, without making the preset Custom
    Value call;                       // a launcher action to run: {"name", "value" (optional)}
    Value ui;                         // the window's new ui state for the page (null: unchanged)
};

bool read_state(const char *state_json, State &state) {
    Value v;
    if (!json::parse(state_json ? state_json : "{}", v) || !v.is_object()) return false;
    if (const Value *c = v.get("config")) { if (!c->is_object()) return false; state.config = *c; }
    if (const Value *i = v.get("ini")) { if (!i->is_object()) return false; state.ini = *i; }
    if (const Value *r = v.get("root")) if (r->kind == Value::String && !r->s.empty()) state.root = r->s;
    if (const Value *g = v.get("gpu")) if (g->is_object()) state.gpu = *g;
    if (const Value *d = v.get("game_dir")) state.game_dir = *d;
    if (const Value *t = v.get("tools")) if (t->is_object()) state.tools = *t;
    if (const Value *u = v.get("ui")) if (u->is_object()) state.ui = *u;
    return true;
}

int give(const std::string &s, char *out, int cap) {
    if ((int)s.size() + 1 > cap) return -(int)(s.size() + 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

Value array() {
    Value v;
    v.kind = Value::Array;
    return v;
}

// The colours of the launcher's look (uncharted_launcher.pyw / Theme.qml): page and section backgrounds, text.
const char *const BG = "#0c0c0e";
const char *const CARD = "#151519";
const char *const MUTED = "#a3a09a";
const char *const DIM = "#6c6a70";
const char *const GOLD = "#e8b23e";

Value label(const std::string &id, const std::string &text, const char *fg, const char *bg, int size) {
    Value l = Value::object();
    l.set("t", Value::text("label"));
    l.set("id", Value::text(id));
    l.set("text", Value::text(text));
    l.set("fg", Value::text(fg));
    l.set("bg", Value::text(bg));
    l.set("size", Value::integer(size));
    l.set("bold", Value::boolean(false));
    l.set("serif", Value::boolean(false));
    l.set("image", Value::text(""));
    l.set("click", Value::boolean(false));
    l.set("rclick", Value::boolean(false));
    l.set("mono", Value::boolean(false));
    l.set("slot", Value::boolean(false));
    return l;
}

Value separator() {
    Value s = Value::object();
    s.set("t", Value::text("sep"));
    return s;
}

Value row(const std::string &id, const std::string &title, const std::string &hint, Value control) {
    Value r = Value::object();
    r.set("t", Value::text("row"));
    r.set("id", Value::text(id));
    r.set("title", Value::text(title));
    r.set("hint", Value::text(hint));
    r.set("control", std::move(control));
    r.set("extra", array());
    return r;
}

Value toggle(const std::string &id, bool on) {
    Value t = Value::object();
    t.set("t", Value::text("toggle"));
    t.set("id", Value::text(id));
    t.set("v", Value::boolean(on));
    return t;
}

// a segmented choice; the recommended option's label carries a star (Segmented.text)
Value choice(const std::string &id, const Value &v, const std::vector<std::pair<std::string, std::string>> &options,
             const std::string &recommended) {
    Value c = Value::object();
    c.set("t", Value::text("choice"));
    c.set("id", Value::text(id));
    c.set("v", v);
    c.set("en", Value::boolean(true));
    Value opts = array();
    for (auto &o : options) {
        Value pair = array();
        pair.items = {Value::text(o.first), Value::text(o.first == recommended ? o.second + "  \xe2\x98\x85" : o.second)};
        opts.items.push_back(std::move(pair));
    }
    c.set("opts", std::move(opts));
    return c;
}

Value section(const std::string &id, const std::string &title, Value items) {
    Value s = Value::object();
    s.set("t", Value::text("section"));
    s.set("id", Value::text(id));
    s.set("title", Value::text(title));
    s.set("note", Value::text(""));
    s.set("items", std::move(items));
    s.set("column", Value::text(""));
    return s;
}

bool config_flag(const Value &config, const std::string &key) {
    const Value *v = config.get(key);
    return v && v->truthy();
}

// ------------------------------------------------------------------------------------------------ cheats

// (name, title, hint) of the games' developer switches (system.cpp's table, as the game starter applies them)
std::vector<std::vector<std::string>> cheat_list() {
    std::vector<std::vector<std::string>> list;
    std::string text(1 << 16, '\0');
    if (un_system_tables(&text[0], (int)text.size()) != 0) return list;
    Value tables;
    if (!json::parse(text.c_str(), tables)) return list;
    if (const Value *c = tables.get("CHEATS"))
        for (auto &item : c->items)
            if (item.items.size() == 3)
                list.push_back({item.items[0].s, item.items[1].s, item.items[2].s});
    return list;
}

Value page_cheats(const Value &config) {
    Value items = array();
    items.items.push_back(label("native:cheats:intro",
                                "The games' own developer switches, applied when a game starts. They need update 1.02.",
                                MUTED, CARD, 9));
    bool first = true;
    for (auto &cheat : cheat_list()) {
        if (!first) items.items.push_back(separator());
        first = false;
        const std::string key = "cheat_" + cheat[0];
        items.items.push_back(row("native:cheats:row:" + key, cheat[1], cheat[2],
                                  toggle("native:cheats:" + key, config_flag(config, key))));
    }
    Value page = array();
    page.items.push_back(section("native:cheats", "Developer switches", std::move(items)));
    return page;
}

// A toggle "native:cheats:cheat_<name>" sets that key.
bool act_cheats(const std::string &id, const std::string &kind, const Value &value, Changes &changes) {
    const std::string prefix = "native:cheats:cheat_";
    if (kind != "toggle" || id.compare(0, prefix.size(), prefix) != 0) return false;
    const std::string key = id.substr(std::strlen("native:cheats:"));
    for (auto &cheat : cheat_list())
        if (key == "cheat_" + cheat[0]) {
            changes.config.set(key, Value::boolean(value.truthy()));
            return true;
        }
    return false;
}

// ------------------------------------------------------------------------------------------------ performance

// rows of a settings table of config.cpp ("ini_defaults": key, value; "fps_limits": value, label)
std::vector<std::pair<std::string, std::string>> settings_table(const char *name) {
    std::vector<std::pair<std::string, std::string>> rows;
    std::string text(1 << 16, '\0');
    if (un_settings_table(name, &text[0], (int)text.size()) != 0) return rows;
    text.resize(std::strlen(text.c_str()));
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        const size_t tab = line.find('\t');
        if (tab != std::string::npos) rows.push_back({line.substr(0, tab), line.substr(tab + 1)});
        at = end + 1;
    }
    return rows;
}

// the selected game's settings file value of `key`, else its default (Python: ini.get(key, INI_DEFAULTS.get(key)))
Value ini_value(const State &state, const std::string &key) {
    if (const Value *v = state.ini.get(key)) return *v;
    for (auto &row : settings_table("ini_defaults"))
        if (row.first == key) return Value::text(row.second);
    return Value::null();
}

// the selected game's launcher settings (config["games"][config["game"]])
const Value *game_settings(const State &state) {
    const Value *game = state.config.get("game"), *games = state.config.get("games");
    if (!game || !games || game->kind != Value::String) return nullptr;
    return games->get(game->s);
}

std::string text_of(const Value &v) {
    if (v.kind == Value::String) return v.s;
    if (v.kind == Value::Int) return std::to_string(v.i);
    return "";
}

typedef std::vector<std::pair<std::string, std::string>> Options;

Options fps_options() {
    Options o;
    for (auto &row : settings_table("fps_limits")) o.push_back({row.first, row.second});
    return o;
}
const Options FRAMES_QUEUED = {{"1", "1"}, {"2", "2"}, {"0", "Unlimited"}};
const Options JOB_SLEEP = {{"auto", "Auto"}, {"on", "Sleep"}, {"off", "Spin"}};
const Options LOW_VRAM = {{"off", "Off"}, {"on", "On"}, {"auto", "Auto"}};

bool has_option(const Options &options, const std::string &v) {
    for (auto &o : options) if (o.first == v) return true;
    return false;
}

// a config value limited to its options (Python: value if value in (...) else default)
std::string config_choice(const State &state, const std::string &key, const std::string &fallback, const Options &options) {
    const Value *v = state.config.get(key);
    std::string s = v ? (v->kind == Value::String ? v->s : text_of(*v)) : fallback;
    if (v && v->kind == Value::Bool) s = v->b ? "True" : "False";   // str() of a Python bool
    return has_option(options, s) ? s : fallback;
}

Value page_performance(const State &state) {
    const std::string p = "native:performance:";
    Value page = array();
    page.items.push_back(label(p + "intro", "\xe2\x98\x85 marks the recommended choice.", DIM, BG, 9));

    Value rate = array();
    const int hz = un_monitor_refresh_hz();
    rate.items.push_back(row(p + "row:fps_limit", "Frame rate limit",
                             "60 is how the games were made. Higher is smoother but experimental: if the game runs too "
                             "fast or Nate falls through the floor, go back to 60. With VSync, your monitor's refresh "
                             "rate (" + std::to_string(hz) + " Hz) is the smoothest.",
                             choice(p + "fps_limit", ini_value(state, "fps_limit"), fps_options(), "60")));
    rate.items.push_back(separator());
    rate.items.push_back(row(p + "row:frames_queued", "Frames queued",
                             "Keep 1. 2 can add a few FPS in heavy scenes, but in Uncharted 2 Nate and other "
                             "characters then float a little above the floor.",
                             choice(p + "frames_queued", ini_value(state, "frames_queued"), FRAMES_QUEUED, "1")));
    page.items.push_back(section(p + "rate", "Frame rate", std::move(rate)));

    Value cpu = array();
    const Value async = ini_value(state, "async_shaders");
    cpu.items.push_back(row(p + "row:async_shaders", "Compile shaders in the background",
                            "Fewer stutters when you reach new areas. Recommended on.",
                            toggle(p + "async_shaders", async.kind == Value::String && async.s == "1")));
    cpu.items.push_back(separator());
    const Value *settings = game_settings(state);
    const Value *mt = settings ? settings->get("multithreaded") : nullptr;
    cpu.items.push_back(row(p + "row:multithreaded", "Multithreaded rendering (experimental)",
                            "Prepares the picture on a second CPU core: more FPS where the CPU is the limit, mostly "
                            "in Uncharted 2 and 3 (on by default in Uncharted 3; keep Frames queued at 1 with it). "
                            "Turn it off if a game hangs while loading. Drake's Fortune always runs without it.",
                            toggle(p + "multithreaded", mt && mt->truthy())));
    cpu.items.push_back(separator());
    cpu.items.push_back(row(p + "row:job_sleep", "Idle game threads",
                            "Sleep: the game's helper threads rest while they wait, which keeps 4-6 core CPUs fast "
                            "and saves power. Spin keeps their cores busy, as on the PS4: smoother on CPUs with "
                            "many cores. Auto picks Spin in Uncharted 1 on CPUs with 8 or more threads (it smooths "
                            "the camera there) and Sleep in Uncharted 2 and 3.",
                            choice(p + "job_sleep", Value::text(config_choice(state, "job_sleep", "auto", JOB_SLEEP)),
                                   JOB_SLEEP, "auto")));
    page.items.push_back(section(p + "cpu", "Processor", std::move(cpu)));

    Value vram = array();
    vram.items.push_back(row(p + "row:low_vram", "Save video memory (experimental)",
                             "For graphics cards with 2-4 GB: smaller memory reserves, about 0.5 GB less video "
                             "memory (Uncharted 2 at 720p: 2.1 -> 1.5 GB). New and not yet tried on a real 2 GB "
                             "card. Auto turns it on for cards with 4 GB or less.",
                             choice(p + "low_vram", Value::text(config_choice(state, "low_vram", "off", LOW_VRAM)),
                                    LOW_VRAM, "")));
    // above 60 FPS, a note in gold (Python's update_fps_note: int(limit) > 60)
    const std::string limit = text_of(ini_value(state, "fps_limit"));
    char *end = nullptr;
    const long fps = limit.empty() ? 60 : std::strtol(limit.c_str(), &end, 10);
    if (fps > 60)
        vram.items.push_back(label(p + "fps_note", "Frame rate unlocked up to " + limit + " FPS (your monitor: " +
                                   std::to_string(hz) + " Hz). Applied when the game starts.", GOLD, CARD, 9));
    page.items.push_back(section(p + "vram", "Video memory", std::move(vram)));
    return page;
}

bool act_performance(const std::string &id, const std::string &kind, const Value &value, Changes &changes) {
    const std::string p = "native:performance:";
    if (id.compare(0, p.size(), p) != 0) return false;
    const std::string key = id.substr(p.size());
    const std::string v = text_of(value);
    if (kind == "choice") {
        if (key == "fps_limit" && has_option(fps_options(), v)) changes.ini.set(key, Value::text(v));
        else if (key == "frames_queued" && has_option(FRAMES_QUEUED, v)) changes.ini.set(key, Value::text(v));
        else if (key == "job_sleep" && has_option(JOB_SLEEP, v)) changes.config.set(key, Value::text(v));
        else if (key == "low_vram" && has_option(LOW_VRAM, v)) changes.config.set(key, Value::text(v));
        else return false;
        return true;
    }
    if (kind == "toggle") {
        if (key == "async_shaders") changes.ini.set(key, Value::text(value.truthy() ? "1" : "0"));
        else if (key == "multithreaded") changes.game.set(key, Value::boolean(value.truthy()));
        else return false;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------------------------------------ display

// a section's items with a separator between two rows that follow each other (a note between them breaks it)
Value with_separators(std::vector<Value> entries) {
    Value items = array();
    bool after_row = false;
    for (auto &e : entries) {
        const bool is_row = e.get("t") && e.get("t")->s == "row";
        if (is_row && after_row) items.items.push_back(separator());
        after_row = is_row;
        items.items.push_back(std::move(e));
    }
    return items;
}

// a native text call (0 ok) as a string
template <typename Call> std::string native_text(Call call) {
    std::string out(1 << 16, '\0');
    if (call(&out[0], (int)out.size()) != 0) return "";
    out.resize(std::strlen(out.c_str()));
    return out;
}

Options rows_of(const std::string &text) {
    Options rows;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        const size_t tab = line.find('\t');
        if (tab != std::string::npos) rows.push_back({line.substr(0, tab), line.substr(tab + 1)});
        else if (!line.empty()) rows.push_back({line, ""});   // a one-column table (temporal_upscalers)
        at = end + 1;
    }
    return rows;
}

Options display_table(const char *name) {
    return rows_of(native_text([&](char *o, int c) { return un_display_table(name, o, c); }));
}

std::wstring widen_utf8(const std::string &s) {
    std::wstring w;
    for (size_t i = 0; i < s.size();) {   // UTF-8 to UTF-16 (the launcher folder's path)
        unsigned c = (unsigned char)s[i], n = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        unsigned v = n == 0 ? c : n == 1 ? c & 0x1f : n == 2 ? c & 0x0f : c & 0x07;
        for (unsigned k = 1; k <= n && i + k < s.size(); ++k) v = v << 6 | ((unsigned char)s[i + k] & 0x3f);
        i += n + 1;
        if (v >= 0x10000) { v -= 0x10000; w += (wchar_t)(0xd800 + (v >> 10)); w += (wchar_t)(0xdc00 + (v & 0x3ff)); }
        else w += (wchar_t)v;
    }
    return w;
}

std::string str_or(const Value *v, const std::string &fallback) {
    return v ? text_of(*v) : fallback;
}

// the ini value of `key` as Python's ini.get(key, default) gives it (a missing key: the default)
std::string ini_text(const State &state, const std::string &key, const std::string &fallback) {
    const Value *v = state.ini.get(key);
    return v ? text_of(*v) : fallback;
}

std::string ini_default(const std::string &key) {
    for (auto &row : settings_table("ini_defaults"))
        if (row.first == key) return row.second;
    return "";
}

// upscaling_choice(ini) and anti_aliasing_choice(ini, settings) of the launcher
std::string upscaling_of(const State &state) {
    const std::string upscaler = ini_text(state, "upscaler", ini_default("upscaler")), fsr1 = ini_text(state, "fsr1", "0");
    return native_text([&](char *o, int c) { return un_upscaling_choice(upscaler.c_str(), fsr1.c_str(), o, c); });
}
std::string anti_aliasing_of(const State &state, const Value *settings) {
    const std::string upscaler = ini_text(state, "upscaler", ini_default("upscaler"));
    const std::string aa = settings ? str_or(settings->get("aa"), "") : "";
    return native_text([&](char *o, int c) { return un_anti_aliasing_choice(upscaler.c_str(), aa.c_str(), o, c); });
}
bool temporal_upscaler(const std::string &u) {
    for (auto &row : display_table("temporal_upscalers"))
        if (row.first == u) return true;
    return false;
}

// (render size, output size or "") as game_sizes(settings, upscaling)
std::pair<std::string, std::string> sizes_of(const Value *settings, const std::string &upscaling) {
    const std::string resolution = settings ? str_or(settings->get("resolution"), "") : "";
    const std::string aspect = settings ? str_or(settings->get("aspect"), "16:9") : "16:9";
    const std::string upscale = settings ? str_or(settings->get("upscale"), "off") : "off";
    const std::string t = native_text([&](char *o, int c) {
        return un_game_sizes(resolution.c_str(), aspect.c_str(), upscale.c_str(), upscaling.c_str(), o, c);
    });
    const size_t tab = t.find('\t');
    return tab == std::string::npos ? std::make_pair(t, std::string()) : std::make_pair(t.substr(0, tab), t.substr(tab + 1));
}

std::string times(std::string size) {   // "1920x1080" -> "1920 × 1080"
    const size_t x = size.find('x');
    return x == std::string::npos ? size : size.substr(0, x) + " \xc3\x97 " + size.substr(x + 1);
}

// a box of controls (a Tk frame of the row's control): its items carry how they were packed
Value box(const std::string &id, const char *dir, Value items) {
    Value b = Value::object();
    b.set("t", Value::text("box"));
    b.set("dir", Value::text(dir));
    b.set("id", Value::text(id));
    b.set("items", std::move(items));
    b.set("bg", Value::text(CARD));
    return b;
}
Value packed(Value item, const char *side) {
    item.set("side", Value::text(side));
    item.set("fill", Value::text("none"));
    item.set("expand", Value::boolean(false));
    return item;
}
Value chip(const std::string &id, bool on, const std::string &text) {
    Value c = Value::object();
    c.set("t", Value::text("chip"));
    c.set("id", Value::text(id));
    c.set("v", Value::boolean(on));
    c.set("text", Value::text(text));
    return c;
}

// a choice from the game's settings file (ini_choice): its value, else the default, else null
Value ini_choice_value(const State &state, const std::string &key) {
    if (const Value *v = state.ini.get(key)) return *v;
    for (auto &row : settings_table("ini_defaults"))
        if (row.first == key) return Value::text(row.second);
    return Value::null();
}

const Options DISPLAY_MODES = {{"windowed", "Windowed"}, {"borderless", "Borderless"}, {"fullscreen", "Fullscreen"}};
const Options SHARPENING = {{"off", "Off"}, {"25", "25%"}, {"50", "50%"}, {"75", "75%"}, {"100", "100%"}};
const Options OVERLAY_ITEMS = {{"overlay_fps", "FPS"}, {"overlay_frametime", "Frame time"}, {"overlay_gpu", "GPU"},
                               {"overlay_cpu", "CPU"}, {"overlay_ram", "RAM"}, {"overlay_vram", "VRAM"},
                               {"overlay_upscaler", "Upscaler"}};
const Options OVERLAY_CORNER = {{"0", "Top left"}, {"1", "Top right"}, {"2", "Bottom left"}, {"3", "Bottom right"}};
const Options OVERLAY_LAYOUT = {{"0", "One line"}, {"1", "Stacked"}};
const Options OVERLAY_SCALE = {{"75", "75%"}, {"100", "100%"}, {"125", "125%"}, {"150", "150%"}, {"200", "200%"}};
const Options OVERLAY_OPACITY = {{"0", "None"}, {"25", "25%"}, {"50", "50%"}, {"75", "75%"}, {"100", "100%"}};
const Options ANTI_ALIASING = {{"off", "Off"}, {"taa", "TAA"}};

Options upscaling_options(const State &state) {
    const std::wstring root = widen_utf8(state.root);
    return rows_of(native_text([&](char *o, int c) { return un_upscaling_options(root.c_str(), o, c); }));
}

Value page_display(const State &state) {
    const std::string p = "native:display:";
    const Value *settings = game_settings(state);
    const std::string aspect = settings ? str_or(settings->get("aspect"), "16:9") : "16:9";
    const std::string upscaling = upscaling_of(state);
    const auto sizes = sizes_of(settings, upscaling);
    Value page = array();

    std::vector<Value> res;
    res.push_back(row(p + "row:aspect", "Aspect ratio",
                      "For ultrawide monitors (21:9, 32:9), with Borderless or Fullscreen: the games show more at "
                      "the sides and the same view top to bottom as on a 16:9 screen. 16:10 (Steam Deck: 800p is "
                      "1280x800) fills the screen and shows more at the top and bottom.",
                      choice(p + "aspect", Value::text(aspect), display_table("aspects"), "")));
    const Options resolutions = rows_of(native_text([&](char *o, int c) { return un_resolution_options(aspect.c_str(), o, c); }));
    res.push_back(row(p + "row:resolution", "Resolution",
                      "How sharp the picture is; higher needs a faster graphics card (the PS4 drew 1080p). With "
                      "upscaling below, the game draws less and is upscaled to it. If a game crashes above 1080p, "
                      "go back to 1080p. Drake's Fortune draws at most 1620p and scales up.",
                      choice(p + "resolution", settings && settings->get("resolution") ? *settings->get("resolution")
                                                                                       : Value::null(),
                             resolutions, "")));
    if (sizes.second.empty())   // with upscaling, the quality row's note shows the sizes instead
        res.push_back(label(p + "render_size", "Render size: " + times(sizes.first), GOLD, CARD, 9));
    res.push_back(row(p + "row:upscaling", "Upscaling",
                      "More FPS: the game draws below the resolution and each frame is upscaled to it; the HUD and "
                      "menus stay sharp at the resolution. FSR 1 is the lightest and works on any card; FSR 3.1, "
                      "FSR 4 and DLSS (NVIDIA RTX cards) also use the previous frames: more detail, and they smooth "
                      "edges themselves. Also in the in-game menu.",
                      choice(p + "upscaling", Value::text(upscaling), upscaling_options(state), "")));
    if (upscaling != "off") {   // the quality row, with an upscaler only
        Options presets;
        for (auto &row : display_table("upscale_presets")) {
            if (upscaling == "fsr1" && row.first == "off") continue;   // FSR 1 has no Native AA
            presets.push_back(row.first == "off" ? std::make_pair(row.first, std::string(upscaling == "dlss" ? "DLAA" : "Native AA"))
                                                 : row);
        }
        Value inner = array();
        inner.items.push_back(packed(choice(p + "upscale", settings && settings->get("upscale") ? *settings->get("upscale")
                                                                                            : Value::text("off"),
                                            presets, ""), "top"));
        if (!sizes.second.empty())
            inner.items.push_back(packed(label(p + "upscale_note", "Draws " + times(sizes.first) + ", upscaled to " +
                                               times(sizes.second), GOLD, CARD, 9), "top"));
        res.push_back(row(p + "row:upscale", "Upscaling quality",
                          "How much below the resolution the game draws: Quality 1/1.5, Balanced 1/1.7, Performance 1/2, "
                          "Ultra Performance 1/3 of its width and height. Native AA (DLAA with DLSS) draws at the "
                          "resolution and only smooths edges.",
                          box(p + "upscale_box", "v", std::move(inner))));
    }
    Value aa = choice(p + "aa", Value::text(anti_aliasing_of(state, settings)), ANTI_ALIASING, "");
    aa.set("en", Value::boolean(!temporal_upscaler(upscaling)));
    res.push_back(row(p + "row:aa", "Anti-aliasing",
                      "Smooths jagged, shimmering edges, with upscaling Off or FSR 1. FSR 3.1, FSR 4 and DLSS do "
                      "it themselves.", aa));
    page.items.push_back(section(p + "resolution", "Resolution and upscaling", with_separators(std::move(res))));

    std::vector<Value> picture;
    picture.push_back(row(p + "row:display_mode", "Display mode",
                          "A normal window, a borderless window (quick Alt+Tab) or fullscreen.",
                          choice(p + "display_mode", ini_choice_value(state, "display_mode"), DISPLAY_MODES, "")));
    picture.push_back(row(p + "row:vsync", "VSync", "Stops tearing (a line across the picture when the camera turns). "
                                                    "Recommended on.",
                          toggle(p + "vsync", ini_text(state, "vsync", "0") == "1")));
    const std::string sharp = ini_text(state, "rcas", "1") == "1" ? ini_text(state, "rcas_strength", "75") : "off";
    picture.push_back(row(p + "row:sharpening", "Sharpening", "Makes the final picture crisper (AMD FidelityFX RCAS).",
                          choice(p + "sharpening", Value::text(sharp), SHARPENING, "")));
    page.items.push_back(section(p + "picture", "Picture", with_separators(std::move(picture))));

    std::vector<Value> counter;
    counter.push_back(row(p + "row:show_fps", "Show performance counter",
                          "FPS, temperatures and memory in a corner of the screen. Insert, then the Overlay "
                          "tab, changes it in the game too.",
                          toggle(p + "show_fps", ini_text(state, "show_fps", "0") == "1")));
    Value chips = array();
    for (auto &item : OVERLAY_ITEMS)
        chips.items.push_back(packed(chip(p + item.first, ini_text(state, item.first, ini_default(item.first)) == "1",
                                          item.second), "left"));
    counter.push_back(row(p + "row:overlay_items", "What it shows",
                          "The frame rate, the frame time in milliseconds, the graphics card and the processor "
                          "with their temperatures, and memory. Pick any; with nothing picked the counter is "
                          "just its box.", box(p + "overlay_items", "h", std::move(chips))));
    counter.push_back(row(p + "row:overlay_corner", "Counter corner", "Where the counter sits.",
                          choice(p + "overlay_corner", ini_choice_value(state, "overlay_corner"), OVERLAY_CORNER, "")));
    counter.push_back(row(p + "row:overlay_layout", "Counter layout", "One line, or one item per line.",
                          choice(p + "overlay_layout", ini_choice_value(state, "overlay_layout"), OVERLAY_LAYOUT, "")));
    counter.push_back(row(p + "row:overlay_scale", "Counter size", "Text size of the counter.",
                          choice(p + "overlay_scale", ini_choice_value(state, "overlay_scale"), OVERLAY_SCALE, "")));
    counter.push_back(row(p + "row:overlay_opacity", "Counter background",
                          "How dark the counter's box is. None leaves the text on the picture.",
                          choice(p + "overlay_opacity", ini_choice_value(state, "overlay_opacity"), OVERLAY_OPACITY, "")));
    page.items.push_back(section(p + "counter", "Performance counter", with_separators(std::move(counter))));
    return page;
}

// What a click on DISPLAY changes: the launcher's change_aspect / change_resolution / change_upscaling /
// change_anti_aliasing / change_sharpening and the plain settings-file values, as settings changes.
bool act_display(const State &state, const std::string &id, const std::string &kind, const Value &value, Changes &changes) {
    const std::string p = "native:display:";
    if (id.compare(0, p.size(), p) != 0) return false;
    const std::string key = id.substr(p.size()), v = text_of(value);
    const Value *settings = game_settings(state);
    auto in = [](const Options &o, const std::string &x) { return has_option(o, x); };
    if (kind == "choice") {
        if (key == "aspect" && in(display_table("aspects"), v)) changes.game.set("aspect", Value::text(v));
        else if (key == "resolution") {
            const std::string aspect = settings ? str_or(settings->get("aspect"), "16:9") : "16:9";
            if (!in(rows_of(native_text([&](char *o, int c) { return un_resolution_options(aspect.c_str(), o, c); })), v))
                return false;
            changes.game.set("resolution", Value::text(v));
            changes.game.set("preset", Value::text("custom"));
        } else if (key == "upscaling" && in(upscaling_options(state), v)) {
            const std::string aa = anti_aliasing_of(state, settings);   // kept for Off and FSR 1
            changes.game.set("aa", Value::text(aa));
            if (v == "fsr1" && (settings ? str_or(settings->get("upscale"), "off") : "off") == "off")
                changes.game.set("upscale", Value::text("quality"));
            changes.ini.set("fsr1", Value::text(v == "fsr1" ? "1" : "0"));
            changes.ini.set("upscaler", Value::text(temporal_upscaler(v) ? v : aa));
        } else if (key == "upscale" && in(display_table("upscale_presets"), v) && !(v == "off" && upscaling_of(state) == "fsr1"))
            changes.game.set("upscale", Value::text(v));   // FSR 1 shows no Native AA
        else if (key == "aa" && in(ANTI_ALIASING, v) && !temporal_upscaler(upscaling_of(state))) {
            changes.game.set("aa", Value::text(v));
            changes.ini.set("upscaler", Value::text(v));
        } else if (key == "sharpening" && in(SHARPENING, v)) {
            if (v == "off") changes.ini.set("rcas", Value::text("0"));
            else { changes.ini.set("rcas_strength", Value::text(v)); changes.ini.set("rcas", Value::text("1")); }
        } else if (key == "display_mode" && in(DISPLAY_MODES, v)) changes.ini.set(key, Value::text(v));
        else if (key == "overlay_corner" && in(OVERLAY_CORNER, v)) changes.ini.set(key, Value::text(v));
        else if (key == "overlay_layout" && in(OVERLAY_LAYOUT, v)) changes.ini.set(key, Value::text(v));
        else if (key == "overlay_scale" && in(OVERLAY_SCALE, v)) changes.ini.set(key, Value::text(v));
        else if (key == "overlay_opacity" && in(OVERLAY_OPACITY, v)) changes.ini.set(key, Value::text(v));
        else return false;
        return true;
    }
    if (kind == "toggle" && (key == "vsync" || key == "show_fps")) {
        changes.ini.set(key, Value::text(value.truthy() ? "1" : "0"));
        return true;
    }
    if (kind == "chip" && has_option(OVERLAY_ITEMS, key)) {
        changes.ini.set(key, Value::text(value.truthy() ? "1" : "0"));
        return true;
    }
    return false;
}

// ------------------------------------------------------------------------------------------------ quick setup
// The preset cards. The IGPU card (its values and the integrated GPU classes) was contributed in PR #223 by
// memeguyoffical.

const char *const TEXT = "#f2efe8";
const char *const INK = "#16110a";
const char *const PRESET_ORDER[] = {"igpu", "low", "medium", "high", "ultra"};

struct Preset {
    std::string name, title, summary, detail, resolution, upscale;
    std::vector<std::pair<std::string, std::string>> values;
};

// config.cpp's presets table: name, title, summary, detail, resolution, upscale, values ("key=value;...")
std::vector<Preset> presets() {
    std::vector<Preset> list;
    std::string text(1 << 16, '\0');
    if (un_settings_table("presets", &text[0], (int)text.size()) != 0) return list;
    text.resize(std::strlen(text.c_str()));
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::vector<std::string> f;
        const std::string line = text.substr(at, end - at);
        size_t a = 0;
        while (true) {
            const size_t t = line.find('\t', a);
            f.push_back(line.substr(a, t == std::string::npos ? std::string::npos : t - a));
            if (t == std::string::npos) break;
            a = t + 1;
        }
        if (f.size() == 7) {
            Preset p{f[0], f[1], f[2], f[3], f[4], f[5], {}};
            size_t b = 0;
            while (b < f[6].size()) {
                size_t e = f[6].find(';', b);
                if (e == std::string::npos) e = f[6].size();
                const std::string pair = f[6].substr(b, e - b);
                const size_t eq = pair.find('=');
                if (eq != std::string::npos) p.values.push_back({pair.substr(0, eq), pair.substr(eq + 1)});
                b = e + 1;
            }
            list.push_back(p);
        }
        at = end + 1;
    }
    return list;
}

const Preset *preset_named(const std::vector<Preset> &list, const std::string &name) {
    for (auto &p : list) if (p.name == name) return &p;
    return nullptr;
}

// Python's str.title() for the preset titles (ASCII words)
std::string title_case(const std::string &s) {
    std::string o = s;
    bool start = true;
    for (auto &c : o) {
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (letter) c = start ? (char)std::toupper((unsigned char)c) : (char)std::tolower((unsigned char)c);
        start = !letter;
    }
    return o;
}

// the selected game's name (system.cpp's GAMES table)
std::string game_name(const State &state) {
    const Value *game = state.config.get("game");
    std::string text(1 << 16, '\0');
    if (!game || un_system_tables(&text[0], (int)text.size()) != 0) return "";
    Value tables;
    if (!json::parse(text.c_str(), tables)) return "";
    if (const Value *games = tables.get("GAMES"))
        for (auto &g : games->items)
            if (const Value *k = g.get("key"))
                if (k->s == game->s) return g.get("name") ? g.get("name")->s : "";
    return "";
}

Value card_text(const std::string &text, const char *fg, int size, bool bold, bool serif) {
    Value t = Value::object();
    t.set("text", Value::text(text));
    t.set("fg", Value::text(fg));
    t.set("size", Value::integer(size));
    t.set("bold", Value::boolean(bold));
    t.set("serif", Value::boolean(serif));
    return t;
}

Value page_setup(const State &state) {
    const std::string p = "native:setup:";
    const std::vector<Preset> list = presets();
    const Value *settings = game_settings(state);
    const std::string current = settings ? str_or(settings->get("preset"), "") : "";
    const Value *gpu = state.gpu.get("name"), *rec = state.gpu.get("recommended");
    const std::string gpu_name = gpu ? text_of(*gpu) : "", recommended = rec ? text_of(*rec) : "";

    std::vector<Value> items;
    std::string intro = "Pick a quality level. Every setting can still be changed in Display and Performance.";
    if (!recommended.empty()) {
        const Preset *r = preset_named(list, recommended);
        const std::string name = recommended == "igpu" ? "iGPU" : r ? title_case(r->title) : recommended;
        intro = "Recommended for your graphics card (" + gpu_name + "): " + name +
                ". Every setting can still be changed in Display and Performance.";
    }
    items.push_back(label(p + "intro", intro, MUTED, CARD, 10));

    Value grid = array();
    Value line = array();
    int n = 0;
    for (const char *name : PRESET_ORDER) {
        const Preset *preset = preset_named(list, name);
        if (!preset) continue;
        const bool selected = current == name;
        Value texts = array();
        texts.items.push_back(card_text(preset->title, selected ? GOLD : TEXT, 14, true, true));
        texts.items.push_back(card_text(preset->summary, TEXT, 10, true, false));
        texts.items.push_back(card_text(preset->detail, MUTED, 9, false, false));
        if (recommended == name) texts.items.push_back(card_text("RECOMMENDED", INK, 7, true, false));
        Value card = Value::object();
        card.set("t", Value::text("card"));
        card.set("id", Value::text(p + "preset:" + name));
        card.set("texts", std::move(texts));
        card.set("click", Value::boolean(true));
        card.set("dclick", Value::boolean(false));
        card.set("selected", Value::boolean(selected));
        line.items.push_back(std::move(card));
        if (++n % 3 == 0) {   // five cards in 3 + 2
            Value row_box = Value::object();
            row_box.set("t", Value::text("box"));
            row_box.set("dir", Value::text("h"));
            row_box.set("items", std::move(line));
            grid.items.push_back(std::move(row_box));
            line = array();
        }
    }
    if (!line.items.empty()) {
        Value row_box = Value::object();
        row_box.set("t", Value::text("box"));
        row_box.set("dir", Value::text("h"));
        row_box.set("items", std::move(line));
        grid.items.push_back(std::move(row_box));
    }
    items.push_back(box(p + "presets", "v", std::move(grid)));

    const std::string game = game_name(state);
    items.push_back(label(p + "custom_note", preset_named(list, current) ? "Settings are kept per game: these are " + game + "'s."
                                                                        : game + ": custom settings (changed by hand).",
                          MUTED, CARD, 9));
    Value page = array();
    page.items.push_back(section(p + "quality", "Quality level", with_separators(std::move(items))));
    return page;
}

// A preset card: apply_preset's settings (the preset's resolution, upscaling and settings-file values; the iGPU card by
// the card's class; on iGPU multithreading only where it helps and no anti-aliasing).
bool act_setup(const State &state, const std::string &id, const std::string &kind, Changes &changes) {
    const std::string prefix = "native:setup:preset:";
    if (kind != "click" || id.compare(0, prefix.size(), prefix) != 0) return false;
    const std::string name = id.substr(prefix.size());
    const std::vector<Preset> list = presets();
    const Preset *preset = preset_named(list, name);
    if (!preset) return false;
    std::string upscale = preset->upscale;
    std::vector<std::pair<std::string, std::string>> values = preset->values;
    if (name == "igpu") {
        const Value *gpu = state.gpu.get("name");
        const std::string gpu_name = gpu ? text_of(*gpu) : "";
        const std::string kind_of = native_text([&](char *o, int c) { return un_gpu_query("igpu_class", gpu_name.c_str(), -1, o, c); });
        if (kind_of == "weak") {
            upscale = "ultra";
            bool set = false;
            for (auto &v : values) if (v.first == "fps_limit") { v.second = "30"; set = true; }
            if (!set) values.push_back({"fps_limit", "30"});
        } else if (kind_of == "strong") upscale = "quality";
    }
    // every settings-file key: the preset's value, else what the file holds (Python: save_ini of all INI_DEFAULTS keys)
    for (auto &row : settings_table("ini_defaults")) {
        std::string v;
        bool found = false;
        for (auto &pv : values) if (pv.first == row.first) { v = pv.second; found = true; }
        if (!found) v = ini_text(state, row.first, row.second);
        changes.ini.set(row.first, Value::text(v));
    }
    changes.game.set("resolution", Value::text(preset->resolution));
    changes.game.set("upscale", Value::text(upscale));
    if (name == "igpu") {
        const Value *game = state.config.get("game");
        changes.game.set("multithreaded", Value::boolean(!(game && game->s == "u1")));
        changes.game.set("aa", Value::text("off"));
    }
    changes.game.set("preset", Value::text(name));   // last: the settings-file changes mark the preset Custom
    return true;
}

// ------------------------------------------------------------------------------------------------ advanced

// Python's str() of a settings value (a float as its shortest round-trip form, a bool as True / False)
std::string py_str(const Value &v) {
    switch (v.kind) {
    case Value::String: return v.s;
    case Value::Int: return std::to_string(v.i);
    case Value::Bool: return v.b ? "True" : "False";
    case Value::Null: return "None";
    case Value::Float: {
        char b[40];
        for (int digits = 1; digits <= 17; ++digits) {
            std::snprintf(b, sizeof b, "%.*g", digits, v.f);
            if (std::strtod(b, nullptr) == v.f) break;
        }
        std::string s = b;
        if (s.find_first_of(".e") == std::string::npos && s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
            s += ".0";   // Python writes 1.0, not 1
        return s;
    }
    default: return "";
    }
}

// str(config.get(key, default))
std::string config_str(const State &state, const std::string &key, const std::string &fallback) {
    const Value *v = state.config.get(key);
    return v ? py_str(*v) : fallback;
}
// config.get(key, default) as a toggle shows it
bool config_bool(const State &state, const std::string &key, bool fallback) {
    const Value *v = state.config.get(key);
    return v ? v->truthy() : fallback;
}

Value dropdown(const std::string &id, const Value &v, const Options &options) {
    Value d = Value::object();
    d.set("t", Value::text("dropdown"));
    d.set("id", Value::text(id));
    d.set("v", v);
    std::string label = py_str(v);   // Dropdown.label: the option's label, else the value itself
    for (auto &o : options) if (o.first == py_str(v) && v.kind == Value::String) label = o.second;
    d.set("label", Value::text(label));
    Value opts = array();
    for (auto &o : options) {
        Value pair = array();
        pair.items = {Value::text(o.first), Value::text(o.second)};
        opts.items.push_back(std::move(pair));
    }
    d.set("opts", std::move(opts));
    return d;
}

// The languages (system.cpp's LANGUAGES), "  (+ voices)" where this copy of the game has their speech
Options language_options(const State &state) {
    Options list;
    std::string text(1 << 16, '\0');
    if (un_system_tables(&text[0], (int)text.size()) != 0) return list;
    Value tables;
    if (!json::parse(text.c_str(), tables)) return list;
    const Value *dir = state.game_dir.kind == Value::String ? &state.game_dir : nullptr;
    std::string voices;
    if (dir && !dir->s.empty()) {
        const std::wstring folder = widen_utf8(dir->s);
        voices = "\n" + native_text([&](char *o, int c) { return un_system("voices", folder.c_str(), o, c); });
    }
    if (const Value *langs = tables.get("LANGUAGES"))
        for (auto &l : langs->items)
            if (l.items.size() == 2)
                list.push_back({l.items[0].s, l.items[1].s + (voices.find("\n" + l.items[0].s + "\n") != std::string::npos
                                                                   ? "  (+ voices)" : "")});
    return list;
}

const Options SAVE_PROFILES = {{"1", "Profile 1"}, {"2", "Profile 2"}, {"3", "Profile 3"}};
const Options EXPOSURES = {{"0", "Game's"}, {"-0.3", "-0.3"}, {"-0.6", "-0.6"}, {"-1.0", "-1.0"}};
const Options SPEAKERS = {{"auto", "Auto"}, {"2", "Stereo"}, {"6", "5.1"}, {"8", "7.1"}};
const Options MENU_SHORTCUTS = {{"lt_r3", "L2 + R3"}, {"view_menu", "Select + Start"}, {"keyboard", "Keyboard only"}};
const Options ANISOTROPY = {{"0", "Game"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}};

// The launcher.json toggles of ADVANCED: key, default, title, hint (in the page's order, by section)
struct ConfigToggle { const char *key; bool fallback; const char *title, *hint; };
const ConfigToggle EXTRA_QUALITY[] = {
    {"hires_shadows", false, "Sharper lamp shadows (Uncharted 2 and 3)",
     "The game's own high-resolution shadow buffer for lamps and flashlights (off on the PS4). Extra quality, being "
     "tested. Needs update 1.02."},
    {"full_res_textures", false, "Full-resolution textures (Uncharted 2 and 3)",
     "Always the sharpest version of every texture, no blurry ones while the game streams (off on the PS4). Extra "
     "quality, being tested. Needs update 1.02."},
    {"no_dof", false, "No depth of field (Uncharted 2 and 3)",
     "Turns off the background blur (while aiming, in cutscenes and in gameplay). Needs update 1.02."},
    {"movie_stretch", false, "Stretch movies to the screen",
     "The prerecorded cutscenes are 16:9 videos. Off: they keep their shape with black bars on ultrawide screens. On: "
     "they fill the whole screen, stretched."}};
const ConfigToggle UNLOCKS[] = {
    {"unlock", false, "Unlock all chapters",
     "Every chapter in Chapter Select and the Brutal difficulty from the start, in all three games. Your saves and "
     "progress stay as they are. Needs update 1.02."},
    {"unlock_rewards", false, "Unlock all rewards",
     "Every reward in Bonuses (skins, weapons, render modes, art galleries, gameplay tweaks) and every story cutscene, "
     "while this is on. Nothing is saved: turned off, the menus show your own progress again. Needs update 1.02."}};
const ConfigToggle LAUNCHER_TOGGLES[] = {
    {"diagnostics", true, "Performance log",
     "Writes frame rate statistics to the log every 5 seconds. Helps when you report a problem."},
    {"torch_forward", true, "Torch follows Nate (Uncharted 2)",
     "The torch lights where Nate points it. Off: as on the PS4, it lights where the camera looks. Needs update 1.02."}};

template <size_t N> void add_toggles(std::vector<Value> &rows, const State &state, const ConfigToggle (&list)[N]) {
    for (auto &t : list)
        rows.push_back(row(std::string("native:advanced:row:") + t.key, t.title, t.hint,
                           toggle(std::string("native:advanced:") + t.key, config_bool(state, t.key, t.fallback))));
}

Value page_advanced(const State &state) {
    const std::string p = "native:advanced:";
    const Value *settings = game_settings(state);
    Value page = array();

    std::vector<Value> game;
    game.push_back(row(p + "row:save_profile", "Save profile",
                       "Separate saves for this game: each profile keeps its own progress and in-game settings, "
                       "like another PS4 user. Profile 1 has your saves from earlier versions; switching never "
                       "deletes anything.",
                       choice(p + "save_profile",
                              Value::text(settings && settings->get("save_profile") ? py_str(*settings->get("save_profile")) : "1"),
                              SAVE_PROFILES, "")));
    const Value *lang = state.config.get("language");
    game.push_back(row(p + "row:language", "Language",
                       "Menus and subtitles in another language: pick it here, then press Square in a game's main "
                       "menu to switch. Languages marked + voices are also spoken in your copy of the game.",
                       dropdown(p + "language", lang ? *lang : Value::text("1"), language_options(state))));
    game.push_back(row(p + "row:u3_exposure", "Uncharted 3 exposure",
                       "If Uncharted 3 looks washed out or too bright on your PC (bright skies and highlights), "
                       "lower its exposure here: -0.3 a little, -1.0 about half as bright. Game's = unchanged.",
                       choice(p + "u3_exposure", Value::text(config_str(state, "u3_exposure", "0")), EXPOSURES, "0")));
    game.push_back(row(p + "row:speakers", "Speakers",
                       "The games have their own stereo, 5.1 and 7.1 sound mixes. Auto picks the one that matches "
                       "your Windows sound output (Sound settings -> speaker setup); headphones get stereo.",
                       choice(p + "speakers", Value::text(config_str(state, "speakers", "auto")), SPEAKERS, "auto")));
    const Value *shortcut = state.ini.get("menu_shortcut");
    game.push_back(row(p + "row:menu_shortcut", "In-game menu shortcut",
                       "The controller buttons that open the in-game menu. L2 + R3 also zooms the scope with some "
                       "guns: pick Select + Start (View + Menu on Xbox) or Keyboard only (Insert) then.",
                       choice(p + "menu_shortcut", shortcut ? *shortcut : Value::text("lt_r3"), MENU_SHORTCUTS, "")));
    page.items.push_back(section(p + "game", "Game", with_separators(std::move(game))));

    std::vector<Value> extra;
    extra.push_back(row(p + "row:anisotropy", "Texture filtering",
                        "Sharper ground and wall textures seen at an angle, in all three games. 16x costs "
                        "almost no frame rate. Game = as on the PS4.",
                        choice(p + "anisotropy", Value::text(config_str(state, "anisotropy", "0")), ANISOTROPY, "16")));
    add_toggles(extra, state, EXTRA_QUALITY);
    page.items.push_back(section(p + "extra", "Extra quality", with_separators(std::move(extra))));

    std::vector<Value> unlocks;
    add_toggles(unlocks, state, UNLOCKS);
    page.items.push_back(section(p + "unlocks", "Unlocks", with_separators(std::move(unlocks))));

    std::vector<Value> launcher;
    add_toggles(launcher, state, LAUNCHER_TOGGLES);
    page.items.push_back(section(p + "launcher", "Launcher", with_separators(std::move(launcher))));
    return page;
}

bool act_advanced(const State &state, const std::string &id, const std::string &kind, const Value &value, Changes &changes) {
    const std::string p = "native:advanced:";
    if (id.compare(0, p.size(), p) != 0) return false;
    const std::string key = id.substr(p.size()), v = text_of(value);
    if (kind == "choice") {
        if (key == "save_profile" && has_option(SAVE_PROFILES, v)) changes.game.set(key, Value::text(v));
        else if (key == "u3_exposure" && has_option(EXPOSURES, v)) changes.config.set(key, Value::text(v));
        else if (key == "speakers" && has_option(SPEAKERS, v)) changes.config.set(key, Value::text(v));
        else if (key == "anisotropy" && has_option(ANISOTROPY, v)) changes.config.set(key, Value::text(v));
        else if (key == "menu_shortcut" && has_option(MENU_SHORTCUTS, v)) changes.ini_all.set(key, Value::text(v));
        else return false;
        return true;
    }
    if (kind == "dropdown" && key == "language" && has_option(language_options(state), v)) {
        changes.config.set(key, Value::text(v));
        return true;
    }
    if (kind == "toggle") {
        auto in = [&](const ConfigToggle *first, size_t n) {
            for (size_t i = 0; i < n; ++i) if (key == first[i].key) return true;
            return false;
        };
        if (in(EXTRA_QUALITY, 4) || in(UNLOCKS, 2) || in(LAUNCHER_TOGGLES, 2)) {
            changes.config.set(key, Value::boolean(value.truthy()));
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------------------------------------ tools

// The anti-aliasing technologies' credits (NVIDIA's DLSS license asks for its attribution).
const char *const CREDITS = "Credits: DLSS and DLAA use NVIDIA DLSS technology. NVIDIA, RTX and DLSS are trademarks of "
                            "NVIDIA Corporation. AMD FidelityFX Super Resolution (FSR 3.1, FSR 4) by AMD. Uncharted camera "
                            "motion for anti-aliasing after SuperMedo's Uncharted port.";

// TOOLS' buttons: the launcher action each one runs (the Python side's calls(), by name), and its label
struct ToolButton { const char *action, *label; };
struct ToolSection { const char *title, *note; std::vector<ToolButton> buttons; };
const std::vector<ToolSection> &tool_sections() {
    static const std::vector<ToolSection> sections = {
        {"Shortcuts", "Game shortcuts: one per game on the desktop, each starts its game directly.",
         {{"desktop_shortcut", "Desktop shortcut"}, {"game_shortcuts", "Game shortcuts"}}},
        {"Steam", "A non-Steam game in your library: the Collection, or one entry per game.",
         {{"add_to_steam", "Add to Steam"}, {"add_games_to_steam", "Add games to Steam"}}},
        {"Folders", "",
         {{"open_logs", "Open log folder"}, {"open_saves", "Open save folder"}, {"choose_game_dir", "Change game folder"},
          {"install_game_pkg", "Install game from .pkg"}, {"check_game_files", "Check game files"}}},
        {"Game update", "Your extracted update 1.02 folder, wherever it is, or the update's .pkg file.",
         {{"choose_update_folder", "Choose update folder"}, {"install_game_update", "Install from .pkg"}}},
        {"Troubleshooting", "",
         {{"check_updates", "Check for updates"}, {"clear_cache", "Clear shader cache"}, {"reset", "Reset all settings"}}}};
    return sections;
}

Value button(const std::string &id, const std::string &text, const char *kind) {
    Value b = Value::object();
    b.set("t", Value::text("button"));
    b.set("id", Value::text(id));
    b.set("text", Value::text(text));
    b.set("kind", Value::text(kind));
    return b;
}

Value section_with_note(const std::string &id, const std::string &title, const std::string &note, Value items) {
    Value s = section(id, title, std::move(items));
    s.set("note", Value::text(note));
    return s;
}

Value page_tools(const State &state) {
    const std::string p = "native:tools:";
    Value page = array();
    for (auto &sec : tool_sections()) {
        Value line = array();
        for (auto &b : sec.buttons) line.items.push_back(packed(button(p + b.action, b.label, "secondary"), "left"));
        std::vector<Value> items;
        items.push_back(box(p + "box:" + sec.title, "h", std::move(line)));
        if (std::string(sec.title) == "Folders")   // the game folder, the game update, the settings file
            for (const char *key : {"game_dir", "update", "ini"})
                if (const Value *t = state.tools.get(key))
                    if (!text_of(*t).empty()) items.push_back(label(p + "line:" + key, text_of(*t), DIM, CARD, 9));
        Value list = array();
        for (auto &i : items) list.items.push_back(std::move(i));
        page.items.push_back(section_with_note(p + sec.title, sec.title, sec.note, std::move(list)));
    }
    Value credits = array();
    credits.items.push_back(label(p + "credits", CREDITS, DIM, CARD, 9));
    page.items.push_back(section(p + "Credits", "Credits", std::move(credits)));
    return page;
}

// A button runs its launcher action (the Python side's calls() list)
bool act_tools(const std::string &id, const std::string &kind, Changes &changes) {
    const std::string p = "native:tools:";
    if (kind != "button" || id.compare(0, p.size(), p) != 0) return false;
    const std::string action = id.substr(p.size());
    for (auto &sec : tool_sections())
        for (auto &b : sec.buttons)
            if (action == b.action) {
                changes.call = Value::object();
                changes.call.set("name", Value::text(action));
                return true;
            }
    return false;
}

// ------------------------------------------------------------------------------------------------ launcher

extern "C" UN_API int un_update_installed(const wchar_t *root, char *out, int cap);   // updater.cpp

const Options UPDATE_MODES = {{"auto", "Automatic"}, {"manual", "Manual"}};

// The installed version as the launcher shows it (updater.version_text: its numbers joined by dots), "" for none
std::string installed_version(const State &state) {
    const std::wstring root = widen_utf8(state.root);
    std::string text = native_text([&](char *o, int c) { return un_update_installed(root.c_str(), o, c); });
    if (text.empty()) return "";
    std::string out;
    size_t at = 0;
    while (at <= text.size()) {
        size_t dot = text.find('.', at);
        if (dot == std::string::npos) dot = text.size();
        const std::string part = text.substr(at, dot - at);
        char *end = nullptr;
        const long n = std::strtol(part.c_str(), &end, 10);
        if (part.empty() || *end) return "";   // not a version: as Python's int() would fail
        out += (out.empty() ? "" : ".") + std::to_string(n);
        at = dot + 1;
    }
    return out;
}

Value page_launcher(const State &state) {
    const std::string p = "native:launcher:";
    std::vector<Value> rows;
    const Value *mode = state.config.get("update_mode");
    rows.push_back(row(p + "row:update_mode", "Launcher updates",
                       "Automatic: a new version installs by itself when no game or import is running. Manual: the "
                       "UPDATE button installs it.",
                       choice(p + "update_mode", mode ? *mode : Value::text("manual"), UPDATE_MODES, "")));
    const std::string version = installed_version(state);
    rows.push_back(row(p + "row:check_updates", "Check for updates at start",
                       "Installed: " + (version.empty() ? std::string("source tree (updates with git)") : version) + ".",
                       toggle(p + "check_updates", config_bool(state, "check_updates", true))));
    rows.push_back(row(p + "row:check_now", "Check for a new version", "", button(p + "check_now", "Check now", "secondary")));
    Value page = array();
    page.items.push_back(section(p + "updates", "Updates", with_separators(std::move(rows))));
    return page;
}

// The update mode runs the launcher's change_update_mode (it also shows or starts the update); Check now its check.
bool act_launcher(const std::string &id, const std::string &kind, const Value &value, Changes &changes) {
    const std::string p = "native:launcher:";
    if (id.compare(0, p.size(), p) != 0) return false;
    const std::string key = id.substr(p.size());
    if (kind == "choice" && key == "update_mode" && has_option(UPDATE_MODES, text_of(value))) {
        changes.call = Value::object();
        changes.call.set("name", Value::text("update_mode"));
        changes.call.set("value", Value::text(text_of(value)));
        return true;
    }
    if (kind == "toggle" && key == "check_updates") {
        changes.config.set(key, Value::boolean(value.truthy()));
        return true;
    }
    if (kind == "button" && key == "check_now") {
        changes.call = Value::object();
        changes.call.set("name", Value::text("check_updates"));
        return true;
    }
    return false;
}

// ------------------------------------------------------------------------------------------------ controls


const char *const FIELD = "#222228";
const char *const FIELD_HI = "#2c2c34";
const char *const STOP_RED = "#c4553f";
const char *const BINDINGS_HINT = "Click a slot, then press a key or a mouse button (right-click a slot to clear it). In game "
                                  "the mouse is captured; Alt+Tab or Insert (menu) frees it. A controller always works too.";

const Value &control_tables() {
    static Value tables;
    static bool ready = false;
    if (!ready) {
        std::string text(1 << 18, '\0');
        if (un_controls_tables(&text[0], (int)text.size()) == 0) json::parse(text.c_str(), tables);
        ready = true;
    }
    return tables;
}
const Value *control_table(const char *name) { return control_tables().get(name); }

Value controls_call(const char *what, const Value &in) {
    const std::string arg = json::dump(in, false);
    Value out;
    json::parse(native_text([&](char *o, int c) { return un_controls(what, arg.c_str(), o, c); }).c_str(), out);
    return out;
}

Options pairs_of(const char *table) {
    Options o;
    if (const Value *t = control_table(table))
        for (auto &p : t->items) if (p.items.size() == 2) o.push_back({text_of(p.items[0]), text_of(p.items[1])});
    return o;
}

bool in_list(const char *table, const std::string &v) {
    if (const Value *t = control_table(table))
        for (auto &x : t->items) if (x.kind == Value::String && x.s == v) return true;
    return false;
}

// the controllers: (count, first one's style, held buttons) through SDL (gamepads.cpp); count -1: no SDL
struct Pads { int count = -1; std::string style; std::vector<std::string> held; };
Pads pad_state(const State &state) {
    static int sdl = -1;
    Pads p;
    if (sdl < 0) {
        const std::wstring root = widen_utf8(state.root);
        sdl = un_pad_init(root.c_str()) ? 1 : 0;
    }
    if (!sdl) return p;
    const std::string text = native_text([&](char *o, int c) { return un_pad_state(o, c); });
    std::vector<std::string> lines;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        lines.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    while (lines.size() < 3) lines.push_back("");
    p.count = std::atoi(lines[0].c_str());
    p.style = lines[1];
    size_t a = 0;
    const std::string &h = lines[2];
    while (a < h.size()) {
        size_t e = h.find(' ', a);
        if (e == std::string::npos) e = h.size();
        if (e > a) p.held.push_back(h.substr(a, e - a));
        a = e + 1;
    }
    return p;
}

// the button icons' style: the setting, or (Auto) the connected controller's, else Xbox
std::string pad_style(const State &state, const Value *settings) {
    const std::string style = settings ? str_or(settings->get("prompts"), "auto") : "auto";
    if (style == "xbox" || style == "playstation") return style;
    const Pads p = pad_state(state);
    return p.style.empty() ? "xbox" : p.style;
}

std::string pad_name(const std::string &button, bool ps) {
    if (const Value *names = control_table("PAD_NAMES"))
        if (const Value *n = names->get(button))
            if (n->items.size() == 2) return text_of(n->items[ps ? 1 : 0]);
    return button;
}

Value slot_label(const std::string &id, const std::string &text, bool waiting, bool click) {
    Value l = label(id, text, waiting ? GOLD : TEXT, waiting ? FIELD_HI : FIELD, 9);
    l.set("click", Value::boolean(click));
    l.set("rclick", Value::boolean(click));
    l.set("slot", Value::boolean(click));
    return l;
}

Value packed_fill(Value item, const char *side, const char *fill) {
    item.set("side", Value::text(side));
    item.set("fill", Value::text(fill));
    item.set("expand", Value::boolean(false));
    return item;
}

// the capture state launcher-qt keeps for this page: {"capturing": [action, slot] | null, "pad_capture": action | null,
// "pad_started": ms, "pad_held": [buttons], "hint": text, "hint_error": bool}
struct Capture {
    std::string action;   // a key slot waiting
    long long slot = -1;
    std::string pad;      // a controller slot waiting
};
Capture capture_of(const State &state) {
    Capture c;
    if (const Value *k = state.ui.get("capturing"))
        if (k->items.size() == 2) { c.action = text_of(k->items[0]); c.slot = k->items[1].i; }
    if (const Value *p = state.ui.get("pad_capture")) c.pad = text_of(*p);
    return c;
}

Value page_controls(const State &state) {
    const std::string p = "native:controls:";
    const Value *settings = game_settings(state);
    auto setting = [&](const char *key, Value fallback) {
        const Value *v = settings ? settings->get(key) : nullptr;
        return v ? *v : fallback;
    };
    Value page = array();

    std::vector<Value> mouse;
    mouse.push_back(row(p + "row:mouse", "Mouse", "", toggle(p + "mouse", setting("mouse", Value::boolean(true)).truthy())));
    mouse.push_back(row(p + "row:mouse_sens", "Sensitivity", "",
                        choice(p + "mouse_sens", setting("mouse_sens", Value::text("1.0")), pairs_of("MOUSE_SENSITIVITIES"), "")));
    mouse.push_back(row(p + "row:mouse_aim_sens", "Aiming", "",
                        choice(p + "mouse_aim_sens", setting("mouse_aim_sens", Value::text("1.0")), pairs_of("MOUSE_AIM_FACTORS"), "")));
    mouse.push_back(row(p + "row:mouse_vertical", "Up/down", "",
                        choice(p + "mouse_vertical", setting("mouse_vertical", Value::text("1.0")), pairs_of("MOUSE_FACTORS"), "")));
    mouse.push_back(row(p + "row:mouse_invert", "Invert Y", "",
                        toggle(p + "mouse_invert", setting("mouse_invert", Value::boolean(false)).truthy())));
    mouse.push_back(row(p + "row:mouse_invert_x", "Invert X", "",
                        toggle(p + "mouse_invert_x", setting("mouse_invert_x", Value::boolean(false)).truthy())));
    page.items.push_back(section(p + "mouse_section", "Mouse", with_separators(std::move(mouse))));

    std::vector<Value> pad;
    pad.push_back(row(p + "row:vibration", "Controller vibration", "",
                      toggle(p + "vibration", setting("vibration", Value::boolean(true)).truthy())));
    pad.push_back(row(p + "row:prompts", "Button icons", "Auto follows the device you play with (a controller's own buttons).",
                      choice(p + "prompts", setting("prompts", Value::text("auto")), pairs_of("PROMPT_MODES"), "")));
    page.items.push_back(section(p + "controller", "Controller", with_separators(std::move(pad))));

    // Bindings: the hint, Reset controls, then the two columns of groups (keys / mouse, and the controller button)
    std::vector<Value> bindings;
    const Value *hint = state.ui.get("hint");
    const Value *error = state.ui.get("hint_error");
    bindings.push_back(label(p + "hint", hint && !text_of(*hint).empty() ? text_of(*hint) : BINDINGS_HINT,
                             error && error->truthy() ? STOP_RED : MUTED, CARD, 9));
    Value reset = array();
    reset.items.push_back(packed(button(p + "reset", "Reset controls", "secondary"), "left"));
    bindings.push_back(box(p + "reset_box", "h", std::move(reset)));

    const Capture cap = capture_of(state);
    const Value merged = controls_call("merged", setting("controls", Value::null()));
    const Value merged_pad = controls_call("merged_pad", setting("pad_controls", Value::null()));
    const Value *pad_defaults = control_table("PAD_DEFAULTS"), *pad_fixed = control_table("PAD_FIXED");
    const std::string style = pad_style(state, settings);
    const bool ps = style == "playstation";
    Value columns[2] = {array(), array()};
    int index = 0;
    if (const Value *groups = control_table("CONTROL_GROUPS"))
        for (auto &g : groups->items) {
            if (g.items.size() != 2) continue;
            Value &column = columns[index < 2 ? 0 : 1];
            column.items.push_back(packed_fill(label(p + "group:" + text_of(g.items[0]), text_of(g.items[0]), GOLD, CARD, 9),
                                               "top", "x"));
            for (auto &entry : g.items[1].items) {
                if (entry.items.size() != 2) continue;
                const std::string action = text_of(entry.items[0]);
                Value line = array();
                line.items.push_back(packed(label(p + "action:" + action, text_of(entry.items[1]), TEXT, CARD, 9), "left"));
                const Value *list = merged.get(action);
                for (int slot = 0; slot < 2; ++slot) {
                    const bool waiting = cap.action == action && cap.slot == slot;
                    Value b = list && (size_t)slot < list->items.size() ? list->items[slot] : Value::null();
                    const std::string text = waiting ? "Press a key\xe2\x80\xa6" : text_of(controls_call("label", b));
                    line.items.push_back(packed(slot_label(p + "slot:" + action + ":" + std::to_string(slot), text, waiting, true),
                                                "left"));
                }
                // the controller button: its icon (clickable where the button can be changed)
                const bool changeable = pad_defaults && pad_defaults->get(action);
                Value cell;
                if (cap.pad == action) {
                    cell = slot_label(p + "pad:" + action, "Press a button\xe2\x80\xa6", true, changeable);
                } else {
                    const Value *bv = changeable ? merged_pad.get(action) : (pad_fixed ? pad_fixed->get(action) : nullptr);
                    const std::string b = bv && bv->kind == Value::String ? bv->s : "";
                    const Value *def = pad_defaults ? pad_defaults->get(action) : nullptr;
                    if (!def && pad_fixed) def = pad_fixed->get(action);
                    if (b.empty()) {
                        cell = slot_label(p + "pad:" + action, "\xe2\x80\x94", false, changeable);
                    } else if (ps && def && text_of(*def) == b && (action == "touchpad_left" || action == "touchpad_right")) {
                        cell = slot_label(p + "pad:" + action, action == "touchpad_left" ? "Touchpad left" : "Touchpad right",
                                          false, changeable);
                    } else {
                        cell = slot_label(p + "pad:" + action, "", false, changeable);
                        Value icon = Value::object(), buttons = array(), labels = array();
                        size_t a = 0;
                        while (a <= b.size()) {   // a combination: its buttons
                            size_t e = b.find('+', a);
                            if (e == std::string::npos) e = b.size();
                            const std::string part = b.substr(a, e - a);
                            buttons.items.push_back(Value::text(part));
                            labels.items.push_back(Value::text(pad_name(part, ps)));
                            a = e + 1;
                        }
                        icon.set("buttons", std::move(buttons));
                        icon.set("labels", std::move(labels));
                        icon.set("style", Value::text(style));
                        cell.set("pad", std::move(icon));
                    }
                }
                line.items.push_back(packed(std::move(cell), "left"));
                Value row_box = box(p + "line:" + action, "h", std::move(line));
                column.items.push_back(packed_fill(std::move(row_box), "top", "x"));
            }
            ++index;
        }
    Value grid_row = Value::object();
    grid_row.set("t", Value::text("box"));
    grid_row.set("dir", Value::text("h"));
    Value cols = array();
    for (int c = 0; c < 2; ++c) cols.items.push_back(box(p + "column:" + std::to_string(c), "v", std::move(columns[c])));
    grid_row.set("items", std::move(cols));
    Value inner_items = array();
    inner_items.items.push_back(std::move(grid_row));
    Value scroller = Value::object();   // the scrolling list (a canvas holding the grid)
    scroller.set("t", Value::text("box"));
    scroller.set("dir", Value::text("v"));
    scroller.set("id", Value::text(p + "scroll"));
    Value scroller_items = array();
    scroller_items.items.push_back(box(p + "grid", "v", std::move(inner_items)));
    scroller.set("items", std::move(scroller_items));
    scroller.set("side", Value::text("left"));
    scroller.set("fill", Value::text("both"));
    scroller.set("expand", Value::boolean(true));
    Value holder_items = array();
    holder_items.items.push_back(std::move(scroller));
    bindings.push_back(box(p + "holder", "h", std::move(holder_items)));
    page.items.push_back(section(p + "bindings", "Bindings", with_separators(std::move(bindings))));
    return page;
}

// the ui state after a change: what it was, with these fields replaced
Value ui_with(const State &state, std::initializer_list<std::pair<const char *, Value>> fields) {
    Value ui = state.ui.is_object() ? state.ui : Value::object();
    for (auto &f : fields) ui.set(f.first, f.second);
    return ui;
}

long long now_ms() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool act_controls(const State &state, const std::string &id, const std::string &kind, const Value &value, Changes &changes) {
    const std::string p = "native:controls:";
    if (id.compare(0, p.size(), p) != 0) return false;
    const std::string key = id.substr(p.size()), v = text_of(value);
    const Value *settings = game_settings(state);
    const std::string name = game_name(state);
    const Capture cap = capture_of(state);
    auto saved_keys = [&]() {
        return ui_with(state, {{"capturing", Value::null()}, {"pad_capture", Value::null()},
                               {"hint", Value::text("Saved for " + name + ". Click a slot, then press a key or a mouse button "
                                                    "(right-click a slot to clear it).")},
                               {"hint_error", Value::boolean(false)}});
    };
    auto saved_pad = [&]() {
        return ui_with(state, {{"pad_capture", Value::null()},
                               {"hint", Value::text("Saved for " + name + ". Right-click a controller slot to clear it; "
                                                    "Reset controls restores the defaults.")},
                               {"hint_error", Value::boolean(false)}});
    };
    auto error = [&](const std::string &text) {
        return ui_with(state, {{"capturing", Value::null()}, {"hint", Value::text(text)}, {"hint_error", Value::boolean(true)}});
    };
    auto set_binding = [&](const std::string &action, long long slot, const Value &binding) {
        Value in = Value::object();
        in.set("controls", settings && settings->get("controls") ? *settings->get("controls") : Value::null());
        in.set("action", Value::text(action));
        in.set("slot", Value::integer(slot));
        in.set("binding", binding);
        changes.game.set("controls", controls_call("set_binding", in));
        changes.ui = saved_keys();
    };
    auto set_pad_binding = [&](const std::string &action, const Value &button) {
        Value in = Value::object();
        in.set("pad_controls", settings && settings->get("pad_controls") ? *settings->get("pad_controls") : Value::null());
        in.set("action", Value::text(action));
        in.set("button", button);
        changes.game.set("pad_controls", controls_call("set_pad_binding", in));
        changes.ui = saved_pad();
    };
    const Value *pad_defaults = control_table("PAD_DEFAULTS");

    // the mouse and controller settings (change_game_setting saves controls to the game's settings file too)
    if (kind == "toggle" && (key == "mouse" || key == "mouse_invert" || key == "mouse_invert_x" || key == "vibration")) {
        changes.game.set(key, Value::boolean(value.truthy()));
        return true;
    }
    if (kind == "choice") {
        const char *table = key == "mouse_sens" ? "MOUSE_SENSITIVITIES" : key == "mouse_aim_sens" ? "MOUSE_AIM_FACTORS"
                          : key == "mouse_vertical" ? "MOUSE_FACTORS" : key == "prompts" ? "PROMPT_MODES" : nullptr;
        if (!table || !has_option(pairs_of(table), v)) return false;
        changes.game.set(key, Value::text(v));
        return true;
    }
    if (kind == "button" && key == "reset") {   // asks first: the launcher's reset_controls
        changes.call = Value::object();
        changes.call.set("name", Value::text("reset_controls"));
        changes.ui = ui_with(state, {{"capturing", Value::null()}, {"pad_capture", Value::null()}});
        return true;
    }
    // a key / mouse slot: click waits for an input, right-click clears it
    if (key.compare(0, 5, "slot:") == 0) {
        const size_t colon = key.rfind(':');
        const std::string action = key.substr(5, colon - 5);
        const long long slot = std::atoll(key.c_str() + colon + 1);
        const Value *merged = control_table("DEFAULT_CONTROLS");
        if (!merged || !merged->get(action) || slot < 0 || slot > 1) return false;
        if (kind == "click") {
            changes.ui = ui_with(state, {{"capturing", [&] { Value a = array(); a.items = {Value::text(action), Value::integer(slot)}; return a; }()},
                                         {"pad_capture", Value::null()}});
            return true;
        }
        if (kind == "rclick") { set_binding(action, slot, Value::null()); return true; }
        return false;
    }
    // a controller slot: click waits for a button (or a stick), right-click clears it
    if (key.compare(0, 4, "pad:") == 0) {
        const std::string action = key.substr(4);
        if (!pad_defaults || !pad_defaults->get(action)) return false;
        if (kind == "rclick") { set_pad_binding(action, Value::null()); return true; }
        if (kind != "click") return false;
        const Pads pads = pad_state(state);
        if (pads.count < 0) {
            changes.ui = error("No controller support found (SDL3.dll).");
            return true;
        }
        if (pads.count == 0) {
            changes.ui = ui_with(state, {{"capturing", Value::null()}, {"pad_capture", Value::null()},
                                         {"hint", Value::text("Connect a controller, then click the slot again.")},
                                         {"hint_error", Value::boolean(true)}});
            return true;
        }
        Value held = array();
        for (auto &h : pads.held) held.items.push_back(Value::text(h));
        const bool stick = action.compare(0, 3, "ls_") == 0 || action.compare(0, 3, "rs_") == 0;
        changes.ui = ui_with(state, {{"capturing", Value::null()}, {"pad_capture", Value::text(action)},
                                     {"pad_started", Value::integer(now_ms())}, {"pad_held", held},
                                     {"hint", Value::text(stick ? "Move a stick (Escape cancels)."
                                                                : "Press a button or trigger on the controller (Escape cancels).")},
                                     {"hint_error", Value::boolean(false)}});
        return true;
    }
    // what the window captured while a slot waits: {"key": Tk keysym} or {"mouse": button name}
    if (kind == "capture" && key == "capture") {
        const std::string keysym = value.get("key") ? text_of(*value.get("key")) : "";
        const std::string mouse = value.get("mouse") ? text_of(*value.get("mouse")) : "";
        if (!cap.pad.empty()) {   // a controller slot waits: Escape cancels
            if (keysym == "Escape") changes.ui = ui_with(state, {{"pad_capture", Value::null()}});
            return true;
        }
        if (cap.action.empty()) return true;
        if (!keysym.empty()) {
            if (in_list("RESERVED_KEYS", keysym)) {
                changes.ui = error(keysym + " is used by the game window (Insert opens the menu, F9 records input, comma "
                                            "separates bindings). Choose another key.");
                return true;
            }
            std::string sdl;
            if (const Value *t = control_table("TK_TO_SDL"))
                if (const Value *n = t->get(keysym)) sdl = text_of(*n);
            if (sdl.empty() && keysym.size() == 1 && std::isalnum((unsigned char)keysym[0]))
                sdl = std::string(1, (char)std::toupper((unsigned char)keysym[0]));
            if (sdl.empty()) {
                changes.ui = error("The key " + keysym + " can't be used. Choose another key.");
                return true;
            }
            set_binding(cap.action, cap.slot, Value::text("key:" + sdl));
            return true;
        }
        if (!mouse.empty()) {
            if (in_list("STICK_ACTIONS", cap.action)) {
                changes.ui = error("Stick directions take keys; the mouse moves the camera by itself.");
                return true;
            }
            set_binding(cap.action, cap.slot, Value::text("mouse:" + mouse));
        }
        return true;
    }
    // the controller slot waiting: a new button (or stick) binds it; 8 seconds without one cancel
    if (kind == "poll" && key == "pad_poll") {
        if (cap.pad.empty()) return true;
        const Pads pads = pad_state(state);
        std::vector<std::string> before;
        if (const Value *h = state.ui.get("pad_held")) for (auto &x : h->items) before.push_back(text_of(x));
        auto has = [](const std::vector<std::string> &l, const std::string &x) { return std::find(l.begin(), l.end(), x) != l.end(); };
        std::vector<std::string> order;
        if (const Value *sdl = control_table("SDL_BUTTONS")) for (auto &x : sdl->items) order.push_back(text_of(x));
        for (const char *x : {"lefttrigger", "righttrigger", "lstick", "rstick"}) order.push_back(x);
        const bool stick_action = cap.pad.compare(0, 3, "ls_") == 0 || cap.pad.compare(0, 3, "rs_") == 0;
        std::vector<std::string> pressed, fresh;
        for (auto &b : order)
            if (has(pads.held, b) && !has(before, b)) {
                pressed.push_back(b);
                if ((b == "lstick" || b == "rstick") == stick_action) fresh.push_back(b);
            }
        Value still = array();   // held &= now held
        for (auto &b : before) if (has(pads.held, b)) still.items.push_back(Value::text(b));
        Value ui = ui_with(state, {{"pad_held", still}});
        if (stick_action && !pressed.empty() && fresh.empty()) {
            ui.set("hint", Value::text("Move and Look take a stick. Move a stick."));
            ui.set("hint_error", Value::boolean(true));
        }
        if (!fresh.empty()) {
            set_pad_binding(cap.pad, Value::text(fresh[0]));
            return true;
        }
        const Value *started = state.ui.get("pad_started");
        if (started && now_ms() - started->i > 8000) ui.set("pad_capture", Value::null());
        changes.ui = ui;
        return true;
    }
    return false;
}

}  // namespace

extern "C" {

// The page `name` for the launcher's state `state_json`: {"config": launcher.json's object, "ini": the selected game's
// settings file}. Its tree as a JSON array. 0 ok, 2 the page is not a native one, 3 the state is not readable.
UN_API int un_page(const char *name, const char *state_json, char *out, int cap) {
    State state;
    if (!read_state(state_json, state)) return 3;
    const std::string n = name ? name : "";
    if (n == "cheats") return give(json::dump(page_cheats(state.config), false), out, cap);
    if (n == "performance") return give(json::dump(page_performance(state), false), out, cap);
    if (n == "display") return give(json::dump(page_display(state), false), out, cap);
    if (n == "setup") return give(json::dump(page_setup(state), false), out, cap);
    if (n == "advanced") return give(json::dump(page_advanced(state), false), out, cap);
    if (n == "tools") return give(json::dump(page_tools(state), false), out, cap);
    if (n == "launcher") return give(json::dump(page_launcher(state), false), out, cap);
    if (n == "controls") return give(json::dump(page_controls(state), false), out, cap);
    return 2;
}

// What a click on a native page's control changes: {"config": {...}, "ini": {...}, "game": {...}} (launcher.json keys,
// the selected game's settings file keys, the selected game's launcher settings). kind: toggle / choice / dropdown /
// chip / button / entry, as the Qt window sends them; value_json: the new value.
UN_API int un_page_act(const char *name, const char *id, const char *kind, const char *value_json, const char *state_json,
                       char *out, int cap) {
    State state;
    Value value;
    if (!read_state(state_json, state)) return 3;
    if (!json::parse(value_json && *value_json ? value_json : "null", value)) value = Value::null();
    const std::string n = name ? name : "", i = id ? id : "", k = kind ? kind : "";
    Changes changes;
    if (n == "cheats") act_cheats(i, k, value, changes);
    else if (n == "performance") act_performance(i, k, value, changes);
    else if (n == "display") act_display(state, i, k, value, changes);
    else if (n == "setup") act_setup(state, i, k, changes);
    else if (n == "advanced") act_advanced(state, i, k, value, changes);
    else if (n == "tools") act_tools(i, k, changes);
    else if (n == "launcher") act_launcher(i, k, value, changes);
    else if (n == "controls") act_controls(state, i, k, value, changes);
    else return 2;
    Value result = Value::object();
    result.set("config", std::move(changes.config));
    result.set("ini", std::move(changes.ini));
    result.set("game", std::move(changes.game));
    result.set("ini_all", std::move(changes.ini_all));
    if (changes.call.is_object()) result.set("call", std::move(changes.call));
    if (changes.ui.is_object()) result.set("ui", std::move(changes.ui));
    return give(json::dump(result, false), out, cap);
}

}  // extern "C"
