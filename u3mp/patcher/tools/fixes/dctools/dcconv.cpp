// dctools/dcconv.cpp: see dcconv.hpp.
#include "dcconv.hpp"

#include "json.h"
#include "u3fix.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <numeric>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace u3::dcconv {
namespace {

using W4 = std::array<uint8_t, 4>;   // a PS3 word, its bytes in file order

// ------------------------------------------------------------------------------------------------ PS3 / PS4 parse
struct Mod {
    Bytes d;
    bool be = false;
    int ps = 4;
    uint64_t reloc = 0, data_start = 0;
    struct Sym {
        uint32_t s, t;
        uint64_t v;
    };
    std::vector<Sym> syms;
    std::vector<uint64_t> slots;    // sorted
    std::vector<uint64_t> dslots;   // slots in the data, sorted
    uint32_t u32(uint64_t o) const {
        if (o + 4 > d.size()) throw Stop("struct.error: unpack_from requires a buffer of at least 4 bytes");
        return be ? be32(d, o) : rd32le(d, o);
    }
    uint64_t ptr(uint64_t o) const {
        if (ps == 4) return u32(o);
        if (o + 8 > d.size()) throw Stop("struct.error: unpack_from requires a buffer of at least 8 bytes");
        return rd64le(d, o);
    }
    bool is_slot(uint64_t o) const { return std::binary_search(slots.begin(), slots.end(), o); }
    explicit Mod(const Bytes &data) : d(data) {
        if (d.size() >= 4 && std::equal(d.begin(), d.begin() + 4, "DC00")) be = true, ps = 4;
        else if (d.size() >= 4 && std::equal(d.begin(), d.begin() + 4, "00CD")) be = false, ps = 8;
        else throw Stop("ValueError: not a DC module");
        reloc = be ? u32(8) : ptr(8);
        const uint32_t n = u32(0x14);
        const uint64_t es = 8 + uint64_t(ps);
        for (uint32_t i = 0; i < n; i++) {
            const uint64_t o = 0x18 + i * es;
            syms.push_back({u32(o), u32(o + 4), ptr(o + 8)});
        }
        data_start = 0x18 + n * es;
        const uint32_t cnt = u32(reloc);
        const uint64_t b0 = std::min<uint64_t>(reloc + 4, d.size()), b1 = std::min<uint64_t>(reloc + 4 + cnt, d.size());
        for (uint64_t bi = b0; bi < b1; bi++) {
            const uint8_t b = d[bi];
            if (!b) continue;
            for (int k = 0; k < 8; k++)
                if (be ? (b >> (7 - k) & 1) : (b >> k & 1)) slots.push_back(((bi - b0) * 8 + uint64_t(k)) * uint64_t(ps));
        }
        std::sort(slots.begin(), slots.end());
        slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
        for (uint64_t s : slots)
            if (s >= data_start) dslots.push_back(s);
    }
    // the data slots in [a, b)
    std::pair<size_t, size_t> slots_in(uint64_t a, uint64_t b) const {
        return {size_t(std::lower_bound(dslots.begin(), dslots.end(), a) - dslots.begin()), size_t(std::lower_bound(dslots.begin(), dslots.end(), b) - dslots.begin())};
    }
    W4 word(uint64_t o) const {
        W4 w{0, 0, 0, 0};
        for (int k = 0; k < 4; k++) w[size_t(k)] = o + uint64_t(k) < d.size() ? d[o + uint64_t(k)] : 0;
        return w;
    }
};

Bytes slice(const Bytes &d, int64_t a, int64_t b) {   // d[a:b] for 0 <= a (Python clamps the end)
    a = std::clamp<int64_t>(a, 0, int64_t(d.size()));
    b = std::clamp<int64_t>(b, a, int64_t(d.size()));
    return Bytes(d.begin() + a, d.begin() + b);
}
bool any_nonzero(const Bytes &d, int64_t a, int64_t b) {   // any(d[a:b])
    a = std::clamp<int64_t>(a, 0, int64_t(d.size()));
    b = std::clamp<int64_t>(b, a, int64_t(d.size()));
    for (int64_t k = a; k < b; k++)
        if (d[size_t(k)]) return true;
    return false;
}
std::string hexs(int64_t v) { return v < 0 ? "-" + u3::fmt("%llx", (unsigned long long)(-v)) : u3::fmt("%llx", (unsigned long long)v); }   // '%x'
int64_t floordiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
int64_t pymod(int64_t a, int64_t b) { return a - floordiv(a, b) * b; }
int64_t al(int64_t x, int64_t a) { return floordiv(x + a - 1, a) * a; }

// ------------------------------------------------------------------------------------------------ objects and keys
struct Obj {
    int64_t t, end;
    std::optional<uint32_t> hdr;
};
std::pair<int64_t, int64_t> region(const Obj &o) { return {o.hdr ? o.t - 4 : o.t, o.end}; }

std::optional<std::string> tailkey(const std::string &k, int n) {
    std::vector<std::string> parts;
    size_t a = 0;
    for (size_t i = 0; i <= k.size(); i++)
        if (i == k.size() || k[i] == '.') {
            parts.push_back(k.substr(a, i - a));
            a = i + 1;
        }
    if (int(parts.size()) <= n + 1) return std::nullopt;
    std::string r = parts[0] + "~";
    for (size_t i = parts.size() - size_t(n); i < parts.size(); i++) r += (i > parts.size() - size_t(n) ? "." : "") + parts[i];
    return r;
}

std::vector<int64_t> targets(const Mod &m) {
    std::set<int64_t> tg;
    for (const auto &s : m.syms) tg.insert(int64_t(s.v));
    for (uint64_t s : m.dslots) tg.insert(int64_t(m.ptr(s)));
    std::vector<int64_t> out;
    for (int64_t t : tg)
        if (int64_t(m.data_start) <= t && t < int64_t(m.reloc)) out.push_back(t);
    return out;
}

// objects + keys; header decisions in BFS order from the field that refers to an object: decide(index, field key,
// header word) (the index into the target list, before a leading padding object is inserted)
struct Graph {
    std::vector<Obj> objs;
    std::vector<std::string> keys, fks;
};
Graph graph(const Mod &m, const std::function<bool(size_t, const std::string &, uint32_t)> &decide) {
    const std::vector<int64_t> tg = targets(m);
    Graph g;
    for (size_t i = 0; i < tg.size(); i++) g.objs.push_back({tg[i], i + 1 < tg.size() ? tg[i + 1] : int64_t(m.reloc), std::nullopt});
    std::vector<std::optional<uint32_t>> cand(g.objs.size());
    for (size_t i = 0; i < g.objs.size(); i++) {
        const int64_t t = g.objs[i].t;
        if (t % 4 == 0 && t - 4 >= int64_t(m.data_start) && !m.is_slot(uint64_t(t - 4)) && (i == 0 || g.objs[i - 1].t < t - 4)) {
            const uint32_t h = m.u32(uint64_t(t - 4));
            if (h > 0xffff) cand[i] = h;
        }
    }
    std::unordered_map<int64_t, size_t> start;
    for (size_t i = 0; i < g.objs.size(); i++) start[g.objs[i].t] = i;
    std::vector<std::vector<uint64_t>> slots_by_obj(g.objs.size());
    for (uint64_t s : m.dslots) {
        const int64_t k = int64_t(std::upper_bound(tg.begin(), tg.end(), int64_t(s)) - tg.begin()) - 1;
        if (k >= 0) slots_by_obj[size_t(k)].push_back(s);
    }
    std::vector<std::optional<std::string>> keys(g.objs.size()), fks(g.objs.size());
    auto visit = [&](size_t i, const std::string &fk) {
        fks[i] = fk;
        if (cand[i] && decide(i, fk, *cand[i])) {
            g.objs[i].hdr = cand[i];
            keys[i] = u3::fmt("H%08x", *cand[i]);
        } else
            keys[i] = fk;
    };
    std::deque<size_t> q;
    for (const auto &s : m.syms) {
        auto it = start.find(int64_t(s.v));
        if (it != start.end() && !keys[it->second]) {
            visit(it->second, u3::fmt("S%08x", s.t));
            q.push_back(it->second);
        }
    }
    while (!q.empty()) {
        const size_t i = q.front();
        q.pop_front();
        for (uint64_t s : slots_by_obj[i]) {
            auto it = start.find(int64_t(m.ptr(s)));
            if (it == start.end() || keys[it->second]) continue;
            visit(it->second, *keys[i] + "." + hexs(int64_t(s) - g.objs[i].t));
            q.push_back(it->second);
        }
    }
    for (size_t i = 0; i < g.objs.size(); i++)
        if (!keys[i]) visit(i, "O");
    for (size_t i = 0; i + 1 < g.objs.size(); i++) g.objs[i].end = region(g.objs[i + 1]).first;
    for (size_t i = 0; i < g.objs.size(); i++) {
        g.keys.push_back(*keys[i]);
        g.fks.push_back(*fks[i]);
    }
    if (!g.objs.empty() && region(g.objs[0]).first > int64_t(m.data_start)) {
        g.objs.insert(g.objs.begin(), Obj{int64_t(m.data_start), region(g.objs[0]).first, std::nullopt});
        g.keys.insert(g.keys.begin(), "O");
        g.fks.insert(g.fks.begin(), "O");
    }
    return g;
}

// ------------------------------------------------------------------------------------------------ transforms
// unit kinds as bits
enum : uint8_t { KW = 1, KH = 2, KR = 4, KQ = 8, Kq = 16, KP = 32, KT = 64, KD = 128 };
char kchar(uint8_t b) {
    switch (b) {
    case KW: return 'W';
    case KH: return 'H';
    case KR: return 'R';
    case KQ: return 'Q';
    case Kq: return 'q';
    case KP: return 'P';
    case KT: return 'T';
    default: return 'D';
    }
}
W4 tr(uint8_t kd, const W4 &w) {
    if (kd == KH) return {w[1], w[0], w[3], w[2]};
    if (kd == KR) return w;
    return {w[3], w[2], w[1], w[0]};   // W (and the converter's default)
}
const uint8_t TRANS[3] = {KW, KH, KR};

// body = one NUL-terminated printable string + zero padding -> the string with its NUL
std::optional<Bytes> as_string(const Bytes &body) {
    const auto z = std::find(body.begin(), body.end(), 0);
    if (z == body.end() || z == body.begin()) return std::nullopt;
    for (auto it = z; it != body.end(); ++it)
        if (*it) return std::nullopt;
    for (auto it = body.begin(); it != z; ++it)
        if (!((*it >= 32 && *it < 127) || *it == 9 || *it == 10 || *it == 13)) return std::nullopt;
    return Bytes(body.begin(), z + 1);
}

// ------------------------------------------------------------------------------------------------ pair alignment
using AMap = std::unordered_map<int64_t, int64_t>;
std::optional<AMap> anchors(const Mod &a, const Mod &b) {
    if (a.dslots.size() != b.dslots.size() || a.syms.size() != b.syms.size()) return std::nullopt;
    for (size_t i = 0; i < a.syms.size(); i++)
        if (a.syms[i].s != b.syms[i].s) return std::nullopt;
    std::set<std::pair<int64_t, int64_t>> pairs;
    for (size_t i = 0; i < a.dslots.size(); i++) {
        pairs.insert({int64_t(a.dslots[i]), int64_t(b.dslots[i])});
        pairs.insert({int64_t(a.ptr(a.dslots[i])), int64_t(b.ptr(b.dslots[i]))});
    }
    for (size_t i = 0; i < a.syms.size(); i++) pairs.insert({int64_t(a.syms[i].v), int64_t(b.syms[i].v)});
    pairs.insert({int64_t(a.data_start), int64_t(b.data_start)});
    pairs.insert({int64_t(a.reloc), int64_t(b.reloc)});
    std::map<int64_t, int64_t> M;
    std::set<int64_t> bad;
    for (const auto &[x, y] : pairs) {
        auto it = M.find(x);
        if (it != M.end() && it->second != y) bad.insert(x);
        M[x] = y;
    }
    for (int64_t x : bad) M.erase(x);
    std::vector<int64_t> xs, ys;
    for (const auto &[x, y] : M) xs.push_back(x), ys.push_back(y);
    bool mono = true;
    for (size_t i = 0; i + 1 < ys.size(); i++) mono = mono && ys[i] <= ys[i + 1];
    AMap out;
    if (mono) {
        for (const auto &[x, y] : M) out[x] = y;
        return out;
    }
    // PS4 may pool strings elsewhere: the longest monotone subset
    std::vector<int64_t> tails;
    std::vector<int64_t> tidx, prev(xs.size(), -1);
    for (size_t i = 0; i < ys.size(); i++) {
        const size_t k = size_t(std::upper_bound(tails.begin(), tails.end(), ys[i]) - tails.begin());
        if (k == tails.size()) tails.push_back(ys[i]), tidx.push_back(int64_t(i));
        else tails[k] = ys[i], tidx[k] = int64_t(i);
        prev[i] = k ? tidx[k - 1] : -1;
    }
    for (int64_t i = tidx.back(); i >= 0; i = prev[size_t(i)]) out[xs[size_t(i)]] = ys[size_t(i)];
    return out;
}

// per word {relative PS4 position: kinds} over every complete alignment of the PS3 words W to the PS4 bytes G
using Feas = std::vector<std::map<int64_t, uint8_t>>;
std::optional<Feas> seg_feasible(const std::vector<W4> &W, const Bytes &G, int64_t base4, std::optional<int64_t> tailmax) {
    const int64_t n = int64_t(W.size()), L = int64_t(G.size());
    if (L % 4) return std::nullopt;
    std::vector<char> zero(size_t(L / 4)), zt(size_t(L / 4 + 1), 1);
    for (int64_t p = 0; p < L; p += 4) zero[size_t(p / 4)] = !G[size_t(p)] && !G[size_t(p + 1)] && !G[size_t(p + 2)] && !G[size_t(p + 3)];
    for (int64_t k = L / 4 - 1; k >= 0; k--) zt[size_t(k)] = zt[size_t(k + 1)] && zero[size_t(k)];
    if (4 * n > L) return std::nullopt;
    struct Edge {
        uint8_t kd;   // 0 = gap
        int64_t i2, p2;
    };
    auto key = [](int64_t i, int64_t p) { return (uint64_t(i) << 32) | uint64_t(p); };
    auto moves = [&](int64_t i, int64_t p) {
        std::vector<Edge> out;
        if (i < n && p + 4 <= L) {
            const W4 g{G[size_t(p)], G[size_t(p + 1)], G[size_t(p + 2)], G[size_t(p + 3)]};
            for (uint8_t kd : TRANS)
                if (tr(kd, W[size_t(i)]) == g) out.push_back({kd, i + 1, p + 4});
            if (i + 1 < n && p + 8 <= L && (base4 + p) % 8 == 0) {
                bool ok = true;   // (W[i] + W[i+1])[::-1] == G[p:p+8]
                for (int k = 0; k < 8 && ok; k++) {
                    const uint8_t src = k < 4 ? W[size_t(i + 1)][size_t(3 - k)] : W[size_t(i)][size_t(7 - k)];
                    ok = src == G[size_t(p + k)];
                }
                if (ok) out.push_back({KQ, i + 2, p + 8});
            }
        }
        if (p + 4 <= L && zero[size_t(p / 4)] && (L - p - 4) >= 4 * (n - i)) out.push_back({0, i, p + 4});
        return out;
    };
    std::unordered_set<uint64_t> F{key(0, 0)};
    std::vector<std::pair<int64_t, int64_t>> order{{0, 0}};
    std::unordered_map<uint64_t, std::vector<Edge>> edges;
    for (size_t k = 0; k < order.size();) {
        const auto st = order[k++];
        auto es = moves(st.first, st.second);
        for (const Edge &e : es)
            if (F.insert(key(e.i2, e.p2)).second) order.push_back({e.i2, e.p2});
        edges[key(st.first, st.second)] = std::move(es);
        if (order.size() > 300000) return std::nullopt;
    }
    std::vector<std::pair<int64_t, int64_t>> ends;
    for (const auto &st : order)
        if (st.first == n && zt[size_t(st.second / 4)] && (!tailmax || L - st.second < *tailmax)) ends.push_back(st);
    if (ends.empty()) return std::nullopt;
    std::unordered_map<uint64_t, std::vector<std::pair<int64_t, int64_t>>> rev;
    for (const auto &st : order)
        for (const Edge &e : edges[key(st.first, st.second)]) rev[key(e.i2, e.p2)].push_back(st);
    std::unordered_set<uint64_t> B;
    std::vector<std::pair<int64_t, int64_t>> stack;
    for (const auto &e : ends)
        if (B.insert(key(e.first, e.second)).second) stack.push_back(e);
    while (!stack.empty()) {
        const auto t = stack.back();
        stack.pop_back();
        auto r = rev.find(key(t.first, t.second));
        if (r == rev.end()) continue;
        for (const auto &s : r->second)
            if (B.insert(key(s.first, s.second)).second) stack.push_back(s);
    }
    Feas res(static_cast<size_t>(n));
    for (const auto &st : order) {
        if (!B.count(key(st.first, st.second))) continue;
        for (const Edge &e : edges[key(st.first, st.second)]) {
            if (!e.kd || !B.count(key(e.i2, e.p2))) continue;
            if (e.kd == KQ) {
                res[size_t(st.first)][st.second] |= KQ;
                res[size_t(st.first + 1)][st.second + 4] |= Kq;
            } else
                res[size_t(st.first)][st.second] |= e.kd;
        }
    }
    return res;
}

// a slot-free object body: ('str', prefix, n) / ('code', op pairs, n) / ('blob', n)
struct Raw {
    std::string kind;
    std::optional<Bytes> prefix;   // str: the prefix (none: PS4 has another text)
    std::vector<std::pair<uint8_t, uint8_t>> ops;
    int64_t n = 0;
};
std::optional<Raw> raw_evidence(const Mod &b, const Bytes &body3, int64_t t4, int64_t e4, const std::vector<Bytes> &prefixes) {
    if (const auto s = as_string(body3)) {
        std::vector<Bytes> pl{Bytes()};
        pl.insert(pl.end(), prefixes.begin(), prefixes.end());
        const int64_t ls = int64_t(s->size());
        for (const Bytes &p : pl) {
            const int64_t lp = int64_t(p.size());
            if (lp <= ls && std::equal(p.begin(), p.end(), s->begin()) && slice(b.d, t4, t4 + ls - lp) == Bytes(s->begin() + lp, s->end()) &&
                !any_nonzero(b.d, t4 + ls - lp, e4))
                return Raw{"str", p, {}, ls - lp};
        }
        for (int64_t k = 1; k < ls - 1; k++)   // a new prefix: PS4 holds a suffix of the PS3 string
            if ((*s)[size_t(k - 1)] == '/' && slice(b.d, t4, t4 + ls - k) == Bytes(s->begin() + k, s->end()) && !any_nonzero(b.d, t4 + ls - k, e4))
                return Raw{"str", Bytes(s->begin(), s->begin() + k), {}, ls - k};
        // PS4 has another text here (retail debug strings differ)
        int64_t z = -1;
        for (int64_t k = std::max<int64_t>(t4, 0); k < int64_t(b.d.size()); k++)
            if (!b.d[size_t(k)]) { z = k; break; }
        if (0 <= z && z < e4 && !any_nonzero(b.d, z, e4)) {
            bool pr = true;
            for (int64_t k = t4; k < z && pr; k++) pr = b.d[size_t(k)] >= 32 && b.d[size_t(k)] < 127;
            if (pr) return Raw{"str", std::nullopt, {}, z + 1 - t4};
        }
        return std::nullopt;
    }
    const int64_t n = int64_t(body3.size());
    if (n % 4 == 0 && n && e4 - t4 >= n) {
        const Bytes g = slice(b.d, t4, t4 + n);
        if (g == body3 && !any_nonzero(b.d, t4 + n, e4)) return Raw{"blob", {}, {}, n};
        bool same = true;   // all(g[k+1:k+4] == body3[k+1:k+4])
        for (int64_t k = 0; k < n && same; k += 4) same = slice(g, k + 1, k + 4) == slice(body3, k + 1, k + 4);
        if (same && !any_nonzero(b.d, t4 + n, e4)) {
            Raw r{"code", {}, {}, n};
            bool diff = false, allnz = true;
            for (int64_t k = 0; k < n; k += 4) {
                r.ops.push_back({body3[size_t(k)], size_t(k) < g.size() ? g[size_t(k)] : uint8_t(0)});
                diff = diff || r.ops.back().first != r.ops.back().second;
                allnz = allnz && !(body3[size_t(k + 1)] == 0 && body3[size_t(k + 2)] == 0 && body3[size_t(k + 3)] == 0);
            }
            if (diff || allnz) return r;
        }
    } else if (n && e4 - t4 >= n && slice(b.d, t4, t4 + n) == body3 && !any_nonzero(b.d, t4 + n, e4))
        return Raw{"blob", {}, {}, n};
    return std::nullopt;
}

const int64_t DEL = -(int64_t(1) << 30);   // position of a PS3 word the PS4 file does not have
const int64_t ZP = -(int64_t(1) << 29);    // vote key: instances where the word was zero and kept

struct Evidence {
    int64_t s4, e4, end4;
    std::string kind;
    std::optional<Raw> raw;
    std::vector<std::pair<int64_t, std::map<int64_t, uint8_t>>> units;
};
std::optional<Evidence> align_object(const Mod &a, const Mod &b, const AMap &M, const Obj &ob, std::optional<int64_t> e4o, const std::vector<Bytes> &prefixes,
                                     bool allow_del) {
    const auto [s3, e3] = region(ob);
    const auto hdr = ob.hdr;
    const auto t4it = M.find(ob.t);
    if (t4it == M.end() || !e4o) return std::nullopt;
    const int64_t t4 = t4it->second, e4 = *e4o;
    const int64_t s4 = hdr ? t4 - 8 : t4;
    if (e4 < t4) return std::nullopt;
    Evidence ev{s4, e4, 0, "", std::nullopt, {}};
    int64_t o = s3;
    if (hdr) {
        Bytes want{0, 0, 0, 0};
        for (int k = 0; k < 4; k++) want.push_back(uint8_t(*hdr >> (8 * k)));
        if (slice(b.d, s4, s4 + 8) != want) return std::nullopt;
        ev.units.push_back({0, {{0, KT}}});
        o += 4;
    }
    std::vector<int64_t> slots;
    for (uint64_t x : a.dslots)
        if (o <= int64_t(x) && int64_t(x) < e3) slots.push_back(int64_t(x));
    const Bytes body3 = slice(a.d, o, e3);
    auto raw_ev = [&](const Raw &r) {
        Evidence e = ev;
        e.kind = r.kind;
        e.raw = r;
        e.end4 = t4 + r.n;
        return e;
    };
    if (slots.empty()) {
        const auto r = raw_evidence(b, body3, t4, e4, prefixes);
        if (r && (r->kind != "blob" || body3.size() % 4 || o % 4)) return raw_ev(*r);
    }
    if ((e3 - o) % 4 || o % 4) {
        const auto r = slots.empty() ? raw_evidence(b, body3, t4, e4, prefixes) : std::nullopt;
        if (!r) return std::nullopt;
        return raw_ev(*r);
    }
    int64_t pos = o, p4 = t4;
    std::vector<int64_t> marks = slots;
    marks.push_back(e3);
    for (int64_t mk : marks) {
        std::vector<W4> W;
        for (int64_t x = pos; x < mk; x += 4) W.push_back(a.word(uint64_t(x)));
        std::optional<int64_t> end4;
        if (mk == e3) end4 = e4;
        else if (auto it = M.find(mk); it != M.end()) end4 = it->second;
        if (!end4 || *end4 < p4) return std::nullopt;
        const Bytes G = slice(b.d, p4, *end4);
        if (!W.empty()) {
            std::optional<int64_t> tm;
            if (mk == e3) tm = *end4 % 8 ? 4 : *end4 % 16 ? 8 : 16;   // trailing padding: bounded by the next object's alignment
            auto f = seg_feasible(W, G, p4, tm);
            size_t ndel = 0;
            if (!f && mk == e3 && allow_del) {   // the old cloud converter dropped trailing zero words
                int64_t k = int64_t(W.size());
                while (k && W[size_t(k - 1)] == W4{0, 0, 0, 0}) k--;
                for (int64_t keep = int64_t(W.size()) - 1; keep >= k; keep--) {
                    f = seg_feasible(std::vector<W4>(W.begin(), W.begin() + keep), G, p4, tm);
                    if (f) {
                        ndel = W.size() - size_t(keep);
                        break;
                    }
                }
            }
            if (!f) {
                if (slots.empty() && mk == e3)
                    if (const auto r = raw_evidence(b, body3, t4, e4, prefixes)) return raw_ev(*r);
                return std::nullopt;
            }
            for (size_t i = 0; i < f->size(); i++) {
                std::map<int64_t, uint8_t> d;
                for (const auto &[r, ks] : (*f)[i]) d[p4 - s4 + r] = ks;
                ev.units.push_back({pos - s3 + 4 * int64_t(i), d});
            }
            for (size_t i = f->size(); i < f->size() + ndel; i++) ev.units.push_back({pos - s3 + 4 * int64_t(i), {{DEL, KD}}});
        } else if (!G.empty() && std::any_of(G.begin(), G.end(), [](uint8_t c) { return c != 0; }))
            return std::nullopt;
        if (mk == e3) break;
        if (*end4 % 8) return std::nullopt;
        ev.units.push_back({mk - s3, {{*end4 - s4, KP}}});
        pos = mk + 4;
        p4 = *end4 + 8;
    }
    int64_t last = 0;
    for (const auto &[rel3, d] : ev.units) {
        if (d.empty()) throw Stop("ValueError: min() arg is an empty sequence");
        const int64_t r4 = d.begin()->first;
        const uint8_t ks = d.begin()->second;
        if (r4 == DEL) continue;
        last = std::max(last, r4 + ((ks & (KP | KT)) ? 8 : 4));
    }
    ev.kind = "units";
    ev.end4 = s4 + last;
    return ev;
}

// ------------------------------------------------------------------------------------------------ insertion-ordered counter
template <class K>
struct OCounter {
    std::vector<std::pair<K, int64_t>> v;
    std::map<K, size_t> idx;
    int64_t &operator[](const K &k) {
        auto it = idx.find(k);
        if (it == idx.end()) {
            idx[k] = v.size();
            v.push_back({k, 0});
            return v.back().second;
        }
        return v[it->second].second;
    }
    int64_t get(const K &k) const {
        auto it = idx.find(k);
        return it == idx.end() ? 0 : v[it->second].second;
    }
    std::vector<std::pair<K, int64_t>> most_common() const {   // by count, ties in insertion order
        auto r = v;
        std::stable_sort(r.begin(), r.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        return r;
    }
};

// ------------------------------------------------------------------------------------------------ templates
struct TUnit {
    int64_t i, p;
    uint8_t kd;
};
struct TPart {
    std::vector<std::array<int64_t, 2>> units;   // [4i, p or -1]
    std::vector<uint8_t> kinds;
    std::vector<std::pair<int64_t, W4>> fills;
    int64_t size = 0;
};
// fixed-size objects whose PS4 form inserts fields PS3 does not have: all instances aligned at once
std::optional<std::pair<std::vector<TUnit>, std::vector<std::pair<int64_t, W4>>>> template_dp(const std::vector<std::pair<std::vector<W4>, Bytes>> &insts,
                                                                                              int64_t band = 256) {
    const int64_t n = int64_t(insts[0].first.size()), L = int64_t(insts[0].second.size());
    if (L % 4) return std::nullopt;
    const int64_t Q = L / 4;
    std::vector<std::vector<W4>> sigG(static_cast<size_t>(Q));
    for (int64_t q = 0; q < Q; q++)
        for (const auto &[w, g] : insts) {
            W4 x{0, 0, 0, 0};
            for (int k = 0; k < 4; k++)
                if (size_t(4 * q + k) < g.size()) x[size_t(k)] = g[size_t(4 * q + k)];
            sigG[size_t(q)].push_back(x);
        }
    std::vector<char> cst(static_cast<size_t>(Q));
    for (int64_t q = 0; q < Q; q++) {
        std::set<W4> s(sigG[size_t(q)].begin(), sigG[size_t(q)].end());
        cst[size_t(q)] = s.size() == 1;
    }
    std::map<uint8_t, std::vector<std::vector<W4>>> sigW;
    for (uint8_t kd : TRANS)
        for (int64_t i = 0; i < n; i++) {
            std::vector<W4> t;
            for (const auto &[w, g] : insts) t.push_back(tr(kd, w.at(size_t(i))));
            sigW[kd].push_back(t);
        }
    using St = std::tuple<int64_t, int64_t, int64_t>;
    std::map<std::pair<int64_t, int64_t>, int64_t> best{{{0, 0}, 0}};
    std::map<std::pair<int64_t, int64_t>, std::tuple<int64_t, int64_t, char>> back;
    std::priority_queue<St, std::vector<St>, std::greater<St>> pq;
    pq.push({0, 0, 0});
    const int64_t INF = int64_t(1) << 60;
    auto bestof = [&](int64_t i, int64_t q) { auto it = best.find({i, q}); return it == best.end() ? INF : it->second; };
    while (!pq.empty()) {
        const auto [c, i, q] = pq.top();
        pq.pop();
        if (bestof(i, q) < c) continue;
        if (i == n && q == Q) break;
        std::vector<std::tuple<int64_t, int64_t, int64_t, char>> mv;
        if (i < n && q < Q)
            for (uint8_t kd : TRANS)
                if (sigW[kd][size_t(i)] == sigG[size_t(q)]) {
                    mv.push_back({c, i + 1, q + 1, kchar(kd)});
                    break;
                }
        if (q < Q && cst[size_t(q)]) mv.push_back({c + 1, i, q + 1, 'F'});
        if (i < n) mv.push_back({c + 3, i + 1, q, 'D'});
        for (const auto &[c2, i2, q2, kd] : mv) {
            if (std::llabs(q2 - i2) > band / 4) continue;
            if (c2 < bestof(i2, q2)) {
                best[{i2, q2}] = c2;
                back[{i2, q2}] = {i, q, kd};
                pq.push({c2, i2, q2});
            }
        }
    }
    std::pair<int64_t, int64_t> st{n, Q};
    if (!back.count(st) && st != std::pair<int64_t, int64_t>{0, 0}) return std::nullopt;
    std::vector<TUnit> units;
    std::vector<std::pair<int64_t, W4>> fills;
    while (st != std::pair<int64_t, int64_t>{0, 0}) {
        const auto [i, q, kd] = back.at(st);
        if (kd == 'F') fills.push_back({4 * q, sigG[size_t(q)][0]});
        else if (kd == 'D') units.push_back({i, -1, KD});
        else units.push_back({i, 4 * q, kd == 'W' ? KW : kd == 'H' ? KH : KR});
        st = {i, q};
    }
    std::reverse(units.begin(), units.end());
    std::reverse(fills.begin(), fills.end());
    return std::make_pair(units, fills);
}

// ------------------------------------------------------------------------------------------------ learning
std::string latin1_utf8(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x80) o += char(c);
        else o += char(0xc0 | (c >> 6)), o += char(0x80 | (c & 0x3f));
    }
    return o;
}
std::string utf8_latin1(const std::string &s) {   // the prefixes are latin-1 text in the schema
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c >= 0xc0 && c < 0xe0 && i + 1 < s.size()) o += char(((c & 0x1f) << 6) | ((unsigned char)s[++i] & 0x3f));
        else o += char(c);
    }
    return o;
}
void jstr(std::string &o, const std::string &latin1) { json::quote(o, latin1_utf8(latin1), true); }
std::string hexbytes(const W4 &w) { return u3::fmt("%02x%02x%02x%02x", w[0], w[1], w[2], w[3]); }

