// Recipe step registry-pak23: each overlay's pak23_ps4.txt = the retail PS4 registry plus a PS4-format definition of
// every ported PS3 level named in <job>/lists/levels-<overlay>.txt.
//   u3fix registry-levels --project P --job J --ps4 D --ps3-update D --ps3-disc D <overlay> [<overlay> ...]
//
// The PS3 level definitions come from the newest pak23.txt: dlc-patch18.psarc > dlc-patch13.psarc > the disc's
// build/main/pak23.txt (copied to J/ps3-pak23.txt). Per overlay (in the order given; the other overlays that exist are
// extra roots, as the launcher loads them): the base is the overlay's pak23_ps4.txt, else the retail one; each listed
// level's PS3 lines are kept only when the data can satisfy them (package, actor, sound bank and particle module
// files present in a root; symbols defined in some dc1/ps4 module, as their sid bytes); dropped lines are reported,
// since a level that waits on a missing dependency never finishes loading. Output: the base's head, its other levels
// in order, then the listed levels; CRLF lines. Replaces run_mkregistry.py and transfer-tools/mkregistry.py (same
// messages; the per-overlay output is printed as the script printed its child's).
#include "u3fix.hpp"
#include "psarc.hpp"

#include <cstdio>
#include <unordered_set>

namespace u3 {
namespace {

std::string latin1_text(const Bytes &b) { return std::string(b.begin(), b.end()); }

struct Defs {
    std::map<std::string, std::vector<std::string>> defs;
    std::vector<std::string> order;   // every level-name line, repeats included
};
Defs parse_defs(const std::string &text) {
    Defs d;
    std::string cur;
    bool have = false;
    for (const std::string &line : py_splitlines(text)) {
        const std::vector<std::string> parts = py_split(line);
        if (parts.empty()) continue;
        if (parts[0] == "level-name") {
            cur = parts.size() > 1 ? parts[1] : throw Stop("IndexError: a level-name line without a name");
            d.defs[cur].clear();
            d.order.push_back(cur);
            have = true;
        }
        if (have) d.defs[cur].push_back(py_rstrip(line));
    }
    return d;
}

// one overlay: the mkregistry run; its printed lines go to `out`, an error to `err` (return 1)
int mkregistry(const std::string &ps4, const std::string &ps3txt, const std::string &ovl, const std::vector<std::string> &roots_extra,
               const std::vector<std::string> &levels, std::string &out) {
    std::string src = ovl + "\\pak23_ps4.txt";
    if (!fs::exists(upath(src))) src = ps4 + "\\pak23_ps4.txt";
    std::string base = latin1_text(read_file(upath(src)));
    for (size_t k; (k = base.find("\r\n")) != std::string::npos;) base.replace(k, 2, "\n");
    out += "base " + src + "\n";
    const Defs d4 = parse_defs(base);
    std::string t3 = latin1_text(read_file(upath(ps3txt)));   // text mode: \r\n and \r read as \n
    for (size_t k; (k = t3.find("\r\n")) != std::string::npos;) t3.replace(k, 2, "\n");
    for (char &c : t3)
        if (c == '\r') c = '\n';
    const Defs d3 = parse_defs(t3);

    std::vector<std::string> roots = {ovl};
    roots.insert(roots.end(), roots_extra.begin(), roots_extra.end());
    roots.push_back(ps4);
    auto exists = [&](const std::string &rel) {
        for (const std::string &r : roots)
            if (fs::exists(upath(r + "/" + rel))) return true;
        return false;
    };
    // the symbols the listed levels ask for, then one pass over every dc1/ps4 module of every root
    std::unordered_set<uint32_t> want, found;
    for (const std::string &lv : levels) {
        auto it = d3.defs.find(lv);
        if (it == d3.defs.end()) continue;
        for (const std::string &line : it->second) {
            const auto p = py_split(line);
            if (p[0] == "symbol") want.insert(sid(p.size() > 1 ? p[1] : ""));
        }
    }
    if (!want.empty())
        for (const std::string &r : roots) {
            const fs::path dcdir = upath(r + "/dc1/ps4");
            if (!fs::is_directory(dcdir)) continue;
            for (const auto &e : fs::recursive_directory_iterator(dcdir)) {
                const std::string n = utf8(e.path().filename());
                if (!e.is_regular_file() || n.size() < 4 || n.compare(n.size() - 4, 4, ".bin") != 0) continue;
                const Bytes b = read_file(e.path());
                for (size_t k = 0; k + 4 <= b.size(); k++) {
                    const uint32_t v = rd32le(&b[k]);
                    if (want.count(v)) found.insert(v);
                }
            }
        }
    std::map<std::string, std::vector<std::string>> out_defs;
    for (const std::string &lv : levels) {
        auto it = d3.defs.find(lv);
        if (it == d3.defs.end()) {
            out += "!! " + lv + ": not in the PS3 level list\n";
            continue;
        }
        std::vector<std::string> keep, drop;
        for (const std::string &line : it->second) {
            const auto p = py_split(line);
            const std::string &kind = p[0], val = p.size() > 1 ? p[1] : "";
            bool ok = true;
            if (kind == "package") {
                const std::string f = val.size() >= 7 && val.compare(val.size() - 7, 7, "-ingame") == 0 ? val.substr(0, val.size() - 7) + "-ingame12.pak" : val + ".pak";
                ok = exists("pak23/ps4/" + f);
            } else if (kind == "actor") ok = exists("actor33/ps4/" + val + ".pak");
            else if (kind == "sound-bank") ok = exists("sound1/banks/ps4/" + val + ".bnk");
            else if (kind == "particle-module") ok = exists("dc2/part-module/" + val + ".bin");
            else if (kind == "symbol") ok = found.count(sid(val)) != 0;
            (ok ? keep : drop).push_back(line);
        }
        std::string dropped;
        for (const std::string &l : drop) {
            const auto p = py_split(l);
            dropped += (dropped.empty() ? "" : ", ") + p[0] + (p.size() > 1 ? " " + p[1] : "");
        }
        out += fmt("%s: %zu lines kept, dropped: %s\n", lv.c_str(), keep.size(), dropped.empty() ? "-" : dropped.c_str());
        out_defs[lv] = keep;
    }
    std::vector<std::string> lines;
    for (const std::string &lv : d4.order)
        if (!out_defs.count(lv)) {
            const auto &v = d4.defs.at(lv);
            lines.insert(lines.end(), v.begin(), v.end());
        }
    for (const std::string &lv : levels)
        if (out_defs.count(lv)) lines.insert(lines.end(), out_defs[lv].begin(), out_defs[lv].end());
    const std::string dst = ovl + "\\pak23_ps4.txt";
    fs::create_directories(upath(ovl));
    const size_t h = base.find("level-name");
    std::string text = h == std::string::npos ? "" : base.substr(0, h);
    for (size_t i = 0; i < lines.size(); i++) text += lines[i] + (i + 1 < lines.size() ? "\n" : "");
    text += "\n";
    Bytes o;
    for (char c : text) {
        if (c == '\n') o.push_back('\r');
        o.push_back(uint8_t(c));
    }
    write_file_replace(upath(dst), o);
    out += "wrote " + dst + "\n";
    return 0;
}

}  // namespace

int registry_levels(const std::vector<std::string> &argv) {
    std::vector<std::string> opts, overlays;
    for (size_t i = 0; i < argv.size(); i++) {
        if (argv[i].rfind("--", 0) == 0 && i + 1 < argv.size()) {
            opts.push_back(argv[i]);
            opts.push_back(argv[++i]);
        } else {
            overlays.push_back(argv[i]);
        }
    }
    const Args a(opts, {}, {"--project", "--job", "--ps4", "--ps3-update", "--ps3-disc"});
    const std::string work = a.need("--project") + "/work/", job = a.need("--job"), ps4main = a.need("--ps4") + "/u3data/build/main";
    const std::string update = a.get("--ps3-update", ""), disc = a.get("--ps3-disc", "");
    // the PS3 1.19 pak23.txt, next to the job's files
    const std::string txt = win_path(job) + "\\ps3-pak23.txt";
    bool got = false;
    for (const char *arc : {"dlc-patch18.psarc", "dlc-patch13.psarc"}) {
        const std::string p = update + "/" + arc;
        if (got || !fs::exists(upath(p))) continue;
        Psarc ps(upath(p));
        for (size_t i = 0; i < ps.names.size() && !got; i++) {
            const std::string n = ps.names[i].substr(std::min(ps.names[i].find_first_not_of('/'), ps.names[i].size()));
            if (n == "build/main/pak23.txt") {
                write_file_replace(upath(txt), ps.read(i + 1));
                got = true;
            }
        }
    }
    if (!got) {
        const std::string src = disc + "/build/main/pak23.txt";
        if (!fs::exists(upath(src))) throw Stop("FileNotFoundError: no PS3 pak23.txt (dlc-patch18/13.psarc or the disc build/main/pak23.txt)");
        write_file_replace(upath(txt), read_file(upath(src)));
    }
    int rc = 0;
    for (const std::string &ov : overlays) {
        std::vector<std::string> levels;
        {
            std::string t = latin1_text(read_file(upath(job + "/lists/levels-" + ov + ".txt")));
            for (size_t k; (k = t.find("\r\n")) != std::string::npos;) t.replace(k, 2, "\n");   // read as text: lines end at \n
            for (char &c : t)
                if (c == '\r') c = '\n';
            size_t a0 = 0;
            for (size_t k = 0; k <= t.size(); k++) {
                if (k < t.size() && t[k] != '\n') continue;
                std::string s = py_rstrip(t.substr(a0, k - a0));   // l.strip()
                size_t b = 0;
                while (b < s.size() && py_space((unsigned char)s[b])) b++;
                if (b < s.size()) levels.push_back(s.substr(b));
                a0 = k + 1;
            }
        }
        const std::string main_dir = work + ov + "\\u3data\\build\\main";
        fs::create_directories(upath(main_dir));
        std::vector<std::string> extra;
        for (const char *o : {"overlay-r3", "overlay-r2", "overlay-port", "overlay-mp"})
            if (ov != o && fs::is_directory(upath(work + o + "\\u3data\\build\\main"))) extra.push_back(work + o + "\\u3data\\build\\main");
        std::printf("== %s %zu levels\n", ov.c_str(), levels.size());
        std::string out, err;
        int r = 0;
        try {
            r = mkregistry(ps4main, txt, main_dir, extra, levels, out);
        } catch (const std::exception &e) {
            err = std::string(e.what()) + "\n";
            r = 1;
        }
        if (out.size() > 3000) out = out.substr(out.size() - 3000);
        if (err.size() > 2000) err = err.substr(err.size() - 2000);
        std::printf("%s %s\n", out.c_str(), err.c_str());
        rc |= r;
    }
    return rc;
}

}  // namespace u3
