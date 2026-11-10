// pages.cpp - the launcher's pages built natively, part of uncharted_native.dll. A page is the tree the Qt window
// (launcher-qt, NodeView.qml / PageView.qml) draws: sections, rows, separators, labels and controls, as JSON. A click
// on one of its controls comes back to un_page_act, which says what changes: settings keys of launcher.json for now
// (the launcher's Python side still writes that file, so there is one writer).
//
//   un_page(name, config)                         the page, or status 2 when the page is not a native one
//   un_page_act(name, id, kind, value, config)    {"config": {key: value, ...}}: the settings the click changes
//
// Pages: cheats (the games' developer switches).

#include "native.h"
#include "json.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

using json::Value;

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

// The colours and fonts of the launcher's look (uncharted_launcher.pyw / Theme.qml).
const char *const BG = "#151519";
const char *const MUTED = "#a3a09a";

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
                                MUTED, BG, 9));
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
bool act_cheats(const std::string &id, const std::string &kind, const Value &value, Value &changes) {
    const std::string prefix = "native:cheats:cheat_";
    if (kind != "toggle" || id.compare(0, prefix.size(), prefix) != 0) return false;
    const std::string key = id.substr(std::strlen("native:cheats:"));
    for (auto &cheat : cheat_list())
        if (key == "cheat_" + cheat[0]) {
            changes.set(key, Value::boolean(value.truthy()));
            return true;
        }
    return false;
}

}  // namespace

extern "C" {

// The page `name` for the settings `config_json` (launcher.json's object): its tree as a JSON array. 0 ok, 2 the page
// is not a native one, 3 the settings are not a JSON object.
UN_API int un_page(const char *name, const char *config_json, char *out, int cap) {
    Value config;
    if (!json::parse(config_json ? config_json : "{}", config) || !config.is_object()) return 3;
    const std::string n = name ? name : "";
    if (n == "cheats") return give(json::dump(page_cheats(config), false), out, cap);
    return 2;
}

// What a click on a native page's control changes: {"config": {key: value, ...}} (an empty object: nothing).
// kind: toggle / choice / dropdown / chip / button / entry, as the Qt window sends them; value_json: the new value.
UN_API int un_page_act(const char *name, const char *id, const char *kind, const char *value_json, const char *config_json,
                       char *out, int cap) {
    Value config, value;
    if (!json::parse(config_json ? config_json : "{}", config) || !config.is_object()) return 3;
    if (!json::parse(value_json && *value_json ? value_json : "null", value)) value = Value::null();
    const std::string n = name ? name : "";
    Value changes = Value::object();
    if (n == "cheats") act_cheats(id ? id : "", kind ? kind : "", value, changes);
    else return 2;
    Value result = Value::object();
    result.set("config", std::move(changes));
    return give(json::dump(result, false), out, cap);
}

}  // extern "C"
