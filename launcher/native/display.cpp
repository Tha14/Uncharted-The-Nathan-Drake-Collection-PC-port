// display.cpp - graphics card and screen logic for the launcher, part of uncharted_native.dll: which Quick Setup preset
// suits the player's graphics card, how strong an integrated GPU is, the monitor's refresh rate, and the resolution /
// aspect ratio / upscaling arithmetic and tables of the Display page.

#include "native.h"

#include <windows.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace {

using Pairs = std::vector<std::pair<std::string, std::string>>;

int give(const std::string &text, char *out, int cap) {
    if ((int)text.size() + 1 > cap) return -(int)(text.size() + 1);
    std::memcpy(out, text.c_str(), text.size() + 1);
    return 0;
}

bool has(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }
bool found(const std::string &s, const std::regex &r) { return std::regex_search(s, r); }

// ------------------------------------------------------------------------------------------------ graphics cards

// lower case, without (tm) / (r), single spaces
std::string name_key(const std::string &name) {
    std::string s;
    for (char c : name) s += (char)std::tolower((unsigned char)c);
    for (const char *mark : {"(tm)", "(r)"})
        for (size_t at; (at = s.find(mark)) != std::string::npos;) s.replace(at, std::strlen(mark), " ");
    std::string out;
    bool space = false;
    for (char c : s) {
        if (std::isspace((unsigned char)c)) { space = !out.empty(); continue; }
        if (space) out += ' ';
        out += c;
        space = false;
    }
    return out;
}

const std::regex &integrated_amd() {
    static const std::regex r(R"(\bradeon\s*(?:610m|640m|660m|680m|740m|760m|780m|860m|880m|890m)\b)"
                              R"(|\bradeon\s+graphics\b|\bradeon\s+vega\b|\bvega\s*(?:3|6|7|8|9|10|11)\b)"
                              R"(|\bryzen\s+.*\bgraphics\b)");
    return r;
}
const std::regex &discrete_arc() { static const std::regex r(R"(\barc\s*(?:pro\s*)?[ab]\d{2,3})"); return r; }
const std::regex &strix_halo() { static const std::regex r(R"(\bradeon\s*80[4-6]0s\b)"); return r; }

// the Ryzen AI Max APUs (Radeon 8040S/8050S/8060S): integrated, but not a low-end GPU
bool is_strix_halo(const std::string &n) { return found(n, strix_halo()); }

// low-end integrated graphics (the IGPU preset's GPUs), from the model name
bool is_integrated(const std::string &n) {
    if (n.empty() || is_strix_halo(n) || has(n, "nvidia") || has(n, "geforce")) return false;
    static const std::regex rx(R"(\brx\s*\d)");
    if (has(n, "vega 56") || has(n, "vega 64") || has(n, "radeon vii") || found(n, rx)) return false;
    if (has(n, "intel")) return !found(n, discrete_arc());
    return found(n, integrated_amd());
}

// how strong an integrated GPU is: "weak" (30 FPS), "mainstream" (60 FPS at 540p) or "strong" (60 FPS at 720p)
std::string igpu_strength(const std::string &name) {
    std::string n = name_key(name);
    static const std::regex strong(R"(\b(880m|890m)\b)"), mainstream(R"(\b(680m|760m|780m|860m)\b)"),
        weak(R"(\b(610m|640m|660m|740m)\b)");
    if ((has(n, "intel") && has(n, "arc")) || found(n, strong)) return "strong";
    if (found(n, mainstream) || (has(n, "intel") && has(n, "iris xe"))) return "mainstream";
    if (has(n, "intel") || has(n, "vega") || found(n, weak) || has(n, "radeon graphics")) return "weak";
    return "mainstream";
}

