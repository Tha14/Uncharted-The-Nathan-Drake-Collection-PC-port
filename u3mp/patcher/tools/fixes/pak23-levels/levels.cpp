// Recipe steps convert-levels-port / -r2 / -r3: the converted level paks of overlay-port / -r2 / -r3, rebuilt row by
// row from plan.tsv with the current converter (f525859, and the sky converter for the sky rows), all C++ in dctools/.
//   u3fix levels <family> --project P --job J --ps4 D --ps3-disc D --ps3-update D --ps3-data2 D --ps3-dlcmaps D
//                [--apply] [--only REGEX] [--strict] [--jobs N] [--cache-mb N] [--keep-work]
//
// A plan row names the level, its method, options, the PS3 source (archive + pak) and the sha256 the result must have.
// A file already equal to that sha256 is skipped. A row that is not 'identical' never replaces an existing file, except a
// 'base' row whose file is neither its own result nor the finished reference (an older plan's build). An 'identical'
// row whose build differs is reported and not installed. Replaced files are backed up once to
// work/backup-pak23-levels/<overlay>/. Rows hidden by a higher overlay ($RECIPE_SHADOWED) are not built.
// The PS3 sources: ps3idx.tsv (every pak23 level pak of the player's PS3 archives, in source priority) and one extract
// per row (a GOTY disc stands in for the DLC map packs); both caches are dropped when the PS3 inputs change.
// In C++: empty (dctools/ps4writer.cpp), ingame (dctools/ps4ingame.cpp), vischeat, windoff, hud (dctools/texcodec.cpp),
// stage (dctools/ps4stage.cpp with the survey of the retail paks in scratch/pak23-levels-work/res; data/overrides/
// <overlay>/<name>.json: gobo), sky (dctools/ps4stage.cpp sky_convert: the sky converter's version with its plan,
// shared template bundle and paramfix; data/water_templates.json).
// Replaces levels.py for these families (the retired lab and maps-batch families are not supported).
#include "u3fix.hpp"
#include "proc.hpp"
#include "psarc.hpp"
#include "ps4ingame.hpp"
#include "vischeat.hpp"
#include "windoff.hpp"
#include "ndpak.hpp"
#include "texcodec.hpp"
#include "ps4stage.hpp"
#include "ps4writer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <regex>
#include <thread>
#include <windows.h>

