// Three overlay-r3 DC edits of net.bin (and the module size table), replacing elim_pic.py, classic.py and
// skins_net_names.py of dctools:
//   u3fix elim-pic        --project P --ps3-data2 D
//       the Elimination playlist picture: net.bin playlist 10 (table 0xf985c7b6, entries 0x58) gets the picture string
//       'net-playlists/gametype-elimination' in its empty picture field; modules.bin's net.bin size word follows. Works
//       from the copies in work/backup-elim-pic (made once); 'already applied' when the picture is set.
//   u3fix classic         --project P --ps3-data2 D [NET] [--dry-run]
//       Classic netgames (final form of 2026-11-01 03:33): every netgame +0x68 = +0x6c (the kickback / weapon-mod flag
//       the helper reads), the 23 type-0x13 netgames loadout +0x2c 0x8cf8e1cc -> 0xd932420c, settings +0x64
//       0x63e35446 -> 0x1ad223b4, +0x6c 1 -> 0 (boosters on). Same size. Backup once to work/backup-classic. Nothing
//       to do when the table is in PS3 1.19 order (netgame-order-119) or has no old-form Classic netgame.
//   u3fix skins-net-names --project P [--apply]
//       weapon skins: every weapon-mod art path 'objects/weapons/<w>/<w>-mod/<name>' of net.bin (tables 0xe77d8fba /
//       0xa94f33c0, entries 0x28, fields +0x18 / +0x20) and weapon-art.bin (0x61d589d9 variants +0x58 / +0x60) is
//       repointed to the full 'path:name,guid,shader' name the converted weapon actor paks of overlay-r3 carry (the
//       first in sort order); the modules.bin size words follow. Backup once to work/backup-skins.
#include "dc.hpp"
#include "proc.hpp"
#include "u3fix.hpp"

