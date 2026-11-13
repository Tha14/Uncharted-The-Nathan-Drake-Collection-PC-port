// Recipe steps convert-levels-port / -r2 / -r3: the converted level paks of overlay-port / -r2 / -r3, rebuilt row by
// row from plan.tsv with the current converter (f525859 = tools/paktools + paktools-f525859/; the sky rows: paktools-sky,
// the only sky converter).
//   u3fix levels <family> --project P --job J --paktools D --ps4 D --ps3-disc D --ps3-update D --ps3-data2 D
//                --ps3-dlcmaps D --python EXE [--apply] [--only REGEX] [--strict] [--jobs N] [--keep-work]
//
// A plan row names the level, its method, options, the PS3 source (archive + pak) and the sha256 the result must have.
// A file already equal to that sha256 is skipped. A row that is not 'identical' never replaces an existing file, except a
// 'base' row whose file is neither its own result nor the finished reference (an older plan's build). An 'identical'
// row whose build differs is reported and not installed. Replaced files are backed up once to
// work/backup-pak23-levels/<overlay>/. Rows hidden by a higher overlay ($RECIPE_SHADOWED) are not built.
// The PS3 sources: ps3idx.tsv (every pak23 level pak of the player's PS3 archives, in source priority) and one extract
// per row (a GOTY disc stands in for the DLC map packs); both caches are dropped when the PS3 inputs change.
// In C++: empty (dctools/ps4writer.cpp), ingame (dctools/ps4ingame.cpp), vischeat (dctools/vischeat.cpp). The methods
// not in C++ yet run their converter script in the converter tree (scratch/pak23-levels-work/pt-<version>, assembled
// with a size manifest): stage (lib/convert_one.py; data/overrides/<overlay>/<name>.json: gobo), hud, windoff, sky.
// Replaces levels.py for these families (the retired lab and maps-batch families are not supported).
#include "u3fix.hpp"
#include "proc.hpp"
#include "psarc.hpp"
#include "ps4ingame.hpp"
#include "vischeat.hpp"
#include "windoff.hpp"
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

std::string g_here, g_project, g_paktools, g_ps4, g_disc, g_update, g_data2, g_dlcmaps, g_python;
fs::path g_tmp;   // <project>/scratch/pak23-levels-work
std::mutex g_pt_lock;
std::set<std::string> g_anim;   // animated actor names for the ingame converter (made once, before the workers)


using Row = std::map<std::string, std::string>;

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

fs::path anim_list() {   // data/ps4_anim_actors.json: retail actor33 paks with an ANIM item
    const fs::path out = g_tmp / "ps4_anim_actors.json";
    if (!fs::exists(out)) {
        fs::create_directories(g_tmp);
        run({g_python, g_here + "/lib/anim_actors.py", g_paktools, P(ps4main() / "actor33" / "ps4"), P(out)});
    }
    return out;
}
fs::path default_lightmaps() {   // the two retail default lightmap textures, from the player's PS4 copy
    const fs::path out = g_tmp / "default_lightmaps.json";
    if (!fs::exists(out)) {
        fs::create_directories(g_tmp);
        run({g_python, g_paktools + "/stage/make_default_lightmaps.py", P(ps4main() / "pak23" / "ps4"), P(out)});
    }
    return out;
}

// the files of a converter tree: relative path -> source (later roots override earlier ones)
std::map<std::string, fs::path> pt_sources(const std::string &ver) {
    std::vector<fs::path> roots;
    if (ver == "f525859") roots = {upath(g_paktools), upath(g_here + "/paktools-f525859")};
    else roots = {upath(g_here + "/paktools-" + ver)};
    std::map<std::string, fs::path> m;
    for (const fs::path &r : roots) {
        if (!fs::is_directory(r)) continue;
        for (auto it = fs::recursive_directory_iterator(r); it != fs::recursive_directory_iterator(); ++it) {
            const std::string n = utf8(it->path().filename());
            if (it->is_directory()) {
                if (n == "__pycache__") it.disable_recursion_pending();
                continue;
            }
            if (n == "OVERLAY_ON" || (n.size() > 4 && n.compare(n.size() - 4, 4, ".pyc") == 0)) continue;
            m[utf8(fs::relative(it->path(), r))] = it->path();
        }
    }
    m["data/conv_anim_actors.json"] = upath(g_here + "/data/conv_anim_actors.json");
    m["data/ps4_anim_actors.json"] = anim_list();
    auto st = m.find("stage/ps4stage.py");
    if (st != m.end() && text_file(st->second).find("default_lightmaps.json") != std::string::npos) m["stage/default_lightmaps.json"] = default_lightmaps();
    return m;
}

