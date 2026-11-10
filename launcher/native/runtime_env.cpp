// runtime_env.cpp - the game's runtime settings, part of uncharted_native.dll: the game starter (tools/probe_uncharted.py)
// hands the runtime (Uncharted.exe) its options as GOW3_* environment values, made here from the launcher's UNCHARTED_*
// ones, the chosen game and render size. Values already in the environment win where noted ("default": only when not
// set), so test-env.txt and explicit GOW3_* values keep working. The reasons behind each value (issues, measurements)
// are kept next to it.

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

// Python's str(Path(p)) for a Windows path: '/' as '\', repeated separators once, no trailing one but a drive root's
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

// UTF-8 with each invalid byte as U+FFFD (Python's errors="replace" for whole bad bytes)
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

// text read as Python's read_text does it (universal newlines): \r\n and \r become \n
std::string universal(const std::string &t) {
    std::string out;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\r') { out += '\n'; if (i + 1 < t.size() && t[i + 1] == '\n') ++i; }
        else out += t[i];
    }
    return out;
}

std::vector<std::string> lines_of(const std::string &t) {   // splitlines over \n (after universal())
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

// the value of key in a key=value settings file (probe-<game>.ini); `found` false when it has none
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

// Cheats (launcher -> Cheats): the games' own switches, kept set while the game runs (GOW3_POKE; 1.02 image offsets)
const std::map<std::string, std::vector<std::pair<std::string, std::string>>> &cheats() {
    static const std::map<std::string, std::vector<std::pair<std::string, std::string>>> t = {
        {"eboot.bin", {{"invincible", "aa5ff8"}, {"ammo", "ab9c01"}}},
        {"big2-ps4_Shipping.elf", {{"invincible", "104f877"}, {"ammo", "104f84a"}, {"noreload", "104f84b"}}},
        {"big3-ps4_Shipping.elf", {{"invincible", "*16e2f80+a7"}, {"ammo", "17faa91"}, {"grenades", "17faa92"},
                                   {"noreload", "17faa93"}, {"onehit", "17fab94"}}}};
    return t;
}
// Unlock all rewards (UNCHARTED_UNLOCK_REWARDS=1): the debug menu's "Give all Store Items" and "Give All Movies",
// kept set like the cheats: every reward and story cutscene counts as unlocked while they are set; nothing is saved.
const std::map<std::string, std::vector<std::string>> &reward_pokes() {
    static const std::map<std::string, std::vector<std::string>> t = {
        {"big2-ps4_Shipping.elf", {"1114178", "1114179"}}, {"big3-ps4_Shipping.elf", {"17b2a94", "17b2a95"}}};
    return t;
}
// Extra quality (launcher -> Advanced): debug-menu render switches kept set like the cheats. Hi-res local shadow
// buffer and full resolution textures (~0% and ~1% FPS in Uncharted 2 at 1440p); no depth of field and no motion blur
// (GitHub #169) kept at 0. Uncharted 1 not mapped.
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
    std::vector<std::pair<std::string, std::string>> items;   // in insertion order, like a Python dict
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

// a line on the game starter's own output, unbuffered (the deletion below runs on its own thread)
void say(const std::string &line) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    std::string text = line + "\n";
    DWORD put = 0;
    WriteFile(out, text.data(), (DWORD)text.size(), &put, nullptr);
}

// Deletes the pipeline binaries earlier versions saved (<cache>\<game>*\pipeline-binaries, ~2 MB each, 20 GB on a
// tester's PC) on a background thread while the game runs: they are no longer used (24 Oct 2026). A start that ends
// first leaves the rest to the next start.
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
    std::snprintf(line, sizeof line, "Pipeline binaries: %lld old files removed (%.1f GB), no longer used", files,
                  size / 1073741824.0);
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
    Env original;          // the starter's own environment (os.environ)
    std::string executable, game_key, game, user, out, root, loadexec;
    int render_w = 1920, render_h = 1080, width = 1920, height = 1080;
    long long extra_mib = 0;
    bool capture = false;
};

std::string orig(const Inputs &in, const std::string &k, const std::string &fallback = "") {
    return in.original.get(upper(k), fallback);
}

// the game's version as the game starter reads it (system.cpp; "" when unknown)
std::string game_version(const std::string &game) {
    char buf[256];
    return un_system("starter_version", widen(game).c_str(), buf, sizeof buf) == 0 ? buf : "";
}