namespace u3 {
namespace {

const uint32_t NG = 0x3b9a067d, PL_TABLE = 0xf985c7b6;
const uint64_t NG3 = 0xe4, NG4 = 0xd0;   // netgame entry size PS3 1.19 / PS4

std::string dir_of(const std::string &project) { return project + "/work/overlay-r3/u3data/build/main/dc1/ps4/"; }
uint64_t u64le(const Bytes &d, uint64_t o) {
    if (o + 8 > d.size()) throw Stop("struct.error: unpack_from out of range");
    return rd64le(&d[o]);
}
void put64(Bytes &d, uint64_t o, uint64_t v) {
    if (o + 8 > d.size()) throw Stop("struct.error: pack_into out of range");
    wr64le(&d[o], v);
}
const std::pair<uint32_t, uint64_t> &sym_of(const Dc &c, uint32_t s) {
    auto it = c.bysid.find(s);
    if (it == c.bysid.end()) throw Stop(u3::fmt("KeyError: %u", s));
    return it->second;
}
// (count, array) of a table symbol (count u32, then a pointer)
std::pair<uint32_t, uint64_t> table(const Dc &c, uint32_t s) {
    const uint64_t o = sym_of(c, s).second;
    return {c.u32(o), c.ptr(o + uint64_t(c.ps))};
}
// is the net.bin netgame table in PS3 1.19 order (netgame-order-119)? exact with net18.bin, else > 256 netgames
bool is_ps3_order(const Dc &c, const std::string &net18) {
    const auto [n, ar] = table(c, NG);
    if (!net18.empty() && fs::is_regular_file(upath(net18))) {
        const Dc b(upath(net18));
        const auto [n3, ar3] = table(b, NG);
        if (n != n3) return false;
        for (uint32_t i = 0; i < n; i++)
            for (uint64_t k : {0, 4, 0xc})
                if (c.u32(ar + i * NG4 + k) != b.u32(ar3 + i * NG3 + k)) return false;
        return true;
    }
    return n > 256;
}

}  // namespace

int elim_pic(const std::vector<std::string> &argv) {
    const Args a(argv, {}, {"--project", "--ps3-data2"});
    const std::string R = a.need("--project") + "/work/", D = dir_of(a.need("--project")), BK = R + "backup-elim-pic/";
    fs::create_directories(upath(BK));
    for (const char *f : {"net.bin", "modules.bin"})
        if (!fs::exists(upath(BK + f))) copy2(upath(D + f), upath(BK + f));
    const Dc b(upath(a.get("--ps3-data2", "") + "/net18.bin"));
    {
        const Dc c(upath(D + "net.bin"));
        const uint64_t e = c.ptr(sym_of(c, PL_TABLE).second + 8) + 10 * 0x58;
        for (uint64_t k = 0; k < 0x58; k += 8)
            if (c.isptr(e + k) && c.cstr(c.ptr(e + k)) == std::optional<std::string>("net-playlists/gametype-elimination")) {
                std::printf("already applied: elimination picture is set\n");
                return 0;
            }
    }
    {   // the PS3 playlist stride and picture field (a report)
        const uint64_t o = sym_of(b, PL_TABLE).second, ar = b.ptr(o + 4);
        for (uint64_t st = 0x40; st < 0x60; st += 4) {
            std::vector<uint64_t> ok, ok0;
            for (uint64_t k = 0; k < st; k += 4) {
                if (b.isptr(ar + 10 * st + k) && b.str_or_empty(b.ptr(ar + 10 * st + k)).rfind("net-playlists", 0) == 0) ok.push_back(k);
                if (b.isptr(ar + k) && b.str_or_empty(b.ptr(ar + k)).rfind("net-playlists", 0) == 0) ok0.push_back(k);
            }
            if (!ok.empty() && ok == ok0) {
                std::string f;
                for (uint64_t k : ok) f += (f.empty() ? "'" : ", '") + u3::fmt("%#llx", (unsigned long long)k) + "'";
                std::printf("ps3 stride %#llx field [%s] %s\n", (unsigned long long)st, f.c_str(), b.str_or_empty(b.ptr(ar + 10 * st + ok[0])).c_str());
            }
        }
    }
    const Dc an(upath(BK + "net.bin"));
    const uint64_t ar = an.ptr(sym_of(an, PL_TABLE).second + 8), e = ar + 10 * 0x58;
    std::vector<uint64_t> fields;
    for (uint64_t k = 0; k < 0x58; k += 8)
        if (an.isptr(e + k)) fields.push_back(k);
    std::string fs_;
    for (uint64_t k : fields) fs_ += (fs_.empty() ? "'" : ", '") + u3::fmt("%#llx", (unsigned long long)k) + "'";
    std::printf("ps4 ptr fields [%s] %#x\n", fs_.c_str(), an.u32(e));
    std::optional<uint64_t> k;
    for (uint64_t x : fields)
        if (an.isptr(ar + x) && an.str_or_empty(an.ptr(ar + x)).rfind("net-playlists", 0) == 0) { k = x; break; }
    if (!k) throw Stop("IndexError: list index out of range");
    if (!an.str_or_empty(an.ptr(e + *k)).empty()) throw Stop("AssertionError: the picture field is not empty");
    DcEdit E(upath(BK + "net.bin"));
    E.putptr(e + *k, E.add_string("net-playlists/gametype-elimination"));
    const Bytes net = E.build();
    write_file_replace(upath(D + "net.bin"), net);
    Bytes m = read_file(upath(BK + "modules.bin"));
    if (u64le(m, 192352) != fs::file_size(upath(BK + "net.bin"))) throw Stop("AssertionError: modules.bin net.bin size word");
    put64(m, 192352, net.size());
    write_file_replace(upath(D + "modules.bin"), m);
    const Dc c(upath(D + "net.bin"));
    std::printf("%s %zu %llu\n", c.str_or_empty(c.ptr(c.ptr(sym_of(c, PL_TABLE).second + 8) + 10 * 0x58 + *k)).c_str(), net.size(), (unsigned long long)u64le(m, 192352));
    return 0;
}

int classic(const std::vector<std::string> &argv) {
    std::vector<std::string> rest;
    std::string net;
    for (const std::string &x : argv) {
        if (net.empty() && rest.empty() && !x.empty() && x.rfind("--", 0) != 0) net = x;   // sys.argv[1] when not a flag
        else rest.push_back(x);
    }
    const Args a(rest, {"--dry-run"}, {"--project", "--ps3-data2"});
    const std::string BK = a.need("--project") + "/work/backup-classic/";
    if (net.empty()) net = dir_of(a.need("--project")) + "net.bin";
    const std::string net18 = a.get("--ps3-data2", "").empty() ? "" : a.get("--ps3-data2", "") + "/net18.bin";
    const Dc c0(upath(net));
    if (is_ps3_order(c0, net18)) {
        std::printf("already applied: the netgame table is in PS3 1.19 order (netgame-order-119), Classic values are PS3's\n");
        return 0;
    }
    const uint64_t q = sym_of(c0, NG).second, ar = c0.ptr(q + 8);
    const uint32_t N = c0.u32(q);
    bool any_old = false;
    for (uint32_t i = 0; i < N; i++) any_old = any_old || (c0.u32(ar + i * 0xd0 + 0xc) == 0x13 && c0.u32(ar + i * 0xd0 + 0x2c) == 0x8cf8e1cc);
    if (!any_old) {
        std::printf("already applied: no Classic netgame in the old form\n");
        return 0;
    }
    DcEdit e(upath(net));
    int n = 0;
    for (uint32_t i = 0; i < N; i++) {
        const uint64_t g = ar + i * 0xd0;
        e.put32(g + 0x68, c0.u32(g + 0x6c));   // the kickback / mod flag
        if (c0.u32(g + 0xc) != 0x13) continue;
        if (!(c0.u32(g + 0x2c) == 0x8cf8e1cc && c0.u32(g + 0x64) == 0x63e35446 && c0.u32(g + 0x6c) == 1)) throw Stop("AssertionError: a Classic netgame not in the old form");
        e.put32(g + 0x2c, 0xd932420c);
        e.put32(g + 0x64, 0x1ad223b4);
        e.put32(g + 0x6c, 0);   // boosters on
        n++;
    }
    if (n != 23) throw Stop(u3::fmt("AssertionError: %d", n));
    const Bytes out = e.build();
    if (out.size() != e.orig.size()) throw Stop("AssertionError: size changed");
    if (a.has("--dry-run")) {
        std::printf("dry run: %d Classic netgames would change\n", n);
        return 0;
    }
    fs::create_directories(upath(BK));
    if (!fs::exists(upath(BK + "net.bin"))) copy2(upath(net), upath(BK + "net.bin"));
    write_file_replace(upath(net), out);
    const Dc c(upath(net));
    const uint64_t q2 = sym_of(c, NG).second, ar2 = c.ptr(q2 + 8);
    int now = 0;
    for (uint32_t i = 0; i < c.u32(q2); i++) now += c.u32(ar2 + i * 0xd0 + 0xc) == 0x13 && c.u32(ar2 + i * 0xd0 + 0x2c) == 0xd932420c;
    std::printf("%d %zu %d\n", n, out.size(), now);
    return 0;
}

int skins_net_names(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply"}, {"--project"});
    const std::string W = a.need("--project") + "/work/overlay-r3/u3data/build/main/", D = W + "dc1/ps4/", B = a.need("--project") + "/work/backup-skins/";
    // full material names (path:name,guid,shader) of the converted weapon actor paks
    std::map<std::string, std::set<std::string>> full;
    if (fs::is_directory(upath(W + "actor33/ps4")))
        for (const auto &ent : fs::directory_iterator(upath(W + "actor33/ps4"))) {
            const std::string fn = utf8(ent.path().filename());
            if (fn.size() < 4 || fn.compare(fn.size() - 4, 4, ".pak")) continue;
            const Bytes d = read_file(ent.path());
            static const std::string PRE = "objects/weapons/";
            auto c1 = [](uint8_t c) { return std::isalnum(c) || c == '_' || c == '.' || c == '/' || c == '-'; };
            auto c2 = [](uint8_t c) { return std::isalnum(c) || c == '_' || c == '.' || c == '-'; };
            size_t i = 0;
            while (true) {   // re.findall(rb'objects/weapons/[a-zA-Z0-9_./-]+:[a-zA-Z0-9_.-]+(?:,[^\0]*)?\0')
                auto it = std::search(d.begin() + std::ptrdiff_t(i), d.end(), PRE.begin(), PRE.end());
                if (it == d.end()) break;
                const size_t s = size_t(it - d.begin());
                size_t p = s + PRE.size();
                const size_t r1 = p;
                while (p < d.size() && c1(d[p])) p++;
                bool ok = p > r1 && p < d.size() && d[p] == ':';
                size_t end = 0;
                if (ok) {
                    const size_t r2 = ++p;
                    while (p < d.size() && c2(d[p])) p++;
                    ok = p > r2;
                    if (ok && p < d.size() && d[p] == ',') {
                        size_t z = p;
                        while (z < d.size() && d[z]) z++;
                        if (z < d.size()) end = z;
                        else ok = false;
                    } else if (ok && p < d.size() && d[p] == 0)
                        end = p;
                    else
                        ok = false;
                }
                if (!ok) {
                    i = s + 1;
                    continue;
                }
                const std::string x(d.begin() + std::ptrdiff_t(s), d.begin() + std::ptrdiff_t(end));
                full[x.substr(0, x.find(','))].insert(x);
                i = end + 1;
            }
        }
    {
        std::vector<std::string> multi;
        for (const auto &[k, v] : full)
            if (v.size() > 1) multi.push_back(k);
        if (!multi.empty()) std::printf("warning: several full names for %zu art paths (the first in sort order is used)\n", multi.size());
    }
    auto net_slots = [](const Dc &c) {
        std::vector<uint64_t> r;
        for (uint32_t sy : {0xe77d8fbau, 0xa94f33c0u}) {
            const uint64_t o = sym_of(c, sy).second, ar = c.ptr(o + 8);
            for (uint32_t i = 0; i < c.u32(o); i++) r.push_back(ar + i * 0x28ull + 0x18), r.push_back(ar + i * 0x28ull + 0x20);
        }
        return r;
    };
    auto art_slots = [](const Dc &c) {
        std::vector<uint64_t> r;
        const uint64_t o = sym_of(c, 0x61d589d9).second, vs = c.ptr(o + 16);
        for (uint32_t i = 0; i < c.u32(o); i++) {
            const uint64_t v = c.ptr(vs + i * 8ull);
            if (v) r.push_back(v + 0x58), r.push_back(v + 0x60);
        }
        return r;
    };
    {
        const Dc cn(upath(D + "net.bin"));
        for (uint64_t q : net_slots(cn))
            if (cn.ptr(q) && cn.str_or_empty(cn.ptr(q)).find(',') != std::string::npos) {
                std::printf("already applied: net.bin weapon-mod art names are full names\n");
                return 0;
            }
    }
    if (full.empty()) {
        std::fprintf(stderr, "no converted weapon actor paks with full material names in %sactor33/ps4\n", W.c_str());
        return 1;
    }
    std::vector<std::pair<std::string, Bytes>> outs;
    for (const auto &[fn, slots] : std::vector<std::pair<std::string, std::function<std::vector<uint64_t>(const Dc &)>>>{{"net.bin", net_slots}, {"weapon-art.bin", art_slots}}) {
        const Dc c(upath(D + fn));
        DcEdit e(upath(D + fn));
        int n = 0, done = 0;
        std::vector<std::string> miss;
        std::map<std::string, uint64_t> cache;
        for (uint64_t q : slots(c)) {
            const uint64_t p = c.ptr(q);
            if (!p) continue;
            const auto s = c.cstr(p);
            if (!s || s->empty() || s->rfind("objects/weapons/", 0) != 0) continue;
            if (s->find(',') != std::string::npos) {
                done++;
                continue;
            }
            std::string k = *s;
            if (k.find(':') == std::string::npos) {
                const size_t sl = k.rfind('/');
                if (sl != std::string::npos) k[sl] = ':';
            }
            auto f = full.find(k);
            if (f != full.end()) {
                const std::string &fname = *f->second.begin();
                if (!cache.count(fname)) cache[fname] = e.add_string(fname);
                e.putptr(q, cache[fname]);
                n++;
            } else
                miss.push_back(k);
        }
        Bytes out = e.build();
        std::string ms;
        for (size_t i = 0; i < std::min<size_t>(8, miss.size()); i++) ms += (ms.empty() ? "'" : ", '") + miss[i] + "'";
        std::printf("%s repointed %d already full %d miss [%s] %llu -> %zu\n", fn.c_str(), n, done, ms.c_str(), (unsigned long long)fs::file_size(upath(D + fn)), out.size());
        outs.push_back({fn, std::move(out)});
    }
    Bytes m = read_file(upath(D + "modules.bin"));
    for (const auto &[fn, out] : outs) {   // the module size words
        const uint64_t old = fs::file_size(upath(D + fn));
        std::vector<size_t> hits;
        for (size_t i = 0; i + 8 <= m.size(); i++)
            if (rd64le(&m[i]) == old) hits.push_back(i);
        if (hits.size() != 1) throw Stop("AssertionError: " + fn + " size word found " + std::to_string(hits.size()) + " times in modules.bin");
        put64(m, hits[0], out.size());
    }
    if (!a.has("--apply")) {
        std::printf("dry run, nothing written\n");
        return 0;
    }
    fs::create_directories(upath(B));
    for (const char *f : {"net.bin", "weapon-art.bin", "modules.bin"})
        if (!fs::exists(upath(B + f))) copy2(upath(D + f), upath(B + f));
    for (const auto &[fn, out] : outs) write_file_replace(upath(D + fn), out);
    write_file_replace(upath(D + "modules.bin"), m);
    std::printf("written\n");
    return 0;
}

}  // namespace u3
