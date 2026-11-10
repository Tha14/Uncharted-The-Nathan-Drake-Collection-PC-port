// controls.cpp - keyboard, mouse and controller bindings for the launcher's Controls page, part of uncharted_native.dll.
// The tables (actions, defaults, controller buttons, sensitivity steps, key names), and the conversion between the
// launcher's per-game settings and the game's settings file, where the runtime (runtime_pad.c) reads them as
// key_map / mouse_map / pad_map and the in-game menu changes them too.
//
// Bindings: an action has up to two, "key:<SDL key name>" or "mouse:<left|middle|right|x1|x2|wheel_up|wheel_down>";
// stick directions take keys only. Map syntax: "action=name,..." ("action=" binds nothing).

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
    Pairs actions;   // action, label
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

// the controller button of each action, as SDL names it (runtime_pad.c's default controller map)
const Pairs PAD_DEFAULTS = {{"cross", "a"},         {"circle", "b"},        {"square", "x"},    {"triangle", "y"},
                            {"l1", "leftshoulder"}, {"r1", "rightshoulder"}, {"l3", "leftstick"}, {"r3", "rightstick"},
                            {"options", "start"},   {"touchpad_right", "back"}, {"up", "dpup"}, {"down", "dpdown"},
                            {"left", "dpleft"},     {"right", "dpright"}};
// the sticks, triggers and the touchpad's left click (Back/View with LB held) can't be rebound
const Pairs PAD_FIXED = {{"l2", "lefttrigger"}, {"r2", "righttrigger"}, {"touchpad_left", "leftshoulder+back"},
                         {"ls_up", "lstick"},   {"ls_down", "lstick"},  {"ls_left", "lstick"}, {"ls_right", "lstick"},
                         {"rs_up", "rstick"},   {"rs_down", "rstick"},  {"rs_left", "rstick"}, {"rs_right", "rstick"}};
// SDL_GamepadButton order (SDL3)
const char *const SDL_BUTTONS[] = {"a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
                                   "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2",
                                   "paddle3", "paddle4", "touchpad"};
// (Xbox, PlayStation): the slot's text where there is no icon, and hints
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
// button icons in the games' text and menus: the device used last, or fixed
const Pairs PROMPT_MODES = {{"auto", "Auto"}, {"keyboard", "Keyboard and mouse"}, {"playstation", "PlayStation"}, {"xbox", "Xbox"}};
const Pairs MOUSE_SENSITIVITIES = {{"0.5", "0.5x"}, {"0.75", "0.75x"}, {"1.0", "1x"}, {"1.5", "1.5x"},
                                   {"2.0", "2x"},   {"3.0", "3x"},     {"4.0", "4x"}, {"5.0", "5x"}};
// while aiming (times the sensitivity) and up/down compared with left/right
const Pairs MOUSE_FACTORS = {{"0.5", "0.5x"}, {"0.75", "0.75x"}, {"1.0", "1x"}, {"1.25", "1.25x"}, {"1.5", "1.5x"}, {"2.0", "2x"}};
const Pairs MOUSE_AIM_FACTORS = {{"0.5", "0.5x"}, {"1.0", "1x"}, {"2.0", "2x"}, {"3.0", "3x"}, {"5.0", "5x"}, {"10.0", "10x"}};
const Pairs MOUSE_LABELS = {{"left", "Left mouse"},    {"middle", "Middle mouse"}, {"right", "Right mouse"},
                            {"x1", "Mouse back"},      {"x2", "Mouse forward"},    {"wheel_up", "Wheel up"},
                            {"wheel_down", "Wheel down"}};
// Tk key names -> SDL key names (letters and digits map to themselves, upper case)
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
// keys the runtime keeps for itself: Insert opens the in-game menu, F9 records the pad
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

// action -> its bindings: the defaults, with a game's own (up to two each) for the known actions
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

// the controller button of each action: the defaults with a game's own (an empty one = none)
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

// Python's float(): surrounding whitespace allowed; nan/inf words allowed
bool to_number(const std::string &s, double &v) {
    std::string t = trim(s);
    if (t.empty()) return false;
    char *end = nullptr;
    v = std::strtod(t.c_str(), &end);
    if (end != t.c_str() + t.size()) return false;
    if (t.find_first_of("xXpP") != std::string::npos) return false;   // strtod reads hex; Python does not
    return true;
}

std::string g_format(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", v);
    std::string s = buf;
    if (s == "-nan" || s == "nan" || s == "-nan(ind)" || s == "nan(ind)") return "nan";
    return s;
}

// a mouse value as one of the options when it is one (the in-game menu sets any)
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

// a game's controls from its settings file into the launcher's settings; false when the file has none yet
bool from_ini(const Value &values, Value &settings) {
    if (!values.get("key_map") && !values.get("mouse_map") && !values.get("pad_map")) return false;
    std::vector<std::pair<std::string, std::vector<std::string>>> controls;
    for (auto &d : DEFAULT_CONTROLS) controls.push_back({d.first, std::vector<std::string>(d.second.begin(), d.second.end())});
    // a map on top, as the runtime applies it: an action's first entry replaces its sources of this kind
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

// every Controls table as one JSON object (CONTROL_GROUPS, PAD_DEFAULTS, PAD_FIXED, SDL_BUTTONS, PAD_NAMES,
// STICK_ACTIONS, DEFAULT_CONTROLS, PROMPT_MODES, MOUSE_SENSITIVITIES, MOUSE_FACTORS, MOUSE_AIM_FACTORS, MOUSE_LABELS,
// TK_TO_SDL, RESERVED_KEYS)
UN_API int un_controls_tables(char *out, int cap) { return give(json::dump(all_tables()), out, cap); }

// what: "to_ini" (in: a game's launcher settings; out: the settings file's control keys), "from_ini" (in: {"values":
// the settings file, "settings": the game's launcher settings}; out: {"found": bool, "settings": updated}),
// "merged" (in: controls; out: action -> bindings), "merged_pad" (in: pad controls; out: action -> button or null)
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
    } else if (w == "label") {
        // a binding as the Controls page shows it: the key's name, the mouse button's label, or a dash
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