// the Quick Setup preset for a graphics card, from its model name; "" when not known
std::string tier(const std::string &name) {
    std::string n = name_key(name);
    if (is_strix_halo(n)) return "medium";
    if (is_integrated(n)) return "igpu";
    static const std::regex nvidia_r(R"(\b(rtx|gtx|gt|mx)\s*(\d{3,4})\s*(ti)?\s*(super)?)"),
        amd_r(R"(\brx\s*(\d{3,4})\s*(xtx|xt|gre)?)"), arc_r(R"(\barc\s*([ab])(\d{3}))"), laptop_r(R"(\d{3,4}m\b)");
    std::smatch m;
    std::string t;
    if (std::regex_search(n, m, nvidia_r)) {
        std::string kind = m[1];
        int number = std::stoi(m[2]);
        bool ti = m[3].matched, super = m[4].matched;
        if (kind == "gt" || kind == "mx") t = "low";
        else if (kind == "gtx") {
            if (number == 1080 && ti) t = "high";
            else if ((1060 <= number && number < 1100) || (1660 <= number && number < 1700) || (number == 1650 && super)) t = "medium";
            else t = "low";
        } else {
            int series = number / 100, model = number % 100;
            if (model >= 80) t = series >= 30 ? "ultra" : "high";
            else if (model == 70) t = (series >= 40 && (ti || super)) ? "ultra" : "high";
            else if (model == 60) t = (ti || series >= 50) ? "high" : "medium";
            else t = "medium";
        }
    } else if (std::regex_search(n, m, amd_r)) {
        int number = std::stoi(m[1]);
        std::string suffix = m[2].matched ? std::string(m[2]) : std::string();
        if (number >= 9000) {
            int d = (number % 100) / 10;
            t = d == 7 ? "ultra" : d == 6 ? "high" : "medium";
        } else if (number >= 1000) {
            int series = number / 1000, model = (number % 1000) / 100;
            if (model >= 9) t = "ultra";
            else if (model == 8) t = suffix.rfind("xt", 0) == 0 ? "ultra" : "high";
            else if (model == 7) t = series == 5 ? "medium" : "high";
            else if (model == 6 || number == 5500) t = "medium";
            else t = "low";
        } else {
            t = (number == 470 || number == 480 || number == 570 || number == 580 || number == 590) ? "medium" : "low";
        }
    } else if (has(n, "vega 56") || has(n, "vega 64")) {
        t = "medium";
    } else if (has(n, "radeon vii")) {
        t = "high";
    } else if (std::regex_search(n, m, arc_r)) {
        t = std::stoi(m[2]) >= 500 ? "medium" : "low";
    } else if (has(n, "intel") || (has(n, "radeon") && has(n, "graphics"))) {
        t = "low";   // other Intel (Arc Pro) and Radeon graphics the checks above did not place
    } else {
        return "";
    }
    if (has(n, "laptop") || has(n, "max-q") || has(n, "mobile") || found(n, laptop_r)) {
        if (t == "ultra") t = "high";
        else if (t == "high") t = "medium";
    }
    return t;
}

// the Quick Setup preset for a card: IGPU for low-end integrated graphics (the game log's kind when known, else the
// model name), else its tier; Strix Halo never takes IGPU. integrated: 1 yes, 0 no, -1 not known
std::string recommendation(const std::string &name, int integrated) {
    std::string n = name_key(name);
    if (is_strix_halo(n)) return tier(n);
    static const std::regex separate(R"(\b(?:nvidia|geforce|radeon vii)\b|\brx\s*(?:\d|vega))");
    bool igpu = integrated == 1;
    if (integrated < 0 || found(n, discrete_arc()) || found(n, separate)) igpu = is_integrated(n);
    return igpu ? "igpu" : tier(name);
}

std::string utf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// the graphics card the game used last, from its newest log ("GPU: using <name>", and its kind from the
// "GPU <n>: <name> (DiscreteGpu|IntegratedGpu)" lines of the first 400 lines); false when no log has it
bool from_logs(const std::vector<std::wstring> &logs, std::string &name, int &integrated) {
    struct Log { std::wstring path; FILETIME written; };
    std::vector<Log> present;
    for (const std::wstring &p : logs) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) present.push_back({p, a.ftLastWriteTime});
    }
    std::sort(present.begin(), present.end(),
              [](const Log &a, const Log &b) { return CompareFileTime(&a.written, &b.written) > 0; });
    for (const Log &log : present) {
        HANDLE h = CreateFileW(log.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::string text(1 << 20, '\0');
        DWORD got = 0;
        ReadFile(h, text.data(), (DWORD)text.size(), &got, nullptr);
        CloseHandle(h);
        text.resize(got);
        std::string using_name;
        std::vector<std::pair<std::string, std::string>> kinds;
        size_t pos = 0;
        for (int number = 0; pos < text.size() && number <= 400; ++number) {
            size_t end = text.find('\n', pos);
            std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? text.size() : end + 1;
            std::string stripped = line;
            while (!stripped.empty() && (stripped.back() == '\r' || stripped.back() == ' ' || stripped.back() == '\t')) stripped.pop_back();
            if (line.rfind("GPU: using ", 0) == 0) {
                using_name = stripped.substr(11);
                size_t a = using_name.find_first_not_of(" \t");
                using_name = a == std::string::npos ? "" : using_name.substr(a);
            } else if (line.rfind("GPU ", 0) == 0 && line.find(": ") != std::string::npos) {
                size_t colon = line.find(": ");
                std::string head = line.substr(4, colon - 4), card = stripped.substr(colon + 2);
                size_t a = head.find_first_not_of(" \t"), b = head.find_last_not_of(" \t");
                head = a == std::string::npos ? "" : head.substr(a, b - a + 1);
                if (!head.empty() && std::all_of(head.begin(), head.end(), ::isdigit) && !card.empty() && card.back() == ')') {
                    size_t open = card.rfind(" (");
                    if (open != std::string::npos)
                        kinds.push_back({card.substr(0, open), card.substr(open + 2, card.size() - open - 3)});
                }
            }
        }
        if (!using_name.empty()) {
            name = using_name;
            integrated = -1;
            for (auto &k : kinds)
                if (k.first == using_name) integrated = k.second == "IntegratedGpu" ? 1 : k.second == "DiscreteGpu" ? 0 : -1;
            return true;
        }
    }
    return false;
}

