// Recipe step registry-final: the final pak23_ps4.txt of overlay-mp / -port / -r2 / -r3, rebuilt from the player's own
// game texts.   u3fix registry-final --project P --job J --ps4 D --ps3-update D --ps3-dlcmaps D --ps3-disc D
//                                    --ps3-data2 D [--apply] [overlay ...]
//
// Sources (cached in <project>/scratch/pak23-registry-sources/, made again when its 'sources-v2' mark is missing):
// retail.txt (PS4 pak23_ps4.txt), ps3-disc.txt, ps3-p18.txt / ps3-p13.txt (pak23.txt of the update psarcs) and every
// level .txt of the update, DLC map, GOTY and DATA2 psarcs as txt__<archive>__<entry>.txt (a GOTY disc without the DLC
// map packs: goty-dlc.psarc's files also under the dlc-map1..4 names). J/recipes/<overlay>.json replays line ranges of
// those sources ('L' ops = our own literal lines), the result must have the sha256 of J/recipes/sha256.tsv (else it
// is reported and not installed), and it replaces the overlay's file (backed up once to work/backup-pak23-registry/).
// Dry run without --apply. Replaces apply_registry.py, regbuild.py and regsources.py (same messages and files).
#include "u3fix.hpp"
#include "psarc.hpp"

#include <algorithm>
#include <cstdio>

