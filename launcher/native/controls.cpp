// controls.cpp - input binding logic behind the launcher's Controls page (built into uncharted_native.dll).
// Holds the fixed data (action list, default bindings, gamepad button names, sensitivity choices, key-name mapping)
// and converts between the per-game launcher settings and the game's settings file. In that file the runtime
// (runtime_pad.c) picks them up as key_map / mouse_map / pad_map, and the in-game menu can edit them as well.
//
// Binding format: each action holds at most two entries, either "key:<name of an SDL key>" or
// "mouse:<left|middle|right|x1|x2|wheel_up|wheel_down>"; the stick-direction actions accept keys only.
// A map string is "action=name,..." and an empty right side ("action=") means unbound.

#include "native.h"
#include "json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

using json::Value;
using Pairs = std::vector<std::pair<const char *, const char *>>;

// ------------------------------------------------------------------------------------------------ tables

struct Group {
    const char *title;
    Pairs actions;   // (action id, text shown on the page)
};

const std::vector<Group> &groups() {
    static const std::vector<Group> g = {
        {"MOVEMENT", {{"ls_up", "Move forward"}, {"ls_down", "Move back"}, {"ls_left", "Move left"}, {"ls_right", "Move right"},
                      {"rs_up", "Look up"}, {"rs_down", "Look down"}, {"rs_left", "Look left"}, {"rs_right", "Look right"}}},
        {"WEAPONS (D-PAD)", {{"left", "Left"}, {"right", "Right"}, {"up", "Up"}, {"down", "Down"}}},
        {"ACTIONS", {{"cross", "Jump, climb"}, {"circle", "Cover, roll, drop"}, {"square", "Melee"},
                     {"triangle", "Interact, pick up"}, {"l2", "Aim"}, {"r2", "Fire"},
                     {"l1", "Grenade (hold, release to throw)"}, {"r1", "Reload"}, {"l3", "Left stick press"},
                     {"r3", "Right stick press"}, {"options", "Pause menu"}, {"touchpad_left", "Touch left"},
                     {"touchpad_right", "Touch right"}}},
    };
    return g;
}

// default gamepad input per action, using SDL's button names; mirrors the built-in pad map in runtime_pad.c
const Pairs PAD_DEFAULTS = {{"cross", "a"},         {"circle", "b"},        {"square", "x"},    {"triangle", "y"},
                            {"l1", "leftshoulder"}, {"r1", "rightshoulder"}, {"l3", "leftstick"}, {"r3", "rightstick"},
                            {"options", "start"},   {"touchpad_right", "back"}, {"up", "dpup"}, {"down", "dpdown"},
                            {"left", "dpleft"},     {"right", "dpright"},
                            // triggers, then the movement stick and the camera stick (as in runtime_pad.c)
                            {"l2", "lefttrigger"},  {"r2", "righttrigger"},
                            {"ls_up", "lstick"},    {"ls_down", "lstick"},  {"ls_left", "lstick"}, {"ls_right", "lstick"},
                            {"rs_up", "rstick"},    {"rs_down", "rstick"},  {"rs_left", "rstick"}, {"rs_right", "rstick"}};
// left touchpad click is hard-wired to LB + Back/View and is not remappable
const Pairs PAD_FIXED = {{"touchpad_left", "leftshoulder+back"}};
// in the same order as SDL3's SDL_GamepadButton enum
const char *const SDL_BUTTONS[] = {"a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
                                   "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2",
                                   "paddle3", "paddle4", "touchpad"};