struct UV {   // the votes for one PS3 unit
    std::map<int64_t, int64_t> feas, uniq;
    std::map<std::pair<int64_t, uint8_t>, int64_t> kc;
};
using UMap = std::map<int64_t, UV>;
struct OutUnit {
    int64_t rel3, r4;
    char kd;
    bool flag;
    bool operator==(const OutUnit &o) const { return rel3 == o.rel3 && r4 == o.r4 && kd == o.kd && flag == o.flag; }
};
struct Fail {
    std::vector<W4> W;
    Bytes G;
    int64_t t4, hl;
};
struct KRec {
    int64_t n = 0, bad = 0, align = 64, n3max = 0;
    OCounter<std::string> kinds;
    UMap units;
    std::vector<std::pair<int64_t, int64_t>> endr;
    std::map<int64_t, int64_t> span;
    std::vector<std::pair<std::string, UMap>> shapes;
    std::map<std::string, size_t> shape_idx;
};

std::vector<OutUnit> fin(const UMap &U, const std::vector<OutUnit> *prior) {
    std::map<int64_t, int64_t> pr;
    if (prior)
        for (const OutUnit &u : *prior) pr[u.rel3] = u.r4;   // a later duplicate wins
    std::vector<OutUnit> units;
    static const std::string pri = "PTWHRQq";
    for (const auto &[rel3, v] : U) {
        const int64_t nd = v.feas.count(DEL) ? v.feas.at(DEL) : 0;
        std::vector<int64_t> cand;
        for (const auto &[r, c] : v.feas)
            if (r != DEL && r != ZP) cand.push_back(r);
        if (cand.empty()) {
            units.push_back({rel3, 0, 'W', true});
            continue;
        }
        auto keyof = [&](int64_t r) {
            auto pit = pr.find(rel3);
            const auto u = v.uniq.find(r);
            return std::make_tuple(v.feas.at(r), u == v.uniq.end() ? 0 : u->second, int(pit != pr.end() && pit->second == r), -r);
        };
        int64_t r4 = cand[0];
        for (int64_t r : cand)
            if (keyof(r) > keyof(r4)) r4 = r;
        std::map<char, int64_t> kinds;
        for (const auto &[rk, c] : v.kc)
            if (rk.first == r4) kinds[kchar(rk.second)] += c;
        char kd = 'W';
        if (!kinds.empty()) {
            auto kk = [&](char c) { return std::make_tuple(int(c == 'P' || c == 'T'), kinds[c], -int(pri.find(c))); };
            bool first = true;
            for (const auto &[c, n] : kinds)
                if (first || kk(c) > kk(kd)) kd = c, first = false;
        }
        const int64_t zp = v.uniq.count(ZP) ? v.uniq.at(ZP) : 0;
        units.push_back({rel3, r4, kd, nd && 2 * nd >= zp});
    }
    // a u64 half without its partner becomes a plain word
    for (size_t i = 0; i < units.size(); i++) {
        OutUnit &u = units[i];
        if (u.kd == 'Q' && !(i + 1 < units.size() && units[i + 1].rel3 == u.rel3 + 4 && units[i + 1].r4 == u.r4 + 4)) u.kd = 'W';
        if (u.kd == 'q' && !(i && units[i - 1].kd == 'Q')) u.kd = 'W';
        if (u.kd == 'Q' && i + 1 < units.size()) units[i + 1].kd = 'q';
    }
    return units;
}