// Windows' graphics adapters (hardware only)
std::vector<std::string> adapters() {
    std::vector<std::string> out;
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory))) return out;
    IDXGIAdapter1 *a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            std::string name = utf8(d.Description);
            if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
        }
        a->Release();
    }
    factory->Release();
    return out;
}

// ------------------------------------------------------------------------------------------------ screen and sizes

const char *const RESOLUTIONS[][2] = {{"960x540", "540p"},   {"1280x720", "720p"},   {"1600x900", "900p"},
                                      {"1920x1080", "1080p"}, {"2560x1440", "1440p"}, {"2880x1620", "1620p"},
                                      {"3840x2160", "4K"}};
const char *const ASPECTS[] = {"16:10", "16:9", "21:9", "32:9"};
const char *const UPSCALE_PRESETS[][2] = {{"off", "Native AA"}, {"quality", "Quality"}, {"balanced", "Balanced"},
                                          {"performance", "Performance"}, {"ultra", "Ultra Performance"}};

double aspect_width(const std::string &a) {
    if (a == "16:10") return 16.0 / 10;
    if (a == "16:9") return 16.0 / 9;
    if (a == "21:9") return 64.0 / 27;
    if (a == "32:9") return 32.0 / 9;
    return 0;
}

double upscale_scale(const std::string &q) {
    if (q == "quality") return 1.5;
    if (q == "balanced") return 1.7;
    if (q == "performance") return 2.0;
    if (q == "ultra") return 3.0;
    return 0;
}

bool temporal(const std::string &u) { return u == "fsr3" || u == "fsr4" || u == "fsr411" || u == "dlss"; }

long long half_even(double v) { return (long long)std::nearbyint(v); }

// the render resolution at the aspect ratio: 1440p at 21:9 is 3440x1440 (widths in steps of 16), 720p at 16:10 is
// 1280x800 (heights in steps of 4)
std::string at_aspect(const std::string &resolution, const std::string &aspect) {
    long long width = std::stoll(resolution.substr(0, resolution.find('x'))), height = std::stoll(resolution.substr(resolution.find('x') + 1));
    double w = aspect_width(aspect);
    if (w == 0 || aspect == "16:9") return resolution;
    if (w < 16.0 / 9) height = half_even(width / w / 4) * 4;
    else if (aspect == "21:9" && (height == 1080 || height == 1440 || height == 1600))
        width = height == 1080 ? 2560 : height == 1440 ? 3440 : 3840;   // the common monitor sizes
    else width = half_even(height * w / 16) * 16;
    return std::to_string(width) + "x" + std::to_string(height);
}

}  // namespace