fs::path paktools(const std::string &ver) {
    const fs::path dst = g_tmp / ("pt-" + ver);
    std::lock_guard<std::mutex> lock(g_pt_lock);
    const auto files = pt_sources(ver);
    std::string want = "paktools-tree v2\n";
    for (const auto &[r, s] : files) want += std::to_string(fs::file_size(s)) + "\t" + r + "\n";
    const fs::path stamp = dst / ".ready";
    if (fs::exists(stamp) && text_file(stamp) == want) {
        bool ok = true;
        for (const auto &[r, s] : files)
            if (!fs::is_regular_file(dst / upath(r)) || fs::file_size(dst / upath(r)) != fs::file_size(s)) { ok = false; break; }
        if (ok) return dst;
    }
    const fs::path tmp = dst.string() + ".tmp-" + std::to_string(GetCurrentProcessId());
    fs::remove_all(tmp);
    for (const auto &[r, s] : files) {
        fs::create_directories((tmp / upath(r)).parent_path());
        fs::copy_file(s, tmp / upath(r), fs::copy_options::overwrite_existing);
    }
    fs::create_directories(tmp / "data");
    for (const char *f : {"hk2010_ps3.json", "hk2011_ps4.json"}) write_text(tmp / "data" / f, "{}\n");   // FG collision tables: empty
    write_text(tmp / ".ready", want);
    if (fs::is_directory(dst)) {
        const fs::path old = dst.string() + ".old-" + std::to_string(GetCurrentProcessId());
        fs::rename(dst, old);
        std::error_code ec;
        fs::remove_all(old, ec);
    }
    fs::rename(tmp, dst);
    return dst;
}

