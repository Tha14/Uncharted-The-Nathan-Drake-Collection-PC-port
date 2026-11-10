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
// upscaling, picture, performance counter), setup (Quick Setup's preset cards). state also has "root": the launcher's
// folder (which upscalers it has), and "gpu": {"name", "recommended"}: the graphics card the launcher found and its preset.

#include "native.h"
#include "json.h"

#include <cctype>
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

namespace {

using json::Value;

// The launcher's state a page is built from, and what a click changes.
struct State {
    Value config = Value::object();
    Value ini = Value::object();
    std::string root = ".";   // the launcher's folder
    Value gpu = Value::object();   // {"name", "recommended"}
};
struct Changes {
    Value config = Value::object();   // launcher.json keys
    Value ini = Value::object();      // the selected game's settings file
    Value game = Value::object();     // the selected game's launcher settings (config["games"][game])
};

bool read_state(const char *state_json, State &state) {
    Value v;
    if (!json::parse(state_json ? state_json : "{}", v) || !v.is_object()) return false;
    if (const Value *c = v.get("config")) { if (!c->is_object()) return false; state.config = *c; }
    if (const Value *i = v.get("ini")) { if (!i->is_object()) return false; state.ini = *i; }
    if (const Value *r = v.get("root")) if (r->kind == Value::String && !r->s.empty()) state.root = r->s;
    if (const Value *g = v.get("gpu")) if (g->is_object()) state.gpu = *g;
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
    else return 2;
    Value result = Value::object();
    result.set("config", std::move(changes.config));
    result.set("ini", std::move(changes.ini));
    result.set("game", std::move(changes.game));
    return give(json::dump(result, false), out, cap);
}

}  // extern "C"
