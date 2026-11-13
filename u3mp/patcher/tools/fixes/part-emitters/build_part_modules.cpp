// Recipe step convert-dc2-part-modules: the 3 overlay-r3 PS4 particle modules dc2/part-module/part-cave-multi.bin,
// part-gompa-multi.bin and part-shambhala-multi.bin rebuilt from the player's PS3 Uncharted 3 (1.19 update + disc) and
// PS4 Collection files. Replaces build_part_modules.py with the batch45 / batch48 tools it ran (graft44, epairs,
// emitconv, rootconv, ptconv, bcconv, donormap, build48, dc2lib, dc).
//   u3fix part-modules --project P --job J --ps4 D --ps3-disc D --ps3-update D [--apply] [--dest DIR]
//  1. batch45: the 31 Sanctuary water effects of 1.19 (batch45-graft.tsv) are copied with their object closures from
//     retail PS4 MP modules into the retail part-gompa-multi.bin (pointers remapped, the symbol table rebuilt sorted).
//  2. The PS3 s11 -> PS4 s8 struct samples the emitter converter needs are learned from the 33 PS3 / PS4 MP part
//     module pairs (mp-modules.tsv); per struct type a word map (PS3 word i -> PS4 word j where the byte-swapped value
//     matches in >= 99 % of the non-zero instances) and the closest sample as the donor for the other words.
//  3. batch48: the 22 PS3 1.19 emitters of 7 effect groups are converted to the PS4 emitter layout (root 0x16c ->
//     0x1e0 by root4rules.tsv, 14 PS3 sub-objects -> 11 PS4 ones; materials by mattab.tsv) and appended; the groups get
//     new roots with their 1.19 emitter lists.
// Every output is md5-checked; on a mismatch nothing is written (exit 1). Dry run by default; files already at the
// reference md5 are left alone; a different existing file is backed up once to work/backup-part-emitters.
#include "proc.hpp"
#include "psarc.hpp"
#include "u3fix.hpp"

#include <chrono>

#include <set>

