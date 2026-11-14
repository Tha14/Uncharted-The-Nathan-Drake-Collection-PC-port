// importer/recipe.cpp: the overlay recipe (the former recipe/rebuild_overlays.py and recipe/recipe_lib.py): the four
// game-data overlays (overlay-r3, overlay-r2, overlay-port, overlay-mp) made from the player's own game copies, step by
// step in the order the fixes were applied.
//
// Layout (u3mp/patcher):
//   recipe/steps/NNN-<id>.json   one step per fix / family; recipe/steps/ORDER.txt is the order
//   recipe/lists/*.tsv           copy lists (destination, source archive, entry, size, sha256)
//   recipe/manifest.tsv          reference hashes of the four overlays
//   tools/fixes/<job>/           a script step's job folder, rendered into <out>/scratch/<job> ({{PLACEHOLDERS}} filled)
// Output (<out>): work/overlay-*/u3data/... (the overlays), work/backup-* (the steps' backups), scratch/<job>,
// _recipe/state.json (which steps ran, with the hashes of the files they wrote), _recipe/log.txt.
//
// A step is done when it ran with the same code (its fingerprint: the step file and its job folders; for copy steps,
// its list) and its recorded files are unchanged; a step whose files are all at
// the reference hashes needs nothing. Step fingerprints and records are those of the Python recipe, so an import made
// with it continues where it was.
#include "recipe.hpp"

#include "inflate.h"   // launcher/native/inflate.h
#include "psarc.hpp"   // dctools

#include <windows.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

namespace u3imp {

namespace {

const std::vector<std::string> OVERLAYS = {"overlay-r3", "overlay-r2", "overlay-port", "overlay-mp"};

bool present(const fs::path &p) {
    std::error_code ec;
    return fs::exists(p, ec);
}
bool is_dir(const fs::path &p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}
bool is_file(const fs::path &p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
std::string win(std::string s) {
    for (char &c : s)
        if (c == '/') c = '\\';
    return s;
}
// os.path.abspath on Windows
fs::path absp(const fs::path &p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    return P(win(S(a.lexically_normal())));
}
std::string rstrip_seps(std::string s) {
    while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    return s;
}
size_t count_sep(const std::string &s) { return size_t(std::count(s.begin(), s.end(), '\\')); }
std::string text_crlf_to_lf(std::string t) {   // universal newlines on read
    std::string o;
    o.reserve(t.size());
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == '\r') {
            o += '\n';
            if (i + 1 < t.size() && t[i + 1] == '\n') i++;
        } else
            o += t[i];
    }
    return o;
}
std::string pad8(const std::string &s) { return s.size() >= 8 ? s : s + std::string(8 - s.size(), ' '); }
std::string first_line(const std::string &s) { return s.substr(0, s.find('\n')); }
std::string vstr(const Value &v) {   // str() of a PARAM.SFO value
    return v.kind == Value::Int ? std::to_string(v.i) : v.kind == Value::String ? v.s : "";
}

// fnmatch.fnmatch on Windows: case-insensitive, '/' and '\' the same, '*' matches across separators
bool fnmatch(const std::string &name, const std::string &pat) {
    auto norm = [](std::string s) {
        for (char &c : s) c = c == '/' ? '\\' : char(std::tolower((unsigned char)c));
        return s;
    };
    const std::string n = norm(name), p = norm(pat);
    std::function<bool(size_t, size_t)> m = [&](size_t i, size_t j) -> bool {
        while (j < p.size()) {
            const char c = p[j];
            if (c == '*') {
                while (j < p.size() && p[j] == '*') j++;
                if (j == p.size()) return true;
                for (size_t k = i; k <= n.size(); k++)
                    if (m(k, j)) return true;
                return false;
            }
            if (i >= n.size()) return false;
            if (c == '?') {
                i++;
                j++;
                continue;
            }
            if (c == '[') {
                size_t e = p.find(']', j + 1);
                if (e != std::string::npos) {
                    bool neg = p[j + 1] == '!', ok = false;
                    for (size_t k = j + 1 + (neg ? 1 : 0); k < e; k++) {
                        if (k + 2 < e && p[k + 1] == '-') {
                            ok = ok || (n[i] >= p[k] && n[i] <= p[k + 2]);
                            k += 2;
                        } else
                            ok = ok || n[i] == p[k];
                    }
                    if (ok == neg) return false;
                    i++;
                    j = e + 1;
                    continue;
                }
            }
            if (n[i] != c) return false;
            i++;
            j++;
        }
        return i == n.size();
    };
    return m(0, 0);
}
bool has_magic(const std::string &s) { return s.find_first_of("*?[") != std::string::npos; }

// glob.glob(os.path.join(base, pat), recursive=True): '*' segments, '**' = any depth (files and folders), hidden
// names (leading '.') not matched by wildcards; results in directory order
std::vector<fs::path> glob(const fs::path &base, const std::string &pat) {
    const std::vector<std::string> segs = split(pat, '/');
    std::vector<fs::path> out;
    std::function<void(const fs::path &, size_t)> go = [&](const fs::path &dir, size_t k) {
        if (k == segs.size()) {
            out.push_back(dir);
            return;
        }
        const std::string &sg = segs[k];
        std::error_code ec;
        if (sg == "**") {
            // the folder itself, then every entry below it, depth first
            std::function<void(const fs::path &)> rl = [&](const fs::path &d) {
                std::error_code e2;
                for (const auto &e : fs::directory_iterator(d, e2)) {
                    const std::string nm = S(e.path().filename());
                    if (!nm.empty() && nm[0] == '.') continue;
                    if (k + 1 == segs.size()) out.push_back(e.path());
                    else go(e.path(), k + 1);
                    std::error_code e3;
                    if (e.is_directory(e3) && !e.is_symlink(e3)) rl(e.path());
                }
            };
            if (k + 1 == segs.size()) out.push_back(dir / "");
            else go(dir, k + 1);
            rl(dir);
            return;
        }
        if (!has_magic(sg)) {
            const fs::path nx = dir / P(sg);
            if (k + 1 == segs.size() ? present(nx) : is_dir(nx)) go(nx, k + 1);
            return;
        }
        for (const auto &e : fs::directory_iterator(dir, ec)) {
            const std::string nm = S(e.path().filename());
            if (!nm.empty() && nm[0] == '.' && sg[0] != '.') continue;
            if (!fnmatch(nm, sg)) continue;
            std::error_code e2;
            if (k + 1 < segs.size() && !e.is_directory(e2)) continue;
            go(e.path(), k + 1);
        }
    };
    go(base, 0);
    return out;
}

std::string rel_of(const fs::path &p, const fs::path &out) {   // os.path.relpath(p, out).replace('\\', '/')
    std::string s = S(p.lexically_relative(out).lexically_normal());
    for (char &c : s)
        if (c == '\\') c = '/';
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// ------------------------------------------------------------------------------------------------ read recording
std::mutex g_reads_m;
std::vector<std::string> g_read_roots;   // normcase'd, no trailing separator
std::set<std::string> g_read_parents, g_read_seen;
fs::path g_read_log;

}  // namespace

void record_reads(const std::vector<fs::path> &roots, const fs::path &log_path) {
    std::lock_guard<std::mutex> l(g_reads_m);
    g_read_roots.clear();
    g_read_parents.clear();
    for (const fs::path &r : roots) {
        if (!is_dir(r)) continue;
        const std::string a = lower(rstrip_seps(S(absp(r))));
        g_read_roots.push_back(a);
        g_read_parents.insert(S(P(a).parent_path()));
    }
    g_read_log = log_path;
}
void note_read(const fs::path &p) {
    std::lock_guard<std::mutex> l(g_reads_m);
    if (g_read_roots.empty() || g_read_log.empty()) return;
    const std::string a = S(absp(p)), q = lower(a);
    if (g_read_seen.count(q)) return;
    bool under = false;
    for (const std::string &r : g_read_roots) under = under || starts(q, r + "\\");
    if (!under && !g_read_parents.count(S(P(q).parent_path()))) return;
    g_read_seen.insert(q);
    std::ofstream f(g_read_log, std::ios::binary | std::ios::app);
    f << a << "\r\n";
}

// ------------------------------------------------------------------------------------------------ folders
fs::path patcher_dir() {
    // u3import.exe lives in u3mp/bin (package) or next to its sources; the patcher folder holds recipe/
    const fs::path here = exe_dir();
    for (const fs::path &c : {here.parent_path() / "patcher", here.parent_path(), here, here.parent_path().parent_path() / "u3mp" / "patcher"})
        if (is_file(c / "recipe" / "VERSION")) return c;
    if (const char *e = std::getenv("U3MP_PATCHER")) return P(e);
    throw u3::Stop("the multiplayer recipe (u3mp/patcher/recipe) was not found next to " + S(here));
}
fs::path recipe_dir() { return patcher_dir() / "recipe"; }
fs::path tools_dir() { return patcher_dir() / "tools"; }
std::string recipe_version() {
    const fs::path v = recipe_dir() / "VERSION";
    return is_file(v) ? strip(read_text(v)) : "0";
}

// ------------------------------------------------------------------------------------------------ PARAM.SFO
std::map<std::string, Value> parse_sfo(const fs::path &path) {
    note_read(path);
    const Bytes d = read_bytes(path);
    auto u16 = [&](size_t o) { if (o + 2 > d.size()) throw u3::Stop("unpack requires a buffer"); return uint32_t(d[o] | d[o + 1] << 8); };
    auto u32 = [&](size_t o) { if (o + 4 > d.size()) throw u3::Stop("unpack requires a buffer"); return uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | uint32_t(d[o + 3]) << 24); };
    if (d.size() < 20 || std::memcmp(d.data(), "\0PSF", 4) != 0) throw u3::Stop("not a PARAM.SFO: " + S(path));
    const uint32_t kto = u32(8), dto = u32(12), n = u32(16);
    std::map<std::string, Value> out;
    for (uint32_t i = 0; i < n; i++) {
        const size_t e = 20 + 16 * size_t(i);
        const uint32_t ko = u16(e), fm = u16(e + 2), ln = u32(e + 4), dof = u32(e + 12);
        size_t k = kto + ko, z = k;
        while (z < d.size() && d[z]) z++;
        if (z >= d.size()) throw u3::Stop("substring not found");
        const std::string key(d.begin() + std::ptrdiff_t(k), d.begin() + std::ptrdiff_t(z));
        const size_t a = std::min<size_t>(size_t(dto) + dof, d.size()), b = std::min<size_t>(a + ln, d.size());
        std::string v(d.begin() + std::ptrdiff_t(a), d.begin() + std::ptrdiff_t(b));
        if (fm == 0x0404) {
            if (v.size() < 4) throw u3::Stop("unpack requires a buffer of 4 bytes");
            out[key] = Value::integer((long long)(uint8_t(v[0]) | uint8_t(v[1]) << 8 | uint8_t(v[2]) << 16 | uint32_t(uint8_t(v[3])) << 24));
        } else {
            while (!v.empty() && v.back() == '\0') v.pop_back();
            out[key] = Value::text(v);
        }
    }
    return out;
}

