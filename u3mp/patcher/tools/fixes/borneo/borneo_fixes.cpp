// Two Borneo (dig-coop) level fixes in the converted PS4 stage paks. Replace fix_water_jitter.py and fix_grass_leaf.py.
//
//   u3fix water-jitter --project P --ps3-disc D --ps3-data2 D2 [--only REGEX] [--apply]
//       The stage converter keeps the donor template's g_fRefractionJitter / g_fRefractionJitterHeight on water
//       materials (it does not split PS3's packed 'g_fRefractionJitter_fRefractionJitterHeight'). For every effective
//       level pak (overlay-r3 > -r2 > -port > -mp, no ingame12) with a water material, the two floats get the PS3
//       material's values (the packed pair, or the two separate ones) where they differ by more than 1e-4; a value
//       shared with another material is reported and left. Same size, in place. --apply writes <pak>.bak-jitter once.
//       PS3 sources: goty-patch, goty-dlc, the disc pak23.psarc, then DATA2's build/main/pak23/*.psarc.
//   u3fix grass-leaf --project P [--apply]
//       dig-grass-main (the moving floor's ground) was converted on a GLOBAL_LEAF (foliage sway) template in
//       dig-coop-approach / -ship / -stage. Its feature list, effect and feature count are pointed at the same pak's
//       dig-tree-rot-log:default when that donor's features are a subset of the target's, GLOBAL_LEAF among the
//       difference, with the same texture slots and the lists on the same pages (ship is refused). 20 bytes per pak.
//       --apply writes <pak>.bak-leaf once.
// Dry run by default.
#include "ndpak.hpp"
#include "ps3bg.hpp"
#include "psarc.hpp"
#include "u3fix.hpp"

#include <cstring>
#include <regex>