extern "C" {

// the recommended preset: from the game's newest log (logs: one path per line), else Windows' adapters (the
// strongest known one). out: name\tintegrated(1/0/-1)\tpreset (preset may be empty)
UN_API int un_gpu_recommendation(const wchar_t *logs, char *out, int cap) {
    std::vector<std::wstring> paths;
    std::wstring all = logs ? logs : L"";
    for (size_t pos = 0; pos <= all.size();) {
        size_t end = all.find(L'\n', pos);
        std::wstring p = all.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
        if (!p.empty() && p.back() == L'\r') p.pop_back();
        if (!p.empty()) paths.push_back(p);
        if (end == std::wstring::npos) break;
        pos = end + 1;
    }
    std::string name;
    int integrated = -1;
    if (!from_logs(paths, name, integrated)) {
        std::vector<std::string> names = adapters();
        const char *order[] = {"igpu", "low", "medium", "high", "ultra"};
        int best = -1;
        for (const std::string &n : names) {
            std::string t = tier(n);
            int rank = -1;
            for (int k = 0; k < 5; ++k) if (t == order[k]) rank = k;
            if (rank > best) { best = rank; name = n; }
        }
        if (name.empty() && !names.empty()) name = names[0];
    }
    std::string preset = name.empty() ? "" : recommendation(name, integrated);
    return give(name + "\t" + std::to_string(integrated) + "\t" + preset, out, cap);
}

// one GPU question: what "tier" | "recommend" | "igpu_class" | "integrated"; integrated as above
UN_API int un_gpu_query(const char *what, const char *name, int integrated, char *out, int cap) {
    std::string w = what ? what : "", n = name ? name : "", r;
    if (w == "tier") r = tier(n);
    else if (w == "recommend") r = recommendation(n, integrated);
    else if (w == "igpu_class") r = igpu_strength(n);
    else if (w == "integrated") r = is_integrated(name_key(n)) ? "1" : "0";
    return give(r, out, cap);
}

UN_API int un_monitor_refresh_hz(void) {
    HDC dc = GetDC(nullptr);
    int hz = dc ? GetDeviceCaps(dc, VREFRESH) : 0;
    if (dc) ReleaseDC(nullptr, dc);
    return hz > 1 ? hz : 60;
}

// tables: "resolutions" (value\tlabel), "aspects" (value\tvalue), "upscale_presets" (value\tlabel)
UN_API int un_display_table(const char *name, char *out, int cap) {
    std::string n = name ? name : "", text;
    if (n == "resolutions") for (auto &r : RESOLUTIONS) text += std::string(r[0]) + "\t" + r[1] + "\n";
    else if (n == "aspects") for (auto *a : ASPECTS) text += std::string(a) + "\t" + a + "\n";
    else if (n == "upscale_presets") for (auto &u : UPSCALE_PRESETS) text += std::string(u[0]) + "\t" + u[1] + "\n";
    else if (n == "temporal_upscalers") text = "fsr3\nfsr4\nfsr411\ndlss\n";
    return give(text, out, cap);
}

// the resolution list labelled by the height the game draws at this aspect ratio (value\tlabel; 2160 lines are 4K)
UN_API int un_resolution_options(const char *aspect, char *out, int cap) {
    std::string text, a = aspect ? aspect : "16:9";
    for (auto &r : RESOLUTIONS) {
        std::string s = at_aspect(r[0], a);
        std::string h = s.substr(s.find('x') + 1);
        text += std::string(r[0]) + "\t" + (h == "2160" ? std::string("4K") : h + "p") + "\n";
    }
    return give(text, out, cap);
}

UN_API int un_aspect_resolution(const char *resolution, const char *aspect, char *out, int cap) {
    return give(at_aspect(resolution ? resolution : "1920x1080", aspect ? aspect : "16:9"), out, cap);
}

// the size a game draws at and, with upscaling, the size it is upscaled to: out "render\toutput" (output empty
// without upscaling). Widths in steps of 16, heights in steps of 4.
UN_API int un_game_sizes(const char *resolution, const char *aspect, const char *upscale, const char *upscaling, char *out,
                         int cap) {
    std::string output = at_aspect(resolution ? resolution : "1920x1080", aspect ? aspect : "16:9");
    double scale = upscale_scale(upscale ? upscale : "off");
    std::string u = upscaling ? upscaling : "off";
    if (scale == 0 || (!temporal(u) && u != "fsr1")) return give(output + "\t", out, cap);
    long long width = std::stoll(output.substr(0, output.find('x'))), height = std::stoll(output.substr(output.find('x') + 1));
    std::string render = std::to_string(std::max(16LL, half_even(width / scale / 16) * 16)) + "x" +
                         std::to_string(std::max(4LL, half_even(height / scale / 4) * 4));
    return give(render + "\t" + output, out, cap);
}

// the upscaler a game's settings hold: a temporal one, else FSR 1, else off; and the anti-aliasing (TAA or off)
UN_API int un_upscaling_choice(const char *upscaler, const char *fsr1, char *out, int cap) {
    std::string u = upscaler ? upscaler : "off";
    if (temporal(u)) return give(u, out, cap);
    return give(std::string(fsr1 && std::string(fsr1) == "1" ? "fsr1" : "off"), out, cap);
}

UN_API int un_anti_aliasing_choice(const char *upscaler, const char *launcher_aa, char *out, int cap) {
    std::string u = upscaler ? upscaler : "off";
    if (u == "off" || u == "taa") return give(u, out, cap);
    return give(std::string(launcher_aa && std::string(launcher_aa) == "taa" ? "taa" : "off"), out, cap);
}

// the upscalers this package can use (value\tlabel): FSR 1 and FSR 3.1 always; FSR 4 with its model, DLSS with its DLLs
UN_API int un_upscaling_options(const wchar_t *root, char *out, int cap) {
    std::wstring r = root ? root : L".";
    auto file = [&](const wchar_t *rel) {
        DWORD a = GetFileAttributesW((r + L"\\" + rel).c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    };
    std::string text = "off\tOff\nfsr1\tFSR 1\nfsr3\tFSR 3.1\n";
    if (file(L"gow3_pc\\fsr4_shaders\\fsr4_model_v07_i8_native_1080_pre.spv")) text += "fsr4\tFSR 4\n";
    if (file(L"gow3_pc\\out\\gow3_dlss.dll") && file(L"gow3_pc\\out\\nvngx_dlss.dll")) text += "dlss\tDLSS\n";
    return give(text, out, cap);
}

}  // extern "C"
