// Recipe step lab-registration: the Lab maps of PS3 1.19 in overlay-r3's net.bin (final form of 2026-11-02 00:26).
// Replaces lab_reg.py. From the copies in work/backup-lab (made once):
//   * net.bin: the 14 PS3 Lab maps (net18.bin map table 0xdec97a4, entries 0x58 -> PS4 0x98) appended to the map
//     table, each with its level list (cargo-plane-static dropped; a lab-multi main pak that the overlay has comes first
//     with sky-village-multi and multi-all); 6 PS3 netgames plus one Lab custom netgame per ready map (from 0xcaffc253)
//     appended to the netgame table (0xe4 -> 0xd0, the three sub-objects copied); one Lab playlist (0x5242014b) after
//     the PS4 ones, laid out like the first;
//   * net-always-loaded.bin: the PS3 loadout 0x3719ce57 as a new symbol (type 0x1755b0c7);
//   * modules.bin: the two size words;
//   * pak23_ps4.txt: the level blocks of the Lab levels from the PS3 dlc-patch18 pak23.txt (symbols the modules do not
//     know dropped, the texture streams added);
//   * work/backup-lab/lab_reg.log.json.
// 'already applied' when the 16th PS3 map is in net.bin.
//   u3fix lab-registration --project P --ps3-data2 D --ps3-update U
#include "dc.hpp"
#include "psarc.hpp"
#include "u3fix.hpp"

#include <regex>

namespace u3 {
namespace {

const uint32_t MAPS = 0xdec97a4, NG = 0x3b9a067d, PL = 0xc1b0be0d, LIST_T = 0xd14c6ffc, NG_OBJ = 0xf0670de8, NG_SUB = 0x8b617b51;

uint64_t sym_off(const Dc &c, uint32_t s) {
    auto it = c.bysid.find(s);
    if (it == c.bysid.end()) throw Stop(u3::fmt("KeyError: %u", s));
    return it->second.second;
}
struct Table {
    uint64_t o;
    uint32_t n;
    uint64_t ar;
};
Table table(const Dc &c, uint32_t s) {
    const uint64_t o = sym_off(c, s);
    return {o, c.u32(o), c.ptr(o + uint64_t(c.ps))};
}
std::string need_str(const Dc &c, uint64_t o) {
    auto s = c.cstr(o);
    if (!s) throw Stop("TypeError: no string");
    return *s;
}
void put32(Bytes &d, uint64_t o, uint32_t v) {
    if (o + 4 > d.size()) throw Stop("struct.error: pack_into out of range");
    wr32le(&d[o], v);
}
void check(bool ok, const std::string &m) {
    if (!ok) throw Stop("AssertionError: " + m);
}
template <class M>
const typename M::mapped_type &key_at(const M &m, uint32_t k) {   // m[k], KeyError like Python's dict
    auto it = m.find(k);
    if (it == m.end()) throw Stop(u3::fmt("KeyError: %#x", k));
    return it->second;
}

// new symbols in a PS4 DC module: the table after the 0x18-byte header grows by 16 per symbol, everything after it
// moves (pointers and their targets past the old table start follow), the relocation bitmap is made again
Bytes add_symbols(const Bytes &d, const std::vector<std::tuple<uint32_t, uint32_t, uint64_t>> &add) {
    check(d.size() >= 0x18 && std::memcmp(d.data(), "00CD", 4) == 0, "not a PS4 DC module");
    const uint64_t reloc = rd64le(d, 8);
    const uint32_t n = rd32le(d, 0x14), nb = rd32le(d, reloc);
    std::set<uint64_t> ptrs;
    for (uint32_t bi = 0; bi < nb; bi++) {
        const uint8_t b = d.at(reloc + 4 + bi);
        for (int k = 0; k < 8; k++)
            if (b >> k & 1) ptrs.insert((uint64_t(bi) * 8 + uint64_t(k)) * 8);
    }
    const uint64_t start = 0x18 + 16ull * n, delta = 16ull * add.size();
    std::vector<std::tuple<uint32_t, uint32_t, uint64_t>> all;
    std::set<uint32_t> have;
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t o = 0x18 + 16ull * i;
        all.push_back({rd32le(d, o), rd32le(d, o + 4), rd64le(d, o + 8)});
        have.insert(rd32le(d, o));
    }
    auto mv = [&](uint64_t o) { return o >= start ? o + delta : o; };
    for (auto &[s, t, v] : all) v = mv(v);
    for (const auto &[s, t, v] : add) {
        check(!have.count(s), "symbol exists");
        all.push_back({s, t, mv(v)});
    }
    std::sort(all.begin(), all.end());
    Bytes body(d.begin() + std::ptrdiff_t(start), d.begin() + std::ptrdiff_t(reloc));
    std::set<uint64_t> np;
    for (uint64_t p : ptrs) {
        if (p < start) continue;
        wr64le(&body[p - start], mv(rd64le(d, p)));
        np.insert(p + delta);
    }
    Bytes out(d.begin(), d.begin() + 0x18);
    wr32le(&out[0x14], uint32_t(n + add.size()));
    for (size_t i = 0; i < all.size(); i++) {
        const auto &[s, t, v] = all[i];
        uint8_t e[16];
        wr32le(e, s), wr32le(e + 4, t), wr64le(e + 8, v);
        out.insert(out.end(), e, e + 16);
        np.insert(0x18 + 16 * i + 8);
    }
    out.insert(out.end(), body.begin(), body.end());
    while (out.size() % 16) out.push_back(0);
    const uint64_t nr = out.size();
    const uint32_t nbytes = uint32_t((nr + 63) / 64);
    Bytes bm(nbytes, 0);
    for (uint64_t p : np) {
        check(p % 8 == 0 && p < nr, "pointer outside the data");
        bm[p / 64] |= uint8_t(1u << (p / 8 % 8));
    }
    wr64le(&out[8], nr);
    uint8_t nbw[4];
    wr32le(nbw, nbytes);
    out.insert(out.end(), nbw, nbw + 4);
    out.insert(out.end(), bm.begin(), bm.end());
    return out;
}

json::Value arr() {
    json::Value v;
    v.kind = json::Value::Array;
    return v;
}
json::Value strs(const std::vector<std::string> &v) {
    json::Value a = arr();
    for (const auto &s : v) a.items.push_back(json::Value::text(s));
    return a;
}

}  // namespace