namespace u3 {
namespace {

std::string g_here, g_project, g_ps4, g_disc, g_update, g_data2, g_dlcmaps;
fs::path g_tmp;   // <project>/scratch/pak23-levels-work
std::set<std::string> g_anim;   // animated actor names for the ingame converter (made once, before the workers)

using Row = std::map<std::string, std::string>;

// The plan's sha256, or the one its note names as 'goty_sha256=<sha>': a Game of the Year disc without the DLC map packs
// gives the DLC levels from goty-dlc.psarc, and a few of those sources differ from dlc-map1..4.psarc
// (gompa-multi-shade-npc-ingame12: the GOTY copy already has the 43 spawners the shade-npc-paks step adds; that step
// accepts it as it is)
bool plan_sha(const Row &r, const std::string &h) {
    if (h == r.at("sha256")) return true;
    auto n = r.find("note");
    if (n == r.end()) return false;
    const size_t k = n->second.find("goty_sha256=");
    return k != std::string::npos && n->second.compare(k + 12, 64, h) == 0 && h.size() == 64;
}

std::vector<Row> plan_rows() {
    std::vector<Row> rows;
    std::vector<std::string> hdr;
    std::string t = text_file(upath(g_here + "/plan.tsv"));
    size_t a = 0;
    for (size_t k = 0; k <= t.size(); k++) {
        if (k < t.size() && t[k] != '\n') continue;
        const std::string l = t.substr(a, k - a);
        a = k + 1;
        if (l.empty() || l[0] == '#' || py_split(l).empty()) continue;
        std::vector<std::string> f;
        size_t b = 0;
        for (size_t j; (j = l.find('\t', b)) != std::string::npos; b = j + 1) f.push_back(l.substr(b, j - b));
        f.push_back(l.substr(b));
        if (f[0] == "family") { hdr = f; continue; }
        Row r;
        for (size_t i = 0; i < hdr.size() && i < f.size(); i++) r[hdr[i]] = f[i];
        rows.push_back(r);
    }
    return rows;
}

std::string version_of(const Row &r) { return r.at("version") == "sky" ? "sky" : "f525859"; }   // the current converter only
fs::path ps4main() { return upath(g_ps4) / "u3data" / "build" / "main"; }

// the two retail default lightmap textures (from the player's PS4 copy), which the stage converter always carries
std::shared_ptr<std::vector<stage::BundleTex>> default_lightmap_texels() {
    static std::mutex lock;
    static std::shared_ptr<std::vector<stage::BundleTex>> v;
    std::lock_guard<std::mutex> g(lock);
    if (!v) v = std::make_shared<std::vector<stage::BundleTex>>(stage::default_lightmaps(ps4main() / "pak23" / "ps4"));
    return v;
}
fs::path res_dir() {   // the mkplan survey of the retail PS4 pak23
    const fs::path d = g_tmp / "res";
    if (!fs::exists(d / "ps4tex.tsv")) stage::make_res(d, ps4main() / "pak23" / "ps4");   // C++ (dctools/stageplan.cpp)
    return d;
}

// the PS3 inputs' signature: TMP/ps3, labsrc and ps3idx.tsv are dropped when it changes
void walk_sig(const fs::path &top, const fs::path &dp, std::vector<std::string> &sig) {
    std::vector<std::string> files;
    std::vector<fs::path> dirs;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(dp, ec)) {
        if (e.is_directory()) dirs.push_back(e.path());
        else files.push_back(utf8(e.path().filename()));
    }
    std::sort(files.begin(), files.end());
    for (const std::string &n : files) {
        if (!(n.size() > 6 && n.compare(n.size() - 6, 6, ".psarc") == 0) && n != "net18.bin") continue;
        struct _stat64 st;
        if (_wstat64((dp / upath(n)).wstring().c_str(), &st)) continue;
        sig.push_back(fmt("%s %lld %lld", win_path(utf8(fs::relative(dp / upath(n), top))).c_str(), (long long)st.st_size, (long long)st.st_mtime));
    }
    std::string rel = utf8(fs::relative(dp, top));
    if (std::count(rel.begin(), rel.end(), '/') >= 3) return;
    for (const fs::path &d : dirs) walk_sig(top, d, sig);
}
void drop_stale_ps3_caches() {
    std::vector<std::string> sig;
    for (const std::string &f : {g_update, g_dlcmaps, g_disc, g_data2}) {
        sig.push_back(f);
        if (!f.empty() && fs::is_directory(upath(f))) walk_sig(upath(f), upath(f), sig);
    }
    std::string s;
    for (size_t i = 0; i < sig.size(); i++) s += (i ? "\n" : "") + sig[i];
    const std::string h = sha256_hex(Bytes(s.begin(), s.end()));
    const fs::path stamp = g_tmp / ".ps3-inputs";
    if (!fs::exists(stamp) || py_rstrip(text_file(stamp)) != h) {
        std::error_code ec;
        fs::remove_all(g_tmp / "ps3", ec);
        fs::remove_all(g_tmp / "labsrc", ec);
        fs::remove(g_tmp / "ps3idx.tsv", ec);
        fs::create_directories(g_tmp);
        write_text(stamp, h);
    }
}

// ps3idx.tsv: every pak23 level pak of the PS3 archives in source priority (name, prio, archive, entry, size, path)
std::map<std::pair<std::string, std::string>, std::vector<std::string>> ps3_index() {
    const fs::path f = g_tmp / "ps3idx.tsv";
    if (!fs::exists(f)) {
        std::vector<std::pair<std::string, std::string>> arcs = {{"dlc-patch18", g_update + "/dlc-patch18.psarc"}, {"dlc-patch13", g_update + "/dlc-patch13.psarc"}};
        for (int k = 1; k <= 4; k++) arcs.push_back({"dlc-map" + std::to_string(k), g_dlcmaps + "/dlc-map" + std::to_string(k) + ".psarc"});
        arcs.push_back({"goty-patch", g_disc + "/goty-patch.psarc"});
        arcs.push_back({"goty-dlc", g_disc + "/goty-dlc.psarc"});
        arcs.push_back({"pak23", g_disc + "/build/main/pak23.psarc"});
        std::vector<std::string> d2;
        for (const std::string &dir : {g_data2 + "/build/main", g_data2 + "/build/main/pak23"}) {
            if (!fs::is_directory(upath(dir))) continue;
            for (const auto &e : fs::directory_iterator(upath(dir))) {
                std::string n = utf8(e.path().filename()), l = n;
                for (char &c : l) c = char(tolower((unsigned char)c));
                if (e.is_regular_file() && l.size() > 6 && l.compare(l.size() - 6, 6, ".psarc") == 0) d2.push_back(dir + "\\" + n);
            }
        }
        std::sort(d2.begin(), d2.end());
        for (const std::string &p : d2) {
            std::string b = p.substr(p.find_last_of("/\\") + 1);
            arcs.push_back({b.substr(0, b.size() - 6), p});
        }
        std::string out = "name\tprio\tarchive\tentry\tsize\tpath\n";
        for (size_t pr = 0; pr < arcs.size(); pr++) {
            const auto &[k, a] = arcs[pr];
            if (!fs::exists(upath(a))) continue;
            Psarc p(upath(a));
            for (size_t i = 0; i < p.names.size(); i++) {
                const std::string &nm = p.names[i];
                if (nm.size() < 4 || nm.compare(nm.size() - 4, 4, ".pak") != 0 || (nm.find("/pak23/") == std::string::npos && nm.find('/') != std::string::npos)) continue;
                std::string b = nm.substr(nm.find_last_of('/') == std::string::npos ? 0 : nm.find_last_of('/') + 1);
                out += fmt("%s\t%zu\t%s\t%zu\t%llu\t%s\n", b.substr(0, b.size() - 4).c_str(), pr, k.c_str(), i + 1, (unsigned long long)p.length(i + 1), a.c_str());
            }
        }
        fs::create_directories(g_tmp);
        write_text(f, out);
    }
    std::map<std::pair<std::string, std::string>, std::vector<std::string>> idx;
    std::string t = text_file(f);
    size_t a = 0;
    for (size_t k = 0; k <= t.size(); k++) {
        if (k < t.size() && t[k] != '\n') continue;
        const std::string l = t.substr(a, k - a);
        a = k + 1;
        if (l.empty()) continue;
        std::vector<std::string> x;
        size_t b = 0;
        for (size_t j; (j = l.find('\t', b)) != std::string::npos; b = j + 1) x.push_back(l.substr(b, j - b));
        x.push_back(l.substr(b));
        if (x[0] != "name" && x.size() >= 6) idx.emplace(std::make_pair(x[0], x[2]), x);   // the first one wins
    }
    return idx;
}

fs::path ps3_source(const Row &row, const std::map<std::pair<std::string, std::string>, std::vector<std::string>> &idx) {
    const std::string &arch = row.at("archive"), &src = row.at("src");
    if (arch == "labsrc") throw Stop("the Lab sources belong to the retired lab family");
    const fs::path d = g_tmp / "ps3" / upath(arch), p = d / upath(src + ".pak");
    if (!fs::exists(p)) {
        auto it = idx.find({src, arch});
        if (it == idx.end() && arch.rfind("dlc-map", 0) == 0) it = idx.find({src, "goty-dlc"});   // a GOTY disc without the DLC map packs
        if (it == idx.end()) throw Stop("PS3 source " + src + " not in " + arch);
        fs::create_directories(d);
        Psarc a(upath(it->second[5]));
        write_file_replace(p, a.read(size_t(std::stoull(it->second[3]))));
    }
    return p;
}

// the converter pass map-colors2 on a built stage or sky pak (PS3 normal-map blends, gloss, stripes); the pak as built
// when the pass finds nothing to do
Bytes with_map_colors2(Bytes built, const std::string &name, const Bytes &ps3) {
    if (auto fixed = map_colors2_pass(built, name + ".pak", ps3, nullptr)) return std::move(*fixed);
    return built;
}

// overlay-r3 hud.pak: the PS4 game kept small copies of two HUD textures (gun-galil-trans, the distress font); their
// full-size PS3 DXT1 texels are tiled 1:1 as PS4 BC1 and appended to the VRAM in this order. Only the two VRAM_DESC
// records (VRAM offset, size, format 0x47, mips, width, height, flag 3) and the header VRAM size change.
Bytes hud_convert(const fs::path &retail, const fs::path &ps3) {
    const Pak r(retail), p(ps3);
    Bytes d = r.d;
    Bytes vram(r.d.begin() + r.vram_base, r.d.end());
    const std::vector<PakTexture> t4s = r.textures(), t3s = p.textures();
    auto find = [](const std::vector<PakTexture> &ts, const std::string &s) -> const PakTexture & {
        for (const PakTexture &t : ts)
            if (t.path.find("/" + s + "/") != std::string::npos) return t;
        throw Stop("IndexError: no texture " + s);
    };
    auto put32 = [&](uint64_t o, uint32_t v) { for (int i = 0; i < 4; i++) d.at(o + i) = uint8_t(v >> (8 * i)); };
    for (const char *s : {"art/general/hud/guns/gun-galil-trans.tga", "art/general/textures/font/distress.tga"}) {
        const PakTexture &t4 = find(t4s, s), &t3 = find(t3s, s);
        const auto f = tc::ps3_format(t3);
        if (!f || f->kind != "DXT1") throw Stop("AssertionError: " + (f ? f->kind : std::string("None")));
        const Bytes til = tc::ps3_to_ps4_texels(p.vram_bytes(t3.vramOffset, t3.vramSize), t3.width, t3.height, t3.mips, f->bpb);
        const uint64_t b = r.pbase(t4.page) + t4.off + 0x20;
        put32(b + 8, uint32_t(vram.size()));
        put32(b + 12, uint32_t(uint64_t(vram.size()) >> 32));
        put32(b + 0x10, uint32_t(til.size()));
        put32(b + 0x1c, 0x47);
        put32(b + 0x24, t3.mips);
        put32(b + 0x28, t3.width);
        put32(b + 0x2c, t3.height);
        put32(b + 0x34, 3);
        vram.insert(vram.end(), til.begin(), til.end());
    }
    put32(0x20, uint32_t(vram.size()));
    Bytes out(d.begin(), d.begin() + r.vram_base);
    out.insert(out.end(), vram.begin(), vram.end());
    return out;
}

Bytes build(const Row &row, const std::map<std::pair<std::string, std::string>, std::vector<std::string>> &idx) {
    const std::string m = row.at("method"), opts_s = row.at("opts"), ver = version_of(row);
    Env opts;
    std::set<std::string> flags;
    std::vector<std::string> items;
    for (size_t b = 0, j;; b = j + 1) {
        j = opts_s.find(',', b);
        const std::string x = opts_s.substr(b, j == std::string::npos ? std::string::npos : j - b);
        if (!x.empty() && x != "-") items.push_back(x);
        if (j == std::string::npos) break;
    }
    if (opts_s != "" && opts_s != "-")
        for (const std::string &x : items) {
            const size_t eq = x.find('=');
            if (eq != std::string::npos) opts[x.substr(0, eq)] = x.substr(eq + 1);
            else flags.insert(x);
        }
    const fs::path pak4 = ps4main() / "pak23" / "ps4", texdict = ps4main() / "texturedict1" / "ps4" / "TextureDictionaries.csv";
    if (m == "empty") {   // the 248-byte empty level: a login page with an empty login and VRAM table, nothing else
        PakWriter w;
        make_login_and_vram_page(w, w.page(), {}, {});
        const Bytes d = w.build();
        if (d.size() != 248) throw Stop(fmt("AssertionError: %zu", d.size()));
        return d;
    }
    if (m == "hud")   // C++: the retail PS4 hud.pak with two HUD textures replaced by their full-size PS3 versions
        return hud_convert(pak4 / "hud.pak", ps3_source(row, idx));
    if (m == "windoff")   // C++ (dctools/windoff.cpp)
        return windoff_convert(pak4 / upath(row.at("src") + ".pak"), items);
    if (m == "sky") {   // C++ (dctools/ps4stage.cpp sky_convert, the sky converter's version)
        ps3_source(row, idx);
        // the PS3 sky pak: the first extract in archive priority (build_skies.py's order)
        const std::string &lv = row.at("name");
        fs::path sp;
        for (const char *a : {"dlc-patch18", "dlc-patch13", "dlc-map1", "dlc-map2", "dlc-map3", "dlc-map4", "goty-patch", "goty-dlc", "pak23"})
            if (fs::exists(g_tmp / "ps3" / a / upath(lv + ".pak"))) {
                sp = g_tmp / "ps3" / a / upath(lv + ".pak");
                break;
            }
        if (sp.empty() && fs::exists(g_tmp / "ps3" / upath(lv + ".pak"))) sp = g_tmp / "ps3" / upath(lv + ".pak");
        if (sp.empty()) throw Stop("PS3 source " + lv + " not found");
        stage::Options o;
        o.res = res_dir();
        o.pak_dir = pak4;
        o.texdict = texdict;
        o.plan_dir = g_tmp / "sky" / upath(row.at("overlay")) / upath(lv) / "work";
        o.default_lightmaps = default_lightmap_texels();
        return with_map_colors2(stage::sky_convert(sp, lv, o, upath(g_here + "/data/water_templates.json")), row.at("name"), read_file(sp));
    }
    if (m == "vischeat") return vischeat_convert(ps3_source(row, idx));   // C++ (dctools/vischeat.cpp)
    if (m == "ingame") return ps4ingame_convert(ps3_source(row, idx), g_anim);   // C++ (dctools/ps4ingame.cpp)
    if (m != "stage") throw Stop("method " + m + " is not supported");
    // C++ (dctools/ps4stage.cpp); 'mb' = paramfix + cubefix. No converter option sets environment values any more.
    if (!opts.empty()) throw Stop("stage option " + opts.begin()->first + " is not supported");
    stage::Options o;
    o.res = res_dir();
    o.pak_dir = pak4;
    o.texdict = texdict;
    o.plan_dir = g_tmp / "plans" / upath(row.at("overlay")) / upath(ver + opts_s);
    o.mapbuild = flags.count("mb") != 0;
    o.fixes = true;   // the fixes the old cloud / lost batches made (cloud-batches-r3 / lost-batches-r3) are converter rules now
    o.water_templates = upath(g_here + "/data/water_templates.json");
    o.default_lightmaps = default_lightmap_texels();
    // data/overrides/<overlay>/<name>.json: 'gobo' = gobo textures taken from another retail pak (co-op Syria)
    const fs::path ovf = upath(g_here + "/data/overrides/" + row.at("overlay") + "/" + row.at("name") + ".json");
    json::Value ov;
    if (fs::exists(ovf)) ov = read_json(ovf);
    if (const json::Value *g = ov.get("gobo"); g && g->truthy()) o.gobo = g;
    const fs::path ps3 = ps3_source(row, idx);
    return with_map_colors2(stage::stage_convert(ps3, o), row.at("name"), read_file(ps3));
}

std::set<std::pair<std::string, std::string>> shadowed() {
    std::set<std::pair<std::string, std::string>> s;
    const char *f = std::getenv("RECIPE_SHADOWED");
    if (!f || !*f || !fs::exists(upath(f))) return s;
    std::string t = text_file(upath(f));
    size_t a = 0;
    for (size_t k = 0; k <= t.size(); k++) {
        if (k < t.size() && t[k] != '\n') continue;
        std::string l = t.substr(a, k - a);
        a = k + 1;
        while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
        const size_t tab = l.find('\t');
        if (tab != std::string::npos) s.insert({l.substr(0, tab), l.substr(tab + 1)});
    }
    return s;
}

std::map<std::pair<std::string, std::string>, std::string> reference_shas() {
    std::map<std::pair<std::string, std::string>, std::string> out;
    const char *f = std::getenv("RECIPE_MANIFEST");
    if (!f || !*f || !fs::exists(upath(f))) return out;
    const std::string pre = "u3data/build/main/pak23/ps4/";
    std::string t = text_file(upath(f));
    size_t a = 0;
    for (size_t k = 0; k <= t.size(); k++) {
        if (k < t.size() && t[k] != '\n') continue;
        std::string l = t.substr(a, k - a);
        a = k + 1;
        std::vector<std::string> x;
        size_t b = 0;
        for (size_t j; (j = l.find('\t', b)) != std::string::npos; b = j + 1) x.push_back(l.substr(b, j - b));
        x.push_back(l.substr(b));
        if (x.size() >= 4 && x[1].rfind(pre, 0) == 0 && x[1].size() > pre.size() + 4 && x[1].compare(x[1].size() - 4, 4, ".pak") == 0)
            out[{x[0], x[1].substr(pre.size(), x[1].size() - pre.size() - 4)}] = x[3];
    }
    return out;
}

}  // namespace