// SDL name -> Xbox name, PlayStation name; used as slot text when no icon exists and in tooltips
const std::vector<std::vector<const char *>> PAD_NAMES = {
    {"a", "A", "Cross"}, {"b", "B", "Circle"}, {"x", "X", "Square"}, {"y", "Y", "Triangle"}, {"back", "View", "Share"},
    {"guide", "Guide", "PS"}, {"start", "Menu", "Options"}, {"leftstick", "LS", "L3"}, {"rightstick", "RS", "R3"},
    {"leftshoulder", "LB", "L1"}, {"rightshoulder", "RB", "R1"}, {"lefttrigger", "LT", "L2"}, {"righttrigger", "RT", "R2"},
    {"dpup", "D-pad up", "D-pad up"}, {"dpdown", "D-pad down", "D-pad down"}, {"dpleft", "D-pad left", "D-pad left"},
    {"dpright", "D-pad right", "D-pad right"}, {"misc1", "Share", "Mute"}, {"paddle1", "P1", "P1"}, {"paddle2", "P2", "P2"},
    {"paddle3", "P3", "P3"}, {"paddle4", "P4", "P4"}, {"touchpad", "Touchpad", "Touchpad"},
    {"lstick", "L stick", "L stick"}, {"rstick", "R stick", "R stick"}};
const char *const STICK_ACTIONS[] = {"ls_up", "ls_down", "ls_left", "ls_right", "rs_up", "rs_down", "rs_left", "rs_right"};
const std::vector<std::pair<const char *, std::vector<const char *>>> DEFAULT_CONTROLS = {
    {"ls_up", {"key:W"}}, {"ls_down", {"key:S"}}, {"ls_left", {"key:A"}}, {"ls_right", {"key:D"}},
    {"rs_up", {"key:Up"}}, {"rs_down", {"key:Down"}}, {"rs_left", {"key:Left"}}, {"rs_right", {"key:Right"}},
    {"left", {"key:1", "mouse:wheel_down"}}, {"right", {"key:2", "mouse:wheel_up"}}, {"up", {"key:3"}}, {"down", {"key:4"}},
    {"cross", {"key:Space"}}, {"circle", {"key:Left Ctrl"}}, {"square", {"key:F"}}, {"triangle", {"key:R"}},
    {"l2", {"mouse:right"}}, {"r2", {"mouse:left"}}, {"l1", {"key:Q", "mouse:x1"}}, {"r1", {"key:E", "mouse:x2"}},
    {"l3", {"key:Left Shift"}}, {"r3", {"key:V", "mouse:middle"}}, {"options", {"key:Escape", "key:Return"}},
    {"touchpad_left", {"key:Tab"}}, {"touchpad_right", {"key:Backspace"}}};
// which button glyphs the games show in prompts and menus: follow the last device used, or one fixed set
const Pairs PROMPT_MODES = {{"auto", "Auto"}, {"keyboard", "Keyboard and mouse"}, {"playstation", "PlayStation"}, {"xbox", "Xbox"}};
const Pairs MOUSE_SENSITIVITIES = {{"0.5", "0.5x"}, {"0.75", "0.75x"}, {"1.0", "1x"}, {"1.5", "1.5x"},
                                   {"2.0", "2x"},   {"3.0", "3x"},     {"4.0", "4x"}, {"5.0", "5x"}};
// multipliers: aim-down-sights speed relative to the base sensitivity, and vertical speed relative to horizontal
const Pairs MOUSE_FACTORS = {{"0.5", "0.5x"}, {"0.75", "0.75x"}, {"1.0", "1x"}, {"1.25", "1.25x"}, {"1.5", "1.5x"}, {"2.0", "2x"}};
const Pairs MOUSE_AIM_FACTORS = {{"0.5", "0.5x"}, {"1.0", "1x"}, {"2.0", "2x"}, {"3.0", "3x"}, {"5.0", "5x"}, {"10.0", "10x"}};
const Pairs MOUSE_LABELS = {{"left", "Left mouse"},    {"middle", "Middle mouse"}, {"right", "Right mouse"},
                            {"x1", "Mouse back"},      {"x2", "Mouse forward"},    {"wheel_up", "Wheel up"},
                            {"wheel_down", "Wheel down"}};