UMap fold(const UMap &U, int64_t o3, int64_t p3, int64_t p4) {
    UMap F;
    for (const auto &[rel3, v] : U) {
        if (rel3 < o3) {
            F[rel3] = v;
            continue;
        }
        const int64_t m = floordiv(rel3 - o3, p3), r = o3 + pymod(rel3 - o3, p3), sh = m * p4;
        UV &w = F[r];
        for (const auto &[x, c] : v.feas) w.feas[x == DEL ? x : x - sh] += c;
        for (const auto &[x, c] : v.uniq) w.uniq[(x == DEL || x == ZP) ? x : x - sh] += c;
        for (const auto &[xk, c] : v.kc) w.kc[{xk.first == DEL ? xk.first : xk.first - sh, xk.second}] += c;
    }
    return F;
}

std::optional<std::array<int64_t, 3>> find_period(const KRec &k, const std::string &ks) {
    if (k.span.size() < 3) return std::nullopt;
    const int64_t o3 = (!ks.empty() && ks[0] == 'H' && ks.find('.') == std::string::npos) ? 4 : 0;
    std::vector<int64_t> ns;
    for (const auto &[n, s] : k.span)
        if (n - o3 > 0) ns.push_back(n - o3);
    OCounter<int64_t> cands;
    for (size_t i = 0; i + 1 < ns.size(); i++)
        if (ns[i + 1] - ns[i] >= 8) cands[ns[i + 1] - ns[i]]++;
    std::optional<std::pair<size_t, std::array<int64_t, 3>>> best;
    const auto mc = cands.most_common();
    for (size_t ci = 0; ci < std::min<size_t>(6, mc.size()); ci++) {
        const int64_t g = mc[ci].first;
        std::vector<std::pair<int64_t, int64_t>> pts;
        for (const auto &[n, s] : k.span)
            if (n - o3 > 0 && pymod(n - o3, g) == 0) pts.push_back({floordiv(n - o3, g), s});
        if (pts.size() < 3) continue;
        const auto [m1, s1] = pts.front();
        const auto [m2, s2] = pts.back();
        if (m2 == m1) continue;
        const int64_t p4 = int64_t(std::nearbyint(double(s2 - s1) / double(m2 - m1) / 4.0)) * 4;   // round(): half to even
        if (p4 <= 0) continue;
        bool ok = true;
        for (const auto &[m, s] : pts) ok = ok && std::llabs((s - s1) - (m - m1) * p4) <= 8;
        if (!ok) continue;
        if (!best || pts.size() > best->first) best = std::make_pair(pts.size(), std::array<int64_t, 3>{o3, g, p4});
    }
    if (!best) return std::nullopt;
    return best->second;
}