int levels(const std::vector<std::string> &argv) {
    std::vector<std::string> opts;
    std::string family;
    for (size_t i = 0; i < argv.size(); i++) {
        if (argv[i].rfind("--", 0) == 0) {
            opts.push_back(argv[i]);
            if (argv[i] != "--apply" && argv[i] != "--strict" && argv[i] != "--keep-work" && i + 1 < argv.size()) opts.push_back(argv[++i]);
        } else if (family.empty()) family = argv[i];
        else throw Stop("unexpected argument " + argv[i]);
    }
    const Args a(opts, {"--apply", "--strict", "--keep-work"},
                 {"--project", "--job", "--ps4", "--ps3-disc", "--ps3-update", "--ps3-data2", "--ps3-dlcmaps", "--only", "--jobs", "--cache-mb"});
    if (family.empty()) throw Stop("missing family");
    if (family != "port" && family != "r2" && family != "r3") throw Stop("family " + family + " is not supported (lab and maps-batch are retired)");
    g_project = a.need("--project"); g_here = a.need("--job"); g_ps4 = a.need("--ps4");
    g_disc = a.get("--ps3-disc", ""); g_update = a.get("--ps3-update", ""); g_data2 = a.get("--ps3-data2", ""); g_dlcmaps = a.get("--ps3-dlcmaps", "");
    g_tmp = upath(g_project) / "scratch" / "pak23-levels-work";
    const fs::path work = upath(g_project) / "work";
    const char *lj = std::getenv("RECIPE_LEVELS_JOBS");
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int jobs = a.val.count("--jobs") ? std::stoi(a.get("--jobs", "1")) : (lj && *lj ? std::atoi(lj) : int(hw));
    const bool apply = a.has("--apply"), strict = a.has("--strict");

    std::vector<Row> rows;
    const std::regex only(a.get("--only", ""));
    for (const Row &r : plan_rows())
        if (r.at("family") == family && (!a.val.count("--only") || std::regex_search(r.at("name"), only))) rows.push_back(r);
    if (rows.empty()) {
        std::printf("no plan rows for %s\n", family.c_str());
        return 0;
    }
    const auto hidden = shadowed();
    if (!hidden.empty()) {
        std::vector<Row> keep;
        for (const Row &r : rows)
            if (!hidden.count({r.at("overlay"), "u3data/build/main/pak23/ps4/" + r.at("name") + ".pak"})) keep.push_back(r);
        if (keep.size() != rows.size())
            std::printf("%s: %zu rows skipped (hidden by a higher overlay, the game never loads them)\n", family.c_str(), rows.size() - keep.size());
        rows = keep;
        if (rows.empty()) {
            std::printf("already applied\n");
            return 0;
        }
    }
    fs::create_directories(g_tmp);
    drop_stale_ps3_caches();
    const auto t0 = std::chrono::steady_clock::now();
    const auto ref = reference_shas();
    std::vector<Row> todo;
    int done = 0;
    for (Row &r : rows) {
        const fs::path dst = work / upath(r.at("overlay")) / "u3data" / "build" / "main" / "pak23" / "ps4" / upath(r.at("name") + ".pak");
        r["dst"] = utf8(dst);
        const bool exists = fs::exists(dst);
        const std::string h = exists ? sha_file(dst) : "";
        if (exists && plan_sha(r, h)) { done++; continue; }
        auto rf = ref.find({r.at("overlay"), r.at("name")});
        if (r.at("status") == "base" && !strict && exists && rf != ref.end() && h != rf->second) {
            r["replace"] = "1";   // an earlier import's build of an older plan row: build it again so the later steps apply
            todo.push_back(r);
            continue;
        }
        if (r.at("status") != "identical" && (strict || exists)) continue;   // never replace an existing file by a non-identical build
        todo.push_back(r);
    }
    std::printf("%s: %zu rows, %d already applied, %zu to build (%d workers)\n", family.c_str(), rows.size(), done, todo.size(), std::max(1, jobs));
    if (todo.empty()) {
        std::printf("already applied\n");
        return 0;
    }
    std::fflush(stdout);
    const auto idx = ps3_index();
    for (const Row &r : todo) {   // PS3 sources first (one reader at a time)
        const std::string &m = r.at("method");
        if (m == "stage" || m == "ingame" || m == "vischeat" || m == "hud" || m == "sky") ps3_source(r, idx);
    }
    for (const Row &r : todo)
        if (r.at("method") == "stage" || r.at("method") == "sky") { res_dir(); break; }
    for (const Row &r : todo)
        if (r.at("method") == "ingame") {
            g_anim = anim_actor_names(ps4main() / "actor33" / "ps4", upath(g_here + "/data/conv_anim_actors.json"));
            break;
        }
    const char *rc = std::getenv("RECIPE_CACHE_MB");
    const size_t cache_mb = a.val.count("--cache-mb") ? size_t(std::stoul(a.get("--cache-mb", "0")))
                                                      : (rc && *rc ? size_t(std::stoul(rc)) : size_t(3072));
    set_read_cache_mb(cache_mb);
    if (cache_mb) std::printf("read cache: %zu MB\n", cache_mb);
    std::vector<std::string> error(todo.size());
    int bad = 0;
    json::Value report;
    report.kind = json::Value::Array;
    const fs::path pak4 = ps4main() / "pak23" / "ps4";
    const size_t batch = std::max<size_t>(8, size_t(std::max(1, jobs)) + 4);
    const size_t nbatches = (todo.size() + batch - 1) / batch;
    auto prefetch_rows = [&](size_t b) -> uint64_t {
        const uint64_t gen = read_cache_new_generation();
        const size_t s = b * batch, e = std::min(todo.size(), s + batch);
        for (size_t i = s; i < e; i++) {
            const Row &r = todo[i];
            const std::string &m = r.at("method");
            if (m == "stage" || m == "ingame" || m == "vischeat" || m == "hud" || m == "sky")
                prefetch_read(g_tmp / "ps3" / upath(r.at("archive")) / upath(r.at("src") + ".pak"));
            if (m == "windoff") prefetch_read(pak4 / upath(r.at("src") + ".pak"));
            else if (m == "hud") prefetch_read(pak4 / "hud.pak");
            else if (m == "stage" || m == "sky") prefetch_read(pak4 / upath(r.at("name") + ".pak"));
        }
        return gen;
    };
    if (cache_mb) {
        prefetch_read(g_tmp / "res" / "ps4mats.jsonl.gz");   // generation 0, kept for the whole step
        prefetch_read(g_tmp / "res" / "ps4tex.tsv");
        prefetch_rows(0);
    }
    for (size_t b = 0; b < nbatches; b++) {
        const size_t start = b * batch, end = std::min(todo.size(), start + batch);
        uint64_t next_gen = 0;
        std::thread pf;
        if (cache_mb && b + 1 < nbatches)   // read the next batch while this one converts
            pf = std::thread([&] { next_gen = prefetch_rows(b + 1); });
        std::vector<Bytes> out(end - start);
        std::vector<std::string> hashes(end - start);
        std::atomic<size_t> next{start};
        std::vector<std::thread> pool;
        for (int w = 0; w < std::max(1, jobs); w++)
            pool.emplace_back([&] {
                for (size_t i; (i = next++) < end;) {
                    try {
                        out[i - start] = build(todo[i], idx);
                        hashes[i - start] = sha256_hex(out[i - start]);
                    } catch (const std::exception &e) {
                        error[i] = e.what();
                    }
                }
            });
        for (auto &t : pool) t.join();
        if (pf.joinable()) pf.join();   // no reads in flight while this batch is written
        for (size_t i = start; i < end; i++) {   // writes only, in row order
            const Row &r = todo[i];
            const Bytes &data = out[i - start];
            json::Value e = json::Value::object();
            e.set("overlay", json::Value::text(r.at("overlay")));
            e.set("name", json::Value::text(r.at("name")));
            if (!error[i].empty() || data.empty()) {
                std::printf("FAIL %s/%s: %s\n", r.at("overlay").c_str(), r.at("name").c_str(), error[i].substr(0, 600).c_str());
                bad++;
                e.set("result", json::Value::text("fail"));
                e.set("error", json::Value::text(error[i].substr(0, 2000)));
                report.items.push_back(e);
                continue;
            }
            const std::string &h = hashes[i - start];
            const bool same = plan_sha(r, h);
            if (!same && r.at("status") == "identical") {
                std::printf("MISMATCH %s/%s: built %s, plan %s (not installed)\n", r.at("overlay").c_str(), r.at("name").c_str(), h.substr(0, 12).c_str(), r.at("sha256").substr(0, 12).c_str());
                bad++;
                e.set("result", json::Value::text("mismatch"));
                e.set("sha256", json::Value::text(h));
                report.items.push_back(e);
                continue;
            }
            const std::string tag = same ? "identical" : "equivalent (sha " + h.substr(0, 12) + ", reference " + r.at("sha256").substr(0, 12) + ")";
            e.set("result", json::Value::text(same ? "identical" : "equivalent"));
            e.set("sha256", json::Value::text(h));
            report.items.push_back(e);
            if (apply) {
                const fs::path dst = upath(r.at("dst"));
                if (fs::exists(dst) && !r.count("replace")) {   // a replaced earlier build needs no backup
                    const fs::path bk = work / "backup-pak23-levels" / upath(r.at("overlay")) / upath(r.at("name") + ".pak");
                    if (!fs::exists(bk)) {
                        fs::create_directories(bk.parent_path());
                        copy2(dst, bk);
                    }
                }
                fs::create_directories(dst.parent_path());
                write_file_replace(dst, data);
                std::printf("installed %s/%s: %s\n", r.at("overlay").c_str(), r.at("name").c_str(), tag.c_str());
            } else {
                std::printf("ok %s/%s: %s (dry run)\n", r.at("overlay").c_str(), r.at("name").c_str(), tag.c_str());
            }
        }
        std::vector<Bytes>().swap(out);
        if (cache_mb) {
            if (b + 1 < nbatches) read_cache_discard_before(next_gen);
            else clear_read_cache();
        }
    }
    write_text(g_tmp / ("report-" + family + ".json"), json_dump_indent1(report));
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%s: %zu built, %d failed/mismatched, %.0f s\n", family.c_str(), todo.size() - size_t(bad), bad, secs);
    if (!a.has("--keep-work")) {
        std::error_code ec;
        fs::remove_all(g_tmp / "out", ec);
    }
    return bad ? 1 : 0;
}

}  // namespace u3