// maps Tk keysym names to SDL key names; letters and digits need no entry (they are used as is, in upper case)
Pairs tk_to_sdl() {
    Pairs p = {{"space", "Space"}, {"Return", "Return"}, {"Escape", "Escape"}, {"Tab", "Tab"}, {"BackSpace", "Backspace"},
               {"Shift_L", "Left Shift"}, {"Shift_R", "Right Shift"}, {"Control_L", "Left Ctrl"}, {"Control_R", "Right Ctrl"},
               {"Alt_L", "Left Alt"}, {"Alt_R", "Right Alt"}, {"Caps_Lock", "CapsLock"}, {"Up", "Up"}, {"Down", "Down"},
               {"Left", "Left"}, {"Right", "Right"}, {"Delete", "Delete"}, {"Home", "Home"}, {"End", "End"},
               {"Prior", "PageUp"}, {"Next", "PageDown"}, {"minus", "-"}, {"equal", "="}, {"bracketleft", "["},
               {"bracketright", "]"}, {"backslash", "\\"}, {"semicolon", ";"}, {"apostrophe", "'"}, {"grave", "`"},
               {"period", "."}, {"slash", "/"}, {"KP_Divide", "Keypad /"}, {"KP_Multiply", "Keypad *"},
               {"KP_Subtract", "Keypad -"}, {"KP_Add", "Keypad +"}, {"KP_Enter", "Keypad Enter"},
               {"KP_Decimal", "Keypad ."}, {"Pause", "Pause"}, {"Scroll_Lock", "ScrollLock"}};
    static std::vector<std::string> extra;
    if (extra.empty()) {
        for (int n = 1; n <= 12; ++n) extra.push_back("F" + std::to_string(n));
        for (int n = 0; n < 10; ++n) { extra.push_back("KP_" + std::to_string(n)); extra.push_back("Keypad " + std::to_string(n)); }
    }
    for (int n = 0; n < 12; ++n) p.push_back({extra[n].c_str(), extra[n].c_str()});
    for (int n = 0; n < 10; ++n) p.push_back({extra[12 + 2 * n].c_str(), extra[13 + 2 * n].c_str()});
    return p;
}
// not bindable because the runtime uses them: Insert toggles the in-game menu, F9 starts a pad recording
const char *const RESERVED_KEYS[] = {"Insert", "F9", "comma"};

Value text(const char *s) { return Value::text(s); }
Value pairs_list(const Pairs &p) {
    Value v;
    v.kind = Value::Array;
    for (auto &kv : p) {
        Value pair;
        pair.kind = Value::Array;
        pair.items = {text(kv.first), text(kv.second)};
        v.items.push_back(pair);
    }
    return v;
}
Value pairs_object(const Pairs &p) {
    Value v = Value::object();
    for (auto &kv : p) v.set(kv.first, text(kv.second));
    return v;
}
Value string_list(const std::vector<std::string> &items) {
    Value v;
    v.kind = Value::Array;
    for (auto &s : items) v.items.push_back(Value::text(s));
    return v;
}

Value all_tables() {
    Value t = Value::object();
    Value g;
    g.kind = Value::Array;
    for (const Group &grp : groups()) {
        Value entry;
        entry.kind = Value::Array;
        entry.items = {text(grp.title), pairs_list(grp.actions)};
        g.items.push_back(entry);
    }
    t.set("CONTROL_GROUPS", g);
    t.set("PAD_DEFAULTS", pairs_object(PAD_DEFAULTS));
    t.set("PAD_FIXED", pairs_object(PAD_FIXED));
    std::vector<std::string> buttons(std::begin(SDL_BUTTONS), std::end(SDL_BUTTONS));
    t.set("SDL_BUTTONS", string_list(buttons));
    Value names = Value::object();
    for (auto &row : PAD_NAMES) {
        Value pair;
        pair.kind = Value::Array;
        pair.items = {text(row[1]), text(row[2])};
        names.set(row[0], pair);
    }
    t.set("PAD_NAMES", names);
    t.set("STICK_ACTIONS", string_list(std::vector<std::string>(std::begin(STICK_ACTIONS), std::end(STICK_ACTIONS))));
    Value defaults = Value::object();
    for (auto &d : DEFAULT_CONTROLS) {
        std::vector<std::string> b(d.second.begin(), d.second.end());
        defaults.set(d.first, string_list(b));
    }
    t.set("DEFAULT_CONTROLS", defaults);
    t.set("PROMPT_MODES", pairs_list(PROMPT_MODES));
    t.set("MOUSE_SENSITIVITIES", pairs_list(MOUSE_SENSITIVITIES));
    t.set("MOUSE_FACTORS", pairs_list(MOUSE_FACTORS));
    t.set("MOUSE_AIM_FACTORS", pairs_list(MOUSE_AIM_FACTORS));
    t.set("MOUSE_LABELS", pairs_object(MOUSE_LABELS));
    t.set("TK_TO_SDL", pairs_object(tk_to_sdl()));
    t.set("RESERVED_KEYS", string_list(std::vector<std::string>(std::begin(RESERVED_KEYS), std::end(RESERVED_KEYS))));
    return t;
}