namespace u3 {
namespace {

using B = Bytes;
B sl(const B &d, int64_t a, int64_t b) {   // d[a:b] (a >= 0)
    a = std::clamp<int64_t>(a, 0, int64_t(d.size()));
    b = std::clamp<int64_t>(b, a, int64_t(d.size()));
    return B(d.begin() + a, d.begin() + b);
}
void app(B &o, const B &b) { o.insert(o.end(), b.begin(), b.end()); }
void putle(B &o, int64_t at, uint64_t v, int n) {
    if (at < 0 || at + n > int64_t(o.size())) throw Stop("struct.error: pack_into out of range");
    for (int k = 0; k < n; k++) o[size_t(at + k)] = uint8_t(v >> (8 * k));
}
B le(uint64_t v, int n) {
    B o;
    for (int k = 0; k < n; k++) o.push_back(uint8_t(v >> (8 * k)));
    return o;
}
B rev(B b) {
    std::reverse(b.begin(), b.end());
    return b;
}
B sw(const B &b) {   // every 4 bytes reversed (a partial last word too)
    B o;
    for (size_t i = 0; i < b.size(); i += 4) app(o, rev(sl(b, int64_t(i), int64_t(i) + 4)));
    return o;
}
B h16(const B &b) {   // every 2 bytes swapped
    B o;
    for (size_t i = 0; i < b.size(); i += 2) app(o, rev(sl(b, int64_t(i), int64_t(i) + 2)));
    return o;
}
uint32_t sid_of(const std::string &s) { return sid(s); }
int64_t pymod16(int64_t x) { return ((x % 16) + 16) % 16; }
int64_t floordiv4(int64_t x) { return x >= 0 ? x / 4 : -((-x + 3) / 4); }

// ---------------------------------------------------------------------------------------------- DC modules (dc.py)
struct DC {
    B d;
    bool be = false;
    int ps = 8;
    uint64_t reloc = 0;
    struct Sym {
        uint32_t s, t;
        uint64_t o;
    };
    std::vector<Sym> syms;
    std::map<uint32_t, std::pair<uint32_t, uint64_t>> bysid;
    std::set<int64_t> ptrs;
    int64_t data0 = 0;
    uint32_t u32(int64_t o) const {
        if (o < 0 || o + 4 > int64_t(d.size())) throw Stop("struct.error: unpack_from requires a buffer of at least 4 bytes");
        return be ? be32(d, size_t(o)) : rd32le(d, size_t(o));
    }
    uint64_t ptr(int64_t o) const {
        if (ps == 4) return u32(o);
        if (o < 0 || o + 8 > int64_t(d.size())) throw Stop("struct.error: unpack_from requires a buffer of at least 8 bytes");
        return rd64le(d, size_t(o));
    }
    explicit DC(const B &data) : d(data) {
        if (d.size() >= 4 && std::equal(d.begin(), d.begin() + 4, "DC00")) be = true, ps = 4;
        else if (d.size() >= 4 && std::equal(d.begin(), d.begin() + 4, "00CD")) be = false, ps = 8;
        else throw Stop("ValueError: not DC");
        reloc = ps == 4 ? u32(8) : ptr(8);
        const uint32_t n = u32(0x14);
        int64_t o = 0x18;
        for (uint32_t i = 0; i < n; i++) {
            if (ps == 4) syms.push_back({u32(o), u32(o + 4), u32(o + 8)}), o += 12;
            else syms.push_back({u32(o), u32(o + 4), ptr(o + 8)}), o += 16;
        }
        for (const Sym &s : syms) bysid[s.s] = {s.t, s.o};
        if (reloc && reloc < d.size()) {
            const uint32_t cnt = u32(int64_t(reloc));
            const B bm = sl(d, int64_t(reloc) + 4, int64_t(reloc) + 4 + cnt);
            for (size_t bi = 0; bi < bm.size(); bi++)
                for (int k = 0; k < 8; k++)
                    if (bm[bi] & (1 << k)) ptrs.insert(int64_t(bi * 8 + size_t(be ? 7 - k : k)) * ps);
        }
    }
    bool isptr(int64_t o) const { return ptrs.count(o) != 0; }
};

// dc2lib.load: a PS4 dc2 particle module (symbol table at 0x20)
DC load_dc2(const B &b) {
    DC c(b);
    if (!(b.size() >= 4 && std::equal(b.begin(), b.begin() + 4, "00CD") && c.ptr(0x18) == 0x20 && c.isptr(0x18))) throw Stop("AssertionError: not dc2");
    const uint32_t n = c.u32(0x14);
    c.syms.clear();
    for (uint32_t i = 0; i < n; i++) c.syms.push_back({c.u32(0x20 + 16ll * i), c.u32(0x24 + 16ll * i), c.ptr(0x28 + 16ll * i)});
    for (uint32_t i = 0; i < n; i++)
        if (!c.isptr(0x28 + 16ll * i)) throw Stop("AssertionError: symbol slot not relocated");
    c.bysid.clear();
    for (const auto &s : c.syms) c.bysid[s.s] = {s.t, s.o};
    c.data0 = 0x20 + 16ll * n;
    return c;
}

// dc2lib.build: d = an existing dc2 image up to its reloc (table for old_n symbols at 0x20) plus appended data
B build_dc2(const B &d, const std::set<int64_t> &ptrs, std::vector<DC::Sym> syms, size_t old_n) {
    const int64_t old0 = 0x20 + 16 * int64_t(old_n);
    std::sort(syms.begin(), syms.end(), [](const DC::Sym &a, const DC::Sym &b) { return std::tie(a.s, a.t, a.o) < std::tie(b.s, b.t, b.o); });
    const int64_t new0 = 0x20 + 16 * int64_t(syms.size()), delta = new0 - old0;
    B out = sl(d, 0, 0x20);
    out.resize(size_t(new0), 0);
    app(out, sl(d, old0, int64_t(d.size())));
    putle(out, 0x14, syms.size(), 4);
    putle(out, 0x18, 0x20, 8);
    std::set<int64_t> P{0x18};
    for (int64_t p : ptrs) {
        if (p < old0) continue;
        const int64_t q = p + delta;
        if (q + 8 > int64_t(out.size())) throw Stop("struct.error: unpack_from out of range");
        const uint64_t v = rd64le(out, size_t(q));
        if (int64_t(v) < old0) throw Stop("AssertionError: pointer into header/table");
        putle(out, q, v + uint64_t(delta), 8);
        P.insert(q);
    }
    for (size_t i = 0; i < syms.size(); i++) {
        if (int64_t(syms[i].o) < old0) throw Stop("AssertionError: symbol offset");
        putle(out, 0x20 + 16 * int64_t(i), syms[i].s, 4);
        putle(out, 0x24 + 16 * int64_t(i), syms[i].t, 4);
        putle(out, 0x28 + 16 * int64_t(i), syms[i].o + uint64_t(delta), 8);
        P.insert(0x28 + 16 * int64_t(i));
    }
    while (out.size() % 4) out.push_back(0);
    const int64_t reloc = int64_t(out.size()), nb = (reloc + 63) / 64;
    B bm(size_t(nb), 0);
    for (int64_t p : P) {
        if (p % 8 || p >= reloc) throw Stop("AssertionError: relocated slot");
        const int64_t w = p / 8;
        bm[size_t(w / 8)] |= uint8_t(1 << (w % 8));
    }
    putle(out, 8, uint64_t(reloc), 8);
    app(out, le(uint64_t(nb), 4));
    app(out, bm);
    load_dc2(out);
    return out;
}

// graft44.Mod / epairs.M3: objects are the spans between pointer targets
struct Spans {
    const DC &c;
    std::vector<int64_t> T;
    Spans(const DC &c_, bool dc2) : c(c_) {
        std::set<int64_t> t;
        for (int64_t p : c.ptrs) {
            const int64_t x = int64_t(c.ptr(p));
            if ((dc2 ? c.data0 <= x : true) && x < int64_t(c.reloc)) t.insert(x);
        }
        for (const auto &s : c.syms) t.insert(int64_t(s.o));
        T.assign(t.begin(), t.end());
    }
    int64_t end(int64_t t) const {
        const auto k = std::upper_bound(T.begin(), T.end(), t);
        return k != T.end() ? *k : int64_t(c.reloc);
    }
    int64_t start(int64_t t) const {
        int64_t k = int64_t(std::upper_bound(T.begin(), T.end(), t) - T.begin()) - 1;
        if (k < 0) k += int64_t(T.size());   // Python's T[-1]
        if (k < 0) throw Stop("IndexError: list index out of range");
        return T[size_t(k)];
    }
    int64_t hdr(int64_t t) const {
        return (t % 8 == 0 && t >= 8 && !c.isptr(t - 8) && !c.isptr(t - 4) && c.u32(t - 4) && !std::any_of(c.d.begin() + (t - 8), c.d.begin() + (t - 4), [](uint8_t x) { return x; }))
                   ? 8
                   : 0;
    }
    std::vector<int64_t> closure(const std::vector<int64_t> &roots) const {
        std::set<int64_t> seen;
        std::vector<int64_t> st;
        for (int64_t r : roots) st.push_back(start(r));
        while (!st.empty()) {
            const int64_t t = st.back();
            st.pop_back();
            if (seen.count(t) || t >= int64_t(c.reloc)) continue;
            seen.insert(t);
            const int64_t e = end(t);
            for (auto it = c.ptrs.lower_bound(t); it != c.ptrs.end() && *it < e; ++it) {
                const int64_t x = int64_t(c.ptr(*it));
                if (x < int64_t(c.reloc)) st.push_back(start(x));
            }
        }
        return std::vector<int64_t>(seen.begin(), seen.end());
    }
};

// ---------------------------------------------------------------------------------------------- batch45 (graft44)
B graft(const B &dst, const std::vector<std::pair<uint32_t, std::string>> &items, const std::map<std::string, B> &sources, std::vector<std::string> &log) {
    const DC a = load_dc2(dst);
    B d = sl(a.d, 0, int64_t(a.reloc));
    std::set<int64_t> ptrs;
    for (int64_t p : a.ptrs)
        if (p >= a.data0) ptrs.insert(p);
    std::vector<DC::Sym> syms = a.syms;
    std::set<uint32_t> have;
    for (const auto &s : syms) have.insert(s.s);
    std::vector<std::pair<std::string, std::vector<uint32_t>>> bysrc;
    for (const auto &[s, p] : items) {
        auto it = std::find_if(bysrc.begin(), bysrc.end(), [&](const auto &x) { return x.first == p; });
        if (it == bysrc.end()) bysrc.push_back({p, {s}});
        else it->second.push_back(s);
    }
    for (auto &[p, sids0] : bysrc) {
        const DC c = load_dc2(sources.at(p));
        const Spans m(c, true);
        std::vector<uint32_t> sids;
        for (uint32_t s : sids0)
            if (!have.count(s)) sids.push_back(s);
        std::vector<int64_t> roots;
        for (uint32_t s : sids) roots.push_back(int64_t(c.bysid.at(s).second));
        const std::vector<int64_t> spans = m.closure(roots);
        std::map<int64_t, int64_t> newaddr;
        for (int64_t t : spans) {
            const int64_t h = m.hdr(t), lo = t - h, e = m.end(t);
            while (int64_t(d.size()) % 16 != pymod16(lo)) d.push_back(0);
            const int64_t base = int64_t(d.size());
            app(d, sl(c.d, lo, e));
            newaddr[t] = base + h;
        }
        auto remap = [&](int64_t x) {
            auto k = newaddr.upper_bound(x);
            if (k == newaddr.begin()) throw Stop("AssertionError: remap");
            --k;
            if (!(x < m.end(k->first))) throw Stop("AssertionError: remap");
            return k->second + (x - k->first);
        };
        for (int64_t t : spans) {
            const int64_t h = m.hdr(t), e = m.end(t), nb = newaddr[t];
            for (auto it = c.ptrs.lower_bound(t - h); it != c.ptrs.end() && *it < e; ++it) {
                const int64_t q = *it, x = int64_t(c.ptr(q));
                if (x >= int64_t(c.reloc)) throw Stop("AssertionError: ptr past reloc");
                putle(d, nb + (q - t), uint64_t(remap(x)), 8);
                ptrs.insert(nb + (q - t));
            }
        }
        for (uint32_t s : sids) {
            const auto [ty, o] = c.bysid.at(s);
            syms.push_back({s, ty, uint64_t(remap(int64_t(o)))});
            have.insert(s);
        }
        log.push_back(u3::fmt("graft44 from %s: %zu symbols, %zu objects", p.c_str(), sids.size(), spans.size()));
    }
    return build_dc2(d, ptrs, syms, a.syms.size());
}

// ---------------------------------------------------------------------------------------------- PS3 emitter inputs (epairs)
const uint32_t E_TYPE = 0x4fec6221;
const uint32_t NT[] = {0x4fec6221, 0x77589471, 0x509ad90c, 0x58a8d2b9, 0x110f9a0f, 0xab7f2b3e, 0xf6b95830, 0xb3c01ba2, 0x91f789d};
std::optional<int64_t> sub3(const DC &a, int64_t o, int k) {
    const int64_t q = o + 0x134 + 4 * k;
    if (!a.isptr(q)) return std::nullopt;
    return int64_t(a.ptr(q));
}
std::optional<int64_t> sub4(const DC &b, int64_t o4, int j) {
    const int64_t q = o4 + 0xa8 + 8 * j;
    if (!b.isptr(q)) return std::nullopt;
    return int64_t(b.ptr(q));
}
int64_t need(const std::optional<int64_t> &x) {
    if (!x) throw Stop("TypeError: unsupported operand type(s) for +: 'NoneType' and 'int'");
    return *x;
}
std::vector<std::optional<B>> spans3(const Spans &a, int64_t o) {
    std::optional<int64_t> addrs[14];
    for (int k = 0; k < 14; k++) addrs[k] = sub3(a.c, o, k);
    std::vector<std::optional<B>> out;
    for (int k = 0; k < 14; k++) {
        if (!addrs[k]) {
            out.push_back(std::nullopt);
            continue;
        }
        const int64_t x = *addrs[k];
        std::set<int64_t> nx;
        for (const auto &y : addrs)
            if (y && *y > x) nx.insert(*y);
        int64_t e = nx.empty() ? a.end(x) : *nx.begin();
        if (!nx.empty())
            for (int kk : {4, 11, 12})
                if (addrs[kk] && *addrs[kk] == *nx.begin()) { e -= 4; break; }
        if (nx.empty()) {
            e = a.end(x);
            while (e - 4 >= x && !a.c.isptr(e - 4)) {
                const uint32_t w = a.c.u32(e - 4);
                if (w != 0x4fec6221 && w != 0x77589471 && w != 0) break;
                e -= 4;
            }
        }
        out.push_back(sl(a.c.d, x, e));
    }
    return out;
}
struct In3 {
    B r3;
    std::vector<std::optional<B>> s3;
    uint32_t t11, t12;
    std::vector<std::pair<uint32_t, B>> nested;
};
In3 ps3_inputs(const Spans &a, int64_t o) {
    In3 r;
    r.s3 = spans3(a, o);
    r.r3 = sl(a.c.d, o, o + 0x16c);
    if (r.r3.size() <= 0x130) throw Stop("IndexError: index out of range");
    const auto x11 = sub3(a.c, o, 11), x12 = sub3(a.c, o, 12), x13 = sub3(a.c, o, 13);
    for (int i = 0; i < r.r3[0x130]; i++) {
        const int64_t t = int64_t(a.c.ptr(need(x13) + 4 * i));
        const uint32_t ty = a.c.u32(t - 4);
        int64_t e = a.end(t);
        if (e < int64_t(a.c.reloc) && std::find(std::begin(NT), std::end(NT), a.c.u32(e - 4)) != std::end(NT)) e -= 4;
        const B w = sl(a.c.d, t, e);
        B o4;
        for (size_t q = 0; q < w.size(); q += 4) app(o4, q == 4 ? sl(w, int64_t(q), int64_t(q) + 4) : rev(sl(w, int64_t(q), int64_t(q) + 4)));
        r.nested.push_back({ty, o4});
    }
    r.t11 = a.c.u32(need(x11) - 4);
    r.t12 = a.c.u32(need(x12) - 4);
    return r;
}

// ---------------------------------------------------------------------------------------------- s11 struct map (donormap)
using Words = std::vector<B>;
struct DonorMap {
    std::vector<std::pair<Words, Words>> samples;
    std::vector<std::pair<size_t, size_t>> map;   // PS3 word -> PS4 word, in the order the words were first non-zero
    explicit DonorMap(const std::vector<std::pair<Words, Words>> &s, double minc = 0.99) : samples(s) {
        std::vector<size_t> nz_order;
        std::map<size_t, int64_t> nz;
        std::map<size_t, std::vector<std::pair<size_t, int64_t>>> votes;   // insertion-ordered counters
        for (const auto &[w3, w4] : samples)
            for (size_t i = 0; i < w3.size(); i++) {
                if (w3[i] == B{0, 0, 0, 0}) continue;
                if (!nz.count(i)) nz_order.push_back(i);
                nz[i]++;
                const B rv = rev(w3[i]);
                for (size_t j = 0; j < w4.size(); j++)
                    if (w4[j] == rv) {
                        auto &v = votes[i];
                        auto it = std::find_if(v.begin(), v.end(), [&](const auto &x) { return x.first == j; });
                        if (it == v.end()) v.push_back({j, 1});
                        else it->second++;
                    }
            }
        for (size_t i : nz_order) {
            auto v = votes.find(i);
            if (v == votes.end() || v->second.empty()) continue;
            std::pair<size_t, int64_t> best = v->second[0];
            for (const auto &x : v->second)
                if (x.second > best.second) best = x;
            if (double(best.second) >= minc * double(nz[i])) map.push_back({i, best.first});
        }
    }
    // the converted words and how many unmapped words differ from the closest sample
    std::optional<std::pair<Words, int64_t>> convert(const Words &w3) const {
        std::set<size_t> mapped;
        for (const auto &[i, j] : map) mapped.insert(i);
        std::optional<std::pair<int64_t, size_t>> best;
        for (size_t idx = 0; idx < samples.size(); idx++) {
            const Words &d3 = samples[idx].first;
            if (d3.size() != w3.size()) continue;
            int64_t diff = 0;
            for (size_t i = 0; i < w3.size(); i++)
                if (!mapped.count(i) && d3[i] != w3[i]) diff++;
            if (!best || diff < best->first) {
                best = std::make_pair(diff, idx);
                if (!diff) break;
            }
        }
        if (!best) return std::nullopt;
        Words out = samples[best->second].second;
        for (const auto &[i, j] : map) out.at(j) = rev(w3.at(i));
        return std::make_pair(out, best->first);
    }
};
const uint32_t S11_TYPES[] = {0x1d9d708e, 0xaee75cf5};

// ---------------------------------------------------------------------------------------------- emitter conversion
// the PS3 -> PS4 script opcode renumbering of the emitter bytecode (bcconv)
B bc_conv(const std::optional<B> &s8, int n8, const std::optional<B> &s9, int n9) {
    static const std::map<int, int> OP = {{0x00, 0x00}, {0x01, 0x02}, {0x02, 0x01}, {0x04, 0x05}, {0x05, 0x06}, {0x06, 0x07}, {0x07, 0x08}, {0x08, 0x09}, {0x09, 0x0a},
                                          {0x0a, 0x0c}, {0x0f, 0x0d}, {0x10, 0x0e}, {0x11, 0x0f}, {0x12, 0x10}, {0x13, 0x11}, {0x14, 0x12}, {0x15, 0x13}, {0x16, 0x14}};
    B out;
    for (const auto &[src, n] : {std::make_pair(&s8, n8), std::make_pair(&s9, n9)})
        for (int i = 0; i < n; i++) {
            if (!*src) throw Stop("TypeError: 'NoneType' object is not subscriptable");
            const B w = sl(**src, 4 * i, 4 * i + 4);
            if (w.empty()) throw Stop("IndexError: index out of range");
            auto it = OP.find(w[0]);
            if (it == OP.end()) throw Stop(u3::fmt("ValueError: opcode %#x", w[0]));
            out.push_back(uint8_t(it->second));
            out.insert(out.end(), w.begin() + 1, w.end());
        }
    return out;
}

// parameter tables (ptconv): entries {value, code: kind, ?, param, component}
const std::vector<int> PT_ORDER = {0x00, 0x10, 0x04, 0x12, 0x13, 0x14, 0x15, 0x16, 0x05, 0x06};
const std::map<int, int> NCOMP = {{0x0, 3}, {0x4, 4}, {0x5, 4}, {0x6, 4}, {0x10, 3}, {0x12, 3}, {0x13, 1}, {0x14, 1}, {0x15, 3}, {0x16, 3}};
using PEnt = std::pair<B, B>;   // value (BE bytes), code
std::vector<PEnt> pt_parse3(const B &w3, std::optional<int> n) {
    std::vector<PEnt> ents;
    for (int64_t i = 0; i < int64_t(w3.size()) - 7; i += 8) {
        const B c = sl(w3, i + 4, i + 8);
        if (!(c[0] == 1 || c[0] == 2 || c[0] == 3) || !NCOMP.count(c[2]) || c[3] >= NCOMP.at(c[2])) break;
        ents.push_back({sl(w3, i, i + 4), c});
        if (n && int(ents.size()) >= *n) break;
    }
    return ents;
}
std::vector<PEnt> pt_conv(const std::vector<PEnt> &ents3, bool empty_prog) {
    std::map<std::pair<int, int>, PEnt> got;
    for (const auto &[v, c] : ents3) got.emplace(std::make_pair(int(c[2]), int(c[3])), PEnt{v, c});
    std::set<int> params;
    if (empty_prog) params = {0x00, 0x12, 0x13}, got.clear();
    else {
        for (const auto &[k, e] : got) params.insert(k.first);
        params.insert({0x00, 0x10, 0x12});
    }
    std::vector<PEnt> out;
    for (int p : PT_ORDER) {
        if (!params.count(p)) continue;
        for (int comp = 0; comp < NCOMP.at(p); comp++) {
            if (p == 0) {
                out.push_back({B{0, 0, 0, 0}, B{1, 0, 0, uint8_t(comp)}});
                continue;
            }
            auto it = got.find({p, comp});
            if (it != got.end()) out.push_back(it->second);
            else out.push_back({p == 0x12 && comp == 2 ? B{0x3f, 0x80, 0, 0} : B{0, 0, 0, 0}, B{1, 0, uint8_t(p), uint8_t(comp)}});
        }
    }
    return out;
}

// the PS3 root (0x16c) -> PS4 root (0x1e0) words: root4rules.tsv + the computed fields
struct Rule {
    int64_t off;
    std::string kind;
    B cst;
    int64_t src;
};
std::vector<Rule> g_rules;
std::map<int64_t, B> root_words(const B &r3, int n4, int n5, int n6, int n8, int n9, uint32_t mat) {
    std::map<int64_t, B> out;
    for (const Rule &r : g_rules) {
        if (r.kind == "const") out[r.off] = r.cst;
        else if (r.kind == "sw") out[r.off] = rev(sl(r3, r.src, r.src + 4));
        else if (r.kind == "id") out[r.off] = sl(r3, r.src, r.src + 4);
        else if (r.kind == "h16") {
            const B w = sl(r3, r.src, r.src + 4);
            B o;
            for (int64_t k = 1; k >= 0; k--)
                if (size_t(k) < w.size()) o.push_back(w[size_t(k)]);
            for (int64_t k = 3; k >= 2; k--)
                if (size_t(k) < w.size()) o.push_back(w[size_t(k)]);
            out[r.off] = o;
        }
    }
    auto u32b = [&](int64_t o) {
        if (o + 4 > int64_t(r3.size())) throw Stop("struct.error: unpack_from requires a buffer of at least 4 bytes");
        return be32(r3, size_t(o));
    };
    const uint64_t x = u32b(0x28), bf28 = (x & 0x7fff) | ((x & ~uint64_t(0x7fff)) << 2);
    if (bf28 > 0xffffffffu) throw Stop("struct.error: 'I' format requires 0 <= number <= 4294967295");
    out[0x040] = le(bf28, 4);
    out[0x04c] = B{r3.at(0x30), 2, 0, 0};
    out[0x050] = le(mat, 4);
    const uint32_t w = u32b(0x70);
    const uint64_t hi = w >> 16, bf70 = ((hi >> 1) << 2) | (hi & 1);
    if (bf70 > 0xffff) throw Stop("struct.error: 'H' format requires 0 <= number <= 65535");
    B v100 = le(bf70, 2);
    app(v100, le(w & 0xffff, 2));
    out[0x100] = v100;
    out[0x150] = B{r3.at(0xb1), r3.at(0xb2), 0, 0};
    B a = le(uint64_t(n4), 2);
    app(a, le(uint64_t(n5), 2));
    out[0x098] = a;
    B b = le(uint64_t(n6), 2);
    b.push_back(r3.at(0x12c));
    b.push_back(r3.at(0x130));
    out[0x09c] = b;
    B c = le(0, 2);
    app(c, le(uint64_t(n8), 2));
    out[0x0a0] = c;
    B e = le(uint64_t(n8), 2);
    app(e, le(uint64_t(n9), 2));
    out[0x0a4] = e;
    return out;
}

struct Emitter {
    B root;
    std::vector<B> objs;   // 0..9
    uint32_t t8;
    std::vector<std::pair<uint32_t, B>> nested;
};
const B S9_TAIL = {0xe1, 0x31, 0xea, 0x00, 0x00, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0, 0, 0, 0};
const B &opt(const std::optional<B> &x) {
    if (!x) throw Stop("TypeError: 'NoneType' object is not subscriptable");
    return *x;
}
Emitter convert_emitter(const In3 &in, uint32_t mat4, uint32_t sid, const std::map<uint32_t, DonorMap> &s11) {
    const B &r3 = in.r3;
    if (r3.size() < 0x16c) throw Stop("IndexError: index out of range");
    auto u16 = [&](size_t o) { return int(r3[o] << 8 | r3[o + 1]); };
    const int n_s0 = u16(0x120), n_s1 = u16(0x122), n_s2 = u16(0x124), n_s3 = u16(0x126);
    const int n6 = r3[0x12b], n7 = r3[0x12c], n8 = r3[0x12d], n9 = r3[0x12e], n5 = r3[0x12f];
    if (r3[0x58] || r3[0x59] || r3[0x5a] || r3[0x5b]) throw Stop("ValueError: PS3 +0x58 set: PS4 extra-pointer variant not supported");
    Emitter em;
    em.objs.resize(10);
    if (n_s0) {
        B o = h16(opt(in.s3[0]));
        o.resize(o.size() + 16, 0);
        o.resize(16);
        em.objs[0] = o;
    }
    em.objs[1] = sw(sl(opt(in.s3[1]), 0, 96 * n_s1));
    {
        const B &s2 = opt(in.s3[2]);
        B o;
        for (int64_t i = 0; i < 4 * n_s2; i += 4) {
            const B e = sl(s2, i, i + 4);
            app(o, sl(e, 0, 2));
            app(o, rev(sl(e, 2, 4)));
        }
        em.objs[2] = o;
    }
    em.objs[3] = sw(sl(opt(in.s3[3]), 0, 16 * n_s3));
    em.objs[4] = bc_conv(in.s3[8], n8, in.s3[9], n9);
    em.objs[5] = sw(sl(opt(in.s3[10]), 0, 4 * n5));
    const bool empty = n8 == 0 && n9 == 0;
    const std::vector<PEnt> tab = pt_conv(pt_parse3(opt(in.s3[6]), n6), empty);
    for (const auto &[v, c] : tab) {
        app(em.objs[6], rev(v));
        app(em.objs[6], c);
    }
    em.objs[7] = sw(sl(opt(in.s3[7]), 0, 8 * n7));
    // s8: the typed struct
    auto dm = s11.find(in.t11);
    if (dm == s11.end()) throw Stop(u3::fmt("KeyError: %u", in.t11));
    if (dm->second.samples.empty()) throw Stop("IndexError: list index out of range");
    Words w3;
    for (size_t i = 0; i < dm->second.samples[0].first.size() * 4; i += 4) w3.push_back(sl(opt(in.s3[11]), int64_t(i), int64_t(i) + 4));
    const auto r = dm->second.convert(w3);
    if (!r) throw Stop("TypeError: can only join an iterable");
    if (r->second) throw Stop(u3::fmt("ValueError: s11 donor not exact (%lld words differ)", (long long)r->second));
    for (const B &w : r->first) app(em.objs[8], w);
    if (in.t12 != 0x869bc537) throw Stop(u3::fmt("ValueError: s12 type %08x", in.t12));
    em.objs[9] = sw(sl(opt(in.s3[12]), 0, 48));
    app(em.objs[9], S9_TAIL);
    em.t8 = in.t11;
    // the root
    std::set<int> present;
    for (const auto &[v, c] : tab) present.insert(c[2]);
    uint32_t mask = 0;
    for (const auto &[b, p] : std::vector<std::pair<int, int>>{{0, 0x15}, {1, 0x16}, {2, 0x14}, {3, 0x05}, {4, 0x06}})
        if (present.count(p)) mask += 1u << b;
    std::map<int64_t, B> words = root_words(r3, n8 + n9, n5, int(tab.size()), n8, n9, mat4);
    words[0x05c] = le(uint64_t(mask) << 16, 4);
    em.root.assign(0x1e0, 0);
    for (const auto &[j, v] : words) {   // root[j:j+4] = v
        B nr = sl(em.root, 0, j);
        app(nr, v);
        app(nr, sl(em.root, j + 4, int64_t(em.root.size())));
        em.root = nr;
    }
    putle(em.root, 0x20, sid, 4);
    em.nested = in.nested;
    return em;
}

// one emitter placed at file offset base (16-aligned, the 8-byte header at base - 8): its bytes and pointer slots
std::pair<B, std::vector<int64_t>> layout(int64_t base, const Emitter &em, int64_t name_addr, const B &r3) {
    B out = em.root;
    std::vector<int64_t> ptr;
    std::map<int, std::optional<int64_t>> addr;
    auto put_ptr = [&](int64_t slot, const std::optional<int64_t> &t) {
        if (!t) throw Stop("struct.error: required argument is not an integer");
        putle(out, slot, uint64_t(*t), 8);
        ptr.push_back(slot);
    };
    int64_t pos = 0x1e0;
    for (int j = 0; j < 8; j++) {
        addr[j] = base + pos;
        app(out, em.objs[size_t(j)]);
        pos += int64_t(em.objs[size_t(j)].size());
    }
    auto pad_to = [&](auto cond) {
        while (cond()) out.push_back(0), pos++;
    };
    auto typed = [&](int j, const B &data, uint32_t ty) {
        pad_to([&] { return (pos + 8) % 16 != 0; });
        app(out, B{0, 0, 0, 0});
        app(out, le(ty, 4));
        pos += 8;
        addr[j] = base + pos;
        app(out, data);
        pos += int64_t(data.size());
    };
    typed(8, em.objs[8], em.t8);
    typed(9, em.objs[9], 0x869bc537);
    pad_to([&] { return pos % 16 != 0; });
    addr[10] = base + pos;
    const int64_t n = int64_t(em.nested.size()), s10 = pos, ln = std::max<int64_t>(8 * n, 8);
    out.resize(out.size() + size_t(ln), 0);
    pos += ln;
    std::vector<int64_t> naddr;
    for (const auto &[ty, data] : em.nested) {
        pad_to([&] { return (pos + 8) % 16 != 0; });
        app(out, B{0, 0, 0, 0});
        app(out, le(ty, 4));
        pos += 8;
        naddr.push_back(base + pos);
        app(out, data);
        pos += int64_t(data.size());
    }
    // empty objects point at the next object that has data
    std::optional<int64_t> nxt;
    for (int j = 10; j >= 0; j--) {
        if (j < 10 && em.objs[size_t(j)].empty()) addr[j] = nxt;
        nxt = addr[j];
    }
    for (int j = 0; j < 11; j++) put_ptr(0xa8 + 8 * j, addr[j]);
    for (size_t i = 0; i < naddr.size(); i++) put_ptr(s10 + 8 * int64_t(i), naddr[i]);
    put_ptr(0x18, name_addr);
    pad_to([&] { return pos % 16 != 0; });
    // root +0 (size): a bound never below retail: max(layout end + 8, PS3 size + 0xd0 rounded to 16)
    const int64_t p3 = int64_t(be32(r3, 4));
    putle(out, 0, uint64_t(std::max<int64_t>(pos + 8, (p3 + 0xd0 + 15) & ~int64_t(15))), 4);
    return {out, ptr};
}


// ---------------------------------------------------------------------------------------------- batch48 (build48)
const std::vector<std::pair<std::string, std::vector<uint32_t>>> GROUPS = {
    {"part-cave-multi", {0xd030cb45, 0xcab6690c}}, {"part-gompa-multi", {0x69bfe094, 0xb534e0a7, 0x6d7d304e, 0x54a60c10}}, {"part-shambhala-multi", {0xde6b6dd9}}};

std::vector<std::vector<std::string>> read_tsv(const fs::path &p) {
    std::vector<std::vector<std::string>> rows;
    const std::string t = text_file(p);
    size_t b = 0;
    for (size_t k = 0; k <= t.size(); k++) {
        if (k < t.size() && t[k] != '\n') continue;
        const std::string l = t.substr(b, k - b);
        b = k + 1;
        if (py_split(l).empty() || l[0] == '#') continue;
        std::vector<std::string> c;
        size_t s = 0;
        for (size_t j = 0; j <= l.size(); j++)
            if (j == l.size() || l[j] == '\t') { c.push_back(l.substr(s, j - s)); s = j + 1; }
        rows.push_back(c);
    }
    return rows;
}

B build48(const std::string &mod, const std::vector<uint32_t> &groups, const B &base_bytes, const B &ps3_bytes, const std::map<uint32_t, std::string> &names,
          const std::vector<std::pair<std::string, std::vector<std::pair<std::string, int64_t>>>> &mat, const std::map<uint32_t, DonorMap> &s11,
          std::vector<std::string> &log) {
    const DC c = load_dc2(base_bytes);
    B d = sl(c.d, 0, int64_t(c.reloc));
    std::set<int64_t> ptrs;
    for (int64_t p : c.ptrs)
        if (p >= c.data0) ptrs.insert(p);
    std::vector<DC::Sym> syms = c.syms;
    const DC a(ps3_bytes);
    const Spans A(a, false);
    std::set<uint32_t> have;
    for (const auto &s : syms) have.insert(s.s);
    auto align = [&](int64_t n, int64_t extra) {
        while ((int64_t(d.size()) + extra) % n) d.push_back(0);
    };
    std::set<uint32_t> want;
    for (uint32_t g : groups) {
        const int64_t o3 = int64_t(a.bysid.at(g).second), x = int64_t(a.ptr(o3 + 0x18));
        const uint32_t n = a.u32(o3 + 0xc);
        for (uint32_t i = 0; i < n; i++) want.insert(a.u32(x + 4 * int64_t(i)));
    }
    for (uint32_t s : want) {   // sorted
        if (have.count(s)) continue;
        const int64_t o3 = int64_t(a.bysid.at(s).second);
        const In3 in = ps3_inputs(A, o3);
        auto nm = names.find(s);
        if (nm == names.end()) throw Stop(u3::fmt("KeyError: emitter %08x of ", s) + mod + " has no name in emitter-names.tsv");
        const std::string &name = nm->second;
        if (sid_of(name) != s) throw Stop("AssertionError: " + name);
        const std::string key = u3::fmt("%02x%02x%02x%02x", in.r3[0x34], in.r3[0x35], in.r3[0x36], in.r3[0x37]);
        auto mk = std::find_if(mat.begin(), mat.end(), [&](const auto &x) { return x.first == key; });
        if (mk == mat.end()) throw Stop("KeyError: PS3 material " + key + " of emitter " + name + " is not in mattab.tsv");
        const auto *best = &mk->second[0];
        for (const auto &kv : mk->second)
            if (kv.second > best->second) best = &kv;
        const uint32_t m4 = uint32_t(std::stoul(best->first, nullptr, 16));
        const Emitter em = convert_emitter(in, m4, s, s11);
        const int64_t name_addr = int64_t(d.size());
        d.insert(d.end(), name.begin(), name.end());
        d.push_back(0);
        align(16, 8);
        app(d, B{0, 0, 0, 0});
        app(d, le(E_TYPE, 4));
        const int64_t base = int64_t(d.size());
        const auto [blob, bp] = layout(base, em, name_addr, in.r3);
        app(d, blob);
        for (int64_t q : bp) ptrs.insert(base + q);
        syms.push_back({s, E_TYPE, uint64_t(base)});
        log.push_back(u3::fmt("%s emitter %08x %s: %zu bytes, size field %#x, mat %08x, nested %zu", mod.c_str(), s, name.c_str(), blob.size(), rd32le(blob, 0), m4, em.nested.size()));
    }
    // the groups: a copy of the retail PS4 group root (0x58) with the 1.19 count and list
    std::map<uint32_t, size_t> symd;
    for (size_t i = 0; i < syms.size(); i++) symd[syms[i].s] = i;
    for (uint32_t g : groups) {
        const auto [t, og0] = c.bysid.at(g);
        const int64_t og = int64_t(og0);
        const int64_t o3 = int64_t(a.bysid.at(g).second), x = int64_t(a.ptr(o3 + 0x18));
        const uint32_t n = a.u32(o3 + 0xc);
        std::vector<uint32_t> lst;
        for (uint32_t i = 0; i < n; i++) lst.push_back(a.u32(x + 4 * int64_t(i)));
        std::set<uint32_t> all;
        for (const auto &s : syms) all.insert(s.s);
        for (uint32_t e : lst)
            if (!all.count(e)) throw Stop(u3::fmt("AssertionError: %#x", e));
        const B rootb = sl(c.d, og, og + 0x58);
        if (c.u32(og + 0x54) != 0x05dcdf85 || c.u32(og + 0x58) != 1) throw Stop("AssertionError: group root");
        align(16, 8);
        app(d, B{0, 0, 0, 0});
        app(d, le(0x77589471, 4));
        const int64_t gb = int64_t(d.size());
        app(d, rootb);
        putle(d, gb + 0x10, n, 4);
        const int64_t Aa = int64_t(d.size());
        app(d, le(1, 4));
        app(d, le(0, 4));
        d.resize(d.size() + 16, 0);
        const int64_t L = int64_t(d.size());
        for (uint32_t e : lst) app(d, le(e, 4));
        align(8, 0);
        const int64_t Z = int64_t(d.size());
        d.resize(d.size() + 8 * size_t(n), 0);
        const int64_t X = int64_t(d.size());
        d.resize(d.size() + 8, 0);
        const int64_t Y = int64_t(d.size());
        d.resize(d.size() + 8, 0);
        for (const auto &[slot, tgt] : std::vector<std::pair<int64_t, int64_t>>{{gb + 0x20, Aa}, {gb + 0x28, L}, {gb + 0x38, L}, {gb + 0x40, Z}, {Aa + 8, X}, {Aa + 0x10, Y}}) {
            putle(d, slot, uint64_t(tgt), 8);
            ptrs.insert(slot);
        }
        ptrs.insert(gb);   // the name pointer copied from retail (an absolute offset into the same module)
        syms[symd.at(g)] = {g, t, uint64_t(gb)};
        std::string ls;
        for (uint32_t e : lst) ls += (ls.empty() ? "" : " ") + u3::fmt("%08x", e);
        log.push_back(u3::fmt("%s group %08x: count %u -> %u, list %s", mod.c_str(), g, c.u32(og + 0x10), n, ls.c_str()));
    }
    return build_dc2(d, ptrs, syms, c.syms.size());
}

}  // namespace

int part_modules(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply"}, {"--project", "--job", "--ps4", "--ps3-disc", "--ps3-update", "--dest"});
    const std::string project = a.need("--project"), ps4d = a.need("--ps4"), disc = a.get("--ps3-disc", ""), upd = a.get("--ps3-update", "");
    for (const auto &[k, v] : std::vector<std::pair<std::string, std::string>>{{"PROJECT", project}, {"PS4", ps4d}, {"PS3_DISC", disc}, {"PS3_UPDATE", upd}})
        if (v.empty() || v.rfind("{{", 0) == 0) {
            std::printf("FAILED: %s is not set\n", k.c_str());
            return 1;
        }
    const fs::path job = upath(a.need("--job"));
    static const char *BATCH45_MD5 = "c77a9e0cb983d9ca2072b561565aa537";
    static const std::vector<std::pair<std::string, std::string>> EXPECT = {
        {"part-cave-multi", "9c9d7dad7bcc21df44cdf6707cec394a"}, {"part-gompa-multi", "e9c4ea7542f5965f019a6105925e235f"}, {"part-shambhala-multi", "3e843674e8996bc1ff018040cbd91038"}};
    const auto t0 = std::chrono::steady_clock::now();
    const fs::path main4 = upath(ps4d) / "u3data" / "build" / "main" / "dc2" / "part-module";
    Ps3Index idx(disc, upd, "");
    std::map<std::pair<std::string, std::string>, std::string> want;
    for (const auto &r : read_tsv(job / "inputs.tsv")) want[{r.at(0), r.at(1)}] = r.at(2);
    std::vector<std::string> differ;
    auto ps4 = [&](const std::string &m) {
        const fs::path p = main4 / upath(m + ".bin");
        if (!fs::exists(p)) throw Stop("FileNotFoundError: PS4 retail module missing: " + win_path(utf8(p)));
        B b = read_file(p);
        auto w = want.find({"ps4", "dc2/part-module/" + m + ".bin"});
        if (w != want.end() && w->second != sha256_hex(b)) differ.push_back("PS4 " + m);
        return b;
    };
    auto ps3 = [&](const std::string &m, const std::string &layer) {
        const std::string rel = "dc1/ps4/" + m + ".bin";
        const auto b = idx.read(rel, layer);
        if (!b)
            throw Stop("FileNotFoundError: PS3 module " + m + " not found in " +
                       (layer == "patch18" ? std::string("dlc-patch18.psarc of the 1.19 update") : std::string("build/main/bin.psarc of the disc")));
        auto w = want.find({"ps3/" + layer, rel});
        if (w != want.end() && w->second != sha256_hex(*b)) differ.push_back("PS3 " + m + " (" + layer + ")");
        return *b;
    };
    auto differ_note = [&](const char *pre) {
        if (differ.empty()) return;
        std::string s;
        for (const std::string &x : differ) s += (s.empty() ? "" : ", ") + x;
        std::printf("%s%s\n", pre, s.c_str());
    };
    std::vector<std::string> log;
    std::map<std::string, B> outs;
    try {
        g_rules.clear();
        for (const auto &r : read_tsv(job / "root4rules.tsv")) {
            Rule x{int64_t(std::stoll(r.at(0), nullptr, 16)), r.at(1), {}, 0};
            if (x.kind == "const")
                for (size_t i = 0; i + 1 < r.at(2).size(); i += 2) x.cst.push_back(uint8_t(std::stoul(r.at(2).substr(i, 2), nullptr, 16)));
            else
                x.src = std::stoll(r.at(2), nullptr, 16);
            auto it = std::find_if(g_rules.begin(), g_rules.end(), [&](const Rule &y) { return y.off == x.off; });
            if (it == g_rules.end()) g_rules.push_back(x);
            else *it = x;
        }
        const auto mods = read_tsv(job / "mp-modules.tsv");
        std::map<std::string, B> p4, p3;
        for (const auto &m : mods) p4[m.at(0)] = ps4(m.at(0));
        for (const auto &m : mods) p3[m.at(0)] = ps3(m.at(0), m.at(1));
        // the s11 struct samples, learned from the PS3 / PS4 MP pairs (in module-list order)
        std::vector<std::pair<uint32_t, std::vector<std::pair<Words, Words>>>> samples;
        for (const auto &m : mods) {
            const DC da(p3[m.at(0)]);
            const Spans A(da, false);
            const DC db = load_dc2(p4[m.at(0)]);
            const Spans Bs(db, true);
            for (const auto &s : da.syms) {
                if (s.t != E_TYPE || !db.bysid.count(s.s)) continue;
                const int64_t o = int64_t(s.o), o4 = int64_t(db.bysid.at(s.s).second);
                const auto x = sub3(da, o, 11), y = sub4(db, o4, 8);
                if (!x) continue;
                const uint32_t ty = da.u32(*x - 4);
                const int64_t n3 = floordiv4(A.end(*x) - *x) - 1;
                const int64_t e4 = Bs.end(need(y)), n4 = floordiv4(e4 - *y) - 2;
                Words w3, w4;
                for (int64_t i = 0; i < n3; i++) w3.push_back(sl(da.d, *x + 4 * i, *x + 4 * i + 4));
                for (int64_t j = 0; j < n4; j++) w4.push_back(sl(db.d, *y + 4 * j, *y + 4 * j + 4));
                auto it = std::find_if(samples.begin(), samples.end(), [&](const auto &v) { return v.first == ty; });
                if (it == samples.end()) samples.push_back({ty, {{w3, w4}}});
                else it->second.push_back({w3, w4});
            }
        }
        {
            auto sorted = samples;
            std::sort(sorted.begin(), sorted.end(), [](const auto &p, const auto &q) { return p.first < q.first; });
            std::string s;
            for (const auto &[t, v] : sorted) s += (s.empty() ? "" : ", ") + u3::fmt("%08x: %zu", t, v.size());
            std::printf("s11 samples: %s\n", s.c_str());
        }
        std::map<uint32_t, DonorMap> s11;
        for (const auto &[t, v] : samples)
            if (std::find(std::begin(S11_TYPES), std::end(S11_TYPES), t) != std::end(S11_TYPES)) s11.emplace(t, DonorMap(v));
        // batch45: the retail gompa module + the 31 water effects grafted from other retail MP modules
        std::vector<std::pair<uint32_t, std::string>> items;
        std::map<std::string, B> srcs;
        for (const auto &r : read_tsv(job / "batch45-graft.tsv")) {
            items.push_back({uint32_t(std::stoul(r.at(0), nullptr, 16)), r.at(1)});
            srcs[r.at(1)] = p4.at(r.at(1));
        }
        const B g45 = graft(p4.at("part-gompa-multi"), items, srcs, log);
        if (md5_hex(g45) != BATCH45_MD5) {
            for (const std::string &l : log) std::printf("%s\n", l.c_str());
            std::printf("FAILED: batch45 gompa base md5 %s, expected %s\n", md5_hex(g45).c_str(), BATCH45_MD5);
            differ_note("inputs that differ from the reference copies: ");
            return 1;
        }
        log.push_back(u3::fmt("batch45 part-gompa-multi base: md5 %s (%zu bytes)", md5_hex(g45).c_str(), g45.size()));
        // batch48
        std::map<uint32_t, std::string> names;
        for (const auto &r : read_tsv(job / "emitter-names.tsv")) names[uint32_t(std::stoul(r.at(0), nullptr, 16))] = r.at(1);
        std::vector<std::pair<std::string, std::vector<std::pair<std::string, int64_t>>>> mat;   // PS3 material -> [(PS4 material, pairs)], in file order
        for (const auto &r : read_tsv(job / "mattab.tsv")) {
            auto it = std::find_if(mat.begin(), mat.end(), [&](const auto &x) { return x.first == r.at(0); });
            if (it == mat.end()) mat.push_back({r.at(0), {}}), it = mat.end() - 1;
            auto v = std::find_if(it->second.begin(), it->second.end(), [&](const auto &x) { return x.first == r.at(1); });
            if (v == it->second.end()) it->second.push_back({r.at(1), std::stoll(r.at(2))});
            else v->second = std::stoll(r.at(2));
        }
        for (const auto &[mod, groups] : GROUPS) outs[mod] = build48(mod, groups, mod == "part-gompa-multi" ? g45 : p4.at(mod), p3.at(mod), names, mat, s11, log);
    } catch (const std::exception &ex) {
        std::printf("FAILED: %s\n", ex.what());
        differ_note("inputs that differ from the reference copies: ");
        return 1;
    }
    for (const std::string &l : log) std::printf("%s\n", l.c_str());
    int bad = 0;
    for (const auto &[m, h] : EXPECT) {
        const std::string got = md5_hex(outs[m]);
        if (got != h) bad++;
        std::printf("%s.bin  md5 %s  %s  (%zu bytes)\n", m.c_str(), got.c_str(), got == h ? "OK" : ("MISMATCH, expected " + h).c_str(), outs[m].size());
    }
    if (bad) {
        differ_note("inputs that differ from the reference copies: ");
        std::printf("FAILED: %d of 3 part modules do not match the reference md5; nothing written\n", bad);
        return 1;
    }
    differ_note("note: some inputs differ from the reference copies, the outputs still match: ");
    const fs::path dest = a.val.count("--dest") ? upath(a.get("--dest", "")) : upath(project) / "work" / "overlay-r3" / "u3data" / "build" / "main" / "dc2" / "part-module";
    int written = 0, already = 0;
    for (const auto &[m, h] : EXPECT) {
        const fs::path p = dest / upath(m + ".bin");
        if (fs::exists(p) && md5_hex(read_file(p)) == h) {
            already++;
            continue;
        }
        if (a.has("--apply")) {
            fs::create_directories(dest);
            if (fs::exists(p) && !a.val.count("--dest")) {
                const fs::path bk = upath(project) / "work" / "backup-part-emitters" / upath(m + ".bin");
                if (!fs::exists(bk)) {
                    fs::create_directories(bk.parent_path());
                    copy2(p, bk);
                }
            }
            write_file_replace(p, outs[m]);
        }
        written++;
    }
    std::printf("part modules: %d written, %d already at the reference md5%s  (%.1f s)\n", written, already, a.has("--apply") ? "" : " (dry run, nothing written)",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    std::printf("convert-dc2-part-modules: 3/3 md5 OK\n");
    return 0;
}

}  // namespace u3