// the environment the runtime starts with; prints: what the starter tells the log
Env build(const Inputs &in, std::vector<std::string> &prints, std::string &capture_dir, std::string &trigger) {
    Env env = in.original;
    const std::string &exe = in.executable, KEY = upper(in.game_key);
    const bool u1 = exe == "eboot.bin", u2 = exe == "big2-ps4_Shipping.elf", u3 = exe == "big3-ps4_Shipping.elf";
    // Cheats, unlocked rewards and extra quality: the games' own switches kept set (GOW3_POKE), update 1.02 only.
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
                std::string line = "Cheats: ";
                for (size_t i = 0; i < named.size(); ++i) line += (i ? ", " : "") + named[i];
                prints.push_back(line);
            }
            auto r = reward_pokes().find(exe);
            if (rewards && r != reward_pokes().end() && !r->second.empty()) {
                pokes.insert(pokes.end(), r->second.begin(), r->second.end());
                prints.push_back("Unlock all rewards: on");
            }
            for (const std::string &name : quality)
                for (auto &kv : q->second) if (kv.first == name) pokes.insert(pokes.end(), kv.second.begin(), kv.second.end());
            if (!quality.empty()) {
                std::string line = "Extra quality: ";
                for (size_t i = 0; i < quality.size(); ++i) line += (i ? ", " : "") + quality[i];
                prints.push_back(line);
            }
            if (!pokes.empty()) {
                std::string v;
                for (size_t i = 0; i < pokes.size(); ++i) v += (i ? "," : "") + pokes[i];
                env.set("GOW3_POKE", v);
            }
        } else {
            prints.push_back("Cheats and unlocked rewards: need update 1.02 (not used)");
        }
    }
    // Texture filtering (launcher -> Advanced -> Texture filtering): sampler.cpp GOW3_ANISOTROPY raises mipmapped
    // colour textures to at least that ratio (~0% FPS measured in Uncharted 2).
    std::string aniso = orig(in, "UNCHARTED_ANISOTROPY", "0");
    if (aniso == "2" || aniso == "4" || aniso == "8" || aniso == "16") {
        env.setdefault("GOW3_ANISOTROPY", aniso);
        prints.push_back("Texture filtering: " + aniso + "x");
    }
    // Measurement logging and live trigger files (frame/fault/wait statistics every 5 s, GPU checkpoints,
    // capture/readback/DMA triggers). On unless the launcher turns it off.
    const bool diagnostics = env.get("UNCHARTED_DIAGNOSTICS", "1") != "0";
    env.set("PATH", join(norm(env.get("MSYS2_ROOT", "C:/msys64")), "clang64\\bin") + ";" + env.get("PATH", ""));
    // UNCHARTED_TEST_GPU_USER_DIR: GPU caches in another folder (tests from an empty shader cache).
    std::string gpu_dir = env.get("UNCHARTED_TEST_GPU_USER_DIR");
    env.set("GOW3_GPU_USER_DIR", gpu_dir.empty() ? in.user : gpu_dir);
    std::string settings = join(in.user, "probe-" + in.game_key + ".ini");
    if (!exists(settings) && exists(join(in.user, "probe.ini")))   // earlier versions shared one file
        CopyFileW(widen(join(in.user, "probe.ini")).c_str(), widen(settings).c_str(), FALSE);
    // UNCHARTED_CONFIG_FILE: another settings file (tests that must not touch the player's).
    std::string config_file = orig(in, "UNCHARTED_CONFIG_FILE");
    env.set("GOW3_CONFIG", config_file.empty() ? settings : config_file);
    // Frame rates above 60 need the fast vblank (GOW3_VBLANK_HZ=0); an explicit one is kept.
    if (!in.original.has("GOW3_VBLANK_HZ")) {
        bool found = false;
        std::string limit = ini_value(env.get("GOW3_CONFIG"), "fps_limit", found);
        if (!found) limit = "60";
        long long n = digits_only(limit) ? std::strtoll(limit.c_str(), nullptr, 10) : -1;   // saturates when huge
        if (digits_only(limit) && (n == 0 || n > 60))
            env.set("GOW3_VBLANK_HZ", "0");
    }
    // Save profiles (launcher Advanced, GitHub #66, 24 Oct 2026): profile N plays as PS4 user N, whose saves are in
    // analysis\uncharted-user\savedata\N (profile 1: the folder of every earlier version). GOW3_USER_ID set outside
    // the launcher (tests) wins.
    std::string profile = orig(in, "UNCHARTED_SAVE_PROFILE_" + KEY, "1");
    long long profile_n = digits_only(profile) ? std::strtoll(profile.c_str(), nullptr, 10) : 0;
    bool profile_ok = digits_only(profile) && profile_n >= 1 && profile_n <= 16;
    env.setdefault("GOW3_USER_ID", profile_ok ? profile : "1");
    const std::string *mt_value = in.original.find("UNCHARTED_MULTITHREADED_" + KEY);
    std::string multithreaded = mt_value ? *mt_value : "";
    bool have_mt = mt_value != nullptr;
    // Drake's Fortune: its weapons (and at times other objects) drew with another draw's look on some frames with
    // multithreaded rendering (flat grey guns, GitHub #23, 18 Oct 2026), and it gains nothing from it. Off until that
    // is fixed; UNCHARTED_U1_MULTITHREADED_TEST=1 keeps the launcher's choice (tests).
    if (u1 && have_mt && multithreaded == "1" && orig(in, "UNCHARTED_U1_MULTITHREADED_TEST") != "1") {
        prints.push_back("Multithreaded rendering: off for Drake's Fortune (its weapons flicker with it)");
        multithreaded = "0";
    }
    // Drake's Deception froze with multithreaded rendering (GitHub #51, fixed in the runtime 19 Oct 2026); since
    // 21 Oct the launcher's choice counts (heavy Cartagena spot: 53.7 FPS with 11 stalls off, 60 FPS with 1 on). An
    // explicit GOW3_DRAW_PIPE / GOW3_VK_RECORD_THREAD (tests: one of the two alone) wins over the launcher's choice.
    if (have_mt) {
        env.set("GOW3_DRAW_PIPE", in.original.get("GOW3_DRAW_PIPE", multithreaded));
        env.set("GOW3_VK_RECORD_THREAD", in.original.get("GOW3_VK_RECORD_THREAD", multithreaded));
    }
    // Keyboard and mouse (launcher Controls tab, per game; explicit GOW3_* values win).
    const char *const controls[][2] = {{"KEY_MAP", "GOW3_KEY_MAP"},       {"MOUSE_MAP", "GOW3_MOUSE_MAP"},
                                       {"MOUSE", "GOW3_MOUSE"},           {"MOUSE_SENS", "GOW3_MOUSE_SENS"},
                                       {"MOUSE_INVERT", "GOW3_MOUSE_INVERT"}, {"PROMPTS", "GOW3_PROMPTS"},
                                       {"PAD_MAP", "GOW3_PAD_MAP"}};
    for (auto &c : controls) {
        const std::string *v = in.original.find(std::string("UNCHARTED_") + c[0] + "_" + KEY);
        if (v && !in.original.has(c[1])) env.set(c[1], *v);
    }
    // GPU readbacks are asynchronous. Unprotected (the runtime default, 2) crashed Uncharted 3 within a minute in the
    // Cartagena museum and let characters fly or NPCs fall from the sky (U2, U3, 16 Oct); waiting for every readback
    // (0) cost U2 ~20 FPS. Protected (1) since 16 Oct: new GPU data stays read-protected until it lands.
    if (u2 || u3) env.setdefault("GOW3_ASYNC_READBACK", "1");
    // The 64-wide copy shader's results (cs 6a6422a9) come back at the frame's end-of-pipe fence: read back a frame
    // late, NPCs floated and Nate's feet sank (U2, 16 Oct; U3 17 Oct); in Drake's Fortune enemy grenades exploded in
    // front of Drake (GitHub #6, 19 Oct).
    if (u1 || u2 || u3) env.setdefault("GOW3_COPY64_EOP_READBACK", "1");
    // Uncharted 2 and 3's aim depth of field reads its focus distance from a 1x1 render target the GPU writes: tiny
    // GPU-written images come back with the frame's end-of-pipe fence (16 Oct, 19 Oct). Drake's Fortune skips it.
    if (u2 || u3) env.setdefault("GOW3_TINY_IMAGE_READBACK", "1");
    // 24 Oct (GitHub #140, Uncharted 3 washed out in sunny areas): its bloom is drawn into mips 1 and 2 of a 7-level
    // texture; GOW3_MIP_TARGETS=1 draws them into the texture's mips.
    if (u3) env.setdefault("GOW3_MIP_TARGETS", "1");
    // 2 Nov 2026 (GitHub #161/#253, Drake's Fortune water flat dark green): HTILE clears of freed depth images are
    // kept (its reflection's depth buffer is cleared only through its HTILE). Drake's Fortune only for now.
    if (u1) env.setdefault("GOW3_HTILE_KEEP_CLEARS", "1");
    // The PS4's system language (launcher -> Advanced -> Language; runtime_services.c). 1 = English (US).
    std::string language = strip(orig(in, "UNCHARTED_LANGUAGE"));
    if (digits_only(language)) env.setdefault("GOW3_LANGUAGE", language);
    // Speakers (launcher -> Advanced): auto = the Windows output device's (stereo, 5.1 or 7.1), or 2, 6, 8.
    std::string speakers = lower(strip(orig(in, "UNCHARTED_SPEAKERS")));
    if (speakers == "auto" || speakers == "2" || speakers == "6" || speakers == "8")
        env.setdefault("GOW3_AUDIO_SPEAKERS", speakers);
    const std::string render = std::to_string(in.render_w) + "x" + std::to_string(in.render_h);
    // Temporal anti-aliasing (in-game menu -> Image: TAA, FSR 3.1/FSR 4 Native AA, DLAA) at the render size: the
    // camera comes from the motion blur's camera velocity pass (shader f4b18264), the anti-aliasing runs on the lit
    // scene before the post processing (compute aec539f0). UNCHARTED_TEMPORAL_AA=0 switches it off.
    if (orig(in, "UNCHARTED_TEMPORAL_AA", "1") == "0") {
        env.set("GOW3_UPSCALER", "none");
    } else {
        env.setdefault("GOW3_VELOCITY_PS", "f4b18264");
        env.setdefault("GOW3_UPSCALE_BEFORE_CS", "aec539f0");
        // Full-screen passes that rebuild positions from the jittered scene depth get the jitter too: the sun shadow
        // pass (Supermedo/uncharted_pc #261), Uncharted 1's boat details and Uncharted 3's London bar surfaces.
        env.setdefault("GOW3_JITTER_PS", "f614041e,b2a42acf,1a08df2d");
        env.set("GOW3_SCENE_SIZE", render);
        // FSR 4's model files, when installed; DLAA needs gow3_dlss.dll and nvngx_dlss.dll beside Uncharted.exe.
        std::string fsr4 = join(in.root, "gow3_pc\\fsr4_shaders");
        if (is_dir(fsr4)) env.setdefault("GOW3_FSR4_DIR", fsr4);
        // The renderer's own default is FSR 3; here it starts off until it is picked.
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
    // The movie player reads its (1080p) movies with the render size: the decoder scales them to it (else movies come
    // out green and tiled above 1080p).
    if (in.render_w != 1920 || in.render_h != 1080) env.set("GOW3_MOVIE_SIZE", render);
    // Output resolution above the render resolution (UNCHARTED_OUTPUT_RES[_U1/_U2/_U3]=WxH): the finished frame is
    // upscaled to the output size where it is copied back into the display buffer (pixel shader 749bbec9) by FSR 3.1,
    // FSR 4 or DLSS, else TAA or nothing, then FSR 1 or a bilinear stretch; the HUD is drawn after that.
    std::string output = orig(in, "UNCHARTED_OUTPUT_RES_" + KEY);
    if (output.empty()) output = orig(in, "UNCHARTED_OUTPUT_RES");
    output = lower(output);
    if (output.find('x') == std::string::npos && (in.render_w != in.width || in.render_h != in.height)) {
        // Drake's Fortune above the size it can render at: that size is upscaled to the one chosen, with FSR 1 unless
        // a temporal upscaler is chosen.
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
            // The tonemap draw finishing the frame: Uncharted 3 draws world-anchored prompts into its target after
            // it, so the upscale runs at the draw after it.
            env.setdefault("GOW3_UI_TONEMAP_PS", "484cdbf5");
            prints.push_back("Upscaling: " + render + " -> " + std::to_string(out_w) + "x" + std::to_string(out_h));
        }
    }
    std::string pad_file = join(in.user, "probe-pad.txt");
    write_file(pad_file, "");
    env.set("GOW3_PAD_FILE", pad_file);
    // A serialized renderer unless chosen above (the base runtime enables these concurrency optimizations).
    env.setdefault("GOW3_DRAW_PIPE", "0");
    env.setdefault("GOW3_VK_RECORD_THREAD", "0");
    env.setdefault("GOW3_NATIVE_TSC", "1");
    // Frame captures on demand: write a pass count (e.g. 600) into this file; captures land in
    // analysis/uncharted-user/captures. Every draw's/dispatch's buffer addresses are listed.
    if (diagnostics) {
        env.setdefault("GOW3_CAPTURE_TRIGGER", join(in.user, "capture.trigger"));
        env.setdefault("GOW3_CAPTURE_ALL_BUFFERS", "1");
    }
    // The file analysis/uncharted-user/precise-readbacks turns Precise readbacks on for the next start.
    if (exists(join(in.user, "precise-readbacks"))) {
        env.setdefault("GOW3_READBACKS", "2");
        prints.push_back("Readbacks: precise (analysis/uncharted-user/precise-readbacks)");
    }
    // Test settings for the next start without code changes: KEY=VALUE lines (# comments) in
    // analysis/uncharted-user/test-env.txt; they override everything set above.
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
            prints.push_back("Test setting: " + key + "=" + value);
        }
    }
    // GPU-written constants back to guest memory for the SRT walker: write "auto"/"off" or "<lo> <hi>" (hex range,
    // one-off) into this file. GOW3_SYNC_GPU_CONSTANTS=1 starts it on.
    if (diagnostics) env.setdefault("GOW3_READBACK_TRIGGER", join(in.user, "readback.trigger"));
    // Grey-world fix (confirmed 14 October 2026): GPU-written constants back to guest memory each submission.
    env.setdefault("GOW3_SYNC_GPU_CONSTANTS", "1");
    // Uncharted 1 compiles shaders asynchronously like the others since 18 Oct (synchronous froze for 1-2.4 s per new
    // area on a Ryzen laptop); UNCHARTED_U1_SYNC_SHADERS=1 restores synchronous compilation.
    if (u1 && orig(in, "UNCHARTED_U1_SYNC_SHADERS") == "1") env.setdefault("GOW3_ASYNC_SHADERS", "0");
    // Every 5 s: fault counts/time, hottest guest instructions and windows; flip rate/frame time spread, compile time
    // on the GPU thread and details of frames over 40 ms; the DMA sync mode is switchable live through dma.trigger.
    if (diagnostics) {
        env.setdefault("GOW3_FAULT_STATS", "1");
        env.setdefault("GOW3_FRAME_STATS", "1");
        env.setdefault("GOW3_DMA_TRIGGER", join(in.user, "dma.trigger"));
    }
    // DMA shaders (tiled lighting; shadow cascade loads of hair/cobwebs/characters): make 256 KiB windows at the guest
    // addresses in their user data resident and current, instead of uploading every CPU-dirty resident range.
    env.setdefault("GOW3_DMA_SYNC", "pointers 256");
    // Which code waits for the GPU (scheduler waits by caller), reported with the frame stats.
    if (diagnostics) env.setdefault("GOW3_PERF_DIAG", "1");
    // The tiled lighting shader (cs 0x43b8ee5e) reads per-light records with an index computed on the GPU: they are
    // read from guest memory on the GPU.
    env.setdefault("GOW3_DIRECT_MEMORY_ACCESS", "1");
    // GPU checkpoints (which draw a lost device was on) cost ~2% of the GPU command thread: only on request.
    if (orig(in, "UNCHARTED_GPU_CHECKPOINTS") == "1") env.setdefault("GOW3_GPU_CHECKPOINTS", "1");
    // Cached pipelines avoid recompiling every shader on each run (GOW3_PIPELINE_CACHE=0 when testing shader
    // recompiler changes).
    env.setdefault("GOW3_PIPELINE_CACHE", "1");
    // No pipeline binaries (24 Oct 2026): Uncharted 2 used 15.5 GB of RAM with them and 7.0 GB without, and loaded its
    // pipelines in 40 s instead of 9.6 s. GOW3_PIPELINE_BINARIES=1 (test-env.txt) turns them back on.
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
        // On-demand image dumps (no trigger file at start, so nothing is dumped until asked): create
        // analysis/uncharted/dumps/trigger, plus guest-images.trigger listing addresses.
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

// The runtime's environment. inputs_json: {env: [[name, value], ...] (the starter's own), executable, game_key,
// game (its folder), user, out, root, loadexec, render: [w, h], size: [w, h], extra_mib,
// capture}. out: {env: [[name, value], ...], print: [lines], capture: dir, trigger: file}. 0 ok, 1 failed (out: why).
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