std::optional<fs::path> find_sfo(const fs::path &folder) {
    for (const fs::path &c : {folder / "PARAM.SFO", folder / "param.sfo", folder / "sce_sys" / "param.sfo", folder.parent_path() / "PARAM.SFO",
                              folder.parent_path() / "param.sfo"})
        if (is_file(c)) return c;
    return std::nullopt;
}

// ------------------------------------------------------------------------------------------------ inputs
namespace {
struct Kind {
    const char *name, *desc;
    std::vector<const char *> markers;
};
const std::vector<Kind> KINDS = {
    {"disc", "U3 PS3 disc / full game data (PS3_GAME/USRDIR)", {"build/main/pak23.psarc", "build/main/actor33.psarc"}},
    {"update", "U3 PS3 title update 1.19 install (USRDIR with dlc-patch13.psarc + dlc-patch18.psarc)", {"dlc-patch18.psarc", "dlc-patch13.psarc"}},
    {"data2", "U3 PS3 game-data install written by the game/server (...DATA2/USRDIR: net18.bin, build/main/*-7.psarc)", {"net18.bin", "build/main/actor-parts-7.psarc"}},
    {"dlcmaps", "U3 PS3 DLC map packs install (USRDIR with dlc-map1..4.psarc)", {"dlc-map1.psarc", "dlc-map2.psarc", "dlc-map3.psarc", "dlc-map4.psarc"}},
};
bool has_any(const fs::path &d, const std::vector<const char *> &rels) {
    for (const char *r : rels)
        if (present(d / P(r))) return true;
    return false;
}
std::string sfo_title_lower(const std::map<std::string, Value> &info) {
    auto it = info.find("TITLE");
    std::string t = lower(it == info.end() ? "" : vstr(it->second));
    for (size_t k; (k = t.find("\xe2\x80\x99")) != std::string::npos;) t.replace(k, 3, "'");
    return t;
}
std::string sget(const std::map<std::string, Value> &m, const char *k) {
    auto it = m.find(k);
    return it == m.end() ? "" : vstr(it->second);
}
}  // namespace

std::vector<fs::path> scan_dirs(const std::vector<fs::path> &roots, int max_depth) {
    std::vector<fs::path> out;
    static const std::set<std::string> SKIP = {"movie1", "gallery1", "cinema23", "cinema5", "users", "http-cache"};
    for (const fs::path &r0 : roots) {
        const fs::path r = absp(r0);
        if (!is_dir(r)) continue;
        const size_t base = count_sep(rstrip_seps(S(r)));
        std::function<void(const fs::path &)> walk = [&](const fs::path &dp) {
            out.push_back(dp);
            std::vector<fs::path> dirs;
            std::error_code ec;
            for (const auto &e : fs::directory_iterator(dp, ec)) {
                std::error_code e2;
                if (e.is_directory(e2)) dirs.push_back(dp / e.path().filename());
            }
            if (count_sep(S(dp)) - base >= size_t(max_depth)) return;
            for (const fs::path &d : dirs) {
                if (SKIP.count(lower(S(d.filename())))) continue;
                std::error_code e3;
                if (fs::is_symlink(d, e3)) continue;
                walk(d);
            }
        };
        walk(r);
    }
    return out;
}

void Inputs::set_ps4(const fs::path &folder0) {
    const fs::path folder = absp(folder0);
    std::vector<std::string> missing;
    for (const char *n : {"eboot.bin", "sce_sys/param.sfo", "u3data/build/main/pak23_ps4.txt", "u3data/build/main/core.psarc", "u3data/build/main/dc1/ps4",
                          "u3data/build/main/pak23/ps4", "u3data/build/main/actor33/ps4", "u3data/build/main/sound1/streams", "u2data/build/main/sound1/streams"})
        if (!present(folder / P(n))) missing.push_back(n);
    if (!missing.empty()) {
        std::string l;
        for (const auto &m : missing) l += (l.empty() ? "" : ", ") + m;
        problems.push_back("PS4 folder " + S(folder) + " is missing: " + l);
        return;
    }
    ps4 = folder;
    try {
        ps4_sfo = parse_sfo(folder / "sce_sys" / "param.sfo");
    } catch (const std::exception &e) {
        problems.push_back(std::string("PS4 param.sfo unreadable: ") + e.what());
    }
    const std::string tid = sget(ps4_sfo, "TITLE_ID");
    bool ok = tid.size() == 9 && starts(tid, "CUSA");
    for (size_t k = 4; ok && k < 9; k++) ok = std::isdigit((unsigned char)tid[k]) != 0;
    if (!ok) problems.push_back("PS4 param.sfo title id '" + tid + "' does not look like a PS4 title (CUSAnnnnn)");
    if (lower(sget(ps4_sfo, "TITLE")).find("uncharted") == std::string::npos)
        notes.push_back("PS4 title is '" + sget(ps4_sfo, "TITLE") + "' (expected an Uncharted collection); continuing");
}