std::string units_json(const std::vector<OutUnit> &u) {
    std::string o = "[";
    for (size_t i = 0; i < u.size(); i++) {
        o += (i ? ",[" : "[") + std::to_string(u[i].rel3) + "," + std::to_string(u[i].r4) + ",\"" + std::string(1, u[i].kd) + "\"";
        if (u[i].flag) o += ",1";
        o += "]";
    }
    return o + "]";
}
std::string tpart_json(const std::vector<TUnit> &units, const std::vector<std::pair<int64_t, W4>> &fills, int64_t size) {
    std::string o = "{\"units\":[";
    for (size_t i = 0; i < units.size(); i++)
        o += (i ? ",[" : "[") + std::to_string(4 * units[i].i) + "," + std::to_string(units[i].kd != KD ? units[i].p : -1) + ",\"" + std::string(1, kchar(units[i].kd)) + "\"]";
    o += "],\"fills\":[";
    for (size_t i = 0; i < fills.size(); i++) o += (i ? ",[" : "[") + std::to_string(fills[i].first) + ",\"" + hexbytes(fills[i].second) + "\"]";
    return o + "],\"size\":" + std::to_string(size) + "}";
}

// array-of-structs template: a PS3 element of w words -> PS4 stride S; the last element is S + c bytes (JSON or none)
std::optional<std::string> build_template(std::vector<Fail> fl) {
    const int64_t hl = fl[0].hl;
    fl.erase(std::remove_if(fl.begin(), fl.end(), [&](const Fail &x) { return x.hl != hl; }), fl.end());
    int64_t g = 0;
    for (const Fail &x : fl) g = std::gcd(g, int64_t(x.W.size()));
    if (!g) return std::nullopt;
    OCounter<std::pair<int64_t, int64_t>> sizes;
    for (const Fail &x : fl) sizes[{int64_t(x.W.size()), int64_t(x.G.size())}]++;
    auto align_of = [&] {
        int64_t a = 16;
        for (const Fail &x : fl)
            while (pymod(x.t4, a)) a = floordiv(a, 2);
        return a;
    };
    const std::string head = "{\"n\":" + std::to_string(fl.size()) + ",\"bad\":0,\"kind\":\"tmpl\",\"align\":";
    if (g < 8 && sizes.v.size() > 1) {   // separate objects of a few fixed sizes: one template per size
        std::vector<std::pair<std::string, std::string>> grp;
        for (const auto &[sz, cnt] : sizes.v) {
            std::vector<std::pair<std::vector<W4>, Bytes>> insts;
            for (const Fail &x : fl)
                if (int64_t(x.W.size()) == sz.first && int64_t(x.G.size()) == sz.second) insts.push_back({x.W, x.G});
            auto hd = [](const std::vector<W4> &W, const Bytes &G) {
                int64_t c = 0;
                for (size_t i = 0; i < std::min<size_t>(16, W.size()); i++) {
                    const W4 s = tr(KW, W[i]);
                    if (4 * i + 4 <= G.size() && std::equal(s.begin(), s.end(), G.begin() + std::ptrdiff_t(4 * i))) c++;
                }
                return c;
            };
            std::vector<std::pair<std::vector<W4>, Bytes>> good;
            for (const auto &x : insts)
                if (hd(x.first, x.second) >= 12) good.push_back(x);
            if (good.size() * 2 > insts.size()) insts = good;   // drop instances that are not conversions of this PS3 data
            const auto r = template_dp(insts);
            if (!r) continue;
            const std::string k = std::to_string(sz.first), v = tpart_json(r->first, r->second, sz.second);
            auto it = std::find_if(grp.begin(), grp.end(), [&](const auto &e) { return e.first == k; });
            if (it == grp.end()) grp.push_back({k, v});
            else it->second = v;
        }
        if (grp.empty()) return std::nullopt;
        std::string o = head + std::to_string(align_of()) + ",\"endround\":1,\"hl\":" + std::to_string(hl) + ",\"w3\":0,\"groups\":{";
        for (size_t i = 0; i < grp.size(); i++) o += (i ? ",\"" : "\"") + grp[i].first + "\":" + grp[i].second;
        return o + "}}";
    }
    std::vector<std::pair<int64_t, int64_t>> ks_;
    std::set<int64_t> kset;
    for (const Fail &x : fl) ks_.push_back({int64_t(x.W.size()) / g, int64_t(x.G.size())}), kset.insert(int64_t(x.W.size()) / g);
    std::optional<int64_t> Sd;
    if (kset.size() >= 2) {
        const auto [k1, l1] = *std::min_element(ks_.begin(), ks_.end());
        const auto [k2, l2] = *std::max_element(ks_.begin(), ks_.end());
        if (pymod(l2 - l1, k2 - k1)) return std::nullopt;
        Sd = floordiv(l2 - l1, k2 - k1);
        const int64_t c = l1 - k1 * *Sd;
        for (const auto &[k, L] : ks_)
            if (L != c + k * *Sd) return std::nullopt;
    } else if (*kset.begin() != 1)
        return std::nullopt;
    // Python slices (negative bounds count from the end)
    auto pyslice = [](int64_t len, int64_t a, int64_t b) {
        auto norm = [&](int64_t x) { return x < 0 ? std::max<int64_t>(0, x + len) : std::min(x, len); };
        a = norm(a);
        b = norm(b);
        return std::make_pair(a, std::max(a, b));
    };
    std::vector<std::pair<std::vector<W4>, Bytes>> inner, last;
    for (const Fail &x : fl) {
        const int64_t k = int64_t(x.W.size()) / g;
        if (!Sd) {
            last.push_back({x.W, x.G});
            continue;
        }
        for (int64_t j = 0; j < k - 1; j++) {
            const auto [ga, gb] = pyslice(int64_t(x.G.size()), j * *Sd, (j + 1) * *Sd);
            inner.push_back({std::vector<W4>(x.W.begin() + j * g, x.W.begin() + (j + 1) * g), Bytes(x.G.begin() + ga, x.G.begin() + gb)});
        }
        const auto [ga, gb] = pyslice(int64_t(x.G.size()), (k - 1) * *Sd, int64_t(x.G.size()));
        last.push_back({std::vector<W4>(x.W.begin() + (k - 1) * g, x.W.end()), Bytes(x.G.begin() + ga, x.G.begin() + gb)});
    }
    std::set<size_t> lens;
    for (const auto &x : last) lens.insert(x.second.size());
    if (lens.size() != 1) return std::nullopt;
    const auto rl = template_dp(last);
    if (!rl) return std::nullopt;
    std::optional<std::pair<std::vector<TUnit>, std::vector<std::pair<int64_t, W4>>>> ri;
    if (!inner.empty()) {
        ri = template_dp(inner);
        if (!ri) return std::nullopt;
    }
    std::string o = head + std::to_string(align_of()) + ",\"endround\":1,\"hl\":" + std::to_string(hl) + ",\"w3\":" + std::to_string(4 * g) +
                    ",\"last\":" + tpart_json(rl->first, rl->second, int64_t(last[0].second.size()));
    if (ri) o += ",\"inner\":" + tpart_json(ri->first, ri->second, *Sd);
    return o + "}";
}