namespace u3 {
namespace {

std::string base_name(const std::string &p) {
    const size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? p : p.substr(k + 1);
}
std::string pyrepr(const std::optional<std::string> &s) { return s ? "'" + *s + "'" : "None"; }
std::string pyrepr(const std::vector<std::optional<std::string>> &v) {
    std::string o = "[";
    for (size_t k = 0; k < v.size(); k++) o += (k ? ", " : "") + pyrepr(v[k]);
    return o + "]";
}
std::string hexs(const Bytes &d, uint64_t a, uint64_t n) {
    std::string o;
    for (uint64_t k = 0; k < n; k++) o += u3::fmt("%02x", d.at(a + k));
    return o;
}

}  // namespace

int water_jitter(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply"}, {"--project", "--ps3-disc", "--ps3-data2", "--only"});
    const bool apply = a.has("--apply");
    const std::string WORK = a.need("--project") + "/work/", PS3 = a.need("--ps3-disc"), DATA2 = a.need("--ps3-data2") + "/build/main/pak23";
    std::optional<std::regex> only;
    if (a.val.count("--only")) only = std::regex(a.need("--only"));
    const std::string PACKED = "g_fRefractionJitter_fRefractionJitterHeight", J = "g_fRefractionJitter", H = "g_fRefractionJitterHeight";
    std::vector<std::unique_ptr<Psarc>> arcs;
    for (const std::string &f : {PS3 + "/goty-patch.psarc", PS3 + "/goty-dlc.psarc", PS3 + "/build/main/pak23.psarc"}) arcs.push_back(std::make_unique<Psarc>(upath(f)));
    if (fs::is_directory(upath(DATA2))) {
        std::vector<std::string> d2;
        for (const auto &e : fs::directory_iterator(upath(DATA2)))
            if (e.path().extension() == ".psarc") d2.push_back(utf8(e.path()));
        std::sort(d2.begin(), d2.end());
        for (const auto &f : d2) arcs.push_back(std::make_unique<Psarc>(upath(f)));
    }
    auto ps3_pak = [&](const std::string &name) -> std::optional<Bytes> {
        for (auto &ar : arcs)
            for (size_t i = 0; i < ar->names.size(); i++)
                if (base_name(ar->names[i]) == name) return ar->read(i + 1);
        return std::nullopt;
    };
    std::map<std::string, std::string> effective;   // name -> path, the first overlay wins
    for (const char *o : {"overlay-r3", "overlay-r2", "overlay-port", "overlay-mp"}) {
        const fs::path d = upath(WORK + o + "/u3data/build/main/pak23/ps4");
        if (!fs::is_directory(d)) continue;
        for (const auto &e : fs::directory_iterator(d))
            if (e.path().extension() == ".pak") effective.emplace(utf8(e.path().filename()), utf8(e.path()));
    }
    int total = 0;
    const std::string wfx = "water.fx";
    for (const auto &[name, path] : effective) {
        if (name.find("ingame12") != std::string::npos || (only && !std::regex_search(name, *only))) continue;
        const Bytes raw = read_file(upath(path));
        if (std::search(raw.begin(), raw.end(), wfx.begin(), wfx.end()) == raw.end()) continue;
        const Pak q(raw, name);
        const auto geo = q.items_of("GEOMETRY_1");
        if (geo.empty()) continue;
        const auto src = ps3_pak(name);
        if (!src) {
            std::printf("NO PS3 SOURCE %s\n", name.c_str());
            continue;
        }
        const auto p3 = bg::all_material_params(Pak(*src, name));
        const PakItem &g = geo[0];
        const uint32_t nmat = q.pu32(g.page, g.data_off() + 0x48);
        const Ref m = bg::need(q, g.page, g.data_off() + 0x50);
        std::map<uint64_t, int> uses;
        struct Edit {
            std::string key, pn;
            uint64_t off;
            double old, v;
        };
        std::vector<Edit> edits;
        for (uint32_t i = 0; i < nmat; i++) {
            const uint64_t o = m.second + uint64_t(i) * 0x48;
            const Ref fr = bg::need(q, m.first, o);
            const std::string full = q.pstr(fr.first, fr.second);
            const ORef sh = bg::res(q, m.first, o + 8);
            const uint32_t npar = rd32le(q.d, q.pbase(m.first) + o + 0x34);
            const ORef pr = bg::res(q, m.first, o + 0x20);
            std::map<std::string, uint64_t> rec;
            for (uint32_t k = 0; k < (pr ? npar : 0); k++) {
                const uint64_t po = pr->second + 0x18ull * k;
                const ORef vr = bg::res(q, pr->first, po + 8);
                const Ref pnr = bg::need(q, pr->first, po);
                const std::string pn = q.pstr(pnr.first, pnr.second);
                if (vr) {
                    const uint64_t off = q.pbase(vr->first) + vr->second;
                    uses[off]++;
                    rec[pn] = off;
                }
            }
            if (!sh || q.pstr(sh->first, sh->second).find("water") == std::string::npos || !rec.count(J)) continue;
            std::string key = full.substr(0, full.find(','));
            std::replace(key.begin(), key.end(), ':', '/');
            auto wi = p3.find(key);
            if (wi == p3.end() || wi->second.empty()) {
                std::printf("  no PS3 material %s %s\n", name.c_str(), key.c_str());
                continue;
            }
            const bg::Params &want = wi->second;
            std::vector<double> vals;
            if (const auto *pk = bg::param(want, PACKED); pk && !pk->empty()) vals = *pk;
            else if (bg::param(want, J) && bg::param(want, H)) vals = {bg::param(want, J)->at(0), bg::param(want, H)->at(0)};
            if (vals.empty()) {
                std::printf("  no PS3 jitter %s %s\n", name.c_str(), key.c_str());
                continue;
            }
            for (const auto &[pn, v] : {std::make_pair(J, vals.at(0)), std::make_pair(H, vals.at(1))}) {
                auto ri = rec.find(pn);
                if (ri == rec.end()) continue;
                float f;
                std::memcpy(&f, &raw.at(ri->second), 4);
                if (std::fabs(double(f) - v) > 1e-4) edits.push_back({key, pn, ri->second, double(f), v});
            }
        }
        if (edits.empty()) continue;
        const std::string rel = path.substr(path.find("work/") + 5);
        std::printf("%s %s\n", name.c_str(), rel.substr(0, rel.find('/')).c_str());
        Bytes data = raw;
        for (const Edit &e : edits) {
            const bool shared = uses[e.off] > 1;
            const std::string k62 = e.key.size() > 62 ? e.key.substr(e.key.size() - 62) : e.key;
            std::printf("  %-62s %-27s @%s %.4f -> %.4f%s\n", k62.c_str(), e.pn.c_str(), pyhex(e.off).c_str(), e.old, e.v, shared ? "  SHARED, skipped" : "");
            if (!shared) {
                const float f = float(e.v);
                std::memcpy(&data[e.off], &f, 4);
                total++;
            }
        }
        if (apply) {
            const fs::path bak = upath(path + ".bak-jitter");
            if (!fs::exists(bak)) write_file_replace(bak, raw);
            write_file_replace(upath(path), data);
        }
    }
    std::printf("%s: %d floats\n", apply ? "APPLIED" : "DRY RUN, would change", total);
    return 0;
}

int grass_leaf(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply"}, {"--project"});
    const bool apply = a.has("--apply");
    const std::string D = a.need("--project") + "/work/overlay-r3/u3data/build/main/pak23/ps4/";
    const std::string TARGET = "levels/dig/ground/dig-grass-main:default", DONOR = "levels/dig/tree/dig-tree-rot-log:default";
    int bad = 0;
    struct Info {
        uint32_t nf, nb;
        std::vector<std::string> fe;
        Ref fr;
        ORef fx;
        std::vector<std::optional<std::string>> sl;
    };
    for (const char *pak : {"dig-coop-approach", "dig-coop-ship", "dig-coop-stage"}) {
        const std::string path = D + pak + ".pak";
        const Bytes raw = read_file(upath(path));
        const Pak q(raw, std::string(pak) + ".pak");
        const auto gs = q.items_of("GEOMETRY_1");
        if (gs.empty()) throw Stop("IndexError: no GEOMETRY_1");
        const PakItem &g = gs[0];
        const uint32_t n = q.pu32(g.page, g.data_off() + 0x48);
        const Ref mr = bg::need(q, g.page, g.data_off() + 0x50);
        const uint32_t mp = mr.first;
        std::map<std::string, uint64_t> m;
        for (uint32_t i = 0; i < n; i++) {
            const uint64_t o = mr.second + uint64_t(i) * 0x48;
            const Ref nr = bg::need(q, mp, o);
            const std::string nm = q.pstr(nr.first, nr.second);
            m.emplace(nm.substr(0, nm.find(',')), o);   // the first of a name
        }
        auto info = [&](uint64_t o) {
            Info r;
            const uint64_t b = q.pbase(mp) + o + 0x30;
            r.nf = rd32le(q.d, b), r.nb = rd32le(q.d, b + 8);
            r.fr = bg::need(q, mp, o + 0x10);
            for (uint32_t k = 0; k < r.nf; k++) {
                const Ref f = bg::need(q, r.fr.first, r.fr.second + 8ull * k);
                r.fe.push_back(q.pstr(f.first, f.second));
            }
            const Ref br = bg::need(q, mp, o + 0x28);
            for (uint32_t k = 0; k < r.nb; k++) {
                const ORef x = bg::res(q, br.first, br.second + 0x20ull * k);
                r.sl.push_back(x ? std::optional<std::string>(q.pstr(x->first, x->second)) : std::nullopt);
            }
            r.fx = bg::res(q, mp, o + 0x18);
            return r;
        };
        if (!m.count(TARGET)) {
            std::printf("%s no target\n", pak);
            continue;
        }
        const Info t = info(m[TARGET]);
        if (std::find(t.fe.begin(), t.fe.end(), "GLOBAL_LEAF") == t.fe.end()) {
            std::printf("%s already without GLOBAL_LEAF\n", pak);
            continue;
        }
        if (!m.count(DONOR)) {
            std::printf("%s NO DONOR, needs a reconvert\n", pak);
            bad++;
            continue;
        }
        const Info d = info(m[DONOR]);
        const std::set<std::string> ts(t.fe.begin(), t.fe.end()), ds(d.fe.begin(), d.fe.end());
        std::vector<std::string> diff;
        std::set_symmetric_difference(ts.begin(), ts.end(), ds.begin(), ds.end(), std::back_inserter(diff));
        bool subset = true;
        for (const auto &x : ds) subset = subset && ts.count(x);
        if (!t.fx || !d.fx) throw Stop("TypeError: material without an effect");
        const bool ok = std::find(diff.begin(), diff.end(), "GLOBAL_LEAF") != diff.end() && subset && t.nb == d.nb && t.sl == d.sl && t.fr.first == d.fr.first &&
                        t.fx->first == d.fx->first;
        std::vector<std::optional<std::string>> dr(diff.begin(), diff.end());
        const uint64_t A = q.pbase(mp) + m[TARGET], B = q.pbase(mp) + m[DONOR];
        std::printf("%s target @%s donor @%s feature diff %s slots %s same page %s %s\n", pak, pyhex(A).c_str(), pyhex(B).c_str(), pyrepr(dr).c_str(), pyrepr(t.sl).c_str(),
                    t.fx->first == d.fx->first ? "True" : "False", ok ? "OK" : "REFUSED");
        if (!ok) {
            bad++;
            continue;
        }
        std::printf("   +0x10 %s -> %s  +0x18 %s -> %s  count %u -> %u\n", hexs(raw, A + 0x10, 8).c_str(), hexs(raw, B + 0x10, 8).c_str(), hexs(raw, A + 0x18, 8).c_str(),
                    hexs(raw, B + 0x18, 8).c_str(), t.nf, d.nf);
        if (apply) {
            Bytes data = raw;
            std::memcpy(&data[A + 0x10], &raw[B + 0x10], 0x10);
            std::memcpy(&data[A + 0x30], &raw[B + 0x30], 4);
            if (!fs::exists(upath(path + ".bak-leaf"))) write_file_replace(upath(path + ".bak-leaf"), raw);
            write_file_replace(upath(path), data);
        }
    }
    std::printf("%s refused: %d\n", apply ? "APPLIED" : "DRY RUN, nothing written", bad);
    return 0;
}

}  // namespace u3