int lab_registration(const std::vector<std::string> &argv) {
    const Args a(argv, {}, {"--project", "--ps3-data2", "--ps3-update"});
    const std::string ROOT = a.need("--project") + "/work/", MAIN = ROOT + "overlay-r3/u3data/build/main/", D = MAIN + "dc1/ps4/", BK = ROOT + "backup-lab/";
    fs::create_directories(upath(BK));
    for (const char *f : {"dc1/ps4/net.bin", "dc1/ps4/net-always-loaded.bin", "dc1/ps4/modules.bin", "pak23_ps4.txt"}) {
        const std::string t = BK + fs::path(f).filename().string();
        if (!fs::exists(upath(t))) copy2(upath(MAIN + f), upath(t));
    }
    const Dc b(upath(a.need("--ps3-data2") + "/net18.bin"));
    {
        const Dc c(upath(D + "net.bin"));
        const Table t = table(c, MAPS);
        std::set<uint32_t> ids;
        for (uint32_t i = 0; i < t.n; i++) ids.insert(c.u32(c.ptr(sym_off(c, MAPS) + 8) + uint64_t(i) * 0x98));
        const uint64_t o3 = sym_off(b, MAPS);
        if (ids.count(b.u32(b.ptr(o3 + 4) + 15 * 0x58))) {
            std::printf("already applied: Lab maps are in net.bin\n");
            return 0;
        }
    }
    const Dc an(upath(BK + "net.bin"));
    DcEdit e(upath(BK + "net.bin"));
    std::map<std::string, uint64_t> strcache;
    auto string = [&](const std::string &s) {
        auto it = strcache.find(s);
        if (it != strcache.end()) return it->second;
        const std::string enc = s + std::string(1, '\0');
        const auto &d = an.d;
        uint64_t pos = std::string::npos;
        for (auto f = std::search(d.begin(), d.end(), enc.begin(), enc.end()); f != d.end(); f = std::search(f + 1, d.end(), enc.begin(), enc.end())) {
            const uint64_t p = uint64_t(f - d.begin());
            if (p >= an.reloc) break;
            if (p == 0 || d[p - 1] == 0) {
                pos = p;
                break;
            }
        }
        if (pos == std::string::npos) pos = e.add_string(s);
        return strcache[s] = pos;
    };
    auto obj = [&](uint32_t t, size_t size) {
        e.align(8);
        uint8_t h[8] = {0, 0, 0, 0};
        wr32le(h + 4, t);
        e.d.insert(e.d.end(), h, h + 8);
        const uint64_t o = e.d.size();
        e.d.resize(e.d.size() + size, 0);
        return o;
    };
    auto obj4 = [&](uint32_t t, const std::vector<uint32_t> &w) {
        e.align(4);
        uint8_t h[4];
        wr32le(h, t);
        e.d.insert(e.d.end(), h, h + 4);
        const uint64_t o = e.d.size();
        for (uint32_t x : w) {
            wr32le(h, x);
            e.d.insert(e.d.end(), h, h + 4);
        }
        return o;
    };
    const std::set<std::string> DROP = {"cargo-plane-static"};
    const std::vector<uint32_t> LABMAPS = {15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 82};
    const std::set<uint32_t> READY = {15, 16, 17, 18, 20, 21, 22, 23, 24, 25, 27};   // 19, 26, 82 need lt-box / mixer-map / ballroom first
    std::set<std::string> PAKS;
    for (const auto &x : fs::directory_iterator(upath(MAIN + "pak23/ps4"))) PAKS.insert(utf8(x.path().filename()));
    json::Value logmaps = arr(), lognet = arr();
    std::set<std::string> levels = {"lab-stage"};
    std::map<uint32_t, std::pair<uint32_t, std::string>> mains;
    {
        const Table t4 = table(an, MAPS), t3 = table(b, MAPS);
        std::set<uint32_t> ids4;
        for (uint32_t i = 0; i < t4.n; i++) ids4.insert(an.u32(t4.ar + uint64_t(i) * 0x98));
        const uint64_t na = e.alloc(0x98 * (t4.n + LABMAPS.size()), 16);
        e.copy_words(t4.ar, na, 0x98ull * t4.n);
        for (size_t j = 0; j < LABMAPS.size(); j++) {
            const uint32_t i3 = LABMAPS[j];
            const uint64_t src = t3.ar + uint64_t(i3) * 0x58;
            const uint32_t mid = b.u32(src);
            check(!ids4.count(mid), "map already in net.bin");
            const uint64_t l3 = b.ptr(src + 0x30);
            check(b.u32(l3 - 4) == LIST_T, "map level list type");
            const uint32_t ln = b.u32(l3);
            const uint64_t li = b.ptr(l3 + 4);
            std::vector<std::string> sub;
            for (uint32_t k = 0; k < ln; k++) {
                const std::string s = need_str(b, b.ptr(li + 8ull * k));
                if (!DROP.count(s)) sub.push_back(s);
            }
            const std::string main = need_str(b, b.ptr(src + 0x18)), g = main.substr(0, main.size() - 4);
            if (main.rfind("lab-multi", 0) == 0 && std::find(sub.begin(), sub.end(), g) == sub.end() && PAKS.count(g + ".pak")) {
                std::vector<std::string> ns = {g, main, "sky-village-multi", "multi-all"};
                for (const auto &s : sub)
                    if (s != "sky-village-multi" && s != "multi-all" && s != main) ns.push_back(s);
                sub = ns;
            }
            const uint64_t items = e.alloc(16 * sub.size(), 8);
            for (size_t k = 0; k < sub.size(); k++) {
                e.putptr(items + 16 * k, string(sub[k]));
                put32(e.d, items + 16 * k + 8, sid(sub[k]));
            }
            const uint64_t lst = obj(LIST_T, 16);
            put32(e.d, lst, uint32_t(sub.size()));
            e.putptr(lst + 8, items);
            const uint64_t m = na + 0x98 * (t4.n + j);
            auto u = [&](uint64_t k3, uint64_t k4) { put32(e.d, m + k4, b.u32(src + k3)); };
            auto p = [&](uint64_t k3, uint64_t k4) {
                if (b.isptr(src + k3)) e.putptr(m + k4, string(need_str(b, b.ptr(src + k3))));
                else check(b.u32(src + k3) == 0, "map field");
            };
            u(0, 0), u(4, 4), u(8, 8), p(0xc, 0x10), p(0x10, 0x18), p(0x18, 0x28), u(0x1c, 0x30), p(0x20, 0x38), u(0x24, 0x40), p(0x28, 0x48), u(0x2c, 0x50);
            e.putptr(m + 0x58, lst);
            u(0x38, 0x68), u(0x3c, 0x6c), p(0x40, 0x70), u(0x44, 0x78), p(0x48, 0x80), u(0x4c, 0x88), u(0x50, 0x90), u(0x54, 0x94);
            mains[i3] = {mid, main};
            for (const auto &s : sub)
                if (s.rfind("lab-", 0) == 0) levels.insert(s);
            levels.insert(main);
            json::Value row = arr();
            row.items = {json::Value::text(u3::fmt("%08x", mid)), json::Value::text(main), json::Value::text(need_str(b, b.ptr(src + 0xc))), strs(sub)};
            logmaps.items.push_back(row);
        }
        e.put32(t4.o, uint32_t(t4.n + LABMAPS.size()));
        e.putptr(t4.o + 8, na);
    }
    // netgames: 6 PS3 ones, then one Lab custom game per ready map
    std::vector<std::tuple<uint32_t, std::optional<uint32_t>, uint32_t>> ALL;
    for (uint32_t s : {0x0d18cb6fu, 0x2653000eu, 0x73bddaa6u, 0x2b1026d7u, 0x22921db9u, 0x3cd54dbcu}) ALL.push_back({s, std::nullopt, s});
    std::vector<uint32_t> CL;
    for (uint32_t i : READY)
        if (i != 18) {
            ALL.push_back({sid("lab-custom-" + key_at(mains, i).second), key_at(mains, i).first, 0xcaffc253});
            CL.push_back(sid("lab-custom-" + key_at(mains, i).second));
        }
    {
        const Table t4 = table(an, NG), t3 = table(b, NG);
        std::map<uint32_t, uint32_t> idx3;
        for (uint32_t i = 0; i < t3.n; i++) idx3[b.u32(t3.ar + uint64_t(i) * 0xe4)] = i;
        const uint64_t na = e.alloc(0xd0 * (t4.n + ALL.size()), 16);
        e.copy_words(t4.ar, na, 0xd0ull * t4.n);
        for (size_t j = 0; j < ALL.size(); j++) {
            const auto &[s, mp, srcs] = ALL[j];
            const uint64_t src = t3.ar + uint64_t(key_at(idx3, srcs)) * 0xe4, g = na + 0xd0 * (t4.n + j);
            for (uint64_t k3 = 0; k3 < 0xe4; k3 += 4)
                if (b.isptr(src + k3) && k3 != 0x74) throw Stop(u3::fmt("ptr %llx", (unsigned long long)k3));
            for (uint64_t k = 0; k < 0x48; k += 4) put32(e.d, g + k, b.u32(src + k));
            put32(e.d, g, s);
            if (mp) put32(e.d, g + 4, *mp);
            std::vector<std::pair<uint64_t, uint64_t>> moves = {{0x50, 0x58}, {0x54, 0x5c}, {0x58, 0x60}, {0x5c, 0x64}, {0x60, 0x68}, {0x64, 0x6c}, {0x78, 0x78}, {0x7c, 0x7c}};
            for (uint64_t k = 0x84; k < 0xbc; k += 4) moves.push_back({k, k + 4});
            for (const auto &[k3, k4] : moves) put32(e.d, g + k4, b.u32(src + k3));
            for (uint64_t k : {0x6cull, 0x70ull})
                check(!b.u32(src + k), "dropped netgame field");
            for (uint64_t k = 0xc4; k < 0xe4; k += 4) check(!b.u32(src + k), "dropped netgame field");
            const uint64_t t3o = b.ptr(src + 0x74);
            check(b.u32(t3o - 4) == NG_OBJ, "netgame object type");
            std::vector<uint64_t> subs;
            for (int k = 0; k < 3; k++) {
                const uint64_t r = b.ptr(t3o + 4ull * uint64_t(k));
                check(b.u32(r - 4) == NG_SUB, "netgame sub-object type");
                std::vector<uint32_t> w;
                for (int x = 0; x < 9; x++) w.push_back(b.u32(r + 4ull * uint64_t(x)));
                subs.push_back(obj4(NG_SUB, w));
            }
            const uint64_t t4o = obj(NG_OBJ, 24);
            for (int k = 0; k < 3; k++) e.putptr(t4o + 8ull * uint64_t(k), subs[size_t(k)]);
            e.putptr(g + 0x70, t4o);
            lognet.items.push_back(json::Value::text(u3::fmt("%08x", s)));
        }
        e.put32(t4.o, uint32_t(t4.n + ALL.size()));
        e.putptr(t4.o + 8, na);
    }
    // the Lab playlist, laid out like the first PS4 playlist
    {
        const Table t4 = table(an, PL), t3 = table(b, PL);
        std::optional<uint64_t> src;
        for (uint32_t i = 0; i < t3.n && !src; i++)
            if (b.u32(t3.ar + uint64_t(i) * 0x5c) == 0x5242014b) src = t3.ar + uint64_t(i) * 0x5c;
        if (!src) throw Stop("IndexError: no PS3 Lab playlist");
        const uint64_t na = e.alloc(0x68 * (t4.n + 1), 16);
        e.copy_words(t4.ar, na, 0x68ull * t4.n);
        const uint64_t m = na + 0x68ull * t4.n;
        e.copy_words(t4.ar, m, 0x68);
        for (uint64_t k : {0, 4, 8, 0xc}) put32(e.d, m + k, b.u32(*src + k));
        e.putptr(m + 0x10, string(need_str(b, b.ptr(*src + 0x10))));
        for (const auto &[k3, k4] : std::vector<std::pair<uint64_t, uint64_t>>{{0x14, 0x18}, {0x18, 0x1c}, {0x1c, 0x20}, {0x20, 0x24}, {0x24, 0x28}, {0x28, 0x2c}})
            put32(e.d, m + k4, b.u32(*src + k3));
        std::vector<std::pair<uint32_t, uint32_t>> pl = {{0x0d18cb6f, 5}, {0x2653000e, 5}};
        for (uint32_t s : CL) pl.push_back({s, 10});
        const uint64_t it = e.alloc(8 * pl.size(), 8);
        for (size_t k = 0; k < pl.size(); k++) put32(e.d, it + 8 * k, pl[k].first), put32(e.d, it + 8 * k + 4, pl[k].second);
        const uint64_t ll = obj(LIST_T, 16);
        put32(e.d, ll, uint32_t(pl.size()));
        e.putptr(ll + 8, it);
        e.putptr(m + 0x30, ll);
        e.put32(t4.o, t4.n + 1);
        e.putptr(t4.o + 8, na);
    }
    const Bytes net = e.build();
    write_file_replace(upath(D + "net.bin"), net);
    // the loadout
    DcEdit e2(upath(BK + "net-always-loaded.bin"));
    std::vector<std::tuple<uint32_t, uint32_t, uint64_t>> nsyms;
    {
        const uint32_t s = 0x3719ce57, T = 0x1755b0c7;
        const uint64_t o = sym_off(b, s);
        uint32_t w[6];
        for (int k = 0; k < 6; k++) w[k] = b.u32(o + 4ull * uint64_t(k));
        e2.align(8);
        const uint32_t hdr[2] = {0, T};
        for (uint32_t x : hdr) e2.d.resize(e2.d.size() + 4), wr32le(&e2.d[e2.d.size() - 4], x);
        const uint64_t v = e2.d.size();
        const uint32_t body[8] = {w[0], w[1], 0, w[2], w[3], 0, w[4], w[5]};
        for (uint32_t x : body) e2.d.resize(e2.d.size() + 4), wr32le(&e2.d[e2.d.size() - 4], x);
        nsyms.push_back({s, T, v});
    }
    const Bytes nal = add_symbols(e2.build(), nsyms);
    write_file_replace(upath(D + "net-always-loaded.bin"), nal);
    // module sizes
    Bytes m = read_file(upath(BK + "modules.bin"));
    for (const auto &[off, old, nv] : std::vector<std::tuple<uint64_t, uint64_t, uint64_t>>{{192352, 699697, net.size()}, {192312, 34487, nal.size()}}) {
        check(rd64le(m, off) == old, "modules.bin size word");
        wr64le(&m[off], nv);
    }
    write_file_replace(upath(D + "modules.bin"), m);
    // the level list
    Psarc P(upath(a.need("--ps3-update") + "/dlc-patch18.psarc"));
    const auto ni = std::find(P.names.begin(), P.names.end(), "build/main/pak23.txt");
    if (ni == P.names.end()) throw Stop("ValueError: build/main/pak23.txt not in dlc-patch18.psarc");
    const Bytes t3b = P.read(size_t(ni - P.names.begin()) + 1);
    std::string t3;
    for (uint8_t c : t3b)
        if (c != '\r') t3 += char(c);
    while (!t3.empty() && t3.back() == '\n') t3.pop_back();
    std::map<std::string, std::vector<std::string>> B;
    {
        std::vector<std::string> blocks;   // split before every '\nlevel-name '
        size_t s0 = 0;
        for (size_t k = t3.find("\nlevel-name "); k != std::string::npos; k = t3.find("\nlevel-name ", k + 1)) blocks.push_back(t3.substr(s0, k - s0)), s0 = k + 1;
        blocks.push_back(t3.substr(s0));
        const std::regex lid("(^|\\n)level-id (\\S+)");
        for (std::string x : blocks) {
            std::smatch mm;
            if (!std::regex_search(x, mm, lid)) throw Stop("AttributeError: block without level-id");
            while (!x.empty() && x.back() == '\n') x.pop_back();
            std::vector<std::string> lines;
            size_t q = 0;
            for (size_t k = x.find('\n'); k != std::string::npos; k = x.find('\n', q)) lines.push_back(x.substr(q, k - q)), q = k + 1;
            lines.push_back(x.substr(q));
            B[mm[2]] = lines;
        }
    }
    const Bytes rawb = read_file(upath(BK + "pak23_ps4.txt"));
    const std::string raw(rawb.begin(), rawb.end());
    size_t ncrlf = 0, nlf = 0;
    for (size_t k = 0; k < raw.size(); k++) nlf += raw[k] == '\n', ncrlf += raw[k] == '\n' && k && raw[k - 1] == '\r';
    check(ncrlf == nlf, "pak23_ps4.txt line ends");
    const std::vector<std::string> TS = {"texturedict1/chunk-41-tstrm", "texturedict1/chunk-41-tstrm-1", "texturedict1/chunk-42-tstrm", "texturedict1/chunk-43-tstrm",
                                         "texturedict1/common-tstrm",   "texturedict1/common-tstrm-1",   "texturedict1/common-tstrm-2",  "texturedict1/common-tstrm-3",
                                         "texturedict1/common-tstrm-4"};
    std::vector<std::string> add, nodef;
    json::Value drops = json::Value::object();
    for (const std::string &lv : levels) {   // sorted
        auto bi = B.find(lv);
        if (bi == B.end()) {
            nodef.push_back(lv);
            continue;
        }
        std::vector<std::string> blk;
        for (const std::string &x : bi->second) {
            const size_t sp = x.find(' ');
            const std::string ty = x.substr(0, sp), nm = sp == std::string::npos ? "" : x.substr(sp + 1);
            if (ty == "symbol") {
                size_t l0 = 0;
                while (l0 < nm.size() && py_space((unsigned char)nm[l0])) l0++;
                const std::string name = py_rstrip(nm.substr(l0));   // str.strip()
                uint8_t k[4];
                wr32le(k, sid(name));
                if (std::search(m.begin(), m.end(), k, k + 4) == m.end()) {
                    json::Value *dl = nullptr;
                    for (auto &[key, v] : drops.fields)
                        if (key == lv) dl = &v;
                    if (!dl) drops.fields.push_back({lv, arr()}), dl = &drops.fields.back().second;
                    dl->items.push_back(json::Value::text(name));
                    continue;
                }
            }
            blk.push_back(py_rstrip(x));
        }
        for (const auto &s : TS) blk.push_back("package-ts " + s);
        std::string j;
        for (size_t k = 0; k < blk.size(); k++) j += (k ? "\r\n" : "") + blk[k];
        add.push_back(j);
    }
    for (const auto &lv : levels) check(raw.find("level-name " + lv + "\r\n") == std::string::npos, "level already listed");
    std::string out = raw + (raw.size() >= 2 && raw.compare(raw.size() - 2, 2, "\r\n") == 0 ? "" : "\r\n");
    for (size_t k = 0; k < add.size(); k++) out += (k ? "\r\n" : "") + add[k];
    out += "\r\n";
    write_file_replace(upath(MAIN + "pak23_ps4.txt"), Bytes(out.begin(), out.end()));
    json::Value log = json::Value::object();
    log.fields.push_back({"maps", logmaps});
    log.fields.push_back({"netgames", lognet});
    log.fields.push_back({"levels", strs(std::vector<std::string>(levels.begin(), levels.end()))});
    log.fields.push_back({"dropped", drops});
    log.fields.push_back({"nodef", strs(nodef)});
    json::Value sizes = json::Value::object();
    sizes.fields.push_back({"net", json::Value::integer((long long)net.size())});
    sizes.fields.push_back({"nal", json::Value::integer((long long)nal.size())});
    log.fields.push_back({"sizes", sizes});
    std::string lj;   // json.dump to a text-mode file on Windows: CRLF line ends
    for (char c : json_dump_indent1(log)) lj += c == 10 ? std::string{char(13), char(10)} : std::string(1, c);
    write_file_replace(upath(BK + "lab_reg.log.json"), Bytes(lj.begin(), lj.end()));
    std::string nd = "[", dr = "{";
    for (size_t k = 0; k < nodef.size(); k++) nd += (k ? ", '" : "'") + nodef[k] + "'";
    nd += "]";
    for (size_t k = 0; k < drops.fields.size(); k++) {
        dr += (k ? ", '" : "'") + drops.fields[k].first + "': [";
        for (size_t q = 0; q < drops.fields[k].second.items.size(); q++) dr += (q ? ", '" : "'") + drops.fields[k].second.items[q].s + "'";
        dr += "]";
    }
    dr += "}";
    std::printf("net %zu nal %zu levels %zu added %zu nodef %s dropped %s netgames %zu\n", net.size(), nal.size(), levels.size(), add.size(), nd.c_str(), dr.c_str(), ALL.size());
    return 0;
}

}  // namespace u3