struct Learner {
    std::vector<std::pair<std::string, KRec>> K;
    std::unordered_map<std::string, size_t> Kidx;
    OCounter<std::string> stats;
    std::vector<std::pair<int, OCounter<int>>> ops;
    std::map<int, size_t> ops_idx;
    OCounter<std::string> prefixes;
    OCounter<uint32_t> hdrs, nohdr;
    std::vector<std::pair<std::string, std::array<int64_t, 2>>> hdrtab;
    std::unordered_map<std::string, size_t> hdrtab_idx;
    std::vector<std::pair<std::string, std::vector<Fail>>> fails;
    std::unordered_map<std::string, size_t> fails_idx;

    KRec &key(const std::string &ks) {
        auto it = Kidx.find(ks);
        if (it != Kidx.end()) return K[it->second].second;
        Kidx[ks] = K.size();
        K.push_back({ks, KRec()});
        return K.back().second;
    }

    void add_pair(const Bytes &d3, const Bytes &d4, bool allow_del) {
        const Mod a(d3), b(d4);
        const auto Mo = anchors(a, b);
        if (!Mo) {
            stats["pair-graph-differs"]++;
            return;
        }
        const AMap &M = *Mo;
        const std::vector<int64_t> objs_t = targets(a);
        auto hdr_ok = [&](uint32_t h, int64_t t) {
            auto it = M.find(t);
            bool ok = false;
            if (it != M.end() && it->second >= 8) {
                Bytes want{0, 0, 0, 0};
                for (int k = 0; k < 4; k++) want.push_back(uint8_t(h >> (8 * k)));
                ok = slice(b.d, it->second - 8, it->second) == want;
            }
            if (it != M.end() && !ok) nohdr[h]++;
            return ok;
        };
        auto decide = [&](size_t i, const std::string &fk, uint32_t h) {
            const bool ok = hdr_ok(h, objs_t[i]);
            const auto t1 = tailkey(fk, 1);
            std::set<std::string> ks{fk, t1 ? *t1 : fk};
            for (const std::string &k_ : ks) {
                auto it = hdrtab_idx.find(k_);
                if (it == hdrtab_idx.end()) {
                    hdrtab_idx[k_] = hdrtab.size();
                    hdrtab.push_back({k_, {0, 0}});
                    it = hdrtab_idx.find(k_);
                }
                hdrtab[it->second].second[ok ? 0 : 1]++;
            }
            return ok;
        };
        const Graph g = graph(a, decide);
        std::vector<std::optional<int64_t>> starts4;
        for (const Obj &ob : g.objs) {
            auto it = M.find(ob.t);
            starts4.push_back(it == M.end() ? std::nullopt : std::optional<int64_t>(ob.hdr ? it->second - 8 : it->second));
        }
        stats["pairs"]++;
        std::vector<Bytes> pref;
        {
            const auto mc = prefixes.most_common();
            for (size_t i = 0; i < std::min<size_t>(8, mc.size()); i++) pref.push_back(Bytes(mc[i].first.begin(), mc[i].first.end()));
        }
        for (size_t i = 0; i < g.objs.size(); i++) {
            const Obj &ob = g.objs[i];
            if (ob.hdr) hdrs[*ob.hdr]++;
            std::optional<int64_t> nx = i + 1 < g.objs.size() ? starts4[i + 1] : std::optional<int64_t>(int64_t(b.reloc));
            if (!nx) {
                size_t j = i + 1;
                while (j < g.objs.size() && !starts4[j]) j++;
                nx = j < g.objs.size() ? starts4[j] : std::nullopt;
                if (j != i + 1) nx.reset();
            }
            const auto ev = align_object(a, b, M, ob, nx, pref, allow_del);
            std::vector<std::string> kl{g.keys[i]};
            for (int n : {2, 1})
                if (auto t = tailkey(g.keys[i], n)) kl.push_back(*t);
            for (const std::string &ks : kl) one(ks, ev, ob, a, b, M, nx);
        }
    }

