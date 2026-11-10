// pages.cpp - the launcher's pages built natively, part of uncharted_native.dll. A page is the tree the Qt window
// (launcher-qt, NodeView.qml / PageView.qml) draws: sections, rows, separators, labels and controls, as JSON. A click
// on one of its controls comes back to un_page_act, which says what changes: settings keys of launcher.json for now
// (the launcher's Python side still writes that file, so there is one writer).
//
//   un_page(name, state)                          the page, or status 2 when the page is not a native one
//   un_page_act(name, id, kind, value, state)     {"config", "ini", "game"}: the settings the click changes
// state: {"config": launcher.json, "ini": the selected game's settings file (analysis/uncharted-user/probe-*.ini)}.
//
// Pages: cheats (the games' developer switches), performance (frame rate, processor, video memory).

#include "native.h"
#include "json.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" UN_API int un_monitor_refresh_hz(void);   // display.cpp

namespace {

using json::Value;

// The launcher's state a page is built from, and what a click changes.
struct State {
    Value config = Value::object();
    Value ini = Value::object();
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
    else return 2;
    Value result = Value::object();
    result.set("config", std::move(changes.config));
    result.set("ini", std::move(changes.ini));
    result.set("game", std::move(changes.game));
    return give(json::dump(result, false), out, cap);
}

}  // extern "C"