void Inputs::add_ps3_u3(const std::vector<fs::path> &folders, bool scan) {
    std::vector<fs::path> cands;
    if (scan) cands = scan_dirs(folders);
    else
        for (const fs::path &f : folders) cands.push_back(absp(f));
    static const std::vector<const char *> U3_WORDS = {"uncharted 3", "drake's deception", "drakes deception"};
    for (const fs::path &d : cands) {
        if (lower(S(d.filename())) != "usrdir" && !has_any(d, {"build/main", "dlc-patch18.psarc", "net18.bin"})) continue;
        for (const Kind &k : KINDS) {
            if (!has_any(d, k.markers) || ps3.count(k.name)) continue;
            const std::optional<fs::path> sfo = find_sfo(d);
            std::map<std::string, Value> info;
            if (sfo) {
                try {
                    info = parse_sfo(*sfo);
                } catch (const std::exception &) {
                    info.clear();
                }
            }
            const std::string title = sfo_title_lower(info);
            bool u3 = title.empty();
            for (const char *w : U3_WORDS) u3 = u3 || title.find(w) != std::string::npos;
            if (!u3) {
                notes.push_back(S(d) + ": PARAM.SFO title '" + sget(info, "TITLE") + "' is not Uncharted 3, folder skipped");
                continue;
            }
            ps3[k.name] = d;
            ps3_sfo[k.name] = {{"TITLE_ID", sget(info, "TITLE_ID")}, {"TITLE", sget(info, "TITLE")}, {"APP_VER", sget(info, "APP_VER")}, {"sfo", sfo ? S(*sfo) : ""}};
        }
    }
    if (!ps3.count("dlcmaps") && ps3.count("disc") && present(ps3["disc"] / "goty-dlc.psarc")) {
        // Game of the Year disc: goty-dlc.psarc carries the DLC map packs' level paks, so the disc stands in for dlc-map1..4
        ps3["dlcmaps"] = ps3["disc"];
        ps3_sfo["dlcmaps"] = ps3_sfo["disc"];
        notes.push_back("DLC map packs: not installed separately; using the Game of the Year disc (goty-dlc.psarc)");
    }
    for (const char *k : {"disc", "update"})
        if (!ps3.count(k))
            for (const Kind &x : KINDS)
                if (std::string(x.name) == k) problems.push_back(std::string("no PS3 Uncharted 3 ") + k + " found (" + x.desc + ")");
    for (const char *k : {"data2", "dlcmaps"})
        if (!ps3.count(k)) notes.push_back(std::string("no PS3 Uncharted 3 ") + k + " found; the steps that need it will be skipped");
}

void Inputs::set_ps3_u2(const std::vector<fs::path> &folders, bool scan) {
    std::vector<fs::path> cands;
    if (scan) cands = scan_dirs(folders);
    else
        for (const fs::path &f : folders) cands.push_back(absp(f));
    for (const fs::path &d : cands) {
        if (!has_any(d, {"build/main/pak22.psarc", "build/main/actor27.psarc", "build/main/dc1.psarc"})) continue;
        const std::optional<fs::path> sfo = find_sfo(d);
        std::map<std::string, Value> info;
        if (sfo) {
            try {
                info = parse_sfo(*sfo);
            } catch (const std::exception &) {
                info.clear();
            }
        }
        const std::string title = lower(sget(info, "TITLE"));
        if (!title.empty() && title.find("uncharted 2") == std::string::npos && title.find("among thieves") == std::string::npos) continue;
        u2 = d;
        u2_sfo = {{"TITLE_ID", sget(info, "TITLE_ID")}, {"TITLE", sget(info, "TITLE")}, {"APP_VER", sget(info, "APP_VER")}, {"sfo", sfo ? S(*sfo) : ""}};
        return;
    }
    problems.push_back("no PS3 Uncharted 2 game data found (a PS3_GAME/USRDIR or RPCS3 USRDIR with build/main/pak22.psarc or actor27.psarc)");
}