    void one(const std::string &ks, const std::optional<Evidence> &ev, const Obj &ob, const Mod &a, const Mod &b, const AMap &M, std::optional<int64_t> nx) {
        KRec &k = key(ks);
        const auto [s3, e3] = region(ob);
        if (!ev) {
            stats["obj-unaligned"]++;
            k.bad++;
            const int64_t hl = ob.hdr ? 4 : 0;
            auto t4 = M.find(ob.t);
            if (t4 != M.end() && nx && pymod(e3 - s3 - hl, 4) == 0 && pymod(s3 + hl, 4) == 0) {
                const auto [sa, sb] = a.slots_in(uint64_t(s3), uint64_t(e3));
                if (sa == sb) {
                    auto it = fails_idx.find(ks);
                    if (it == fails_idx.end()) {
                        fails_idx[ks] = fails.size();
                        fails.push_back({ks, {}});
                        it = fails_idx.find(ks);
                    }
                    auto &fl = fails[it->second].second;
                    if (fl.size() < 64) {
                        Fail f;
                        for (int64_t x = s3 + hl; x < e3; x += 4) f.W.push_back(a.word(uint64_t(x)));
                        f.G = slice(b.d, t4->second, *nx);
                        f.t4 = t4->second;
                        f.hl = hl;
                        fl.push_back(std::move(f));
                    }
                }
            }
            return;
        }
        stats["obj-ok"]++;
        k.n++;
        k.kinds[ev->kind]++;
        const int64_t s4 = ev->s4 + (ob.hdr ? 8 : 0);
        int64_t al_ = 1;
        while (al_ < 16 && pymod(s4, al_ * 2) == 0) al_ *= 2;
        k.align = std::min(k.align, al_);
        k.n3max = std::max(k.n3max, e3 - s3);
        k.endr.push_back({ev->end4, ev->e4});
        auto span_min = [&] {
            auto it = k.span.find(e3 - s3);
            const int64_t v = ev->e4 - ev->s4;
            if (it == k.span.end()) k.span[e3 - s3] = std::min<int64_t>(int64_t(1) << 30, v);
            else it->second = std::min(it->second, v);
        };
        if (ev->kind == "str") {
            if (ev->raw->prefix && !ev->raw->prefix->empty()) prefixes[std::string(ev->raw->prefix->begin(), ev->raw->prefix->end())]++;
        } else if (ev->kind == "code") {
            for (const auto &[x, y] : ev->raw->ops) {
                auto it = ops_idx.find(x);
                if (it == ops_idx.end()) {
                    ops_idx[x] = ops.size();
                    ops.push_back({x, {}});
                    it = ops_idx.find(x);
                }
                ops[it->second].second[y]++;
            }
        } else if (ev->kind == "units") {
            span_min();
            std::string shape = hexs(e3 - s3) + ":";
            const auto [sa, sb] = a.slots_in(uint64_t(s3), uint64_t(e3));
            for (size_t i = sa; i < sb; i++) shape += (i > sa ? "," : "") + hexs(int64_t(a.dslots[i]) - s3);
            auto si = k.shape_idx.find(shape);
            if (si == k.shape_idx.end()) {
                k.shape_idx[shape] = k.shapes.size();
                k.shapes.push_back({shape, {}});
                si = k.shape_idx.find(shape);
            }
            for (UMap *U : {&k.units, &k.shapes[si->second].second})
                for (const auto &[rel3, d] : ev->units) {
                    UV &v = (*U)[rel3];
                    uint8_t allk = 0;
                    for (const auto &[r, kset] : d) {
                        v.feas[r]++;
                        if (d.size() == 1) v.uniq[r]++;
                        for (uint8_t bit = 1; bit; bit <<= 1)
                            if (kset & bit) v.kc[{r, bit}]++;
                        allk |= kset;
                    }
                    if (!(allk & KD) && (allk & KW) && (allk & KR)) v.uniq[ZP]++;   // a zero word that was kept
                }
        }
        if (ev->kind != "units") span_min();
    }

    std::string finish(size_t npairs) {
        std::vector<std::pair<std::string, std::string>> S;   // key -> entry JSON
        std::unordered_map<std::string, size_t> Sidx;
        std::unordered_map<std::string, int64_t> Sn;
        for (auto &[ks, k] : K) {
            if (!k.n) continue;
            std::string kind = k.kinds.most_common()[0].first;
            if (kind == "blob" && k.kinds.get("units")) kind = "units";
            std::string o = "{\"n\":" + std::to_string(k.n) + ",\"bad\":" + std::to_string(k.bad) + ",\"align\":" + std::to_string(k.align) + ",\"kind\":\"" + kind +
                            "\",\"n3max\":" + std::to_string(k.n3max);
            if (kind == "units") {
                const auto per = find_period(k, ks);
                std::vector<OutUnit> units = per ? fin(fold(k.units, (*per)[0], (*per)[1], (*per)[2]), nullptr) : fin(k.units, nullptr);
                if (per) o += ",\"period\":[" + std::to_string((*per)[0]) + "," + std::to_string((*per)[1]) + "," + std::to_string((*per)[2]) + "]";
                o += ",\"units\":" + units_json(units);
                std::vector<OutUnit> full = units;
                if (per) {
                    const auto [o3, p3, p4] = *per;
                    std::map<int64_t, OutUnit> base;
                    for (const OutUnit &u : units) base[u.rel3] = u;   // a later duplicate wins
                    full.clear();
                    for (const OutUnit &u : units)
                        if (u.rel3 < o3) full.push_back(u);
                    for (int64_t r = o3; r < k.n3max; r += 4) {
                        auto v = base.find(o3 + pymod(r - o3, p3));
                        if (v != base.end()) full.push_back({r, v->second.r4 + floordiv(r - o3, p3) * p4, v->second.kd, false});
                    }
                }
                std::string shp;
                for (const auto &[shape, U] : k.shapes) {
                    const std::vector<OutUnit> u = fin(U, &full);
                    const bool same = u.size() <= full.size() && std::equal(u.begin(), u.end(), full.begin());
                    if (!same) {
                        shp += shp.empty() ? "" : ",";
                        jstr(shp, shape);
                        shp += ":" + units_json(u);
                    }
                }
                if (!shp.empty()) o += ",\"shapes\":{" + shp + "}";
            }
            if (!k.span.empty()) {
                o += ",\"span\":{";
                bool first = true;
                for (const auto &[n3, v] : k.span) {
                    o += std::string(first ? "" : ",") + "\"" + hexs(n3) + "\":" + std::to_string(v);
                    first = false;
                }
                o += "}";
            }
            int64_t er = 1;
            for (int64_t c : {2, 4, 8, 16}) {
                bool ok = true;
                for (const auto &[e, nx] : k.endr) ok = ok && al(e, c) <= nx;
                if (!ok) break;
                er = c;
            }
            o += ",\"endround\":" + std::to_string(er) + "}";
            Sidx[ks] = S.size();
            Sn[ks] = k.n;
            S.push_back({ks, o});
        }
        for (auto &[ks, fl] : fails) {
            if (Sn.count(ks) && Sn[ks] >= int64_t(fl.size())) continue;
            if (ks.find('~') != std::string::npos || fl.size() < 2) continue;   // templates only for full keys seen twice
            const auto r = build_template(fl);
            if (!r) {
                stats["template-failed"]++;
                continue;
            }
            if (auto it = Sidx.find(ks); it != Sidx.end()) S[it->second].second = *r;
            else Sidx[ks] = S.size(), S.push_back({ks, *r});
            Sn[ks] = int64_t(fl.size());
            stats["templates"]++;
        }
        std::string o = "{\"keys\":{";
        for (size_t i = 0; i < S.size(); i++) {
            if (i) o += ",";
            jstr(o, S[i].first);
            o += ":" + S[i].second;
        }
        o += "},\"opmap\":{";
        for (size_t i = 0; i < ops.size(); i++) o += (i ? ",\"" : "\"") + std::to_string(ops[i].first) + "\":" + std::to_string(ops[i].second.most_common()[0].first);
        o += "},\"prefixes\":[";
        const auto pm = prefixes.most_common();
        for (size_t i = 0; i < pm.size(); i++) {
            if (i) o += ",";
            jstr(o, pm[i].first);
        }
        o += "],\"hdrs\":[";
        std::vector<uint32_t> hs;
        for (const auto &[h, n] : hdrs.v)
            if (n >= 2 && n >= 4 * nohdr.get(h)) hs.push_back(h);
        std::sort(hs.begin(), hs.end());
        for (size_t i = 0; i < hs.size(); i++) o += (i ? "," : "") + std::to_string(hs[i]);
        o += "],\"hdrtab\":{";
        bool first = true;
        for (const auto &[k_, v] : hdrtab) {
            if (v[0] == v[1]) continue;
            if (!first) o += ",";
            first = false;
            jstr(o, k_);
            o += ":[" + std::to_string(v[0]) + "," + std::to_string(v[1]) + "]";
        }
        o += "},\"version\":2,\"pairs\":" + std::to_string(npairs) + ",\"stats\":{";
        for (size_t i = 0; i < stats.v.size(); i++) {
            if (i) o += ",";
            jstr(o, stats.v[i].first);
            o += ":" + std::to_string(stats.v[i].second);
        }
        return o + "}}";
    }
};

}  // namespace

size_t slot_count(const Bytes &module) { return Mod(module).slots.size(); }

std::string learn(const std::vector<std::pair<Bytes, Bytes>> &pairs, const std::function<void(const std::string &)> &log) {
    Learner L;
    for (size_t i = 0; i < pairs.size(); i++) {
        try {
            L.add_pair(pairs[i].first, pairs[i].second, false);
        } catch (const std::exception &ex) {
            L.stats["pair-error"]++;
            log(u3::fmt("error pair %zu %s", i, ex.what()));
        }
    }
    const std::string s = L.finish(pairs.size());
    log(u3::fmt("learned %zu keys from %zu pairs", L.K.size(), pairs.size()));
    return s;
}