fs::path res_dir() {   // the mkplan survey of the retail PS4 pak23
    const fs::path d = g_tmp / "res";
    if (!fs::exists(d / "ps4tex.tsv")) {
        const fs::path pt = paktools("f525859");
        run({g_python, g_here + "/lib/mkres.py", P(d), P(ps4main() / "pak23" / "ps4")}, {{"PYTHONPATH", P(pt)}});
    }
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

fs::path build(const Row &row, const std::map<std::pair<std::string, std::string>, std::vector<std::string>> &idx) {
    const fs::path od = g_tmp / "out" / upath(row.at("overlay")), op = od / upath(row.at("name") + ".pak");
    fs::create_directories(od);
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
        write_file_replace(op, d);
        return op;
    }
    if (m == "hud") {
        run({g_python, g_here + "/lib/hud_textures.py", P(paktools(ver)), P(pak4 / "hud.pak"), P(ps3_source(row, idx)), P(op)});
        return op;
    }
    if (m == "windoff") {   // C++ (dctools/windoff.cpp)
        write_file_replace(op, windoff_convert(pak4 / upath(row.at("src") + ".pak"), items));
        return op;
    }
    if (m == "sky") {
        ps3_source(row, idx);
        const fs::path sd = g_tmp / "sky" / upath(row.at("overlay")) / upath(row.at("name"));
        std::error_code ec;
        fs::remove_all(sd, ec);
        run({g_python, g_here + "/lib/build_skies.py", P(paktools(ver)), P(g_tmp / "ps3"), P(pak4), P(res_dir()), P(sd), row.at("name")}, {{"U3_TEXDICT", P(texdict)}});
        fs::copy_file(sd / upath(row.at("name") + ".pak"), op, fs::copy_options::overwrite_existing);
        return op;
    }
    if (m == "vischeat") {   // C++ (dctools/vischeat.cpp)
        write_file_replace(op, vischeat_convert(ps3_source(row, idx)));
        return op;
    }
    if (m == "ingame") {   // C++ (dctools/ps4ingame.cpp)
        write_file_replace(op, ps4ingame_convert(ps3_source(row, idx), g_anim));
        return op;
    }
    if (m != "stage" && m != "ingame" && m != "vischeat") throw Stop("method " + m + " is not supported");
    const fs::path pt = paktools(ver);
    Env env = {{"PYTHONPATH", P(pt) + ";" + P(pt / "stage")}, {"U3_TEXDICT", P(texdict)}};
    for (const auto &[k, v] : opts) env[k] = v;
    // data/overrides/<overlay>/<name>.json: 'gobo' = gobo textures taken from another retail pak (co-op Syria)
    const fs::path ovf = upath(g_here + "/data/overrides/" + row.at("overlay") + "/" + row.at("name") + ".json");
    json::Value ov;
    if (fs::exists(ovf)) ov = read_json(ovf);
    if (const json::Value *g = ov.get("gobo"); g && g->truthy()) {
            const fs::path od2 = g_tmp / "overrides" / upath(row.at("overlay"));
            fs::create_directories(od2);
            std::string s;
            dump_compact(s, *g);
            const fs::path f = od2 / upath(row.at("name") + ".gobo.json");
            write_text(f, s);
            env["U3_GOBO"] = P(f);
        }
    const fs::path sp = ps3_source(row, idx);
    std::vector<std::string> cmd = {g_python, g_here + "/lib/convert_one.py", P(pt), m, P(sp), P(op), m == "stage" ? P(res_dir()) : "-", P(pak4),
                                    P(g_tmp / "plans" / upath(row.at("overlay")) / upath(ver + opts_s))};
    if (flags.count("mb")) cmd.push_back("--mapbuild");
    run(cmd, env);
    return op;
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
                 {"--project", "--job", "--paktools", "--ps4", "--ps3-disc", "--ps3-update", "--ps3-data2", "--ps3-dlcmaps", "--python", "--only", "--jobs"});
    if (family.empty()) throw Stop("missing family");
    if (family != "port" && family != "r2" && family != "r3") throw Stop("family " + family + " is not supported (lab and maps-batch are retired)");
    g_project = a.need("--project"); g_here = a.need("--job"); g_paktools = a.need("--paktools"); g_ps4 = a.need("--ps4"); g_python = a.need("--python");
    g_disc = a.get("--ps3-disc", ""); g_update = a.get("--ps3-update", ""); g_data2 = a.get("--ps3-data2", ""); g_dlcmaps = a.get("--ps3-dlcmaps", "");
    g_tmp = upath(g_project) / "scratch" / "pak23-levels-work";
    const fs::path work = upath(g_project) / "work";
    const char *rj = std::getenv("RECIPE_JOBS");
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int jobs = a.val.count("--jobs") ? std::stoi(a.get("--jobs", "1")) : (rj && *rj ? std::atoi(rj) : int(std::max(1u, hw / 2)));
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
        if (exists && h == r.at("sha256")) { done++; continue; }
        auto rf = ref.find({r.at("overlay"), r.at("name")});
        if (r.at("status") == "base" && !strict && exists && rf != ref.end() && h != rf->second) {
            r["replace"] = "1";   // an earlier import's build of an older plan row: build it again so the later steps apply
            todo.push_back(r);
            continue;
        }
        if (r.at("status") != "identical" && (strict || exists)) continue;   // never replace an existing file by a non-identical build
        todo.push_back(r);
    }
    std::printf("%s: %zu rows, %d already applied, %zu to build\n", family.c_str(), rows.size(), done, todo.size());
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
    std::set<std::string> vers;
    for (const Row &r : todo)
        if (r.at("method") != "empty" && r.at("method") != "ingame" && r.at("method") != "vischeat" && r.at("method") != "windoff") vers.insert(version_of(r));   // the methods that still run a converter script
    for (const Row &r : todo)
        if (r.at("method") == "ingame") {
            g_anim = anim_actor_names(ps4main() / "actor33" / "ps4", upath(g_here + "/data/conv_anim_actors.json"));
            break;
        }
    for (const std::string &v : vers) paktools(v);   // every converter tree before the workers
    std::vector<std::string> result(todo.size()), error(todo.size());
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (int w = 0; w < std::max(1, jobs); w++)
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < todo.size();) {
                try {
                    result[i] = utf8(build(todo[i], idx));
                } catch (const std::exception &e) {
                    error[i] = e.what();
                }
            }
        });
    for (auto &t : pool) t.join();
    int bad = 0;
    json::Value report;
    report.kind = json::Value::Array;
    for (size_t i = 0; i < todo.size(); i++) {
        const Row &r = todo[i];
        json::Value e = json::Value::object();
        e.set("overlay", json::Value::text(r.at("overlay")));
        e.set("name", json::Value::text(r.at("name")));
        if (!error[i].empty() || result[i].empty()) {
            std::printf("FAIL %s/%s: %s\n", r.at("overlay").c_str(), r.at("name").c_str(), error[i].substr(0, 600).c_str());
            bad++;
            e.set("result", json::Value::text("fail"));
            e.set("error", json::Value::text(error[i].substr(0, 2000)));
            report.items.push_back(e);
            continue;
        }
        const std::string h = sha_file(upath(result[i]));
        const bool same = h == r.at("sha256");
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
            write_file_replace(dst, read_file(upath(result[i])));
            std::printf("installed %s/%s: %s\n", r.at("overlay").c_str(), r.at("name").c_str(), tag.c_str());
        } else {
            std::printf("ok %s/%s: %s (dry run)\n", r.at("overlay").c_str(), r.at("name").c_str(), tag.c_str());
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
