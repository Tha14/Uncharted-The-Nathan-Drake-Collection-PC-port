// config.cpp - the launcher's settings core, part of uncharted_native.dll: the per-game runtime settings files
// (analysis/uncharted-user/probe-<game>.ini, shared with the in-game menu), their defaults, the frame-rate limits and the
// graphics presets. The launcher's pages show and change these through the functions below.
//
// The settings file is plain "key=value" lines. Writing changes the listed keys where they are and appends new ones;
// every other line (comments, keys the in-game menu writes) stays as it was. Lines end in CR LF.

#include "native.h"

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
        // the performance counter: items, corner, layout, text size and background opacity, as the runtime starts
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
        // IGPU card adapted from PR #223 by memeguyoffical (the card, its values and the iGPU detection)
        Pairs igpu = common("60", "1", "75");
        igpu.push_back({"upscaler", "off"});
        p.push_back({"igpu", "IGPU", "30\xE2\x80\x93" "60 FPS  \xE2\x80\xA2  540\xE2\x80\x93" "720p, FSR 1",
                     "Intel and AMD integrated graphics: the game draws at 540p (720p on the strongest iGPUs, 360p on "
                     "the weakest) and FSR 1 upscales it to 1080p. About 30 FPS on Vega and 2-CU Radeon, 60 on Radeon "
                     "680M/760M/780M and Iris Xe. No anti-aliasing; the lowest memory use.",
                     "1920x1080", "performance", igpu});
        p.push_back({"low", "LOW", "60 FPS  \xE2\x80\xA2  1080p, FSR 1",
                     "For older graphics cards: the game draws at half of 1080p and FSR 1 upscales it; the HUD stays "
                     "sharp.",
                     "1920x1080", "performance", common("60", "1", "75")});
        p.push_back({"medium", "MEDIUM", "60 FPS  \xE2\x80\xA2  as on PS4",
                     "Like the PS4: 1080p at 60 FPS. Right for most PCs.", "1920x1080", "off", common("60", "0", "75")});
        p.push_back({"high", "HIGH", "60 FPS  \xE2\x80\xA2  1440p", "Sharper: the game draws at 1440p.", "2560x1440",
                     "off", common("60", "0", "50")});
        p.push_back({"ultra", "ULTRA", "4K  \xE2\x80\xA2  up to 120 FPS",
                     "For fast graphics cards: native 4K and up to 120 FPS (experimental). Drake's Fortune draws at "
                     "1620p and scales up.",
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

// lines as Python's splitlines() gives them for these files (CR LF, LF or CR), without the endings
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

// the game's file, else the shared file of earlier versions as its starting point, else a header line
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

// a listed limit; unlimited (0) and faster ones (earlier versions offered 144-240) become the top one
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

}  // namespace

extern "C" {

// name: "ini_defaults" (key\tvalue), "fps_limits" (value\tlabel), "presets" (name\ttitle\tsummary\tdetail\t
// resolution\tupscale\tkey=value;key=value...)
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

// the game's runtime settings: the defaults, then the file's keys (key\tvalue lines, defaults first, then the file's
// own keys in file order); the frame-rate limit normalized
UN_API int un_ini_read(const wchar_t *user_dir, const char *game, char *out, int cap) {
    Pairs values = ini_defaults();
    for (const std::string &line : settings_lines(user_dir, game)) {
        if (!is_setting(line)) continue;
        std::string key = key_of(line), value = trim(line.substr(line.find('=') + 1));
        bool found = false;
        for (auto &kv : values)
            if (kv.first == key) { kv.second = value; found = true; }
        if (!found) values.push_back({key, value});
    }
    std::string text;
    for (auto &kv : values) text += kv.first + "\t" + (kv.first == "fps_limit" ? fps_limit(kv.second) : kv.second) + "\n";
    return give(text, out, cap);
}

// updates: key\tvalue lines. Changes those keys in place (every line with the key) and appends the missing ones;
// returns 0, or 1 when the file can't be written
UN_API int un_ini_write(const wchar_t *user_dir, const char *game, const char *updates) {
    Pairs changes;
    std::string u = updates ? updates : "";
    for (const std::string &line : lines_of(u)) {
        size_t tab = line.find('\t');
        if (tab != std::string::npos) changes.push_back({line.substr(0, tab), line.substr(tab + 1)});
    }
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
            bool earlier = false;   // a key listed twice is appended once (the last value), as a dict would
            for (size_t j = i + 1; j < changes.size(); ++j) earlier |= changes[j].first == changes[i].first;
            if (!earlier) lines.push_back(changes[i].first + "=" + changes[i].second);
        }
    std::string text;
    for (const std::string &line : lines) text += line + "\r\n";
    std::wstring dir = user_dir;
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE h = CreateFileW(settings_file(dir, game).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 1;
    DWORD put = 0;
    BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr) && put == text.size();
    ok = CloseHandle(h) && ok;
    return ok ? 0 : 1;
}

}  // extern "C"