// ------------------------------------------------------------------------------------------------ conversion
namespace {

struct SUnit {
    int64_t rel3, rel4;
    char kd;
    bool flag;   // a 4th element: the old converter dropped this word when it was zero
};
struct SPart {
    std::vector<SUnit> units;
    std::vector<std::pair<int64_t, Bytes>> fills;
    int64_t size = 0;
};
struct Ent {
    std::optional<int64_t> n, bad, align, endround, hl, w3;
    std::string kind;
    std::optional<std::vector<SUnit>> units;
    std::optional<std::array<int64_t, 3>> period;
    std::map<std::string, std::vector<SUnit>> shapes;
    std::map<std::string, int64_t> span;
    std::map<std::string, SPart> groups;
    std::optional<SPart> last, inner;
};

int64_t ival(const json::Value &v) {
    if (v.kind == json::Value::Int) return v.i;
    if (v.kind == json::Value::Float) return int64_t(v.f);
    if (v.kind == json::Value::Bool) return v.b;
    throw Stop("dcconv schema: a number expected");
}
std::vector<SUnit> parse_units(const json::Value &a) {
    std::vector<SUnit> out;
    for (const json::Value &u : a.items) {
        if (u.items.size() < 3) throw Stop("dcconv schema: a unit needs 3 fields");
        out.push_back({ival(u.items[0]), ival(u.items[1]), u.items[2].s.empty() ? 'W' : u.items[2].s[0], u.items.size() > 3});
    }
    return out;
}
Bytes unhex(const std::string &h) {
    Bytes b;
    for (size_t i = 0; i + 1 < h.size(); i += 2) b.push_back(uint8_t(std::stoul(h.substr(i, 2), nullptr, 16)));
    return b;
}
SPart parse_part(const json::Value &p) {
    SPart s;
    if (const json::Value *u = p.get("units")) s.units = parse_units(*u);
    if (const json::Value *f = p.get("fills"))
        for (const json::Value &x : f->items) s.fills.push_back({ival(x.items.at(0)), unhex(x.items.at(1).s)});
    if (const json::Value *z = p.get("size")) s.size = ival(*z);
    return s;
}
Ent parse_ent(const json::Value &e) {
    Ent t;
    auto opt = [&](const char *k, std::optional<int64_t> &dst) {
        if (const json::Value *v = e.get(k)) dst = ival(*v);
    };
    opt("n", t.n);
    opt("bad", t.bad);
    opt("align", t.align);
    opt("endround", t.endround);
    opt("hl", t.hl);
    opt("w3", t.w3);
    if (const json::Value *k = e.get("kind")) t.kind = k->s;
    if (const json::Value *u = e.get("units")) t.units = parse_units(*u);
    if (const json::Value *p = e.get("period")) t.period = std::array<int64_t, 3>{ival(p->items.at(0)), ival(p->items.at(1)), ival(p->items.at(2))};
    if (const json::Value *s = e.get("shapes"))
        for (const auto &[k, v] : s->fields) t.shapes[k] = parse_units(v);
    if (const json::Value *s = e.get("span"))
        for (const auto &[k, v] : s->fields) t.span[k] = ival(v);
    if (const json::Value *g = e.get("groups"))
        for (const auto &[k, v] : g->fields) t.groups[k] = parse_part(v);
    if (const json::Value *l = e.get("last")) t.last = parse_part(*l);
    if (const json::Value *l = e.get("inner")) t.inner = parse_part(*l);
    return t;
}

struct Plan {
    int64_t s3 = 0, e3 = 0, align = 1, endround = 1, size4 = 0, s4 = 0, end = 0;
    std::optional<uint32_t> hdr;
    std::string key, kind;
    std::optional<int64_t> span;
    std::vector<SUnit> units;
    std::vector<std::pair<int64_t, Bytes>> fills;
    Bytes bytes;
};

std::vector<SUnit> generic_units(const Mod &m, int64_t s3, int64_t e3, const std::optional<uint32_t> &hdr) {
    std::vector<SUnit> out;
    int64_t r4 = 0, o = s3;
    if (hdr) out.push_back({0, 0, 'T', false}), r4 = 8, o += 4;
    for (; o + 4 <= e3; o += 4) {
        if (m.is_slot(uint64_t(o))) {
            r4 = al(r4, 8);
            out.push_back({o - s3, r4, 'P', false});
            r4 += 8;
        } else {
            out.push_back({o - s3, r4, 'W', false});
            r4 += 4;
        }
    }
    return out;
}

// extend a learned unit list to cover n3 PS3 bytes by repeating its tail period
std::optional<std::vector<SUnit>> periodic_units(const std::vector<SUnit> &units, int64_t n3) {
    if (units.empty()) return std::nullopt;
    const int64_t have = units.back().rel3 + 4;
    std::map<int64_t, SUnit> idx;
    for (const SUnit &u : units) idx[u.rel3] = u;   // a later duplicate wins
    for (int64_t p = 4; p < have; p += 4) {
        bool ok = true;
        std::optional<int64_t> d4;
        int64_t seen = 0;
        for (const SUnit &u : units) {
            if (u.rel3 < have - 2 * p) continue;
            auto v = idx.find(u.rel3 - p);
            if (v == idx.end()) continue;
            if (v->second.kd != u.kd) { ok = false; break; }
            const int64_t dd = u.rel4 - v->second.rel4;
            if (!d4) d4 = dd;
            else if (dd != *d4) { ok = false; break; }
            seen++;
        }
        if (ok && d4 && *d4 && seen) {
            std::vector<SUnit> out = units;
            for (int64_t r = have; r < n3; r += 4) {
                auto v = idx.find(r - p);
                if (v == idx.end()) return std::nullopt;
                const SUnit u{r, v->second.rel4 + *d4, v->second.kd, false};
                out.push_back(u);
                idx[r] = u;
            }
            return out;
        }
    }
    return std::nullopt;
}

}  // namespace

struct Converter::Impl {
    std::unordered_map<std::string, Ent> S;
    std::map<int, int> opmap;
    std::vector<Bytes> prefixes;
    std::set<uint32_t> hdrs;
    std::unordered_map<std::string, std::pair<int64_t, int64_t>> hdrtab;
};

Converter::Converter(const std::vector<std::string> &schemas) : p(std::make_unique<Impl>()) {
    std::vector<std::string> pref;   // latin-1
    for (auto it = schemas.rbegin(); it != schemas.rend(); ++it) {
        json::Value s;
        if (!json::parse(*it, s)) throw Stop("dcconv: a schema is not JSON");
        if (const json::Value *k = s.get("keys"))
            for (const auto &[ks, e] : k->fields) p->S[ks] = parse_ent(e);
        if (const json::Value *o = s.get("opmap"))
            for (const auto &[k, v] : o->fields) p->opmap[std::stoi(k)] = int(ival(v));
        std::vector<std::string> np;
        if (const json::Value *pr = s.get("prefixes"))
            for (const json::Value &x : pr->items) {
                const std::string l1 = utf8_latin1(x.s);
                if (std::find(pref.begin(), pref.end(), l1) == pref.end()) np.push_back(l1);
            }
        np.insert(np.end(), pref.begin(), pref.end());
        pref = np;
        if (const json::Value *h = s.get("hdrs"))
            for (const json::Value &x : h->items) p->hdrs.insert(uint32_t(ival(x)));
        if (const json::Value *h = s.get("hdrtab"))
            for (const auto &[k, v] : h->fields) p->hdrtab[k] = {ival(v.items.at(0)), ival(v.items.at(1))};
    }
    for (const std::string &x : pref) p->prefixes.push_back(Bytes(x.begin(), x.end()));
}
Converter::~Converter() = default;