// ---- maps-batch-dc ---------------------------------------------------------------------------------------------
// Recipe step maps-batch-dc: the Lab map 34 / 34b / 37 netgames and the 24th Classic netgame (Drydock) in overlay-r3's
// net.bin, and the net.bin size word of modules.bin. Replaces apply_maps.py --dc-only (the step's only use of it; the
// pak part, maps-batch-files, is retired):
//   * maps 0x17d1af73 and 0xc77d7f07 get the Stadium level lists lab-multi-34-pak, lab34-stage, lab34-spawners,
//     sky-village-multi, multi-all + their sound / chat levels (PS3 reused campaign level names for those pieces);
//   * the three Lab modes from net18.bin (same field map as lab-registration) and a clone of Classic Graveyard's PS4
//     netgame as 0xebdaa7b7 on map 0xdd1b3f0f are appended to the netgame table (at most 256);
//   * the Lab playlist gets 0xe5592a17 / 0xcaffc253 (weight 10), the Classic playlist 0xebdaa7b7 with its PS3 weight.
// Backup once to work/backup-maps/dc1/ps4. Nothing to do when 0xebdaa7b7 is in net.bin or the netgame table is in
// PS3 1.19 order (netgame-order-119).
//   u3fix maps-batch-dc --project P --ps3-data2 D
namespace u3 {

int maps_batch_dc(const std::vector<std::string> &argv) {
    const Args a(argv, {}, {"--project", "--ps3-data2"});
    const std::string WORK = a.need("--project") + "/work/", ROOT = WORK + "overlay-r3/u3data/build/main/", BK = WORK + "backup-maps/";
    const std::string NET18 = a.need("--ps3-data2") + "/net18.bin", cur = ROOT + "dc1/ps4/net.bin";
    const uint32_t CLASSIC_NEW = 0xebdaa7b7, CLASSIC_MAP = 0xdd1b3f0f, CLASSIC_TWIN3 = 0xc4e6ad46, CLASSIC_TWIN4 = 0x43109556, PL_LAB = 0x5242014b,
                   PL_CLASSIC = 0xaac6efd8;
    const uint64_t NET_SIZE_OFF = 192352;
    {
        const Dc c(upath(cur));
        if (net_is_ps3_order(c, NET18)) {
            std::printf("already applied: the netgame table is in PS3 1.19 order (netgame-order-119)\n");
            return 0;
        }
        const Table t = table(c, NG);
        for (uint32_t i = 0; i < t.n; i++)
            if (c.u32(t.ar + uint64_t(i) * 0xd0) == CLASSIC_NEW) {
                std::printf("already applied: %08x is in net.bin\n", CLASSIC_NEW);
                return 0;
            }
    }
    const Dc b(upath(NET18)), an(upath(cur));
    DcEdit e(upath(cur));
    std::map<std::string, uint64_t> strcache;
    auto string = [&](const std::string &s) {
        auto it = strcache.find(s);
        if (it != strcache.end()) return it->second;
        const std::string enc = s + std::string(1, '\0');
        const auto &d = an.d;
        uint64_t pos = std::string::npos;
        for (auto f = std::search(d.begin(), d.end(), enc.begin(), enc.end()); f != d.end(); f = std::search(f + 1, d.end(), enc.begin(), enc.end())) {
            const uint64_t p = uint64_t(f - d.begin());
            if (p >= an.reloc) break;
            if (p == 0 || d[p - 1] == 0) {
                pos = p;
                break;
            }
        }
        if (pos == std::string::npos) pos = e.add_string(s);
        return strcache[s] = pos;
    };
    auto obj = [&](uint32_t t, size_t size) {
        e.align(8);
        uint8_t h[8] = {0, 0, 0, 0};
        wr32le(h + 4, t);
        e.d.insert(e.d.end(), h, h + 8);
        const uint64_t o = e.d.size();
        e.d.resize(e.d.size() + size, 0);
        return o;
    };
    auto obj4 = [&](uint32_t t, const std::vector<uint32_t> &w) {
        e.align(4);
        uint8_t h[4];
        wr32le(h, t);
        e.d.insert(e.d.end(), h, h + 4);
        const uint64_t o = e.d.size();
        for (uint32_t x : w) wr32le(h, x), e.d.insert(e.d.end(), h, h + 4);
        return o;
    };
    // map level lists
    const std::vector<std::string> SUB34 = {"lab-multi-34-pak", "lab34-stage", "lab34-spawners", "sky-village-multi", "multi-all"};
    std::vector<std::pair<uint32_t, std::vector<std::string>>> SUBLISTS = {{0x17d1af73, SUB34}, {0xc77d7f07, SUB34}};
    SUBLISTS[0].second.insert(SUBLISTS[0].second.end(), {"chat-obj-pak", "yemen-sound"});
    SUBLISTS[1].second.insert(SUBLISTS[1].second.end(), {"cha-sound-forest", "yemen-sound"});
    const Table tm = table(an, MAPS);
    std::map<uint32_t, uint32_t> idx;
    for (uint32_t i = 0; i < tm.n; i++) idx[an.u32(tm.ar + uint64_t(i) * 0x98)] = i;
    for (const auto &[mid, sub] : SUBLISTS) {
        const uint64_t items = e.alloc(16 * sub.size(), 8);
        for (size_t k = 0; k < sub.size(); k++) e.putptr(items + 16 * k, string(sub[k])), put32(e.d, items + 16 * k + 8, sid(sub[k]));
        const uint64_t lst = obj(LIST_T, 16);
        put32(e.d, lst, uint32_t(sub.size()));
        e.putptr(lst + 8, items);
        e.putptr(tm.ar + uint64_t(key_at(idx, mid)) * 0x98 + 0x58, lst);
    }
    // netgames
    const Table t4 = table(an, NG), t3 = table(b, NG);
    std::map<uint32_t, uint32_t> ids4, idx3;
    for (uint32_t i = 0; i < t4.n; i++) ids4[an.u32(t4.ar + uint64_t(i) * 0xd0)] = i;
    for (uint32_t i = 0; i < t3.n; i++) idx3.emplace(b.u32(t3.ar + uint64_t(i) * 0xe4), i);   // the first of an id
    const std::vector<std::pair<uint32_t, uint32_t>> LAB_MODES = {{0x7eeec759, 0x17d1af73}, {0xcaffc253, 0xc77d7f07}, {0xe5592a17, 0x195b0985}};
    const uint32_t nnew = uint32_t(LAB_MODES.size() + 1);
    check(t4.n + nnew <= 256, "netgame index limit");
    const uint64_t na = e.alloc(0xd0 * (t4.n + nnew), 16);
    e.copy_words(t4.ar, na, 0xd0ull * t4.n);
    for (size_t j = 0; j < LAB_MODES.size(); j++) {
        const auto [s, mp] = LAB_MODES[j];
        const uint64_t src = t3.ar + uint64_t(key_at(idx3, s)) * 0xe4, g = na + 0xd0 * (t4.n + j);
        check(b.u32(src + 4) == mp && idx.count(mp), "Lab mode map");
        for (uint64_t k3 = 0; k3 < 0xe4; k3 += 4)
            if (b.isptr(src + k3) && k3 != 0x74) throw Stop(u3::fmt("ptr %llx", (unsigned long long)k3));
        for (uint64_t k = 0; k < 0x48; k += 4) put32(e.d, g + k, b.u32(src + k));
        std::vector<std::pair<uint64_t, uint64_t>> moves = {{0x50, 0x58}, {0x54, 0x5c}, {0x58, 0x60}, {0x5c, 0x64}, {0x60, 0x68}, {0x64, 0x6c}, {0x78, 0x78}, {0x7c, 0x7c}};
        for (uint64_t k = 0x84; k < 0xbc; k += 4) moves.push_back({k, k + 4});
        for (const auto &[k3, k4] : moves) put32(e.d, g + k4, b.u32(src + k3));
        for (uint64_t k : {0x6cull, 0x70ull}) check(!b.u32(src + k), "dropped netgame field");
        for (uint64_t k = 0xc4; k < 0xe4; k += 4) check(!b.u32(src + k), "dropped netgame field");
        const uint64_t t3o = b.ptr(src + 0x74);
        check(b.u32(t3o - 4) == NG_OBJ, "netgame object type");
        std::vector<uint64_t> subs;
        for (int k = 0; k < 3; k++) {
            const uint64_t r = b.ptr(t3o + 4ull * uint64_t(k));
            check(b.u32(r - 4) == NG_SUB, "netgame sub-object type");
            std::vector<uint32_t> w;
            for (int x = 0; x < 9; x++) w.push_back(b.u32(r + 4ull * uint64_t(x)));
            subs.push_back(obj4(NG_SUB, w));
        }
        const uint64_t t4o = obj(NG_OBJ, 24);
        for (int k = 0; k < 3; k++) e.putptr(t4o + 8ull * uint64_t(k), subs[size_t(k)]);
        e.putptr(g + 0x70, t4o);
    }
    // the 24th Classic: Classic Graveyard's PS4 netgame with the new id and map (on PS3 the two differ only there)
    const uint64_t twin3 = t3.ar + uint64_t(key_at(idx3, CLASSIC_TWIN3)) * 0xe4, cl3 = t3.ar + uint64_t(key_at(idx3, CLASSIC_NEW)) * 0xe4;
    for (uint64_t k = 8; k < 0xe4; k += 4)
        if (k != 0x74) check(b.u32(twin3 + k) == b.u32(cl3 + k), "classic differs from twin");
    for (int k = 0; k < 3; k++) {
        const uint64_t p1 = b.ptr(b.ptr(twin3 + 0x74) + 4ull * uint64_t(k)), p2 = b.ptr(b.ptr(cl3 + 0x74) + 4ull * uint64_t(k));
        for (int w = 0; w < 9; w++) check(b.u32(p1 + 4ull * uint64_t(w)) == b.u32(p2 + 4ull * uint64_t(w)), "classic sub table differs");
    }
    const uint64_t twin4 = t4.ar + uint64_t(key_at(ids4, CLASSIC_TWIN4)) * 0xd0, g = na + 0xd0 * (t4.n + LAB_MODES.size());
    check(an.u32(twin4 + 0xc) == 0x13 && an.u32(twin4 + 0x2c) == 0xd932420c && an.u32(twin4 + 0x64) == 0x1ad223b4, "Classic Graveyard fields");
    e.copy_words(twin4, g, 0xd0);
    put32(e.d, g, CLASSIC_NEW), put32(e.d, g + 4, CLASSIC_MAP);
    check(idx.count(CLASSIC_MAP) != 0, "Classic map");
    e.put32(t4.o, t4.n + nnew);
    e.putptr(t4.o + 8, na);
    // playlists
    const Table p4 = table(an, PL), p3 = table(b, PL);
    std::optional<uint32_t> weight;
    for (uint32_t i = 0; i < p3.n; i++) {
        const uint64_t m3 = p3.ar + uint64_t(i) * 0x5c;
        if (b.u32(m3) != PL_CLASSIC) continue;
        const uint64_t l3 = b.ptr(m3 + 0x2c), it3 = b.ptr(l3 + 4);
        std::optional<uint32_t> wv;
        for (uint32_t k = 0; k < b.u32(l3); k++)
            if (b.u32(it3 + 8ull * k) == CLASSIC_NEW && !wv) wv = b.u32(it3 + 8ull * k + 4);
        if (!wv) throw Stop("IndexError: Classic playlist without 0xebdaa7b7");
        weight = wv;
    }
    check(weight.has_value(), "Classic weight");
    auto extend = [&](uint32_t pid, const std::vector<std::pair<uint32_t, uint32_t>> &add) -> size_t {
        for (uint32_t i = 0; i < p4.n; i++) {
            const uint64_t m = p4.ar + uint64_t(i) * 0x68;
            if (an.u32(m) != pid) continue;
            const uint64_t l = an.ptr(m + 0x30), it = an.ptr(l + 8);
            std::vector<std::pair<uint32_t, uint32_t>> pl;
            for (uint32_t k = 0; k < an.u32(l); k++) pl.push_back({an.u32(it + 8ull * k), an.u32(it + 8ull * k + 4)});
            for (const auto &x : add)
                for (const auto &y : pl) check(x.first != y.first, "netgame already in the playlist");
            pl.insert(pl.end(), add.begin(), add.end());
            const uint64_t ni = e.alloc(8 * pl.size(), 8);
            for (size_t k = 0; k < pl.size(); k++) put32(e.d, ni + 8 * k, pl[k].first), put32(e.d, ni + 8 * k + 4, pl[k].second);
            const uint64_t ll = obj(LIST_T, 16);
            put32(e.d, ll, uint32_t(pl.size()));
            e.putptr(ll + 8, ni);
            e.putptr(m + 0x30, ll);
            return pl.size();
        }
        throw Stop(u3::fmt("no playlist %08x", pid));
    };
    const size_t nl = extend(PL_LAB, {{0xe5592a17, 10}, {0xcaffc253, 10}}), nc = extend(PL_CLASSIC, {{CLASSIC_NEW, *weight}});
    std::printf("playlists: Lab %zu Classic %zu weight %u netgames %u\n", nl, nc, *weight, t4.n + nnew);
    const Bytes net = e.build();
    Bytes mod = read_file(upath(ROOT + "dc1/ps4/modules.bin"));
    const uint64_t old = rd64le(mod, NET_SIZE_OFF);
    if (old != fs::file_size(upath(cur))) throw Stop("modules.bin net size slot does not match net.bin");
    fs::create_directories(upath(BK + "dc1/ps4/"));
    for (const char *x : {"net.bin", "modules.bin"})
        if (!fs::exists(upath(BK + "dc1/ps4/" + x))) copy2(upath(ROOT + "dc1/ps4/" + x), upath(BK + "dc1/ps4/" + x));
    write_file_replace(upath(cur), net);
    wr64le(&mod[NET_SIZE_OFF], net.size());
    write_file_replace(upath(ROOT + "dc1/ps4/modules.bin"), mod);
    std::printf("applied (dc only): net.bin %llu -> %zu\n", (unsigned long long)old, net.size());
    return 0;
}

}  // namespace u3