// ------------------------------------------------------------------------------------------------ conversion

bool is_stick(const std::string &a) {
    for (auto *s : STICK_ACTIONS) if (a == s) return true;
    return false;
}

std::string trim(const std::string &s) {
    const char *ws = " \t\r\n\v\f";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return std::string();
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

// full action -> bindings table: start from the defaults and, for every action we know, take the game's saved list
// instead (capped at two)
std::vector<std::pair<std::string, std::vector<std::string>>> merged(const Value *controls) {
    std::vector<std::pair<std::string, std::vector<std::string>>> m;
    for (auto &d : DEFAULT_CONTROLS) m.push_back({d.first, std::vector<std::string>(d.second.begin(), d.second.end())});
    if (controls && controls->is_object())
        for (auto &kv : controls->fields)
            for (auto &entry : m)
                if (entry.first == kv.first) {
                    entry.second.clear();
                    for (const Value &b : kv.second.items) {
                        if (entry.second.size() == 2) break;
                        entry.second.push_back(b.s);
                    }
                }
    return m;
}

// full action -> gamepad button table: defaults overlaid with the game's overrides; an empty override means unbound
Value merged_pad(const Value *pad) {
    Value m = Value::object();
    for (auto &kv : PAD_DEFAULTS) m.set(kv.first, text(kv.second));
    if (pad && pad->is_object())
        for (auto &kv : pad->fields)
            if (m.get(kv.first)) m.set(kv.first, kv.second.truthy() ? kv.second : Value::null());
    return m;
}

std::string pad_map(const Value *pad) {
    Value m = merged_pad(pad);
    bool defaults = true;
    for (auto &kv : PAD_DEFAULTS) defaults &= m.get(kv.first)->equals_text(kv.second);
    if (defaults) return "";
    std::string out;
    for (size_t i = 0; i < m.fields.size(); ++i) {
        const Value &b = m.fields[i].second;
        out += (i ? "," : "") + m.fields[i].first + "=" + (b.kind == Value::String ? b.s : std::string());
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> parse_map(const std::string &spec) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t start = 0;
    for (;;) {
        size_t comma = spec.find(',', start);
        std::string entry = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        size_t eq = entry.find('=');
        if (eq != std::string::npos) out.push_back({trim(entry.substr(0, eq)), trim(entry.substr(eq + 1))});
        if (comma == std::string::npos) return out;
        start = comma + 1;
    }
}

// parses a decimal number; leading/trailing whitespace is ignored and "nan"/"inf" are accepted
bool to_number(const std::string &s, double &v) {
    std::string t = trim(s);
    if (t.empty()) return false;
    char *end = nullptr;
    v = std::strtod(t.c_str(), &end);
    if (end != t.c_str() + t.size()) return false;
    if (t.find_first_of("xXpP") != std::string::npos) return false;   // reject hex floats, which strtod would take
    return true;
}

std::string g_format(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", v);
    std::string s = buf;
    if (s == "-nan" || s == "nan" || s == "-nan(ind)" || s == "nan(ind)") return "nan";
    return s;
}

// snaps a mouse factor to the matching option string if it equals one; the in-game menu may store values that are
// not in the list, and those are returned in %g form
std::string option_value(const Value *v, const Pairs &options) {
    double value;
    if (!v || v->kind != Value::String || !to_number(v->s, value)) return "1.0";
    for (auto &o : options)
        if (std::fabs(std::strtod(o.first, nullptr) - value) < 0.005) return o.first;
    return g_format(value);
}

Value to_ini(const Value &settings) {
    std::string keys, mouse;
    for (auto &entry : merged(settings.get("controls"))) {
        std::vector<std::string> k, m;
        for (auto &b : entry.second) {
            if (b.rfind("key:", 0) == 0) k.push_back(b.substr(4));
            else if (b.rfind("mouse:", 0) == 0) m.push_back(b.substr(6));
        }
        if (k.empty()) keys += (keys.empty() ? "" : ",") + entry.first + "=";
        for (auto &n : k) keys += (keys.empty() ? "" : ",") + entry.first + "=" + n;
        if (!is_stick(entry.first)) {
            if (m.empty()) mouse += (mouse.empty() ? "" : ",") + entry.first + "=";
            for (auto &n : m) mouse += (mouse.empty() ? "" : ",") + entry.first + "=" + n;
        }
    }
    auto flag = [&](const char *key, bool def) {
        const Value *v = settings.get(key);
        return text((v ? v->truthy() : def) ? "1" : "0");
    };
    auto plain = [&](const char *key, const char *def) {
        const Value *v = settings.get(key);
        return v ? *v : text(def);
    };
    Value out = Value::object();
    out.set("key_map", Value::text(keys));
    out.set("mouse_map", Value::text(mouse));
    out.set("pad_map", Value::text(pad_map(settings.get("pad_controls"))));
    out.set("mouse", flag("mouse", true));
    out.set("mouse_sens", plain("mouse_sens", "1.0"));
    out.set("mouse_invert", flag("mouse_invert", false));
    out.set("prompts", plain("prompts", "auto"));
    out.set("mouse_aim_sens", plain("mouse_aim_sens", "1.0"));
    out.set("mouse_vertical", plain("mouse_vertical", "1.0"));
    out.set("mouse_invert_x", flag("mouse_invert_x", false));
    out.set("vibration", flag("vibration", true));
    return out;
}

// copies the control keys found in a game's settings file into its launcher settings; returns false if the file
// contains none of the map keys yet
bool from_ini(const Value &values, Value &settings) {
    if (!values.get("key_map") && !values.get("mouse_map") && !values.get("pad_map")) return false;
    std::vector<std::pair<std::string, std::vector<std::string>>> controls;
    for (auto &d : DEFAULT_CONTROLS) controls.push_back({d.first, std::vector<std::string>(d.second.begin(), d.second.end())});
    // apply each map over the defaults the way the runtime does: the first entry for an action drops that action's
    // existing bindings of the same kind, later entries add to it
    for (auto &kind : {std::make_pair("key_map", "key:"), std::make_pair("mouse_map", "mouse:")}) {
        const Value *spec = values.get(kind.first);
        std::set<std::string> replaced;
        for (auto &e : parse_map(spec && spec->kind == Value::String ? spec->s : std::string())) {
            for (auto &c : controls) {
                if (c.first != e.first) continue;
                if (!replaced.count(e.first)) {
                    replaced.insert(e.first);
                    std::vector<std::string> kept;
                    for (auto &b : c.second) if (b.rfind(kind.second, 0) != 0) kept.push_back(b);
                    c.second = kept;
                }
                if (!e.second.empty()) c.second.push_back(kind.second + e.second);
            }
        }
    }
    Value c = Value::object();
    for (auto &entry : controls) {
        std::vector<std::string> first(entry.second.begin(), entry.second.begin() + std::min<size_t>(2, entry.second.size()));
        c.set(entry.first, string_list(first));
    }
    settings.set("controls", c);
    Value pad = Value::object();
    for (auto &kv : PAD_DEFAULTS) pad.set(kv.first, text(kv.second));
    std::set<std::string> replaced;
    const Value *ps = values.get("pad_map");
    for (auto &e : parse_map(ps && ps->kind == Value::String ? ps->s : std::string()))
        if (pad.get(e.first) && !replaced.count(e.first)) {
            replaced.insert(e.first);
            pad.set(e.first, e.second.empty() ? Value::null() : Value::text(e.second));
        }
    Value changed = Value::object();
    for (auto &kv : pad.fields) {
        const char *def = nullptr;
        for (auto &d : PAD_DEFAULTS) if (kv.first == d.first) def = d.second;
        if (!(def && kv.second.equals_text(def))) changed.set(kv.first, kv.second);
    }
    settings.set("pad_controls", changed);
    auto str = [&](const char *key) -> const Value * { return values.get(key); };
    if (const Value *v = str("mouse")) settings.set("mouse", Value::boolean(!v->equals_text("0")));
    if (const Value *v = str("mouse_sens")) settings.set("mouse_sens", Value::text(option_value(v, MOUSE_SENSITIVITIES)));
    if (const Value *v = str("mouse_invert")) settings.set("mouse_invert", Value::boolean(v->equals_text("1")));
    if (const Value *v = str("mouse_aim_sens")) settings.set("mouse_aim_sens", Value::text(option_value(v, MOUSE_AIM_FACTORS)));
    if (const Value *v = str("mouse_vertical")) settings.set("mouse_vertical", Value::text(option_value(v, MOUSE_FACTORS)));
    if (const Value *v = str("mouse_invert_x")) settings.set("mouse_invert_x", Value::boolean(v->equals_text("1")));
    if (const Value *v = str("vibration")) settings.set("vibration", Value::boolean(v->equals_text("1")));
    if (const Value *v = str("prompts")) {
        bool known = false;
        for (auto &p : PROMPT_MODES) known |= v->equals_text(p.first);
        settings.set("prompts", known ? *v : text("auto"));
    }
    return true;
}

int give(const std::string &s, char *out, int cap) {
    if ((int)s.size() + 1 > cap) return -(int)(s.size() + 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

}  // namespace

extern "C" {

// Returns all Controls data in a single JSON object with these members: CONTROL_GROUPS, PAD_DEFAULTS, PAD_FIXED,
// SDL_BUTTONS, PAD_NAMES, STICK_ACTIONS, DEFAULT_CONTROLS, PROMPT_MODES, MOUSE_SENSITIVITIES, MOUSE_FACTORS,
// MOUSE_AIM_FACTORS, MOUSE_LABELS, TK_TO_SDL, RESERVED_KEYS.
UN_API int un_controls_tables(char *out, int cap) { return give(json::dump(all_tables()), out, cap); }

// Operations selected by `what`:
//   "to_ini"     in = one game's launcher settings          -> out = the control keys for its settings file
//   "from_ini"   in = {"values": file keys, "settings": ...} -> out = {"found": bool, "settings": updated settings}
//   "merged"     in = controls                               -> out = action -> binding list
//   "merged_pad" in = pad controls                           -> out = action -> button (or null)
// plus "set_binding", "set_pad_binding" and "label", described where they are handled.
UN_API int un_controls(const char *what, const char *in_json, char *out, int cap) {
    Value in;
    if (!json::parse(in_json ? in_json : "null", in)) return give("null", out, cap) < 0 ? -5 : 2;
    std::string w = what ? what : "";
    Value result;
    if (w == "to_ini") {
        result = to_ini(in.is_object() ? in : Value::object());
    } else if (w == "from_ini") {
        Value values = in.get("values") ? *in.get("values") : Value::object();
        Value settings = in.get("settings") ? *in.get("settings") : Value::object();
        bool found = from_ini(values, settings);
        result = Value::object();
        result.set("found", Value::boolean(found));
        result.set("settings", settings);
    } else if (w == "merged") {
        result = Value::object();
        for (auto &e : merged(&in)) result.set(e.first, string_list(e.second));
    } else if (w == "merged_pad") {
        result = merged_pad(&in);
    } else if (w == "set_binding") {
        // Input {"controls", "action", "slot", "binding"} (binding null = clear); output is the controls table with
        // that slot filled. Bindings are unique: assigning one removes it from whichever action/slot used it before.
        // No action ends up with more than two.
        const Value *c = in.get("controls"), *a = in.get("action"), *sl = in.get("slot"), *bv = in.get("binding");
        const std::string action = a && a->kind == Value::String ? a->s : "";
        const size_t slot = sl && sl->kind == Value::Int && sl->i >= 0 ? (size_t)sl->i : 0;
        const std::string binding = bv && bv->kind == Value::String ? bv->s : "";
        auto m = merged(c);
        bool known = false;
        for (auto &e : m) known |= e.first == action;
        if (!known) return 1;
        if (!binding.empty())
            for (auto &e : m) {
                auto it = std::find(e.second.begin(), e.second.end(), binding);
                if (it != e.second.end() && !(e.first == action && (size_t)(it - e.second.begin()) == slot)) e.second.erase(it);
            }
        result = Value::object();
        for (auto &e : m) {
            std::vector<std::string> list = e.second;
            if (e.first == action) {
                while (list.size() <= slot) list.push_back("");
                list[slot] = binding;
                std::vector<std::string> kept;
                for (auto &b : list) if (!b.empty() && kept.size() < 2) kept.push_back(b);
                list = kept;
            }
            result.set(e.first, string_list(list));
        }
    } else if (w == "set_pad_binding") {
        // Input {"pad_controls", "action", "button"} (button null = clear); output is pad_controls reduced to the
        // entries that differ from PAD_DEFAULTS. Stick actions move as a group of four directions; picking the stick
        // the other group uses swaps the two. For buttons, the action that previously owned the button receives this
        // action's former button, so the two trade places.
        const Value *p = in.get("pad_controls"), *a = in.get("action"), *bv = in.get("button");
        Value pad = p && p->is_object() ? *p : Value::object();
        std::string action = a && a->kind == Value::String ? a->s : "";
        std::string button = bv && bv->kind == Value::String ? bv->s : "";
        const Value m = merged_pad(&pad);
        if (!m.get(action)) return 1;
        auto at = [&](const std::string &key) { const Value *v = m.get(key); return v ? *v : Value::null(); };
        if (action.compare(0, 3, "ls_") == 0 || action.compare(0, 3, "rs_") == 0) {
            const std::string group = action.substr(0, 3), other = group == "ls_" ? "rs_" : "ls_";
            const char *dirs[] = {"up", "down", "left", "right"};
            const Value other_up = at(other + "up");
            if (!button.empty() && other_up.kind == Value::String && other_up.s == button) {
                const Value own = at(group + "up");
                for (const char *d : dirs) pad.set(other + d, own.truthy() ? own : Value::text(""));
            }
            for (const char *d : dirs) pad.set(group + d, Value::text(button));
            action.clear();
            button.clear();
        }
        if (!action.empty() && !button.empty())
            for (auto &kv : m.fields)
                if (kv.second.kind == Value::String && kv.second.s == button && kv.first != action) pad.set(kv.first, at(action));
        if (!action.empty()) pad.set(action, Value::text(button));
        result = Value::object();
        const Value after = merged_pad(&pad);
        for (auto &kv : after.fields) {
            std::string def;
            bool has_default = false;
            for (auto &d : PAD_DEFAULTS) if (d.first == kv.first) { def = d.second; has_default = true; }
            const bool same = has_default && kv.second.kind == Value::String && kv.second.s == def;
            if (!same) result.set(kv.first, kv.second);
        }
    } else if (w == "label") {
        // display text for one binding: key name for keys, MOUSE_LABELS text for mouse buttons, an em dash if empty
        std::string b = in.kind == Value::String ? in.s : std::string(), label = "\xE2\x80\x94";
        if (!b.empty()) {
            size_t colon = b.find(':');
            std::string kind = b.substr(0, colon), name = colon == std::string::npos ? std::string() : b.substr(colon + 1);
            label = name;
            if (kind == "mouse")
                for (auto &m : MOUSE_LABELS) if (name == m.first) label = m.second;
        }
        result = Value::text(label);
    } else {
        return 1;
    }
    return give(json::dump(result), out, cap);
}

}  // extern "C"