Bytes Converter::convert(const Bytes &d3, std::map<std::string, int64_t> *rep_) const {
    std::map<std::string, int64_t> own;
    std::map<std::string, int64_t> &rep = rep_ ? *rep_ : own;
    const Mod m(d3);
    if (!m.be) throw Stop("ValueError: input is not a PS3 module");
    const Impl &I = *p;
    // ---- plan
    auto decide = [&](size_t, const std::string &fk, uint32_t h) {
        for (const auto &k_ : {std::optional<std::string>(fk), tailkey(fk, 1)}) {
            if (!k_) continue;
            auto it = I.hdrtab.find(*k_);
            if (it != I.hdrtab.end()) return it->second.first > it->second.second;
        }
        return I.hdrs.count(h) != 0;
    };
    const Graph g = graph(m, decide);
    std::vector<Plan> plans;
    for (size_t i = 0; i < g.objs.size(); i++) {
        const Obj &ob = g.objs[i];
        const auto [s3, e3] = region(ob);
        const std::string &ks = g.keys[i];
        const Ent *ent = nullptr;
        if (auto it = I.S.find(ks); it != I.S.end()) ent = &it->second;
        if (!ent || ent->bad.value_or(0) > ent->n.value_or(0))
            for (int n : {2, 1}) {
                const auto t = tailkey(ks, n);
                if (!t) continue;
                auto it = I.S.find(*t);
                if (it != I.S.end() && it->second.n.value_or(0) >= it->second.bad.value_or(0)) {
                    ent = &it->second;
                    rep["general-key"]++;
                    break;
                }
            }
        const int64_t hl = ob.hdr ? 4 : 0;
        const Bytes body = slice(m.d, s3 + hl, e3);
        const auto [sa, sb] = m.slots_in(uint64_t(s3), uint64_t(e3));
        const bool has_slot = sa != sb;
        Plan pl;
        pl.s3 = s3;
        pl.e3 = e3;
        pl.hdr = ob.hdr;
        pl.key = ks;
        if (ks == "O" && !any_nonzero(m.d, s3, e3)) {   // PS3 alignment padding before the first object
            rep["lead-pad-dropped"]++;
            continue;
        }
        if (!ent) {
            rep["key-unknown"]++;
            rep["unknown-keys"] += 0;
            if (!has_slot && as_string(body)) pl.kind = "str", pl.align = 1, pl.endround = 1;
            else pl.kind = "units", pl.align = 8, pl.endround = 8, pl.units = generic_units(m, s3, e3, ob.hdr);
            plans.push_back(std::move(pl));
            continue;
        }
        if (!ent->align || !ent->endround) throw Stop("KeyError: 'align'");
        pl.align = *ent->align;
        pl.endround = *ent->endround;
        if (auto sp = ent->span.find(hexs(e3 - s3)); sp != ent->span.end()) pl.span = sp->second;
        std::string kind = ent->kind;
        if ((kind == "str" || kind == "code" || kind == "blob") && !has_slot) {
            if (kind == "str" && !as_string(body)) kind = "blob";
            if (kind == "code" && as_string(body)) kind = "str";
            pl.kind = kind;
            plans.push_back(std::move(pl));
            continue;
        }
        if (kind == "tmpl" && !ent->hl) kind = "units";
        if (kind == "tmpl") {
            const int64_t nb = e3 - s3 - *ent->hl, w3 = ent->w3.value_or(0);
            const int64_t hl4 = hl ? 8 : 0;
            auto gp = ent->groups.find(std::to_string(floordiv(nb, 4)));
            if (gp != ent->groups.end() && *ent->hl == hl) {
                if (hl) pl.units.push_back({0, 0, 'T', false});
                for (const SUnit &u : gp->second.units) pl.units.push_back({hl + u.rel3, u.kd != 'D' ? hl4 + u.rel4 : -1, u.kd, false});
                for (const auto &[q, v] : gp->second.fills) pl.fills.push_back({hl4 + q, v});
                pl.kind = "tmpl";
                pl.size4 = hl4 + gp->second.size;
                plans.push_back(std::move(pl));
                continue;
            }
            if (w3 && *ent->hl == hl && nb > 0 && pymod(nb, w3) == 0 && (floordiv(nb, w3) == 1 || ent->inner)) {
                const int64_t k = floordiv(nb, w3);
                int64_t o4 = hl4;
                if (hl) pl.units.push_back({0, 0, 'T', false});
                for (int64_t j = 0; j < k; j++) {
                    if (!(j < k - 1 ? ent->inner : ent->last)) throw Stop("KeyError: 'last'");
                    const SPart &part = j < k - 1 ? *ent->inner : *ent->last;
                    for (const SUnit &u : part.units) pl.units.push_back({hl + j * w3 + u.rel3, u.kd != 'D' ? o4 + u.rel4 : -1, u.kd, false});
                    for (const auto &[q, v] : part.fills) pl.fills.push_back({o4 + q, v});
                    o4 += part.size;
                }
                pl.kind = "tmpl";
                pl.size4 = o4;
                plans.push_back(std::move(pl));
                continue;
            }
            rep["template-size-differs"]++;
        }
        std::vector<SUnit> units = ent->units ? *ent->units : std::vector<SUnit>{};
        const int64_t n3 = e3 - s3;
        if (!ent->shapes.empty()) {
            std::string shape = hexs(n3) + ":";
            for (size_t k = sa; k < sb; k++) shape += (k > sa ? "," : "") + hexs(int64_t(m.dslots[k]) - s3);
            if (auto it = ent->shapes.find(shape); it != ent->shapes.end()) {
                units = it->second;
                rep["shape-hit"]++;
            }
        }
        auto covers = [&](const std::vector<SUnit> &us) {
            std::set<int64_t> have;
            for (const SUnit &u : us) have.insert(u.rel3);
            for (int64_t r = 0; r < n3 - 3; r += 4)
                if (!have.count(r)) return false;
            return true;
        };
        if (ent->period && !covers(units)) {
            const auto [o3, p3, p4] = *ent->period;
            std::map<int64_t, SUnit> base;
            for (const SUnit &u : units) base[u.rel3] = u;
            std::vector<SUnit> ext;
            for (const SUnit &u : units)
                if (u.rel3 < o3) ext.push_back(u);
            bool ok = true;
            for (int64_t r = o3; r < n3 - 3; r += 4) {
                auto v = base.find(o3 + pymod(r - o3, p3));
                if (v == base.end()) { ok = false; break; }
                ext.push_back({r, v->second.rel4 + floordiv(r - o3, p3) * p4, v->second.kd, false});
            }
            if (ok) {
                units = ext;
                rep["period-used"]++;
            }
        }
        if (!covers(units)) {
            auto ext = periodic_units(units, n3);
            if (!ext || !covers(*ext)) {
                rep["layout-extend-failed"]++;
                ext = generic_units(m, s3, e3, ob.hdr);
            } else
                rep["layout-extended"]++;
            units = *ext;
        }
        units.erase(std::remove_if(units.begin(), units.end(), [&](const SUnit &u) { return u.rel3 >= n3; }), units.end());
        bool bad_slot = false;
        for (const SUnit &u : units) bad_slot = bad_slot || (m.is_slot(uint64_t(s3 + u.rel3)) && u.kd != 'P');
        if (bad_slot) {
            rep["slot-not-ptr"]++;
            units = generic_units(m, s3, e3, ob.hdr);
        }
        for (const SUnit &u : units)
            if ((u.kd == 'P' || u.kd == 'Q') && pymod(u.rel4, 8) == 0) pl.align = std::max<int64_t>(pl.align, 8);
        pl.kind = "units";
        pl.units = std::move(units);
        plans.push_back(std::move(pl));
    }
    // ---- layout
    const size_t n = m.syms.size();
    int64_t prev_end = 0x18 + 16 * int64_t(n);
    std::unordered_map<int64_t, int64_t> M;
    const char *em = std::getenv("DCCONV_END");
    const std::string mode = em ? em : "span";
    for (Plan &pl : plans) {
        const int64_t hd = pl.hdr ? 8 : 0, hl3 = pl.hdr ? 4 : 0, hl4 = pl.hdr ? 8 : 0;
        const int64_t s4 = al(prev_end + hd, pl.align) - hd;
        pl.s4 = s4;
        const Bytes body = slice(m.d, pl.s3 + hl3, pl.e3);
        int64_t end;
        if (pl.kind == "str") {
            Bytes s = body;
            for (const Bytes &pf : I.prefixes)
                if (s.size() >= pf.size() && std::equal(pf.begin(), pf.end(), s.begin())) {
                    s.erase(s.begin(), s.begin() + std::ptrdiff_t(pf.size()));
                    break;
                }
            pl.bytes = s;   // PS3 zero padding is kept (old converter)
            end = s4 + hl4 + int64_t(s.size());
        } else if (pl.kind == "code") {
            Bytes c = body;
            for (size_t k = 0; k + 3 < c.size(); k += 4)
                if (auto it = I.opmap.find(c[k]); it != I.opmap.end()) c[k] = uint8_t(it->second);
            pl.bytes = c;
            end = s4 + hl4 + int64_t(c.size());
        } else if (pl.kind == "blob") {
            pl.bytes = body;
            end = s4 + hl4 + int64_t(body.size());
        } else if (pl.kind == "tmpl")
            end = s4 + pl.size4;
        else {
            std::optional<int64_t> mx;
            for (const SUnit &u : pl.units) {
                if (u.flag && slice(m.d, pl.s3 + u.rel3, pl.s3 + u.rel3 + 4) == Bytes{0, 0, 0, 0}) continue;
                const int64_t v = u.rel4 + ((u.kd == 'P' || u.kd == 'T' || u.kd == 'Q') ? 8 : 4);
                mx = mx ? std::max(*mx, v) : v;
            }
            end = s4 + (mx ? *mx : hl4);
        }
        M[pl.s3 + hl3] = s4 + hl4;
        pl.end = end;
        prev_end = mode == "none" ? end : al(end, pl.endround);
        if (mode == "span" && pl.kind != "str" && pl.span && s4 + *pl.span > prev_end) prev_end = s4 + *pl.span;
    }
    const int64_t reloc = al(prev_end, 8);
    Bytes out(size_t(reloc), 0);
    auto need = [&](int64_t o, int64_t len) {
        if (o < 0 || o + len > int64_t(out.size())) throw Stop("struct.error: pack_into out of range");
    };
    auto w32 = [&](int64_t o, uint32_t v) { need(o, 4); for (int k = 0; k < 4; k++) out[size_t(o + k)] = uint8_t(v >> (8 * k)); };
    auto w64 = [&](int64_t o, uint64_t v) { need(o, 8); for (int k = 0; k < 8; k++) out[size_t(o + k)] = uint8_t(v >> (8 * k)); };
    auto put = [&](int64_t o, const Bytes &b, int64_t len) {   // out[o:o+len] = b (Python slice assignment)
        if (o >= 0 && o + len <= int64_t(out.size()) && int64_t(b.size()) == len) {
            std::copy(b.begin(), b.end(), out.begin() + o);
            return;
        }
        const int64_t a = std::min<int64_t>(o, int64_t(out.size())), e = std::max(a, std::min<int64_t>(o + len, int64_t(out.size())));
        Bytes nb(out.begin(), out.begin() + a);
        nb.insert(nb.end(), b.begin(), b.end());
        nb.insert(nb.end(), out.begin() + e, out.end());
        out.swap(nb);
    };
    if (out.size() < 0x18) throw Stop("struct.error: pack_into out of range");
    out[0] = '0', out[1] = '0', out[2] = 'C', out[3] = 'D';
    w32(4, 1);
    w64(8, uint64_t(reloc));
    w32(0x10, m.u32(0x10));
    w32(0x14, uint32_t(n));
    std::vector<int64_t> slots4;
    for (size_t i = 0; i < n; i++) {
        const int64_t o = 0x18 + 16 * int64_t(i);
        auto it = M.find(int64_t(m.syms[i].v));
        if (it == M.end()) rep["sym-target-missing"]++;
        w32(o, m.syms[i].s);
        w32(o + 4, m.syms[i].t);
        w64(o + 8, uint64_t(it == M.end() ? 0 : it->second));
        slots4.push_back(o + 8);
    }
    for (const Plan &pl : plans) {
        const int64_t s3 = pl.s3, s4 = pl.s4;
        if (pl.hdr && pl.kind != "units") {
            w32(s4, 0);
            w32(s4 + 4, *pl.hdr);
        }
        if (pl.kind == "tmpl")
            for (const auto &[q, v] : pl.fills) put(s4 + q, v, 4);
        if (pl.kind != "units" && pl.kind != "tmpl") {
            put(s4 + (pl.hdr ? 8 : 0), pl.bytes, int64_t(pl.bytes.size()));
            continue;
        }
        for (const SUnit &u : pl.units) {
            const int64_t o3 = s3 + u.rel3, o4 = s4 + u.rel4;
            const Bytes w = slice(m.d, o3, o3 + 4);
            if (u.flag && w == Bytes{0, 0, 0, 0}) continue;   // a zero word the old converter dropped
            if (u.kd == 'D') continue;
            if (w.size() < 4 && u.kd != 'Q' && u.kd != 'q') throw Stop("struct.error: unpack requires a buffer of 4 bytes");
            const uint32_t be = w.size() == 4 ? (uint32_t(w[0]) << 24 | uint32_t(w[1]) << 16 | uint32_t(w[2]) << 8 | w[3]) : 0;
            if (u.kd == 'T') {
                w32(o4, 0);
                w32(o4 + 4, be);
            } else if (u.kd == 'P') {
                if (m.is_slot(uint64_t(o3))) {
                    auto it = M.find(int64_t(m.ptr(uint64_t(o3))));
                    if (it == M.end()) rep["ptr-target-missing"]++;
                    w64(o4, uint64_t(it == M.end() ? 0 : it->second));
                    slots4.push_back(o4);
                } else
                    w64(o4, be);
            } else if (u.kd == 'Q') {
                Bytes r = slice(m.d, o3, o3 + 8);
                std::reverse(r.begin(), r.end());
                put(o4, r, 8);
            } else if (u.kd == 'q') {
            } else {
                const uint8_t kb = u.kd == 'H' ? KH : u.kd == 'R' ? KR : KW;
                const W4 t = tr(kb, {w[0], w[1], w[2], w[3]});
                put(o4, Bytes(t.begin(), t.end()), 4);
            }
        }
    }
    const int64_t nbm = (reloc + 63) / 64;
    Bytes bm(size_t(nbm), 0);
    for (int64_t q : slots4) {
        const int64_t wd = q / 8;
        bm.at(size_t(wd / 8)) |= uint8_t(1 << (wd % 8));
    }
    for (int k = 0; k < 4; k++) out.push_back(uint8_t(uint32_t(nbm) >> (8 * k)));
    out.insert(out.end(), bm.begin(), bm.end());
    return out;
}

}  // namespace u3::dcconv