std::vector<std::string> Inputs::speech_langs() const {
    std::vector<std::string> out;
    auto it = ps3.find("disc");
    if (it == ps3.end()) return out;
    const fs::path sd = it->second / "build" / "main" / "sound1" / "streams" / "speech";
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(sd, ec)) {
        const std::string f = S(e.path().filename());
        if (ends(f, "-mp.psarc")) out.push_back(f.substr(0, f.size() - 9));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::pair<std::string, std::string>> Inputs::placeholders(const fs::path &out0) const {
    const fs::path out = absp(out0);
    auto k = [&](const char *n) { auto it = ps3.find(n); return it == ps3.end() ? std::string() : fwd(it->second); };
    return {{"PROJECT", fwd(out)},
            {"WORK", fwd(out / "work")},
            {"SCRATCH", fwd(out / "scratch")},
            {"PS4", ps4 ? fwd(*ps4) : ""},
            {"PS3_DISC", k("disc")},
            {"PS3_UPDATE", k("update")},
            {"PS3_DATA2", k("data2")},
            {"PS3_DLCMAPS", k("dlcmaps")},
            {"PS3_U2", u2 ? fwd(*u2) : ""},
            {"PAKTOOLS", fwd(tools_dir() / "paktools")},
            {"TRANSFER_TOOLS", fwd(tools_dir() / "transfer-tools")},
            {"RECOVERED", fwd(out / "scratch")}};
}

std::string Inputs::describe() const {
    std::string o = "Inputs:";
    if (ps4) o += "\n  PS4  " + S(*ps4) + "  [" + (ps4_sfo.count("TITLE_ID") ? sget(ps4_sfo, "TITLE_ID") : "CUSA?????") + " " + sget(ps4_sfo, "TITLE") + "]";
    for (const Kind &k : KINDS) {
        auto it = ps3.find(k.name);
        if (it == ps3.end()) continue;
        const auto &s = ps3_sfo.at(k.name);
        o += "\n  PS3 U3 " + pad8(k.name) + " " + S(it->second) + "  [" + s.at("TITLE_ID") + " " + first_line(s.at("TITLE")) + " " + s.at("APP_VER") + "]";
    }
    if (u2) o += "\n  PS3 U2 " + pad8("") + " " + S(*u2) + "  [" + u2_sfo.at("TITLE_ID") + " " + first_line(u2_sfo.at("TITLE")) + " " + u2_sfo.at("APP_VER") + "]";
    for (const auto &n : notes) o += "\n  note: " + n;
    for (const auto &p : problems) o += "\n  PROBLEM: " + p;
    return o;
}

// ------------------------------------------------------------------------------------------------ input checks
namespace {
std::set<std::string> accepted_net18_sha256() {
    // the net18.bin versions the DC recipes accept (tools/dcconv/net18-versions.json), else the one they were made from
    std::set<std::string> out;
    if (auto v = try_json(tools_dir() / "dcconv" / "net18-versions.json"))
        if (const Value *a = v->get("accepted")) {
            for (const Value &x : a->items) out.insert(x.s);
            return out;
        }
    try {
        const std::string gz = read_text(tools_dir() / "dcconv" / "dc-recipes.json.gz");
        Value recipes;
        if (!json::parse(inflate::gunzip(gz), recipes)) return out;
        for (const auto &[k, rec] : recipes.fields)
            if (const Value *s = rec.get("sources"))
                if (const Value *h = s->get("ps3/data2:dc1/ps4/net.bin"); h && h->truthy()) {
                    out.insert(h->s);
                    return out;
                }
    } catch (const std::exception &) {
    } catch (const inflate::InflateError &) {
    }
    return out;
}
}  // namespace

std::vector<std::string> full_import_problems(Inputs &inp) {
    std::vector<std::string> out;
    if (!inp.ps3.count("dlcmaps"))
        out.push_back("the Uncharted 3 DLC map packs are not installed (no dlc-map1..4.psarc found in the folders given). Install the DLC map packs in "
                      "RPCS3 (they go to dev_hdd0/game/<the DLC title id>, e.g. BCES01176) and add RPCS3's dev_hdd0/game folder.");
    auto it = inp.ps3.find("data2");
    if (it == inp.ps3.end()) {
        out.push_back("the Uncharted 3 game data folder (dev_hdd0/game/BCES01670DATA2) was not found. Enter the community server DNS in \"Set up "
                      "multiplayer\" and import again: the import then downloads it, as the game does when it goes online. Or start Uncharted 3 in "
                      "RPCS3 with update 1.19 and go online once, so the game writes it, then add dev_hdd0/game.");
        return out;
    }
    const fs::path d2 = it->second;
    std::vector<std::string> missing;
    for (const char *r : {"net18.bin", "build/main/actor-parts-7.psarc"})
        if (!present(d2 / P(r))) missing.push_back(r);
    if (!missing.empty()) {
        std::string l;
        for (const auto &m : missing) l += (l.empty() ? "" : ", ") + m;
        out.push_back("the game data folder " + S(d2) + " is incomplete (missing " + l +
                      "): it is written by Uncharted 3 when it goes online in RPCS3 (net tables and map updates). Enter the community server DNS in "
                      "\"Set up multiplayer\" and import again, so the import downloads it; or go online in Uncharted 3 multiplayer once in RPCS3 "
                      "with update 1.19, then import again.");
        return out;
    }
    const std::set<std::string> want = accepted_net18_sha256();
    note_read(d2 / "net18.bin");
    const std::string have = sha256_file(d2 / "net18.bin");
    if (!want.empty() && !want.count(have))
        out.push_back("the game data folder " + S(d2) + " holds a net18.bin version this recipe does not know yet (sha256 " + have +
                      "): the menus and modes cannot be converted from it. Report this hash so the version can be added; until then Uncharted 3 "
                      "starts without multiplayer.");
    return out;
}

// ------------------------------------------------------------------------------------------------ steps
namespace {

struct Row {
    std::map<std::string, std::string> f;
    const std::string &at(const char *k) const {
        static const std::string empty;
        auto it = f.find(k);
        return it == f.end() ? empty : it->second;
    }
};
std::vector<Row> read_tsv(const fs::path &path) {
    std::vector<Row> rows;
    const std::string t = text_crlf_to_lf(read_text(path));
    const std::vector<std::string> lines = split(t, '\n');
    if (lines.empty()) return rows;
    const std::vector<std::string> head = split(lines[0], '\t');
    for (size_t i = 1; i < lines.size(); i++) {
        if (strip(lines[i]).empty()) continue;
        const std::vector<std::string> c = split(lines[i], '\t');
        Row r;
        for (size_t k = 0; k < head.size() && k < c.size(); k++) r.f[head[k]] = c[k];
        rows.push_back(std::move(r));
    }
    return rows;
}

std::string val_s(const Value &s, const char *k, const std::string &def = "") {
    const Value *v = s.get(k);
    return v && v->kind == Value::String ? v->s : def;
}
bool val_b(const Value &s, const char *k, bool def) {
    const Value *v = s.get(k);
    return v ? v->truthy() : def;
}
std::vector<std::string> val_list(const Value &s, const char *k) {
    std::vector<std::string> out;
    if (const Value *v = s.get(k))
        for (const Value &x : v->items) out.push_back(x.s);
    return out;
}

std::vector<Value> load_steps(const std::function<void(const std::string &)> &print) {
    const fs::path steps = recipe_dir() / "steps";
    std::vector<std::string> order;
    for (const std::string &line : split(text_crlf_to_lf(read_text(steps / "ORDER.txt")), '\n')) {
        const std::string l = strip(line.substr(0, line.find('#')));
        if (!l.empty()) order.push_back(l);
    }
    std::map<std::string, Value> files;
    std::vector<std::string> ids;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(steps, ec)) {
        const std::string f = S(e.path().filename());
        if (!ends(f, ".json")) continue;
        Value s = load_json(e.path());
        s.set("_file", str(f));
        const std::string id = val_s(s, "id");
        if (!files.count(id)) ids.push_back(id);
        files[id] = s;
    }
    std::vector<std::string> missing, unlisted;
    for (const auto &i : order)
        if (!files.count(i)) missing.push_back(i);
    if (!missing.empty()) {
        std::string l;
        for (const auto &m : missing) l += (l.empty() ? "" : ", ") + ("'" + m + "'");
        throw u3::Stop("steps/ORDER.txt names steps without a json file: [" + l + "]");
    }
    for (const auto &i : ids)
        if (std::find(order.begin(), order.end(), i) == order.end()) unlisted.push_back(i);
    if (!unlisted.empty()) {
        std::string l;
        for (const auto &m : unlisted) l += (l.empty() ? "" : ", ") + ("'" + m + "'");
        print("warning: step files not in ORDER.txt (ignored): [" + l + "]");
    }
    std::vector<Value> out;
    for (const auto &i : order) out.push_back(files[i]);
    return out;
}

// step records: <out>/_recipe/state.json
struct State {
    fs::path path;
    Value d;
    explicit State(const fs::path &out) : path(out / "_recipe" / "state.json") {
        if (present(path)) d = load_json(path);
        else d = obj({{"recipe_version", str(recipe_version())}, {"steps", Value::object()}});
        if (!d.get("steps")) d.set("steps", Value::object());
    }
    void save() {
        fs::create_directories(path.parent_path());
        write_text(P(S(path) + ".tmp"), dump_indent1(d));
        fs::rename(P(S(path) + ".tmp"), path);
    }
    Value *done(const std::string &id) { return d.get("steps")->get(id); }
    void mark(const std::string &id, const Value &outputs, const std::string &status, const std::string &note = "", const std::string &fp = "") {
        d.get("steps")->set(id, obj({{"status", str(status)},
                                     {"when", str(now_text())},
                                     {"recipe_version", str(recipe_version())},
                                     {"outputs", outputs},
                                     {"note", str(note)},
                                     {"fp", str(fp)}}));
        save();
    }
};

// sha256 of a tool folder's files (relative path + content; text files with \n line ends)
std::map<std::string, std::string> g_tree_fp;
std::string tree_fp(const fs::path &folder) {
    const std::string key = S(folder);
    if (auto it = g_tree_fp.find(key); it != g_tree_fp.end()) return it->second;
    Sha256 h;
    std::function<void(const fs::path &)> walk = [&](const fs::path &dp) {
        std::vector<std::string> dirs, files;
        std::error_code ec;
        for (const auto &e : fs::directory_iterator(dp, ec)) {
            const std::string n = S(e.path().filename());
            std::error_code e2;
            if (e.is_directory(e2)) {
                if (n != "__pycache__") dirs.push_back(n);
            } else
                files.push_back(n);
        }
        std::sort(dirs.begin(), dirs.end());
        std::sort(files.begin(), files.end());
        for (const std::string &f : files) {
            if (ends(f, ".pyc") || ends(f, ".orig")) continue;
            const fs::path p = dp / P(f);
            std::string b = read_text(p);
            for (const char *x : {".py", ".sh", ".txt", ".json", ".tsv", ".md", ".cmd", ".c"})
                if (ends(f, x)) {
                    std::string o;
                    o.reserve(b.size());
                    for (size_t i = 0; i < b.size(); i++)
                        if (!(b[i] == '\r' && i + 1 < b.size() && b[i + 1] == '\n')) o += b[i];
                    b.swap(o);
                    break;
                }
            const std::string rel = rel_of(p, folder);
            h.update(rel.data(), rel.size());
            h.update("", 1);
            const Bytes dg = sha256_raw(b.data(), b.size());
            h.update(dg.data(), dg.size());
        }
        for (const std::string &d : dirs) walk(dp / P(d));
    };
    if (is_dir(folder)) walk(folder);
    return g_tree_fp[key] = h.hex();
}

std::string lf_sha_raw(const fs::path &p) {
    const std::string b = read_text(p);
    std::string o;
    o.reserve(b.size());
    for (size_t i = 0; i < b.size(); i++)
        if (!(b[i] == '\r' && i + 1 < b.size() && b[i + 1] == '\n')) o += b[i];
    const Bytes d = sha256_raw(o.data(), o.size());
    return std::string(d.begin(), d.end());
}

std::string step_fingerprint(const Value &s) {
    Value c = Value::object();
    for (const auto &[k, v] : s.fields)
        if (k.empty() || k[0] != '_') c.set(k, v);
    const std::string j = dumps(c, true);
    Sha256 h;
    h.update(j.data(), j.size());
    if (val_s(s, "kind") == "script") {
        std::vector<std::string> jobs = {val_s(s, "job")};
        for (const auto &w : val_list(s, "with_jobs")) jobs.push_back(w);
        for (const auto &jb : jobs) {
            const std::string t = tree_fp(tools_dir() / "fixes" / P(jb));
            h.update(t.data(), t.size());
        }
    } else if (!val_s(s, "list").empty()) {
        const fs::path p = recipe_dir() / "lists" / P(val_s(s, "list"));
        if (present(p)) {
            const std::string d = lf_sha_raw(p);
            h.update(d.data(), d.size());
        }
    }
    return h.hex();
}

bool outputs_unchanged(const fs::path &out, const Value &rec) {
    if (const Value *o = rec.get("outputs"))
        for (const auto &[rel, hv] : o->fields) {
            const fs::path p = out / P(rel);
            if (hv.kind == Value::Null) {
                if (present(p)) return false;
                continue;
            }
            if (!present(p) || sha256_cached(p) != hv.s) return false;
        }
    return true;
}

// ------------------------------------------------------------------------------------------------ rendering
bool is_ph_char(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

std::string fill(const std::string &text, const std::vector<std::pair<std::string, std::string>> &values, const std::string &where,
                 const std::function<void(const std::string &)> &lg) {
    auto value_of = [&](const std::string &k) -> const std::string * {
        for (const auto &[n, v] : values)
            if (n == k) return &v;
        return nullptr;
    };
    // the placeholders it names
    std::set<std::string> unknown;
    struct M {
        size_t a, b;
        std::string k;
    };
    std::vector<M> ms;
    for (size_t i = 0; (i = text.find("{{", i)) != std::string::npos;) {
        size_t j = i + 2;
        while (j < text.size() && is_ph_char(text[j])) j++;
        if (j > i + 2 && text.compare(j, 2, "}}") == 0) {
            ms.push_back({i, j + 2, text.substr(i + 2, j - i - 2)});
            if (!value_of(ms.back().k)) unknown.insert(ms.back().k);
            i = j + 2;
        } else
            i++;
    }
    if (!unknown.empty()) {
        std::string l;
        for (const auto &u : unknown) l += (l.empty() ? "" : ", ") + ("'" + u + "'");
        lg("  warning: " + where + " has unknown placeholders [" + l + "]");
    }
    std::string o;
    size_t last = 0;
    for (const M &m : ms) {
        o.append(text, last, m.a - last);
        last = m.b;
        const std::string *v = value_of(m.k);
        if (!v) {
            o.append(text, m.a, m.b - m.a);
            continue;
        }
        std::string val = *v;
        if (val.find('\'') != std::string::npos) {
            // inside a single-quoted literal an apostrophe in the value is escaped
            const size_t ls = text.rfind('\n', m.a == 0 ? 0 : m.a - 1);
            const size_t line_start = (ls == std::string::npos || (m.a == 0)) ? 0 : ls + 1;
            const std::string before = text.substr(line_start, m.a - line_start);
            const size_t q1 = before.rfind('\''), q2 = before.rfind('"');
            const size_t q = q1 == std::string::npos ? q2 : q2 == std::string::npos ? q1 : std::max(q1, q2);
            if (q != std::string::npos && before[q] == '\'') {
                if (q > 0 && (before[q - 1] == 'r' || before[q - 1] == 'R'))
                    lg("  warning: " + where + ": raw single-quoted literal cannot hold \"" + val + "\"");
                std::string e;
                for (char c : val) e += c == '\'' ? std::string("\\'") : std::string(1, c);
                val = e;
            }
        }
        o += val;
    }
    o.append(text, last, std::string::npos);
    return o;
}

bool render_text(const std::string &f) {
    for (const char *x : {".py", ".sh", ".txt", ".json", ".tsv", ".md", ".cmd"})
        if (ends(f, x)) return true;
    return false;
}

fs::path render_job(const std::string &job, const fs::path &out, const std::vector<std::pair<std::string, std::string>> &values,
                    const std::function<void(const std::string &)> &lg) {
    const fs::path fixes = tools_dir() / "fixes", src = fixes / P(job), dst = out / "scratch" / P(job);
    if (!is_dir(src)) throw u3::Stop("fix job folder missing: " + S(src));
    if (is_dir(dst)) rmtree(dst);
    fs::create_directories(out / "scratch");
    std::function<void(const fs::path &)> walk = [&](const fs::path &dp) {
        const std::string rel = dp == src ? "" : rel_of(dp, src);
        const fs::path dd = rel.empty() ? dst : dst / P(rel);
        fs::create_directories(dd);
        std::vector<fs::path> dirs;
        std::error_code e3;
        for (const auto &e : fs::directory_iterator(dp, e3)) {
            std::error_code e4;
            if (e.is_directory(e4)) {
                dirs.push_back(e.path());
                continue;
            }
            const std::string f = S(e.path().filename());
            if (render_text(f)) write_text(dd / P(f), fill(text_crlf_to_lf(read_text(e.path())), values, job + "/" + (rel.empty() ? "" : rel + "/") + f, lg));
            else if (!CopyFileW(e.path().c_str(), (dd / P(f)).c_str(), FALSE))
                throw u3::Stop("cannot copy " + S(e.path()));
        }
        for (const fs::path &d : dirs) walk(d);
    };
    walk(src);
    return dst;
}

// ------------------------------------------------------------------------------------------------ running a command
struct Captured {
    int code = 0;
    std::string out, err;
};
Captured run_capture(const std::vector<std::string> &argv, const fs::path &cwd, const std::vector<std::pair<std::string, std::string>> &env) {
    Captured c;   // the step's output is shown as stdout + stderr
    c.code = run_split(argv, cwd, env, c.out, c.err);
    return c;
}

std::vector<std::string> py_splitlines(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); i++) {
        const char ch = s[i];
        if (ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f' || ch == '\x1c' || ch == '\x1d' || ch == '\x1e') {
            out.push_back(cur);
            cur.clear();
            if (ch == '\r' && i + 1 < s.size() && s[i + 1] == '\n') i++;
        } else
            cur += ch;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
std::string cut_chars(const std::string &s, size_t n) {   // the first n code points of UTF-8 text
    size_t i = 0, k = 0;
    while (i < s.size() && k < n) {
        const unsigned char c = (unsigned char)s[i];
        i += c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        k++;
    }
    return s.substr(0, std::min(i, s.size()));
}
std::string pystrip(const std::string &s) {
    size_t a = 0, b = s.size();
    auto sp = [](unsigned char c) { return c == ' ' || (c >= 9 && c <= 13) || (c >= 0x1c && c <= 0x1f); };
    while (a < b && sp((unsigned char)s[a])) a++;
    while (b > a && sp((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string basename_of(const std::string &a) {
    const size_t k = a.find_last_of("/\\");
    return k == std::string::npos ? a : a.substr(k + 1);
}

struct InputMissing : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ------------------------------------------------------------------------------------------------ copy lists
Value step_copy_list(const Value &s, const Inputs &inp, const fs::path &out, const std::vector<std::string> &langs,
                     const std::function<void(const std::string &)> &lg) {
    const std::vector<Row> rows = read_tsv(recipe_dir() / "lists" / P(val_s(s, "list")));
    auto kind = [&](const char *k) -> std::optional<fs::path> {
        auto it = inp.ps3.find(k);
        return it == inp.ps3.end() ? std::nullopt : std::optional<fs::path>(it->second);
    };
    std::vector<fs::path> arch_dirs;
    for (const char *k : {"disc", "update", "data2", "dlcmaps"})
        if (auto d = kind(k))
            for (const fs::path &x : {*d, *d / "build" / "main", *d / "build" / "main" / "pak23", *d / "build" / "main" / "sound1" / "streams",
                                      *d / "build" / "main" / "sound1" / "streams" / "speech"})
                arch_dirs.push_back(x);
    std::map<std::string, std::unique_ptr<u3::Psarc>> arch;
    auto archive = [&](const std::string &name) -> u3::Psarc & {
        auto it = arch.find(name);
        if (it != arch.end()) return *it->second;
        for (const fs::path &d : arch_dirs) {
            const fs::path p = d / P(name);
            if (is_file(p)) {
                note_read(p);
                return *(arch[name] = std::make_unique<u3::Psarc>(p));
            }
        }
        throw std::out_of_range("archive " + name + " not found in any PS3 input");
    };
    Value outputs = Value::object();
    size_t n = 0;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> bad;
    std::vector<std::string> variants = {""};
    if (val_b(s, "speech", false))
        for (const auto &l : langs)
            if (l != "english") variants.push_back(l);
    auto replace_all = [](std::string t, const std::string &a, const std::string &b) {
        for (size_t k = 0; (k = t.find(a, k)) != std::string::npos; k += b.size()) t.replace(k, a.size(), b);
        return t;
    };
    for (const Row &r : rows)
        for (const std::string &lv : variants) {
            std::string dest = r.at("dest"), arc = r.at("archive"), ent = r.at("entry");
            if (!lv.empty()) {
                dest = replace_all(dest, "/speech/english/", "/speech/" + lv + "/");
                arc = replace_all(replace_all(arc, "english-mp.psarc", lv + "-mp.psarc"), "goty-speech-english", "goty-speech-" + lv);
                ent = replace_all(ent, "/speech/english/", "/speech/" + lv + "/");
            }
            const fs::path dp = out / "work" / P(r.at("overlay")) / P(dest);
            const std::string rel = rel_of(dp, out);
            if (lv.empty() && present(dp) && sha256_cached(dp) == r.at("sha256")) {
                outputs.set(rel, str(r.at("sha256")));
                continue;
            }
            Bytes data;
            try {
                if (arc == "loose") {
                    std::optional<fs::path> base;
                    if (r.at("source_kind") == "ps3-u3-disc-loose") base = kind("disc");
                    else if (r.at("source_kind") == "ps4-retail") base = inp.ps4;
                    else throw u3::Stop("KeyError: '" + r.at("source_kind") + "'");
                    if (!base) throw u3::Stop("TypeError: no " + r.at("source_kind") + " folder");
                    const fs::path src = *base / P(ent);
                    if (!is_file(src)) throw std::out_of_range("[Errno 2] No such file or directory: '" + S(src) + "'");
                    note_read(src);
                    data = read_bytes(src);
                } else {
                    u3::Psarc &ps = archive(arc);
                    auto f = std::find(ps.names.begin(), ps.names.end(), ent);
                    if (f == ps.names.end()) f = std::find(ps.names.begin(), ps.names.end(), "/" + ent);
                    if (f == ps.names.end()) throw std::out_of_range(arc + " has no entry " + ent);
                    data = ps.read(size_t(f - ps.names.begin()) + 1);
                }
            } catch (const std::out_of_range &e) {
                if (!lv.empty()) continue;   // a language the disc does not have
                bad.push_back(e.what());
                continue;
            }
            const std::string h = sha256_hex(data);
            if (lv.empty() && h != r.at("sha256")) {
                bad.push_back(dest + ": content differs from the reference (" + h.substr(0, 12) + ")");
                continue;
            }
            fs::create_directories(dp.parent_path());
            write_bytes(P(S(dp) + ".tmp"), data);
            fs::rename(P(S(dp) + ".tmp"), dp);
            outputs.set(rel, str(h));
            if (++n % 1000 == 0)
                lg(fmt("        %zu files, %.0f s", n, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()));
        }
    if (!bad.empty()) {
        lg(fmt("        %zu file(s) could not be copied:", bad.size()));
        for (size_t i = 0; i < bad.size() && i < 20; i++) lg("          " + bad[i]);
        const Value *tol = s.get("tolerate_missing");
        const double t = tol ? (tol->kind == Value::Float ? tol->f : double(tol->i)) : 0.0;
        if (double(bad.size()) > double(rows.size()) * t) throw u3::Stop(fmt("%zu of %zu files missing or different", bad.size(), rows.size()));
    }
    return outputs;
}

// ------------------------------------------------------------------------------------------------ script steps
Value step_script(const Value &s, const Inputs &inp, const fs::path &out, const std::vector<std::pair<std::string, std::string>> &values,
                  const std::vector<std::pair<std::string, std::string>> &base_env, const std::function<void(const std::string &)> &lg) {
    const std::string job = val_s(s, "job");
    const fs::path jd = render_job(job, out, values, lg);
    std::vector<std::string> extra;
    for (const auto &j : val_list(s, "with_jobs")) extra.push_back(S(render_job(j, out, values, lg)));
    const std::vector<std::string> writes = val_list(s, "writes");
    std::map<std::string, std::string> before;
    std::vector<std::string> before_order;
    for (const auto &pat : writes)
        for (const fs::path &p : glob(out, pat))
            if (is_file(p)) {
                const std::string k = S(p);
                if (!before.count(k)) before_order.push_back(k);
                before[k] = sha256_cached(p);
            }
    auto vget = [&](const char *k) {
        for (const auto &[n, v] : values)
            if (n == k) return v;
        return std::string();
    };
    std::vector<std::pair<std::string, std::string>> env = base_env;
    env.push_back({"PKT", vget("PAKTOOLS")});
    env.push_back({"PAKTOOLS", vget("PAKTOOLS")});
    for (const auto &[k, v] : values) env.push_back({"RECIPE_" + k, v});
    const std::string jds = fwd(jd);
    for (const Value &cmdv : s.get("commands")->items) {
        std::vector<std::string> argv;
        for (const Value &c0 : cmdv.items) {
            std::string c = c0.s;
            auto rep = [&](const std::string &a, const std::string &b) {
                for (size_t k = 0; (k = c.find(a, k)) != std::string::npos; k += b.size()) c.replace(k, a.size(), b);
            };
            rep("{{OUT}}", S(out));
            rep("{{JOB}}", S(jd));
            for (const auto &[k, v] : values) rep("{{" + k + "}}", v);
            if (!c.empty() && c[0] == '@' && is_file(P(c.substr(1)))) c = strip(text_crlf_to_lf(read_text(P(c.substr(1)))));
            argv.push_back(c);
        }
        std::string joined;
        for (const auto &x : argv) joined += (joined.empty() ? "" : " ") + x;
        if (joined.size() >= 300) {
            joined.clear();
            for (size_t i = 0; i < argv.size() && i < 4; i++) joined += (joined.empty() ? "" : " ") + argv[i];
            joined += " ...";
        }
        lg("        $ " + joined);
        // the step programs: u3fix.exe next to u3import.exe (the package's u3mp/bin), else on PATH
        if (!argv.empty() && argv[0] == "u3fix.exe" && is_file(exe_dir() / "u3fix.exe")) argv[0] = S(exe_dir() / "u3fix.exe");
        fs::path cwd = jd;
        if (!val_s(s, "cwd").empty()) {
            std::string c = val_s(s, "cwd");
            for (const auto &[a, b] : {std::pair<std::string, std::string>{"{{OUT}}", S(out)}, {"{{JOB}}", S(jd)}})
                for (size_t k = 0; (k = c.find(a, k)) != std::string::npos; k += b.size()) c.replace(k, a.size(), b);
            cwd = P(c);
        }
        const Captured r = run_capture(argv, cwd, env);
        std::vector<std::string> tail = py_splitlines(pystrip(r.out + r.err));
        const Value *ll = s.get("log_lines");
        const size_t keep = ll ? size_t(ll->i) : 8;
        for (size_t i = tail.size() > keep ? tail.size() - keep : 0; i < tail.size(); i++) lg("          | " + cut_chars(tail[i], 220));
        std::vector<long long> ok_codes = {0};
        if (const Value *oc = s.get("ok_codes")) {
            ok_codes.clear();
            for (const Value &x : oc->items) ok_codes.push_back(x.i);
        }
        const std::string who = basename_of(argv.size() > 1 ? argv[1] : argv[0]);
        if (std::find(ok_codes.begin(), ok_codes.end(), (long long)r.code) == ok_codes.end()) throw u3::Stop(fmt("%s exited %d", who.c_str(), r.code));
        if (r.code) throw InputMissing(fmt("%s exited %d", who.c_str(), r.code));
        const std::string expect = val_s(s, "expect");
        if (!expect.empty()) {
            bool found = false;
            for (const auto &l : tail) found = found || l.find(expect) != std::string::npos;
            if (!found) {
                std::string a12;
                for (size_t i = 1; i < argv.size() && i < 3; i++) a12 += (a12.empty() ? "" : ", ") + ("'" + argv[i] + "'");
                throw u3::Stop("expected '" + expect + "' in the output of [" + a12 + "]");
            }
        }
    }
    Value outputs = Value::object();
    for (const auto &pat : writes)
        for (const fs::path &p : glob(out, pat))
            if (is_file(p)) {
                const std::string h = sha256_cached(p);
                auto b = before.find(S(p));
                if (b == before.end() || b->second != h) outputs.set(rel_of(p, out), str(h));
            }
    if (val_b(s, "must_write", false) && outputs.fields.empty()) {
        std::string l;
        for (const auto &w : writes) l += (l.empty() ? "" : ", ") + ("'" + w + "'");
        throw u3::Stop("the step changed no file under [" + l + "]");
    }
    if (outputs.fields.empty()) {
        // exit 0 without a change: remember the current hashes of the files the step owns
        for (const auto &pat : writes)
            for (const fs::path &p : glob(out, pat))
                if (is_file(p) && before.count(S(p))) outputs.set(rel_of(p, out), str(before[S(p)]));
        lg("        no change (already applied)");
    }
    return outputs;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ the recipe
int rebuild_overlays(const RecipeArgs &a, const std::function<void(const std::string &)> &print) {
    const std::vector<Value> steps = load_steps(print);
    if (a.ps4.empty() || a.ps3_u3.empty() || a.out.empty()) throw u3::Stop("--ps4, --ps3-u3 and --out are required");
    Inputs inp;
    inp.set_ps4(P(a.ps4));
    std::vector<fs::path> u3f, u2f;
    for (const auto &f : a.ps3_u3) u3f.push_back(P(f));
    for (const auto &f : a.ps3_u2) u2f.push_back(P(f));
    inp.add_ps3_u3(u3f);
    if (!u2f.empty()) inp.set_ps3_u2(u2f);
    else if (!a.allow_missing_u2) inp.problems.push_back("--ps3-u2 is required: point it at your PS3 Uncharted 2 game data");
    for (const auto &l : split(inp.describe(), '\n')) print(l);
    if (a.only.empty() && !a.allow_partial_inputs) {
        // A full import needs every PS3 source: without them the map conversions are skipped and the DC recipes fail,
        // and the half-made overlays do not start. Checked here, before anything is written.
        const std::vector<std::string> extra = full_import_problems(inp);
        for (const auto &p : extra) print("  PROBLEM: " + p);
        inp.problems.insert(inp.problems.end(), extra.begin(), extra.end());
    }
    if (!inp.problems.empty()) {
        print("");
        print("Cannot start: fix the problems above.");
        return 2;
    }
    const fs::path out = absp(P(a.out));
    fs::create_directories(out / "_recipe");
    std::ofstream logf(out / "_recipe" / "log.txt", std::ios::binary | std::ios::app);
    auto lg = [&](const std::string &msg) {
        print(msg);
        std::string m = msg;
        for (size_t k = 0; (k = m.find('\n', k)) != std::string::npos; k += 2) m.replace(k, 1, "\r\n");
        logf << m << "\r\n";
        logf.flush();
    };
    lg("");
    lg("=== rebuild_overlays recipe " + recipe_version() + "  " + now_text());
    lg(inp.describe());
    auto values = inp.placeholders(out);
    std::vector<std::string> langs;
    if (a.speech_langs == "all") langs = inp.speech_langs();
    else
        for (const auto &l : split(a.speech_langs, ','))
            if (!strip(l).empty()) langs.push_back(strip(l));
    std::string sl;
    for (const auto &l : langs) sl += (sl.empty() ? "" : ",") + l;
    values.push_back({"SPEECH_LANGS", sl});
    State state(out);
    const std::vector<Row> man_rows = read_tsv(recipe_dir() / "manifest.tsv");
    std::vector<std::pair<std::string, std::string>> manifest;   // rel -> sha256 (first position, last value)
    std::map<std::string, size_t> man_at;
    for (const Row &m : man_rows) {
        const std::string rel = "work/" + m.at("overlay") + "/" + m.at("path");
        auto it = man_at.find(rel);
        if (it == man_at.end()) {
            man_at[rel] = manifest.size();
            manifest.push_back({rel, m.at("sha256")});
        } else
            manifest[it->second].second = m.at("sha256");
    }
    // level / actor paks a higher overlay hides: never loaded by the game, so the builders skip them (RECIPE_SHADOWED)
    std::set<std::pair<std::string, std::string>> hidden;
    if (!a.all_files) {
        std::map<std::string, std::set<std::string>> have;
        for (const Row &m : man_rows) have[m.at("overlay")].insert(lower(m.at("path")));
        for (const Row &m : man_rows) {
            const std::string &p = m.at("path");
            if (!starts(p, "u3data/build/main/pak23/ps4/") && !starts(p, "u3data/build/main/actor33/ps4/")) continue;
            auto oi = std::find(OVERLAYS.begin(), OVERLAYS.end(), m.at("overlay"));
            if (oi == OVERLAYS.end()) continue;
            for (auto hi = OVERLAYS.begin(); hi != oi; ++hi)
                if (have[*hi].count(lower(p))) {
                    hidden.insert({m.at("overlay"), p});
                    break;
                }
        }
    }
    const fs::path sf = out / "_recipe" / "shadowed.tsv";
    {
        std::string t;
        for (const auto &[o, p] : hidden) t += o + "\t" + p + "\n";
        write_text(sf, t);
    }
    std::vector<std::pair<std::string, std::string>> env = a.env;
    env.push_back({"RECIPE_SHADOWED", S(sf)});
    env.push_back({"RECIPE_MANIFEST", S(recipe_dir() / "manifest.tsv")});
    if (!hidden.empty()) lg(fmt("%zu level/actor paks hidden by a higher overlay are not built (--all-files builds them too)", hidden.size()));
    std::set<std::string> hidden_rel;
    for (const auto &[o, p] : hidden) hidden_rel.insert("work/" + o + "/" + p);
    const int cores = int(std::thread::hardware_concurrency());
    const std::string jobs = std::to_string(a.jobs > 0 ? a.jobs : std::max(1, (cores ? cores : 3) - 2));
    env.push_back({"RECIPE_JOBS", jobs});
    lg("parallel steps use " + jobs + " worker processes");
    std::set<std::string> only, skip;
    if (!a.only.empty())
        for (const auto &x : split(a.only, ',')) only.insert(x);
    if (!a.skip.empty())
        for (const auto &x : split(a.skip, ',')) skip.insert(x);
    bool started = a.from.empty();
    int c_ok = 0, c_skipped = 0, c_already = 0, c_failed = 0, c_unavail = 0;
    const auto t_all = std::chrono::steady_clock::now();
    for (size_t i = 0; i < steps.size(); i++) {
        const Value &s = steps[i];
        const std::string sid = val_s(s, "id"), kind = val_s(s, "kind");
        if (!started) {
            if (sid == a.from) started = true;
            else continue;
        }
        const std::string head = fmt("[%2zu/%zu] ", i + 1, steps.size()) + sid + " - " + val_s(s, "title");
        if ((!only.empty() && !only.count(sid)) || skip.count(sid)) {
            lg(head + ": skipped (selection)");
            c_skipped++;
            continue;
        }
        std::vector<std::string> missing;
        for (const auto &n : val_list(s, "needs")) {
            bool ok = false;
            if (n == "ps4") ok = inp.ps4.has_value();
            else if (n == "ps3_u2") ok = inp.u2.has_value();
            else if (starts(n, "ps3_")) ok = inp.ps3.count(n.substr(4)) != 0;
            else if (starts(n, "tool:")) ok = present(tools_dir() / P(n.substr(5)));
            if (!ok) missing.push_back(n);
        }
        if (!missing.empty()) {
            std::string l;
            for (const auto &m : missing) l += (l.empty() ? "" : ", ") + m;
            lg(head + ": SKIP, input not available: " + l);
            c_unavail++;
            continue;
        }
        if (kind == "unavailable") {
            lg(head + ": NOT REPRODUCIBLE HERE - " + val_s(s, "reason"));
            c_unavail++;
            continue;
        }
        Value *rec = state.done(sid);
        const std::string fp = step_fingerprint(s);
        if (rec && !rec->get("fp") && val_s(*rec, "status") == "ok") {
            rec->set("fp", str(fp));   // recorded by recipe <= 1.7.0 (no fingerprints)
            state.save();
            rec = state.done(sid);
        }
        // recipe <= 1.9.6 recorded a step found at the reference state with no outputs: checked again
        const bool stale_ref = rec && val_s(*rec, "note") == "files already at the reference state" && !(rec->get("outputs") && rec->get("outputs")->truthy());
        if (rec && !a.force && val_s(*rec, "status") == "ok" && val_s(*rec, "fp") == fp && !stale_ref && outputs_unchanged(out, *rec)) {
            lg(head + ": already applied (" + val_s(*rec, "when") + ")");
            c_already++;
            continue;
        }
        if (rec && val_s(*rec, "status") == "ok" && val_s(*rec, "fp") != fp) lg(head + ": updated since it last ran, running it again");
        // "reference_check": false = a step newer than manifest.tsv: its files at the reference state still need it
        if (kind == "script" && !a.force && val_b(s, "reference_check", true)) {
            const std::vector<std::string> pats = val_list(s, "writes");
            Value ref = Value::object();
            bool okref = !pats.empty();
            if (okref) {
                std::vector<const std::pair<std::string, std::string> *> covered;
                for (const auto &m : manifest) {
                    if (hidden_rel.count(m.first)) continue;
                    for (const auto &pt : pats)
                        if (fnmatch(m.first, pt)) {
                            covered.push_back(&m);
                            break;
                        }
                }
                okref = !covered.empty();
                for (const auto *m : covered) {
                    const fs::path p = out / P(m->first);
                    if (!okref) break;
                    if (!present(p) || sha256_cached(p) != m->second) okref = false;
                    else ref.set(m->first, str(m->second));
                }
            }
            if (okref) {
                state.mark(sid, ref, "ok", "files already at the reference state", fp);
                lg(head + ": already at the reference state (manifest hashes match)");
                c_already++;
                continue;
            }
        }
        if (a.dry_run) {
            lg(head + ": would run");
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        lg(head + " ...");
        try {
            Value outputs;
            if (kind == "copy-list") outputs = step_copy_list(s, inp, out, langs, lg);
            else if (kind == "script") outputs = step_script(s, inp, out, values, env, lg);
            else throw u3::Stop("unknown step kind '" + kind + "'");
            state.mark(sid, outputs, "ok", "", fp);
            lg(fmt("        ok  %zu file(s)  %.1f s", outputs.fields.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()));
            c_ok++;
        } catch (const InputMissing &e) {
            // an accepted non-zero exit (an input such as the decrypted EBOOT.elf is not there yet): runs again next time
            state.mark(sid, Value::object(), "unavailable", std::string(e.what()).substr(0, 500));
            lg(std::string("        SKIP, script found an input missing (") + e.what() + ")");
            c_unavail++;
        } catch (const std::exception &e) {
            state.mark(sid, Value::object(), "failed", std::string(e.what()).substr(0, 500));
            lg(fmt("        FAILED after %.1f s: ", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()) + e.what());
            c_failed++;
            if (!a.continue_on_error) {
                lg("stopping (use --continue-on-error to go on)");
                break;
            }
        }
    }
    lg(fmt("done in %.0f s: ok %d, skipped %d, already %d, failed %d, unavailable %d", std::chrono::duration<double>(std::chrono::steady_clock::now() - t_all).count(),
           c_ok, c_skipped, c_already, c_failed, c_unavail));
    return c_failed ? 1 : 0;
}

}  // namespace u3imp

namespace u3imp {
std::vector<std::pair<std::string, std::string>> step_fingerprints() {
    std::vector<std::pair<std::string, std::string>> out;
    for (const Value &s : load_steps([](const std::string &) {})) out.push_back({val_s(s, "id"), step_fingerprint(s)});
    return out;
}
}  // namespace u3imp

namespace u3imp {
// verify_output: a rebuilt output folder against manifest.tsv (the reference overlays' sha256), per overlay / family:
// files in the manifest, matched, differing, missing, hidden (a higher overlay has the same file), and extra files;
// the full list goes to <out>/_recipe/verify.tsv (or `report`)
int verify_output(const fs::path &out0, const fs::path &report0) {
    const fs::path out = absp(out0), report = report0.empty() ? out / "_recipe" / "verify.tsv" : report0;
    const std::vector<Row> man = read_tsv(recipe_dir() / "manifest.tsv");
    std::map<std::pair<std::string, std::string>, std::string> prov;
    if (is_file(recipe_dir() / "provenance.tsv"))
        for (const Row &r : read_tsv(recipe_dir() / "provenance.tsv")) prov[{r.at("overlay"), r.at("path")}] = r.at("class");
    std::set<std::pair<std::string, std::string>> hidden;
    {
        std::map<std::string, std::set<std::string>> have;
        for (const Row &m : man) have[m.at("overlay")].insert(lower(m.at("path")));
        for (const Row &m : man) {
            const std::string &p = m.at("path");
            if (!starts(p, "u3data/build/main/pak23/ps4/") && !starts(p, "u3data/build/main/actor33/ps4/")) continue;
            auto oi = std::find(OVERLAYS.begin(), OVERLAYS.end(), m.at("overlay"));
            if (oi == OVERLAYS.end()) continue;
            for (auto hi = OVERLAYS.begin(); hi != oi; ++hi)
                if (have[*hi].count(lower(p))) {
                    hidden.insert({m.at("overlay"), p});
                    break;
                }
        }
    }
    struct C {
        int match = 0, differs = 0, missing = 0, hidden = 0;
        int n() const { return match + differs + missing + hidden; }
    };
    std::map<std::string, C> stats;
    std::vector<std::array<std::string, 4>> rows;
    std::set<std::string> known;
    for (const Row &m : man) {
        const std::vector<std::string> parts = split(m.at("path"), '/');
        std::string fam = m.at("overlay") + "/";
        for (size_t k = 2; k < 4 && k < parts.size(); k++) fam += (k > 2 ? "/" : "") + parts[k];
        const fs::path p = out / "work" / P(m.at("overlay")) / P(m.at("path"));
        known.insert(lower(S(absp(p))));
        std::string st;
        if (!present(p)) st = hidden.count({m.at("overlay"), m.at("path")}) ? "hidden" : "missing";
        else st = sha256_file(p) == m.at("sha256") ? "match" : "differs";
        const auto pc = prov.find({m.at("overlay"), m.at("path")});
        const std::string cls = pc == prov.end() ? "" : pc->second;
        for (C *c : {&stats[fam], &stats["class:" + cls]})
            (st == "match" ? c->match : st == "differs" ? c->differs : st == "missing" ? c->missing : c->hidden)++;
        rows.push_back({m.at("overlay"), m.at("path"), st, cls});
    }
    std::vector<std::string> extra;
    for (const std::string &ov : OVERLAYS) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(out / "work" / P(ov), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2) || known.count(lower(S(absp(it->path()))))) continue;
            extra.push_back(rel_of(it->path(), out / "work"));
        }
    }
    fs::create_directories(report.parent_path());
    std::string t = "overlay\tpath\tstatus\tprovenance_class\n";
    for (const auto &r : rows) t += r[0] + "\t" + r[1] + "\t" + r[2] + "\t" + r[3] + "\n";
    for (const auto &e : extra) {
        const size_t k = e.find('/');
        t += e.substr(0, k) + "\t" + (k == std::string::npos ? "" : e.substr(k + 1)) + "\textra\t\n";
    }
    write_text(report, t);
    std::printf("%-50s %7s %7s %7s %7s %7s\n", "family", "files", "match", "differ", "missing", "hidden");
    C tot;
    for (const auto &[fam, c] : stats) {
        std::printf("%-50s %7d %7d %7d %7d %7d\n", fam.c_str(), c.n(), c.match, c.differs, c.missing, c.hidden);
        if (!starts(fam, "class:")) {
            tot.match += c.match;
            tot.differs += c.differs;
            tot.missing += c.missing;
            tot.hidden += c.hidden;
        }
    }
    int loaded = 0, lm = 0;
    for (const auto &r : rows)
        if (!hidden.count({r[0], r[1]})) {
            loaded++;
            lm += r[2] == "match";
        }
    std::printf("%-50s %7d %7d %7d %7d %7d   match rate %.1f%% (%d of the %d files the game loads)\n", "TOTAL", tot.n(), tot.match, tot.differs, tot.missing,
                tot.hidden, 100.0 * lm / std::max(loaded, 1), lm, loaded);
    std::printf("hidden: not built, a higher overlay has the same file so the game never loads it (rebuild with --all-files to build them)\n");
    std::printf("extra files not in the manifest: %zu\n", extra.size());
    for (size_t i = 0; i < extra.size() && i < 15; i++) std::printf("    %s\n", extra[i].c_str());
    std::printf("report: %s\n", S(report).c_str());
    return 0;
}
}  // namespace u3imp

namespace u3imp {
void recipe_list() {
    for (const Value &s : load_steps([](const std::string &l) { log(l); }))
        log(fmt("%-34s %-10s %-24s %s", val_s(s, "id").c_str(), val_s(s, "kind").c_str(), val_s(s, "status").c_str(), val_s(s, "title").c_str()));
}
}  // namespace u3imp
