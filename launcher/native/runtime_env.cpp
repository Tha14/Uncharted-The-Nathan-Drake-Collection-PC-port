// runtime_env.cpp - builds the runtime's option set; lives in uncharted_native.dll. The runtime (Uncharted.exe) is
// configured through GOW3_* environment variables. This file derives them from three inputs: the UNCHARTED_* variables
// the launcher exports, which game is starting, and the render size. Where a comment says so, a value that is already
// present in the environment is left alone ("default" = only filled in when missing); that is what lets test-env.txt
// and hand-set GOW3_* variables take effect. Each value carries a note with the issue or measurement that led to it.
// (This replaces the environment set-up that tools/probe_uncharted.py used to do.)

#include "native.h"
#include "json.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {

using json::Value;

int give(const std::string &s, char *out, int cap) {
    if ((int)s.size() + 1 > cap) return -(int)(s.size() + 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

std::wstring widen(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string strip(const std::string &s) {
    const char *ws = " \t\r\n\v\f";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string upper(std::string s) {
    for (char &c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

bool digits_only(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit((unsigned char)c)) return false;
    return true;
}

// Normalises a Windows path the way Python's str(Path(p)) does: forward slashes turn into backslashes, runs of
// separators collapse to one, and a trailing separator is dropped unless the path is a bare drive root.
std::string norm(std::string p) {
    std::replace(p.begin(), p.end(), '/', '\\');
    std::string out;
    bool unc = p.rfind("\\\\", 0) == 0;
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '\\' && !out.empty() && out.back() == '\\' && !(unc && i == 1)) continue;
        out += p[i];
    }
    while (out.size() > 1 && out.back() == '\\' && !(out.size() == 3 && out[1] == ':')) out.pop_back();
    if (out == ".") return out;
    return out;
}

std::string join(const std::string &a, const std::string &b) { return norm(a + "\\" + b); }

bool exists(const std::string &p) { return GetFileAttributesW(widen(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
bool is_dir(const std::string &p) {
    DWORD a = GetFileAttributesW(widen(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool read_file(const std::string &path, std::string &data) {
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    data.clear();
    char buf[1 << 16];
    DWORD got = 0;
    bool ok = true;
    while ((ok = ReadFile(h, buf, sizeof buf, &got, nullptr) != 0) && got) data.append(buf, got);
    CloseHandle(h);
    return ok;
}

bool write_file(const std::string &path, const std::string &data) {
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size());
    CloseHandle(h);
    return ok;
}

bool make_dirs(const std::string &p) {
    if (is_dir(p)) return true;
    size_t cut = p.find_last_of('\\');
    if (cut != std::string::npos && cut > 2) make_dirs(p.substr(0, cut));
    return CreateDirectoryW(widen(p).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// Returns the text as valid UTF-8, swapping every byte that cannot start a valid sequence for U+FFFD (matching how
// Python's errors="replace" handles single bad bytes).
std::string utf8_replace(const std::string &t) {
    std::string out;
    for (size_t i = 0; i < t.size();) {
        unsigned char c = (unsigned char)t[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = n > 0 && i + n <= t.size();
        for (int k = 1; ok && k < n; ++k) ok = ((unsigned char)t[i + k] >> 6) == 2;
        if (ok && n > 1 && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, t.data() + i, n, nullptr, 0) == 0) ok = false;
        if (ok) { out.append(t, i, n); i += n; }
        else { out += "\xEF\xBF\xBD"; ++i; }
    }
    return out;
}

// Newline normalisation like Python's universal-newlines reading: every \r\n pair and every lone \r turns into \n.
std::string universal(const std::string &t) {
    std::string out;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\r') { out += '\n'; if (i + 1 < t.size() && t[i + 1] == '\n') ++i; }
        else out += t[i];
    }
    return out;
}

std::vector<std::string> lines_of(const std::string &t) {   // splits on \n; run universal() on the text first
    std::vector<std::string> out;
    size_t at = 0;
    while (at < t.size()) {
        size_t end = t.find('\n', at);
        if (end == std::string::npos) end = t.size();
        out.push_back(t.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

// Looks up `key` in a key=value settings file such as probe-<game>.ini. `found` stays false if the key is absent.
std::string ini_value(const std::string &path, const std::string &key, bool &found) {
    found = false;
    std::string data;
    if (!read_file(path, data)) return "";
    for (const std::string &line : lines_of(universal(utf8_replace(data)))) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (strip(line.substr(0, eq)) == key) { found = true; return strip(line.substr(eq + 1)); }
    }
    return "";
}

// ------------------------------------------------------------------------------------------------ tables

// Cheats page of the launcher. Each entry is a built-in switch of the game that the runtime holds on for the whole
// session through GOW3_POKE. The offsets refer to the update 1.02 image.
const std::map<std::string, std::vector<std::pair<std::string, std::string>>> &cheats() {
    static const std::map<std::string, std::vector<std::pair<std::string, std::string>>> t = {
        {"eboot.bin", {{"invincible", "aa5ff8"}, {"ammo", "ab9c01"}}},
        {"big2-ps4_Shipping.elf", {{"invincible", "104f877"}, {"ammo", "104f84a"}, {"noreload", "104f84b"}}},
        {"big3-ps4_Shipping.elf", {{"invincible", "*16e2f80+a7"}, {"ammo", "17faa91"}, {"grenades", "17faa92"},
                                   {"noreload", "17faa93"}, {"onehit", "17fab94"}}}};
    return t;
}
// UNCHARTED_UNLOCK_REWARDS=1: the flags behind the debug-menu entries "Give all Store Items" and "Give All Movies",
// held on the same way as the cheats. As long as they stay set, all rewards and story cutscenes appear unlocked; the
// save file is never touched.
const std::map<std::string, std::vector<std::string>> &reward_pokes() {
    static const std::map<std::string, std::vector<std::string>> t = {
        {"big2-ps4_Shipping.elf", {"1114178", "1114179"}}, {"big3-ps4_Shipping.elf", {"17b2a94", "17b2a95"}}};
    return t;
}
// The Advanced page's extra-quality options: render flags from the debug menu, held like the cheats. The
// high-resolution local shadow buffer cost about 0% and full-resolution textures about 1% FPS (Uncharted 2, 1440p).
// The depth-of-field and motion-blur entries (GitHub #169) work the other way round: they pin a flag to 0. No
// addresses are known for Uncharted 1.
const std::map<std::string, std::vector<std::pair<std::string, std::vector<std::string>>>> &quality_pokes() {
    static const std::map<std::string, std::vector<std::pair<std::string, std::vector<std::string>>>> t = {
        {"big2-ps4_Shipping.elf", {{"hires_shadows", {"1247d04"}}, {"full_res_textures", {"1248300"}},
                                   {"no_dof", {"1247cfd=0"}}, {"no_motion_blur", {"*104ad90+1b9=0", "*104ad90+1ba=0"}}}},
        {"big3-ps4_Shipping.elf", {{"hires_shadows", {"18c6444"}}, {"full_res_textures", {"18c6a40"}},
                                   {"no_dof", {"18c643d=0"}}, {"no_motion_blur", {"*16e3078+1b9=0", "*16e3078+1ba=0"}}}}};
    return t;
}

// ------------------------------------------------------------------------------------------------ environment

struct Env {
    std::vector<std::pair<std::string, std::string>> items;   // keeps the order entries were first added
    const std::string *find(const std::string &k) const {
        for (auto &kv : items) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    bool has(const std::string &k) const { return find(k) != nullptr; }
    std::string get(const std::string &k, const std::string &fallback = "") const {
        const std::string *v = find(k);
        return v ? *v : fallback;
    }
    void set(const std::string &k, const std::string &v) {
        for (auto &kv : items) if (kv.first == k) { kv.second = v; return; }
        items.push_back({k, v});
    }
    void setdefault(const std::string &k, const std::string &v) { if (!has(k)) items.push_back({k, v}); }
};

struct Failure { std::string message; };

uint64_t time_ns() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ULL) * 100;
}

// Writes one line straight to the starter's stdout with no buffering, since the cleanup thread below calls it too.
void say(const std::string &line) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    std::string text = line + "\n";
    DWORD put = 0;
    WriteFile(out, text.data(), (DWORD)text.size(), &put, nullptr);
}

// Since 24 Oct 2026 pipeline binaries are not used any more, but older versions left them in
// <cache>\<game>*\pipeline-binaries (around 2 MB apiece; one tester had 20 GB of them). This thread clears them out in
// the background while the game is running. If the session ends before it is done, the next start picks up the rest.
DWORD WINAPI remove_binaries(void *arg) {
    std::vector<std::wstring> *folders = (std::vector<std::wstring> *)arg;
    long long files = 0;
    unsigned long long size = 0;
    for (const std::wstring &folder : *folders) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                std::wstring p = folder + L"\\" + fd.cFileName;
                if (DeleteFileW(p.c_str())) {
                    size += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                    ++files;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(folder.c_str());
    }
    char line[160];
    std::snprintf(line, sizeof line, "Pipeline binaries: cleared %lld leftover files (%.1f GB) that are no longer needed",
                  files, size / 1073741824.0);
    say(line);
    delete folders;
    return 0;
}

void remove_pipeline_binaries(const std::string &cache) {
    auto *folders = new std::vector<std::wstring>;
    WIN32_FIND_DATAW fd;
    std::wstring base = widen(cache);
    HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            std::wstring p = base + L"\\" + fd.cFileName + L"\\pipeline-binaries";
            DWORD a = GetFileAttributesW(p.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) folders->push_back(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (folders->empty()) { delete folders; return; }
    HANDLE t = CreateThread(nullptr, 0, remove_binaries, folders, 0, nullptr);
    if (t) CloseHandle(t);
    else delete folders;
}

struct Inputs {
    Env original;          // environment the starter process itself was given
    std::string executable, game_key, game, user, out, root, loadexec;
    int render_w = 1920, render_h = 1080, width = 1920, height = 1080;
    long long extra_mib = 0;
    bool capture = false;
};

std::string orig(const Inputs &in, const std::string &k, const std::string &fallback = "") {
    return in.original.get(upper(k), fallback);
}

// Game version as system.cpp's starter_version reports it; empty if it can't be determined.
std::string game_version(const std::string &game) {
    char buf[256];
    return un_system("starter_version", widen(game).c_str(), buf, sizeof buf) == 0 ? buf : "";
}

// Produces the full environment for the runtime process. Lines meant for the starter's log are appended to `prints`.
Env build(const Inputs &in, std::vector<std::string> &prints, std::string &capture_dir, std::string &trigger) {
    Env env = in.original;
    const std::string &exe = in.executable, KEY = upper(in.game_key);
    const bool u1 = exe == "eboot.bin", u2 = exe == "big2-ps4_Shipping.elf", u3 = exe == "big3-ps4_Shipping.elf";
    // Cheats, reward unlocks and extra-quality flags all become GOW3_POKE entries. Their addresses only exist for
    // update 1.02, so older game versions get none of them.
    std::vector<std::string> wanted;
    {
        std::string list = orig(in, "UNCHARTED_CHEATS"), t;
        list.erase(std::remove(list.begin(), list.end(), ' '), list.end());
        size_t start = 0;
        for (;;) {
            size_t cut = list.find(',', start);
            t = list.substr(start, cut == std::string::npos ? std::string::npos : cut - start);
            if (!t.empty()) wanted.push_back(t);
            if (cut == std::string::npos) break;
            start = cut + 1;
        }
    }
    const bool rewards = orig(in, "UNCHARTED_UNLOCK_REWARDS", "0") == "1";
    std::vector<std::string> quality;
    auto q = quality_pokes().find(exe);
    for (const char *name : {"hires_shadows", "full_res_textures", "no_dof", "no_motion_blur"}) {
        if (orig(in, std::string("UNCHARTED_") + upper(name), "0") != "1" || q == quality_pokes().end()) continue;
        for (auto &kv : q->second) if (kv.first == name) { quality.push_back(name); break; }
    }
    if ((!wanted.empty() || rewards || !quality.empty()) && !env.has("GOW3_POKE")) {
        if (game_version(in.game) >= "01.02") {
            std::vector<std::string> pokes, named;
            auto c = cheats().find(exe);
            for (const std::string &w : wanted)
                if (c != cheats().end())
                    for (auto &kv : c->second) if (kv.first == w) { pokes.push_back(kv.second); named.push_back(w); break; }
            if (!wanted.empty()) {
                std::string line = "Cheats in use: ";
                for (size_t i = 0; i < named.size(); ++i) line += (i ? ", " : "") + named[i];
                prints.push_back(line);
            }
            auto r = reward_pokes().find(exe);
            if (rewards && r != reward_pokes().end() && !r->second.empty()) {
                pokes.insert(pokes.end(), r->second.begin(), r->second.end());
                prints.push_back("Unlock all rewards: enabled");
            }
            for (const std::string &name : quality)
                for (auto &kv : q->second) if (kv.first == name) pokes.insert(pokes.end(), kv.second.begin(), kv.second.end());
            if (!quality.empty()) {
                std::string line = "Extra quality in use: ";
                for (size_t i = 0; i < quality.size(); ++i) line += (i ? ", " : "") + quality[i];
                prints.push_back(line);
            }
            if (!pokes.empty()) {
                std::string v;
                for (size_t i = 0; i < pokes.size(); ++i) v += (i ? "," : "") + pokes[i];
                env.set("GOW3_POKE", v);
            }
        } else {
            prints.push_back("Cheats and unlocked rewards skipped: they require update 1.02");
        }
    }
    // Advanced -> Texture filtering. With GOW3_ANISOTROPY set, sampler.cpp lifts the anisotropy of mipmapped colour
    // textures to at least the chosen ratio. Measured cost in Uncharted 2: about 0% FPS.
    std::string aniso = orig(in, "UNCHARTED_ANISOTROPY", "0");
    if (aniso == "2" || aniso == "4" || aniso == "8" || aniso == "16") {
        env.setdefault("GOW3_ANISOTROPY", aniso);
        prints.push_back("Texture filtering set to " + aniso + "x");
    }
    // Diagnostics switch: covers the performance logging (frame, fault and wait statistics every 5 s, GPU checkpoints)
    // and the trigger files that can be poked while the game runs (capture, readback, DMA). Enabled by default; only
    // UNCHARTED_DIAGNOSTICS=0 from the launcher disables it.
    const bool diagnostics = env.get("UNCHARTED_DIAGNOSTICS", "1") != "0";
    env.set("PATH", join(norm(env.get("MSYS2_ROOT", "C:/msys64")), "clang64\\bin") + ";" + env.get("PATH", ""));
    // UNCHARTED_TEST_GPU_USER_DIR moves the GPU caches elsewhere, so a test can begin with no shader cache at all.
    std::string gpu_dir = env.get("UNCHARTED_TEST_GPU_USER_DIR");
    env.set("GOW3_GPU_USER_DIR", gpu_dir.empty() ? in.user : gpu_dir);
    std::string settings = join(in.user, "probe-" + in.game_key + ".ini");
    if (!exists(settings) && exists(join(in.user, "probe.ini")))   // one probe.ini used to serve all three games
        CopyFileW(widen(join(in.user, "probe.ini")).c_str(), widen(settings).c_str(), FALSE);
    // UNCHARTED_CONFIG_FILE points the runtime at a different settings file, for tests that must leave the player's
    // own file untouched.
    std::string config_file = orig(in, "UNCHARTED_CONFIG_FILE");
    env.set("GOW3_CONFIG", config_file.empty() ? settings : config_file);
    // An fps_limit of 0 (unlimited) or above 60 only works with the fast vblank, GOW3_VBLANK_HZ=0. A GOW3_VBLANK_HZ
    // that was already set is respected.
    if (!in.original.has("GOW3_VBLANK_HZ")) {
        bool found = false;
        std::string limit = ini_value(env.get("GOW3_CONFIG"), "fps_limit", found);
        if (!found) limit = "60";
        long long n = digits_only(limit) ? std::strtoll(limit.c_str(), nullptr, 10) : -1;   // clamps on overflow
        if (digits_only(limit) && (n == 0 || n > 60))
            env.set("GOW3_VBLANK_HZ", "0");
    }
    // Save profiles from the Advanced page (GitHub #66, added 24 Oct 2026). Choosing profile N signs in as PS4 user N,
    // which keeps its saves under analysis\uncharted-user\savedata\N. Profile 1 is the folder all earlier versions
    // used. A GOW3_USER_ID coming from outside the launcher (for tests) takes priority.
    std::string profile = orig(in, "UNCHARTED_SAVE_PROFILE_" + KEY, "1");
    long long profile_n = digits_only(profile) ? std::strtoll(profile.c_str(), nullptr, 10) : 0;
    bool profile_ok = digits_only(profile) && profile_n >= 1 && profile_n <= 16;
    env.setdefault("GOW3_USER_ID", profile_ok ? profile : "1");
    const std::string *mt_value = in.original.find("UNCHARTED_MULTITHREADED_" + KEY);
    std::string multithreaded = mt_value ? *mt_value : "";
    bool have_mt = mt_value != nullptr;
    // Uncharted 1 is forced to single-threaded rendering. With the threaded path some frames rendered its guns (and
    // sometimes other objects) using the state of a different draw, giving flat grey weapons (GitHub #23,
    // 18 Oct 2026), and the game runs no faster with it anyway. This stays until the bug is found. For testing,
    // UNCHARTED_U1_MULTITHREADED_TEST=1 lets the launcher's setting through unchanged.
    if (u1 && have_mt && multithreaded == "1" && orig(in, "UNCHARTED_U1_MULTITHREADED_TEST") != "1") {
        prints.push_back("Multithreaded rendering disabled for Drake's Fortune: it makes the weapons flicker");
        multithreaded = "0";
    }
    // Uncharted 3 used to hang with threaded rendering (GitHub #51); the runtime fix landed 19 Oct 2026 and from
    // 21 Oct the launcher's setting is honoured again. In a demanding Cartagena scene it made the difference between
    // 53.7 FPS with 11 stalls (off) and 60 FPS with 1 (on). If GOW3_DRAW_PIPE or GOW3_VK_RECORD_THREAD is already set
    // (tests sometimes set just one of them), that value beats the launcher's setting.
    if (have_mt) {
        env.set("GOW3_DRAW_PIPE", in.original.get("GOW3_DRAW_PIPE", multithreaded));
        env.set("GOW3_VK_RECORD_THREAD", in.original.get("GOW3_VK_RECORD_THREAD", multithreaded));
    }
    // Per-game input settings from the Controls tab, forwarded unless the matching GOW3_* variable is already set.
    const char *const controls[][2] = {{"KEY_MAP", "GOW3_KEY_MAP"},       {"MOUSE_MAP", "GOW3_MOUSE_MAP"},
                                       {"MOUSE", "GOW3_MOUSE"},           {"MOUSE_SENS", "GOW3_MOUSE_SENS"},
                                       {"MOUSE_INVERT", "GOW3_MOUSE_INVERT"}, {"PROMPTS", "GOW3_PROMPTS"},
                                       {"PAD_MAP", "GOW3_PAD_MAP"}};
    for (auto &c : controls) {
        const std::string *v = in.original.find(std::string("UNCHARTED_") + c[0] + "_" + KEY);
        if (v && !in.original.has(c[1])) env.set(c[1], *v);
    }
    // Readback mode for Uncharted 2 and 3, set on 16 Oct. Mode 2 (asynchronous with no protection, the runtime's own
    // default) made Uncharted 3 crash inside a minute in the Cartagena museum, and in both games characters flew off
    // or NPCs dropped out of the sky. Mode 0 (block on every readback) was correct but cost Uncharted 2 about 20 FPS.
    // Mode 1 keeps readbacks asynchronous but read-protects fresh GPU data until it has arrived.
    if (u2 || u3) env.setdefault("GOW3_ASYNC_READBACK", "1");
    // Output of the 64-wide copy shader (cs 6a6422a9) is fetched at the end-of-pipe fence of the same frame. When it
    // arrived one frame late, NPCs hovered and Nate's feet went into the floor (Uncharted 2 on 16 Oct, Uncharted 3 on
    // 17 Oct), and in Uncharted 1 enemy grenades went off right in front of Drake (GitHub #6, 19 Oct).
    if (u1 || u2 || u3) env.setdefault("GOW3_COPY64_EOP_READBACK", "1");
    // While aiming, Uncharted 2 and 3 take the depth-of-field focus distance from a 1x1 render target written on the
    // GPU. Very small GPU-written images are therefore fetched at the end-of-pipe fence too (16 Oct for Uncharted 2,
    // 19 Oct for Uncharted 3). Uncharted 1 has no need for it and is left out.
    if (u2 || u3) env.setdefault("GOW3_TINY_IMAGE_READBACK", "1");
    // GitHub #140 (24 Oct): sunny areas in Uncharted 3 looked washed out. The game renders its bloom into mips 1 and 2
    // of a texture with 7 levels; GOW3_MIP_TARGETS=1 makes those draws land in the texture's own mip levels.
    if (u3) env.setdefault("GOW3_MIP_TARGETS", "1");
    // GitHub #161/#253 (2 Nov 2026): water in Uncharted 1 showed as flat dark green. Its reflection uses a depth
    // buffer that is cleared solely through HTILE, so clears on depth images that have been freed must survive.
    // Only enabled for Uncharted 1 so far.
    if (u1) env.setdefault("GOW3_HTILE_KEEP_CLEARS", "1");
    // Console system language, from Advanced -> Language (handled in runtime_services.c). 1 means English (US).
    std::string language = strip(orig(in, "UNCHARTED_LANGUAGE"));
    if (digits_only(language)) env.setdefault("GOW3_LANGUAGE", language);
    // Speaker layout from the Advanced page: 2, 6 or 8 channels, or auto to follow the Windows playback device
    // (stereo, 5.1 or 7.1).
    std::string speakers = lower(strip(orig(in, "UNCHARTED_SPEAKERS")));
    if (speakers == "auto" || speakers == "2" || speakers == "6" || speakers == "8")
        env.setdefault("GOW3_AUDIO_SPEAKERS", speakers);
    const std::string render = std::to_string(in.render_w) + "x" + std::to_string(in.render_h);
    // Temporal anti-aliasing at the render resolution (picked in the in-game Image menu: TAA, FSR 3.1 or FSR 4 Native
    // AA, DLAA). Camera motion is taken from the camera velocity pass used by motion blur (shader f4b18264), and the
    // AA pass is inserted on the lit scene ahead of post processing (compute aec539f0). Setting
    // UNCHARTED_TEMPORAL_AA=0 disables all of this.
    if (orig(in, "UNCHARTED_TEMPORAL_AA", "1") == "0") {
        env.set("GOW3_UPSCALER", "none");
    } else {
        env.setdefault("GOW3_VELOCITY_PS", "f4b18264");
        env.setdefault("GOW3_UPSCALE_BEFORE_CS", "aec539f0");
        // Some full-screen pixel shaders reconstruct positions from the jittered depth, so they must be jittered as
        // well: the sun shadow pass (Supermedo/uncharted_pc #261), boat detail in Uncharted 1 and the London bar
        // surfaces in Uncharted 3.
        env.setdefault("GOW3_JITTER_PS", "f614041e,b2a42acf,1a08df2d");
        env.set("GOW3_SCENE_SIZE", render);
        // Point the runtime at the FSR 4 model files if that folder exists. (DLAA instead relies on gow3_dlss.dll
        // and nvngx_dlss.dll sitting next to Uncharted.exe.)
        std::string fsr4 = join(in.root, "gow3_pc\\fsr4_shaders");
        if (is_dir(fsr4)) env.setdefault("GOW3_FSR4_DIR", fsr4);
        // Left alone, the renderer would choose FSR 3. A settings file without an upscaler entry gets upscaler=off
        // written into it, so nothing is used until the player picks something.
        std::string config = env.get("GOW3_CONFIG");
        bool found = false;
        ini_value(config, "upscaler", found);
        if (!found) {
            std::string data, text;
            bool readable = !exists(config) || read_file(config, data);
            text = universal(utf8_replace(data));
            if (!text.empty() && text.back() != '\n') text += "\n";
            text += "upscaler=off\n";
            std::string crlf;
            for (char c : text) { if (c == '\n') crlf += "\r\n"; else crlf += c; }
            if (!readable || !write_file(config, crlf)) env.setdefault("GOW3_UPSCALER", "off");
        }
    }
    if (in.extra_mib) env.set("GOW3_DMEM_MB", std::to_string(5056 + in.extra_mib + 64));
    // Cutscene videos are 1080p, but the movie player expects frames at the render size. The decoder is told that size
    // and scales to it; without this, videos turn green and tiled at resolutions above 1080p.
    if (in.render_w != 1920 || in.render_h != 1080) env.set("GOW3_MOVIE_SIZE", render);
    // UNCHARTED_OUTPUT_RES (optionally suffixed _U1/_U2/_U3) = WxH, larger than the render size. The upscale happens at
    // the point where the completed frame is copied into the display buffer (pixel shader 749bbec9): first FSR 3.1,
    // FSR 4 or DLSS (or TAA, or no temporal step), then FSR 1 or plain bilinear scaling. The HUD is drawn on top
    // afterwards.
    std::string output = orig(in, "UNCHARTED_OUTPUT_RES_" + KEY);
    if (output.empty()) output = orig(in, "UNCHARTED_OUTPUT_RES");
    output = lower(output);
    if (output.find('x') == std::string::npos && (in.render_w != in.width || in.render_h != in.height)) {
        // The Uncharted 1 case where the chosen size is more than it can render: it renders at the smaller size and
        // the frame is scaled up to the chosen one, using FSR 1 when no temporal upscaler has been selected.
        output = std::to_string(in.width) + "x" + std::to_string(in.height);
        env.setdefault("GOW3_FSR1", "1");
    }
    if (output.find('x') != std::string::npos && orig(in, "UNCHARTED_TEMPORAL_AA", "1") != "0") {
        size_t x = output.find('x');
        std::string a = strip(output.substr(0, x)), b = strip(output.substr(x + 1));
        if (b.find('x') != std::string::npos || !digits_only(a) || !digits_only(b))
            throw Failure{"invalid output resolution: " + output};
        long long out_w = std::strtoll(a.c_str(), nullptr, 10), out_h = std::strtoll(b.c_str(), nullptr, 10);
        if (out_w > in.render_w || out_h > in.render_h) {
            env.set("GOW3_RENDER_RES", render);
            env.set("GOW3_OUTPUT_RES", std::to_string(out_w) + "x" + std::to_string(out_h));
            env.setdefault("GOW3_UI_COMPOSITE_PS", "749bbec9");
            // Last tonemap draw of the frame. Uncharted 3 renders prompts tied to world positions into the same target
            // after that draw, so the upscale is placed at the draw that follows it.
            env.setdefault("GOW3_UI_TONEMAP_PS", "484cdbf5");
            prints.push_back("Output upscaled: " + render + " -> " + std::to_string(out_w) + "x" + std::to_string(out_h));
        }
    }
    std::string pad_file = join(in.user, "probe-pad.txt");
    write_file(pad_file, "");
    env.set("GOW3_PAD_FILE", pad_file);
    // Unless a choice was made earlier, render single-threaded; the upstream runtime would otherwise switch these
    // concurrency features on.
    env.setdefault("GOW3_DRAW_PIPE", "0");
    env.setdefault("GOW3_VK_RECORD_THREAD", "0");
    env.setdefault("GOW3_NATIVE_TSC", "1");
    // Frame capture trigger: put a pass count such as 600 in capture.trigger and a capture is written to
    // analysis/uncharted-user/captures. The capture lists the buffer addresses of every draw and dispatch.
    if (diagnostics) {
        env.setdefault("GOW3_CAPTURE_TRIGGER", join(in.user, "capture.trigger"));
        env.setdefault("GOW3_CAPTURE_ALL_BUFFERS", "1");
    }
    // If a file named precise-readbacks exists in analysis/uncharted-user, the game starts with precise readbacks.
    if (exists(join(in.user, "precise-readbacks"))) {
        env.setdefault("GOW3_READBACKS", "2");
        prints.push_back("Precise readbacks on (found analysis/uncharted-user/precise-readbacks)");
    }
    // analysis/uncharted-user/test-env.txt lets a tester try settings without rebuilding anything. Each KEY=VALUE line
    // (lines starting with # are ignored) replaces whatever was set up to this point.
    std::string test_env = join(in.user, "test-env.txt");
    if (exists(test_env)) {
        std::string data;
        if (!read_file(test_env, data)) throw Failure{"cannot read " + test_env};
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data(), (int)data.size(), nullptr, 0) == 0 && !data.empty())
            throw Failure{"test-env.txt is not UTF-8"};
        for (std::string line : lines_of(universal(data))) {
            line = strip(line);
            if (line.empty() || line[0] == '#' || line.find('=') == std::string::npos) continue;
            size_t eq = line.find('=');
            std::string key = strip(line.substr(0, eq)), value = strip(line.substr(eq + 1));
            env.set(key, value);
            prints.push_back("From test-env.txt: " + key + "=" + value);
        }
    }
    // readback.trigger controls copying GPU-written constants back into guest memory for the SRT walker. It accepts
    // "auto", "off", or a hex range "<lo> <hi>" for a single copy. GOW3_SYNC_GPU_CONSTANTS=1 has it running from start.
    if (diagnostics) env.setdefault("GOW3_READBACK_TRIGGER", join(in.user, "readback.trigger"));
    // Fix for the grey world, confirmed 14 October 2026: after every submission, constants the GPU wrote are copied
    // back into guest memory.
    env.setdefault("GOW3_SYNC_GPU_CONSTANTS", "1");
    // Since 18 Oct Uncharted 1 compiles shaders in the background like the other two games; compiling synchronously
    // stalled it for 1 to 2.4 s whenever a new area loaded (Ryzen laptop). UNCHARTED_U1_SYNC_SHADERS=1 goes back to
    // synchronous compilation.
    if (u1 && orig(in, "UNCHARTED_U1_SYNC_SHADERS") == "1") env.setdefault("GOW3_ASYNC_SHADERS", "0");
    // Statistics logged every 5 s: how many faults and how long they took, the busiest guest instructions and memory
    // windows, flip rate and frame time spread, GPU-thread compile time, and a breakdown of any frame slower than 40 ms.
    // dma.trigger switches the DMA sync mode while the game runs.
    if (diagnostics) {
        env.setdefault("GOW3_FAULT_STATS", "1");
        env.setdefault("GOW3_FRAME_STATS", "1");
        env.setdefault("GOW3_DMA_TRIGGER", join(in.user, "dma.trigger"));
    }
    // Shaders that read memory directly (tiled lighting, and the shadow cascade loads for hair, cobwebs and
    // characters): only the 256 KiB windows around guest addresses found in their user data are made resident and
    // up to date, rather than uploading every resident range the CPU has dirtied.
    env.setdefault("GOW3_DMA_SYNC", "pointers 256");
    // Adds a per-caller breakdown of scheduler waits on the GPU to the frame statistics.
    if (diagnostics) env.setdefault("GOW3_PERF_DIAG", "1");
    // The tiled lighting compute shader (0x43b8ee5e) indexes per-light records with a value it computes on the GPU,
    // so the GPU fetches those records from guest memory itself.
    env.setdefault("GOW3_DIRECT_MEMORY_ACCESS", "1");
    // GPU checkpoints record which draw was running when the device was lost. They take about 2% of the GPU command
    // thread, so they are only enabled when UNCHARTED_GPU_CHECKPOINTS=1 asks for them.
    if (orig(in, "UNCHARTED_GPU_CHECKPOINTS") == "1") env.setdefault("GOW3_GPU_CHECKPOINTS", "1");
    // The pipeline cache saves recompiling all shaders on every launch. Turn it off with GOW3_PIPELINE_CACHE=0 while
    // testing changes to the shader recompiler.
    env.setdefault("GOW3_PIPELINE_CACHE", "1");
    // Pipeline binaries are off since 24 Oct 2026. With them Uncharted 2 needed 15.5 GB of RAM against 7.0 GB without,
    // and took 40 s rather than 9.6 s to load its pipelines. A test-env.txt line GOW3_PIPELINE_BINARIES=1 brings them
    // back.
    env.setdefault("GOW3_PIPELINE_BINARIES", "0");
    if (env.get("GOW3_PIPELINE_BINARIES") == "0") remove_pipeline_binaries(join(env.get("GOW3_GPU_USER_DIR"), "cache"));
    env.set("GOW3_TOGGLE_FILE", join(in.user, "disabled-optimizations.txt"));
    env.set("GOW3_LOADEXEC_FILE", in.loadexec);
    if (in.capture) {
        capture_dir = join(in.out, "captures\\" + std::to_string(time_ns()));
        make_dirs(capture_dir);
        trigger = join(capture_dir, "trigger");
        env.set("GOW3_FRAME_DUMP_TRIGGER", trigger);
        env.set("GOW3_DUMP_DIR", capture_dir);
        write_file(trigger, "");
    } else if (diagnostics) {
        // Image dumps only when requested: no trigger file exists at launch, so nothing is written until someone
        // creates analysis/uncharted/dumps/trigger (and, optionally, guest-images.trigger with a list of addresses).
        std::string dumps = join(in.out, "dumps");
        make_dirs(dumps);
        env.setdefault("GOW3_FRAME_DUMP_TRIGGER", join(dumps, "trigger"));
        env.setdefault("GOW3_DUMP_DIR", dumps);
    }
    return env;
}

const Value *field(const Value &v, const char *k) { return v.get(k); }
std::string text_of(const Value &v, const char *k) {
    const Value *f = v.get(k);
    return f && f->kind == Value::String ? f->s : "";
}
long long int_of(const Value &v, const char *k) {
    const Value *f = v.get(k);
    return f && f->kind == Value::Int ? f->i : 0;
}

}  // namespace

extern "C" {

// Computes the runtime's environment.
// inputs_json: {env: [[name, value], ...] taken from the starter process, executable, game_key, game (game folder),
//               user, out, root, loadexec, render: [w, h], size: [w, h], extra_mib, capture}
// out on success: {env: [[name, value], ...], print: [log lines], capture: dir, trigger: file}
// Returns 0 on success, 1 on failure (with the reason in out).
UN_API int un_runtime_env(const char *inputs_json, char *out, int cap) {
    Value in_json;
    if (!json::parse(inputs_json ? inputs_json : "", in_json) || !in_json.is_object()) return give("bad inputs", out, cap) < 0 ? -12 : 1;
    Inputs in;
    if (const Value *e = field(in_json, "env"))
        for (const Value &kv : e->items)
            if (kv.items.size() == 2) in.original.items.push_back({kv.items[0].s, kv.items[1].s});
    in.executable = text_of(in_json, "executable");
    in.game_key = text_of(in_json, "game_key");
    in.game = text_of(in_json, "game");
    in.user = text_of(in_json, "user");
    in.out = text_of(in_json, "out");
    in.root = text_of(in_json, "root");
    in.loadexec = text_of(in_json, "loadexec");
    if (const Value *r = field(in_json, "render"); r && r->items.size() == 2) { in.render_w = (int)r->items[0].i; in.render_h = (int)r->items[1].i; }
    if (const Value *s = field(in_json, "size"); s && s->items.size() == 2) { in.width = (int)s->items[0].i; in.height = (int)s->items[1].i; }
    in.extra_mib = int_of(in_json, "extra_mib");
    if (const Value *c = field(in_json, "capture")) in.capture = c->kind == Value::Bool && c->b;
    std::vector<std::string> prints;
    std::string capture_dir, trigger;
    try {
        Env env = build(in, prints, capture_dir, trigger);
        Value result = Value::object(), list, lines;
        list.kind = Value::Array;
        lines.kind = Value::Array;
        for (auto &kv : env.items) {
            Value pair;
            pair.kind = Value::Array;
            pair.items = {Value::text(kv.first), Value::text(kv.second)};
            list.items.push_back(pair);
        }
        for (auto &p : prints) lines.items.push_back(Value::text(p));
        result.set("env", list);
        result.set("print", lines);
        result.set("capture", Value::text(capture_dir));
        result.set("trigger", Value::text(trigger));
        return give(json::dump(result), out, cap);
    } catch (const Failure &f) {
        int status = give(f.message, out, cap);
        return status < 0 ? status : 1;
    }
}

}  // extern "C"