namespace u3 {
namespace {

std::string lower(std::string s) {
    for (char &c : s) c = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    return s;
}

// the files of folder `dir` whose names match: prefix, then the rest (glob on Windows: case-insensitive);
// returned as glob writes them: <dir>\<name>
std::vector<std::string> glob_dir(const std::string &dir, bool (*match)(const std::string &)) {
    std::vector<std::string> r;
    if (!fs::is_directory(upath(dir))) return r;
    for (const auto &e : fs::directory_iterator(upath(dir))) {
        const std::string n = utf8(e.path().filename());
        if (match(lower(n))) r.push_back(dir + "\\" + n);
    }
    std::sort(r.begin(), r.end());
    return r;
}
bool is_dlc_map(const std::string &n) { return n.size() == 14 && n.rfind("dlc-map", 0) == 0 && n[7] >= '0' && n[7] <= '9' && n.substr(8) == ".psarc"; }
bool is_psarc(const std::string &n) { return n.size() >= 6 && n.compare(n.size() - 6, 6, ".psarc") == 0; }
std::string stem6(const std::string &path) {   // os.path.basename(f)[:-6]
    const size_t s = path.find_last_of("/\\");
    const std::string b = s == std::string::npos ? path : path.substr(s + 1);
    return b.substr(0, b.size() >= 6 ? b.size() - 6 : 0);
}
std::string ls(const std::string &n) { return n.substr(std::min(n.find_first_not_of('/'), n.size())); }
std::string flat(const std::string &n) {
    std::string s = ls(n), o;
    for (char c : s) o += c == '/' ? std::string("__") : std::string(1, c);
    return o;
}

// regsources: the game texts into `out`; returns the count it prints
int regsources(const std::string &ps4, const std::string &upd, const std::string &dlc, const std::string &disc, const std::string &d2, const std::string &out) {
    fs::create_directories(upath(out));
    auto w = [&](const std::string &n, const Bytes &b) { write_file_replace(upath(out + "/" + n), b); };
    w("retail.txt", read_file(upath(ps4 + "/u3data/build/main/pak23_ps4.txt")));
    if (fs::exists(upath(disc + "/build/main/pak23.txt"))) w("ps3-disc.txt", read_file(upath(disc + "/build/main/pak23.txt")));
    std::vector<std::pair<std::string, std::string>> arcs = {{upd + "\\dlc-patch18.psarc", "p18"}, {upd + "\\dlc-patch13.psarc", "p13"}};
    const std::vector<std::string> dlcmaps = dlc.empty() ? std::vector<std::string>{} : glob_dir(dlc, is_dlc_map);
    for (const std::string &f : dlcmaps) arcs.push_back({f, stem6(f)});
    for (const char *n : {"goty-patch.psarc", "goty-dlc.psarc"}) arcs.push_back({disc + "\\" + n, std::string(n).substr(0, std::strlen(n) - 6)});
    std::vector<std::string> d2arcs = glob_dir(d2 + "/build/main", is_psarc), p23 = glob_dir(d2 + "/build/main/pak23", is_psarc);
    d2arcs.insert(d2arcs.end(), p23.begin(), p23.end());
    std::sort(d2arcs.begin(), d2arcs.end());
    for (const std::string &f : d2arcs) arcs.push_back({f, stem6(f)});
    int n = 0;
    for (const auto &[a, k] : arcs) {
        if (!fs::exists(upath(a))) continue;
        Psarc p(upath(a));
        for (size_t i = 0; i < p.names.size(); i++) {
            const std::string &nm = p.names[i];
            if (nm.size() < 4 || nm.compare(nm.size() - 4, 4, ".txt") != 0) continue;
            const std::string last = nm.substr(nm.find_last_of('/') == std::string::npos ? 0 : nm.find_last_of('/') + 1);
            if (ls(nm) == "build/main/pak23.txt" && (k == "p18" || k == "p13")) {
                w("ps3-" + k + ".txt", p.read(i + 1));
                n++;
            } else if (nm.find("/pak23/") != std::string::npos || nm.find('/') == std::string::npos || last.rfind("lab", 0) == 0) {
                w("txt__" + k + "__" + flat(nm), p.read(i + 1));
                n++;
                if (k == "goty-dlc" && dlcmaps.empty())
                    for (int m = 1; m <= 4; m++) w("txt__dlc-map" + std::to_string(m) + "__" + flat(nm), p.read(i + 1));
            }
        }
    }
    return n + 2;
}

// regbuild: the registry of one recipe; empty message = built (out = the bytes)
std::string regbuild(const json::Value &recipe, const std::string &srcdir, Bytes &out) {
    std::map<std::string, std::vector<std::string>> cache;
    auto lines = [&](const std::string &sid) -> const std::vector<std::string> & {
        auto it = cache.find(sid);
        if (it != cache.end()) return it->second;
        const Bytes b = read_file(upath(srcdir + "/" + sid + ".txt"));
        std::vector<std::string> v;
        size_t a = 0;
        for (size_t k = 0; k <= b.size(); k++)
            if (k == b.size() || b[k] == '\n') {
                std::string x(b.begin() + std::ptrdiff_t(a), b.begin() + std::ptrdiff_t(k));
                if (!x.empty() && x.back() == '\r') x.pop_back();
                v.push_back(x);
                a = k + 1;
            }
        return cache[sid] = v;
    };
    std::vector<std::string> o;
    const json::Value *ops = recipe.get("ops");
    if (!ops) throw Stop("recipe without ops");
    for (const json::Value &op : ops->items) {
        if (op.items.at(0).kind == json::Value::String && op.items.at(0).s == "L") {
            for (const json::Value &x : op.items.at(1).items) o.push_back(x.s + (op.items.at(2).truthy() ? "\r" : ""));
        } else {
            const std::string sid = op.items.at(1).s;
            const int64_t i = op.items.at(2).i, n = op.items.at(3).i;
            const bool crlf = op.items.at(4).truthy(), strip = op.items.at(5).truthy();
            const auto &src = lines(sid);
            const int64_t have = std::max<int64_t>(0, std::min<int64_t>(int64_t(src.size()), i + n) - std::min<int64_t>(i, int64_t(src.size())));
            if (have != n) return fmt("source %s too short (%lld lines from %lld)", sid.c_str(), (long long)n, (long long)i);
            for (int64_t k = i; k < i + n; k++) o.push_back((strip ? py_rstrip(src[size_t(k)]) : src[size_t(k)]) + (crlf ? "\r" : ""));
        }
    }
    std::string s;
    for (size_t k = 0; k < o.size(); k++) s += o[k] + (k + 1 < o.size() ? "\n" : "");
    out.assign(s.begin(), s.end());
    return "";
}

}  // namespace

int registry_final(const std::vector<std::string> &argv) {
    std::vector<std::string> opts, want;
    bool apply = false;
    for (size_t i = 0; i < argv.size(); i++) {
        if (argv[i] == "--apply") apply = true;
        else if (argv[i].rfind("--", 0) == 0 && i + 1 < argv.size()) {
            opts.push_back(argv[i]);
            opts.push_back(argv[++i]);
        } else if (argv[i].rfind("--", 0) != 0) want.push_back(argv[i]);
    }
    if (want.empty()) want = {"overlay-mp", "overlay-port", "overlay-r2", "overlay-r3"};
    const Args a(opts, {}, {"--project", "--job", "--ps4", "--ps3-update", "--ps3-dlcmaps", "--ps3-disc", "--ps3-data2"});
    const std::string project = a.need("--project"), job = a.need("--job"), work = project + "/work", src = project + "/scratch\\pak23-registry-sources";
    std::map<std::string, std::string> ref;
    {
        std::string t;
        for (uint8_t c : read_file(upath(job + "/recipes/sha256.tsv"))) t += char(c);
        for (const std::string &l : py_splitlines(t)) {
            const auto p = py_split(l);
            if (p.size() != 3) throw Stop("ValueError: recipes/sha256.tsv: a line without 3 fields");
            ref[p[0]] = p[2];
        }
    }
    const std::string mark = src + "\\sources-v2";
    if (!fs::exists(upath(src + "\\retail.txt")) || !fs::exists(upath(mark))) {
        std::string printed, err;
        try {
            printed = "sources " + std::to_string(regsources(a.need("--ps4"), a.get("--ps3-update", ""), a.get("--ps3-dlcmaps", ""), a.get("--ps3-disc", ""),
                                                             a.get("--ps3-data2", ""), src));
        } catch (const std::exception &e) {
            err = e.what();
        }
        std::printf("%s %s\n", printed.c_str(), err.c_str());
        if (!err.empty()) return 1;
        const std::string m = "regsources with GOTY aliases\r\n";   // written in text mode
        write_file_replace(upath(mark), Bytes(m.begin(), m.end()));
    }
    int rc = 0;
    for (const std::string &o : want) {
        const std::string dst = work + "\\" + o + "\\u3data\\build\\main\\pak23_ps4.txt", tmp = src + "\\" + o + ".pak23_ps4.txt";
        if (!ref.count(o)) throw Stop("KeyError: '" + o + "' not in recipes/sha256.tsv");
        Bytes data;
        std::string err;
        try {
            err = regbuild(read_json(upath(job + "/recipes/" + o + ".json")), src, data);
        } catch (const std::exception &e) {
            err = e.what();
        }
        if (!err.empty()) {   // the child printed nothing, then its error
            std::printf("\n%s\n", err.c_str());
            rc = 1;
            continue;
        }
        const std::string h = sha256_hex(data);
        if (h != ref[o]) {
            std::printf("MISMATCH %s: built %s, reference %s (not written)\n\n", tmp.c_str(), h.substr(0, 16).c_str(), ref[o].substr(0, 16).c_str());
            rc = 1;
            continue;
        }
        if (fs::exists(upath(tmp)) && read_file(upath(tmp)) == data) {
            std::printf("already applied %s\n", tmp.c_str());
        } else {
            fs::create_directories(upath(tmp).parent_path());
            write_file_replace(upath(tmp), data);
            std::printf("wrote %s (%zu bytes, sha256 %s)\n", tmp.c_str(), data.size(), h.substr(0, 16).c_str());
        }
        if (fs::exists(upath(dst)) && read_file(upath(dst)) == data) {
            std::printf("already applied %s\n", o.c_str());
            continue;
        }
        if (!apply) {
            std::printf("would write %s\n", dst.c_str());
            continue;
        }
        if (fs::exists(upath(dst))) {
            const std::string bk = work + "\\backup-pak23-registry\\" + o + "\\pak23_ps4.txt";
            if (!fs::exists(upath(bk))) {
                fs::create_directories(upath(bk).parent_path());
                copy2(upath(dst), upath(bk));
            }
        }
        fs::create_directories(upath(dst).parent_path());
        write_file_replace(upath(dst), data);
        std::printf("installed %s\n", o.c_str());
    }
    return rc;
}

}  // namespace u3
