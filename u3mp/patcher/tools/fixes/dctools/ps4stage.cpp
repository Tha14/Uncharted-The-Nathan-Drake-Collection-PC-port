// dctools/ps4stage.cpp: see ps4stage.hpp.
#include "ps4stage.hpp"

#include "collision.hpp"
#include "proc.hpp"
#include "ps3fp.hpp"
#include "ps4writer.hpp"
#include "texcodec.hpp"
#include "u3fix.hpp"
#include "visinfo.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <regex>
#include <set>
#include <unordered_map>

#if defined(__clang__)
#pragma clang fp contract(off)   // float results must match numpy's
#endif

namespace u3::stage {
namespace {

const std::string PS4_PREFIX = "d:/dev/CP9/Data/U3/CookData/ND/build/texture2/";
const size_t PAGE_LIMIT = 0x1c0000;
const uint32_t PAD32 = 0xaaaaaaaa;
const uint32_t MAX_BLOCKS = 20;   // PS4 splits a PS3 material group every 20 blocks

// PS3 attribute id -> (PS4 slot, components, PS4 type); type 6 f32x3, 0x22 f16x2, 0x0a f16x4
struct AttrMap {
    int slot, nc, ty;
};
const std::vector<std::pair<int, AttrMap>> ATTR = {{1, {0, 3, 6}},     {0x11, {1, 2, 0x22}}, {0x12, {12, 2, 0x22}},
                                                   {2, {2, 4, 0x0a}},  {3, {3, 4, 0x0a}},    {0x15, {4, 4, 0x0a}},
                                                   {0x4c, {8, 4, 0x0a}}, {0x13, {5, 2, 0x22}}, {0x4e, {255, 3, 6}}};
const AttrMap *attr_map(int aid) {
    for (const auto &[a, m] : ATTR)
        if (a == aid) return &m;
    return nullptr;
}
bool inst_attr(int aid) { return aid == 0x4c || aid == 0x13 || aid == 0x4e; }
size_t tsize(int ty) { return ty == 6 ? 12 : ty == 0x22 ? 4 : 8; }

bool starts_with(const std::string &s, const std::string &e) { return s.compare(0, e.size(), e) == 0; }
bool ends_with(const std::string &s, const std::string &e) { return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; }
bool has(const std::string &s, const char *x) { return s.find(x) != std::string::npos; }
std::string after_prefix(const std::string &s) {   // s.split(PS4_PREFIX, 1)[-1]
    const size_t k = s.find(PS4_PREFIX);
    return k == std::string::npos ? s : s.substr(k + PS4_PREFIX.size());
}
uint32_t sidu(const std::string &latin1) { return sid(bg::utf8_of(latin1)); }

// a float32 as Python gives it back after struct.unpack + struct.pack (a signalling NaN comes out quiet)
uint32_t quiet(uint32_t bits) { return ((bits & 0x7f800000u) == 0x7f800000u && (bits & 0x7fffffu)) ? bits | 0x400000u : bits; }
uint32_t f32b(double d) {
    const float f = float(d);
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return quiet(v);   // a NaN from a double is quiet in Python too
}
float as_float(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
double py_round5(double x) {   // Python round(x, 5): the decimal rounding of the exact value
    if (!std::isfinite(x)) return x;
    char b[512];
    std::snprintf(b, sizeof b, "%.5f", x);
    return std::strtod(b, nullptr);
}
// struct.pack of a value array with its first float replaced (the others come back as Python gives them)
void set_first_float(Bytes &vals, double v) {
    if (vals.size() < 4) throw Stop("IndexError: list assignment index out of range");
    Bytes nv;
    for (size_t j = 0; j + 4 <= vals.size(); j += 4) {
        const uint32_t x = j == 0 ? f32b(v) : quiet(rd32le(vals, j));
        for (int k = 0; k < 4; k++) nv.push_back(uint8_t(x >> (8 * k)));
    }
    vals = nv;
}
// numpy float64 -> float16 (round half to even, overflow to inf, NaN payload kept)
uint16_t f16b(double v) {
    uint64_t d;
    std::memcpy(&d, &v, 8);
    const uint16_t h_sgn = uint16_t((d & 0x8000000000000000ull) >> 48);
    uint64_t d_exp = d & 0x7ff0000000000000ull, d_sig;
    if (d_exp >= 0x40f0000000000000ull) {
        if (d_exp == 0x7ff0000000000000ull) {
            d_sig = d & 0x000fffffffffffffull;
            if (d_sig) {
                uint16_t ret = uint16_t(0x7c00u + (d_sig >> 42));
                if (ret == 0x7c00u) ret++;
                return uint16_t(h_sgn + ret);
            }
        }
        return uint16_t(h_sgn + 0x7c00u);
    }
    if (d_exp <= 0x3f00000000000000ull) {
        if (d_exp < 0x3e60000000000000ull) return h_sgn;
        d_exp >>= 52;
        d_sig = 0x0010000000000000ull + (d & 0x000fffffffffffffull);
        d_sig <<= (d_exp - 998);
        if ((d_sig & 0x003fffffffffffffull) != 0x0010000000000000ull) d_sig += 0x0010000000000000ull;
        return uint16_t(h_sgn + uint16_t(d_sig >> 53));
    }
    const uint16_t h_exp = uint16_t((d_exp - 0x3f00000000000000ull) >> 42);
    d_sig = d & 0x000fffffffffffffull;
    if ((d_sig & 0x000007ffffffffffull) != 0x0000020000000000ull) d_sig += 0x0000020000000000ull;
    const uint16_t h_sig = uint16_t(d_sig >> 42);
    return uint16_t(h_sgn + h_exp + h_sig);
}

void put16(Bytes &o, uint32_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); }
void put32(Bytes &o, uint32_t v) { for (int k = 0; k < 4; k++) o.push_back(uint8_t(v >> (8 * k))); }
void put64(Bytes &o, uint64_t v) { for (int k = 0; k < 8; k++) o.push_back(uint8_t(v >> (8 * k))); }
void set32(Bytes &o, size_t at, uint32_t v) { for (int k = 0; k < 4; k++) o.at(at + k) = uint8_t(v >> (8 * k)); }
void set64(Bytes &o, size_t at, uint64_t v) { for (int k = 0; k < 8; k++) o.at(at + k) = uint8_t(v >> (8 * k)); }

Bytes stream_bytes(int ty, const Mat &a) {
    Bytes o;
    const size_t nc = ty == 6 ? 3 : ty == 0x22 ? 2 : 4;
    o.reserve(a.rows * nc * (ty == 6 ? 4 : 2));
    for (size_t i = 0; i < a.rows; i++)
        for (size_t c = 0; c < nc; c++) {
            if (c >= a.cols) throw Stop("IndexError: stream with too few components");
            if (ty == 6) put32(o, f32b(a.at(i, c)));
            else put16(o, f16b(a.at(i, c)));
        }
    return o;
}

Mat concat(const std::vector<Mat> &parts, size_t cols) {
    size_t rows = 0;
    for (const Mat &m : parts) {
        rows += m.rows;
        if (m.cols != cols) throw Stop("ValueError: all the input array dimensions must match");
    }
    Mat o(rows, cols);
    size_t r = 0;
    for (const Mat &m : parts) {
        std::copy(m.v.begin(), m.v.end(), o.v.begin() + std::ptrdiff_t(r * cols));
        r += m.rows;
    }
    return o;
}

// PS3 decoded attribute -> PS4 stream array (components per PS4 type)
Mat to_ps4(int aid, const Mat &val, size_t nv) {
    const AttrMap &m = *attr_map(aid);
    Mat v(nv, size_t(m.nc));
    const size_t k = std::min<size_t>(size_t(m.nc), val.cols);
    if (val.rows != nv) throw Stop("ValueError: attribute rows");
    for (size_t i = 0; i < nv; i++)
        for (size_t c = 0; c < k; c++) v.at(i, c) = val.at(i, c);
    if ((aid == 2 || aid == 0x15) && val.cols < 4)
        for (size_t i = 0; i < nv; i++) v.at(i, 3) = 1.0;
    if (aid == 0x15 && val.cols < 3)
        for (size_t i = 0; i < nv; i++)
            for (size_t c = val.cols; c < 3; c++) v.at(i, c) = 0.0;
    if (aid == 3 && val.cols < 4)
        for (size_t i = 0; i < nv; i++) v.at(i, 3) = 1.0;
    if (aid == 0x4e)
        for (double &x : v.v) x = std::nearbyint(x * 16.0) / 16.0;   // PS4 keeps 1/16 steps
    return v;
}

Mat attr_values(const Pak &p, uint32_t fpg, uint64_t fmt, const bg::Attr &a, const ORef &data, uint32_t nv, const std::map<int, int> &prec) {
    auto pi = prec.find(a.id);
    Mat val = bg::decode_attr(p, fpg, fmt, a, data ? data->first : 0, data ? data->second : 0, nv, pi == prec.end() ? -1 : pi->second);
    if ((a.id == 0x15 || a.id == 0x4e) && (a.fmt & 0xf0) == 0)
        for (double &x : val.v) x = (x + 1.0) / 2.0;   // colour / instance constant: unorm
    return val;
}

struct Block {
    uint32_t nv = 0;
    std::vector<std::pair<int, Mat>> attrs;   // in format order
    std::map<int, Mat> base_inst;
    std::vector<std::map<int, Mat>> inst;      // per instance
    std::vector<bool> inst_lm;                 // the instance carries its own lightmap uvs
    std::vector<int> order;
    std::vector<std::array<uint32_t, 3>> tris;
    bool has_skin = false;
    std::vector<bg::Influence> skin;
    const Mat *attr(int aid) const {
        for (const auto &[a, m] : attrs)
            if (a == aid) return &m;   // dict(b['attrs']): the last of an id wins
        return nullptr;
    }
};
const Mat *last_attr(const Block &b, int aid) {
    const Mat *r = nullptr;
    for (const auto &[a, m] : b.attrs)
        if (a == aid) r = &m;
    return r;
}

Block decode_block3(const Pak &p, const Ref &blk, const std::vector<std::pair<ORef, ORef>> &insts, bool sky) {
    const auto [pg, bo] = blk;
    Block out;
    const Ref cnt = bg::need(p, pg, bo + 0x24);
    out.nv = p.pu32(cnt.first, cnt.second);
    const Ref f = bg::need(p, pg, bo + 0x28), s = bg::need(p, pg, bo);
    const std::vector<bg::Attr> attrs = bg::read_format(p, f.first, f.second);
    const std::map<int, int> prec = bg::block_prec(attrs);
    for (const bg::Attr &a : attrs) out.order.push_back(a.id);
    for (size_t ai = 0; ai < attrs.size(); ai++) {
        const bg::Attr &a = attrs[ai];
        if (!attr_map(a.id)) continue;
        const ORef r = bg::res(p, s.first, s.second + 8 * ai);
        Mat v = to_ps4(a.id, attr_values(p, f.first, f.second, a, r, out.nv, prec), out.nv);
        if (inst_attr(a.id)) out.base_inst[a.id] = std::move(v);
        else out.attrs.push_back({a.id, std::move(v)});
    }
    const Ref ii = bg::need(p, pg, bo + 4);
    const uint64_t ib = p.pbase(ii.first) + ii.second;
    const uint32_t ntris = bg::be16(p, ib), isz = bg::be16(p, ib + 4);
    bg::be16(p, ib + 6);
    const Ref idat = bg::need(p, ii.first, ii.second + 8);
    const uint64_t bi = p.pbase(idat.first) + idat.second;
    const uint64_t s0 = std::min<uint64_t>(bi, p.d.size()), e0 = std::min<uint64_t>(bi + isz, p.d.size());
    out.tris = bg::decode_indices(p.d.data() + s0, e0 > s0 ? e0 - s0 : 0, ntris);
    if (const ORef sk = bg::res(p, pg, bo + 0xc)) {
        try {
            out.skin = bg::decode_skin(p, sk->first, sk->second, out.nv);
            out.has_skin = true;
        } catch (const std::exception &) {
        }
    }
    for (const auto &[stbl, ifmt] : insts) {
        std::map<int, Mat> d;
        bool lm = false;
        if (ifmt) {
            const std::vector<bg::Attr> iattrs = bg::read_format(p, ifmt->first, ifmt->second);
            const std::map<int, int> iprec = bg::block_prec(iattrs);
            uint32_t k = 0;
            for (const bg::Attr &a : iattrs) {
                if (!inst_attr(a.id)) continue;
                const ORef r = stbl ? bg::res(p, stbl->first, stbl->second + 4 * k) : std::nullopt;
                k++;
                d[a.id] = to_ps4(a.id, attr_values(p, ifmt->first, ifmt->second, a, r, out.nv, iprec), out.nv);
                if (a.id == 0x13 && r) {
                    // the sky converter: a vertex-lit PS3 block still carries a placeholder lightmap-uv stream (every value
                    // an integer: atlas corners); only real lightmap uvs flag the instance
                    bool integ = false;
                    if (sky) {
                        const Mat &v13 = d[a.id];
                        integ = !v13.v.empty();
                        for (double x : v13.v) integ = integ && std::fabs(x - std::nearbyint(x)) < 1e-4;
                    }
                    if (!integ) lm = true;
                }
            }
        }
        out.inst.push_back(std::move(d));
        out.inst_lm.push_back(lm);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ fxremap
// PS4 prime-shader EFFECT: the string-table index fields (techniques, inputs, samplers, textures, cbuffer variables)
// header +0 ptr passes, +8 u64 npasses; pass 0xd0: +0 u16 x3 names, +0x10 inputs (16 B, u32 name), +0x18 ninputs,
// +0x68 nVSCbuf, +0x80 VS cbuffers, +0x98 nSamplers, +0x9c nTextures, +0xa0 nPSCbuf, +0xa8 samplers (0x3c, u32 name),
// +0xb0 textures (8 B, u16 texture name, u16 sampler name), +0xb8 PS cbuffers; cbuffer 0x30 {vars*, u32 nvars};
// cbvar 0x20 {u16 x3 names}
Bytes remap(const Bytes &data, const std::map<uint64_t, std::optional<int64_t>> &ptrs, const std::map<uint32_t, uint32_t> &mapping) {
    auto P = [&](uint64_t o) -> std::optional<int64_t> {
        auto it = ptrs.find(o);
        return it == ptrs.end() ? std::nullopt : it->second;
    };
    auto u32 = [&](int64_t o) { if (o < 0) throw Stop("struct.error: negative offset"); return rd32le(data, uint64_t(o)); };
    std::set<std::pair<int64_t, int>> out;
    const auto passes = P(0);
    const uint32_t npass = u32(8);
    if (!passes || npass == 0 || npass > 64) throw Stop("FxLayoutError: header");
    std::set<int64_t> seen_cb, seen;
    auto cbufs = [&](std::optional<int64_t> lst, uint32_t n) {
        if (n == 0) return;
        if (!lst) throw Stop("FxLayoutError: cbuffer list");
        for (uint32_t k = 0; k < n; k++) {
            const int64_t r = *lst + 0x30 * int64_t(k);
            if (seen_cb.count(r)) continue;
            seen_cb.insert(r);
            const auto vars = P(uint64_t(r));
            const uint32_t nv = u32(r + 8);
            if (nv && !vars) throw Stop("FxLayoutError: cbvars");
            for (uint32_t j = 0; j < nv; j++) {
                const int64_t v = *vars + 0x20 * int64_t(j);
                out.insert({v, 2});
                out.insert({v + 2, 2});
                out.insert({v + 4, 2});
            }
        }
    };
    for (uint32_t i = 0; i < npass; i++) {
        const int64_t p = *passes + 0xd0 * int64_t(i);
        out.insert({p, 2});
        out.insert({p + 2, 2});
        out.insert({p + 4, 2});
        const auto inp = P(uint64_t(p + 0x10));
        const uint32_t nin = u32(p + 0x18);
        if (nin && !inp) throw Stop("FxLayoutError: inputs");
        if (nin && !seen.count(*inp)) {
            seen.insert(*inp);
            for (uint32_t k = 0; k < nin; k++) out.insert({*inp + 16 * int64_t(k), 4});
        }
        cbufs(P(uint64_t(p + 0x80)), u32(p + 0x68));
        const uint32_t ns = u32(p + 0x98), nt = u32(p + 0x9c), npc = u32(p + 0xa0);
        const auto sm = P(uint64_t(p + 0xa8)), tx = P(uint64_t(p + 0xb0));
        if ((ns && !sm) || (nt && !tx)) throw Stop("FxLayoutError: samplers/textures");
        if (ns && !seen.count(*sm)) {
            seen.insert(*sm);
            for (uint32_t k = 0; k < ns; k++) out.insert({*sm + 0x3c * int64_t(k), 4});
        }
        if (nt && !seen.count(*tx)) {
            seen.insert(*tx);
            for (uint32_t k = 0; k < nt; k++) {
                out.insert({*tx + 8 * int64_t(k), 2});
                out.insert({*tx + 8 * int64_t(k) + 2, 2});
            }
        }
        cbufs(P(uint64_t(p + 0xb8)), npc);
    }
    Bytes b = data;
    for (const auto &[o, sz] : out) {
        if (o < 0 || uint64_t(o) + sz > b.size()) throw Stop("struct.error: index field past the end");
        const uint32_t v = sz == 2 ? uint32_t(b[o] | b[o + 1] << 8) : rd32le(b, uint64_t(o));
        auto m = mapping.find(v);
        if (m == mapping.end()) throw Stop(u3::fmt("FxLayoutError: index %u at %#llx outside the source string table", v, (long long)o));
        if (sz == 2) {
            if (m->second > 0xffff) throw Stop("struct.error: 'H' format requires 0 <= number <= 65535");
            b[o] = uint8_t(m->second);
            b[o + 1] = uint8_t(m->second >> 8);
        } else
            set32(b, size_t(o), m->second);
    }
    return b;
}

// ------------------------------------------------------------------------------------------------ resources
struct RTex {
    std::string path, shrt;
    Bytes desc, texels;
    std::optional<Bytes> gtex;
};
struct Resources {
    const Bundle *bundle;
    std::map<std::pair<std::string, std::string>, const MatRec *> mats;
    std::vector<RTex> tex;   // in insertion order
    std::map<std::string, size_t> tex_at;
    std::map<std::string, Bytes> gtex;
    std::map<std::string, std::vector<std::string>> dict;   // TextureDictionaries.csv: short -> row

    void add_tex(RTex t, bool replace) {
        auto it = tex_at.find(t.path);
        if (it != tex_at.end()) {
            if (replace) tex[it->second] = std::move(t);
            return;
        }
        t.shrt = short_of(t.path);
        tex_at[t.path] = tex.size();
        tex.push_back(std::move(t));
    }
    const RTex *get(const std::string &path) const {
        auto it = tex_at.find(path);
        return it == tex_at.end() ? nullptr : &tex[it->second];
    }
    const RTex *by_short(const std::string &s) const {
        for (const RTex &t : tex)
            if (t.shrt == s) return &t;
        return nullptr;
    }
};

std::vector<std::string> csv_row(const std::string &line) {   // csv.reader (no quotes in the PS4 dictionary)
    std::vector<std::string> f;
    size_t a = 0;
    for (size_t i = 0;; i++)
        if (i == line.size() || line[i] == ',') {
            f.push_back(line.substr(a, i - a));
            if (i == line.size()) break;
            a = i + 1;
        }
    return f;
}

std::string strip(const std::string &s) {
    size_t a = 0, e = s.size();
    while (a < e && py_space((unsigned char)s[a])) a++;
    while (e > a && py_space((unsigned char)s[e - 1])) e--;
    return s.substr(a, e - a);
}

std::mutex g_dict_lock;
std::map<std::string, std::shared_ptr<std::map<std::string, std::vector<std::string>>>> g_dicts;

std::shared_ptr<std::map<std::string, std::vector<std::string>>> load_dict(const fs::path &dp) {
    std::lock_guard<std::mutex> lock(g_dict_lock);
    auto &slot = g_dicts[utf8(dp)];
    if (slot) return slot;
    slot = std::make_shared<std::map<std::string, std::vector<std::string>>>();
    if (!dp.empty() && fs::exists(dp)) {
        const Bytes d = read_file(dp);
        const std::string t(d.begin(), d.end());
        size_t a = 0;
        while (a < t.size()) {
            size_t e = t.find('\n', a);
            if (e == std::string::npos) e = t.size();
            std::string line = t.substr(a, e - a);
            a = e + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::vector<std::string> row = csv_row(line);
            if (row.size() < 11 || row[0] == "dictionary") continue;
            for (std::string &x : row) x = strip(x);
            (*slot)[row[2]] = row;
        }
    }
    return slot;
}

// ------------------------------------------------------------------------------------------------ writer helpers
struct AutoPages {
    PakWriter &w;
    Page *cur;
    size_t size = 0x200;
    explicit AutoPages(PakWriter &w_) : w(w_), cur(&w_.page()) {}
    void room(size_t n) {
        if (size + n > PAGE_LIMIT && size > 0x200) {
            cur = &w.page();
            size = 0x200;
        }
        size += n;
    }
    std::shared_ptr<PakWItem> item(const std::string &type, const std::shared_ptr<Blob> &data, const std::string &name = "",
                                   std::optional<std::pair<size_t, size_t>> phase = std::nullopt) {
        size_t n = (data ? data->size() : 0) + 0x60 + type.size() + name.size();
        if (phase) n += phase->first;
        room((n + 15) & ~size_t(15));
        auto it = cur->item(type, data, name);
        if (phase) it->phase = phase;
        return it;
    }
};

std::shared_ptr<Blob> blob(const char *name) { return std::make_shared<Blob>(name); }

struct TexE {
    std::string full, path;
    uint32_t hash = 0, fmt = 0;
    std::shared_ptr<PakWItem> desc, name;
    std::optional<Bytes> gtex;
};
using TexP = std::shared_ptr<TexE>;

struct Uni {   // uniform-stream sharing between the two passes
    bool collecting = false;
    std::map<std::pair<int, Bytes>, size_t> collect, plan;
    std::map<std::pair<int, Bytes>, std::shared_ptr<Blob>> bufs;
    std::set<std::pair<int, Bytes>> seen;
};

struct BytesHash {
    size_t operator()(const Bytes &b) const {
        uint64_t h = 1469598103934665603ull;
        for (uint8_t c : b) h = (h ^ c) * 1099511628211ull;
        return size_t(h);
    }
};

// ------------------------------------------------------------------------------------------------ the converter
struct Submesh {
    std::string name, mat, ps3mat;
    size_t nv = 0, ni = 0, ns = 0, ns2 = 0;
    std::shared_ptr<Blob> st, st2, ib;
    uint32_t flags = 0, njoints = 0;
    bool skin = false;
    uint64_t skin_tot = 0;
    std::shared_ptr<Blob> sk_c, sk_o, sk_j, sk_w;
    std::vector<uint32_t> chunk;
    std::vector<int> iorder;
    bool has_infl = false;
    std::vector<uint16_t> counts, joints;
    std::vector<float> wts;
    Mat pos;
};
struct ProtoW {
    std::string name;
    std::vector<size_t> subs;
    uint32_t hw0 = 0, hw14 = 0, cb3 = 0;
    bool skinned = false;
    std::optional<std::pair<uint32_t, uint32_t>> fade;
    size_t off = 0;
};
struct InstSub {
    size_t sub;
    std::shared_ptr<Blob> st;
    size_t n = 0;
    double mn[3] = {}, mx[3] = {};
    uint32_t flags = 0, flags2 = 0;
};
struct InstW {
    std::string proto;
    uint32_t id = 0, x8 = 0, nj = 0;
    std::vector<InstSub> subs;
    double M[16] = {};
    double mn[3] = {}, mx[3] = {};
    std::shared_ptr<Blob> jm;
    size_t off = 0;
};
struct MatW {
    std::string name, pak;
    MatRec rec;
    struct B {
        std::string param, shrt, full;
        TexP e;
    };
    std::vector<B> binds;
};

// the sky converter's extra texture roles for names tex_role_final calls 'color': '-height', '-blend<n>', '-inc'
std::optional<std::string> xrole(const std::string &shrt) {
    std::string b = shrt;
    for (char &c : b) c = char(std::tolower((unsigned char)c));
    b = b.substr(b.rfind('/') == std::string::npos ? 0 : b.rfind('/') + 1);
    static const std::regex COL(R"(-(color|diffuse|col)\.[a-z]+$)"), HEIGHT(R"(-height(?=[-._]))"), BLENDN(R"(-blend\d+(?=[-._]))"),
        INC(R"(-(inc|incan|incand)(?=[-._]))");
    if (std::regex_search(b, COL)) return std::nullopt;
    if (std::regex_search(b, HEIGHT)) return std::string("height");
    if (std::regex_search(b, BLENDN)) return std::string("blend");
    if (std::regex_search(b, INC)) return std::string("inc");
    return std::nullopt;
}

// invert a BC1 texel blob value-wise (v -> 1 - v), block by block: 4-colour blocks exactly (endpoints NOT and swapped,
// indices XOR 1), 3-colour blocks re-fitted as a 4-colour block (transparent black has no inverse: white)
Bytes bc1_invert(const Bytes &blob) {
    Bytes out = blob;
    auto d565 = [](uint32_t c, int p[3]) {
        p[0] = int((c >> 11) & 31) * 255 / 31;
        p[1] = int((c >> 5) & 63) * 255 / 63;
        p[2] = int(c & 31) * 255 / 31;
    };
    auto e565 = [](const int c[3]) { return uint32_t(((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 63 + 127) / 255) << 5 | ((c[2] * 31 + 127) / 255)); };
    for (size_t o = 0; o + 8 <= blob.size(); o += 8) {
        const uint32_t c0 = blob[o] | blob[o + 1] << 8, c1 = blob[o + 2] | blob[o + 3] << 8, ix = rd32le(blob, o + 4);
        uint32_t n0, n1, ni;
        if (c0 > c1) {
            n0 = c1 ^ 0xffff;
            n1 = c0 ^ 0xffff;
            ni = ix ^ 0x55555555u;
        } else {
            int pal[4][3];
            d565(c0, pal[0]);
            d565(c1, pal[1]);
            for (int j = 0; j < 3; j++) {
                pal[2][j] = (pal[0][j] + pal[1][j]) / 2;
                pal[3][j] = 0;
            }
            int px[16][3], lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
            for (int k = 0; k < 16; k++)
                for (int j = 0; j < 3; j++) {
                    px[k][j] = 255 - pal[(ix >> (2 * k)) & 3][j];
                    lo[j] = std::min(lo[j], px[k][j]);
                    hi[j] = std::max(hi[j], px[k][j]);
                }
            n0 = e565(hi);
            n1 = e565(lo);
            ni = 0;
            if (n0 != n1) {
                if (n0 < n1) std::swap(n0, n1);
                int q[4][3];
                d565(n0, q[0]);
                d565(n1, q[1]);
                for (int j = 0; j < 3; j++) {
                    q[2][j] = (2 * q[0][j] + q[1][j]) / 3;
                    q[3][j] = (q[0][j] + 2 * q[1][j]) / 3;
                }
                for (int k = 0; k < 16; k++) {
                    int best = 0, bd = 0;
                    for (int i = 0; i < 4; i++) {
                        int d = 0;
                        for (int j = 0; j < 3; j++) d += (px[k][j] - q[i][j]) * (px[k][j] - q[i][j]);
                        if (i == 0 || d < bd) { bd = d; best = i; }
                    }
                    ni |= uint32_t(best) << (2 * k);
                }
            }
        }
        out[o] = uint8_t(n0);
        out[o + 1] = uint8_t(n0 >> 8);
        out[o + 2] = uint8_t(n1);
        out[o + 3] = uint8_t(n1 >> 8);
        set32(out, o + 4, ni);
    }
    return out;
}

std::shared_ptr<Blob> convert_light_table(const Pak &p, AutoPages &P, const std::function<std::optional<std::vector<std::string>>(const std::vector<uint32_t> &)> &gobo,
                                          std::shared_ptr<PakWItem> &item);
std::shared_ptr<Blob> water_blob(const Pak &p, const std::function<std::string(const std::string &)> &matkey);

Bytes convert_once(const Pak &p, const std::string &stage, const Plan &plan, const Bundle &bundle, const Options &opt, Uni &U, bool build) {
    Resources R;
    R.bundle = &bundle;
    // the records by (pak, key) in first-seen order, the last record of a key winning (a dict)
    std::vector<std::pair<std::pair<std::string, std::string>, const MatRec *>> mats_order;
    for (const auto &[k, r] : bundle.mats) {
        R.mats[k] = &r;
        auto it = std::find_if(mats_order.begin(), mats_order.end(), [&](const auto &x) { return x.first == k; });
        if (it == mats_order.end()) mats_order.push_back({k, &r});
        else it->second = &r;
    }
    for (const BundleTex &t : bundle.tex) R.add_tex({t.path, "", t.desc, t.texels, t.gtex}, true);
    // material lightmap slots of unlit props fall back to the retail default-lightmap0/1: always carry the two copies
    if (opt.default_lightmaps)
        for (const BundleTex &t : *opt.default_lightmaps) R.add_tex({t.path, "", t.desc, t.texels, std::nullopt}, false);
    for (const auto &[k, r] : mats_order)
        for (const auto &[full, e] : r->textures)
            if (e.has_desc) R.add_tex({e.path, "", e.desc, e.texels, e.gtex}, false);
    for (const auto &[k, r] : mats_order)
        for (const auto &[full, e] : r->textures)
            if (e.gtex && !e.gtex->empty()) R.gtex[full] = *e.gtex;
    if (opt.sky && !plan.mats.empty()) {
        // the sky converter: the GEOMETRY_1 texture entry of a shared retail texture comes from the donors this level's
        // plan uses (first in plan order), then any donor except the engine default textures (those fall back to the
        // neutral entry unless a used donor binds them)
        std::vector<std::pair<std::string, std::string>> used;
        for (const auto &[n, e] : plan.mats) {
            const std::pair<std::string, std::string> k{e.pak, e.key};
            if (R.mats.count(k) && std::find(used.begin(), used.end(), k) == used.end()) used.push_back(k);
        }
        std::map<std::string, Bytes> pinned;
        for (const auto &k : used)
            for (const auto &[full, e] : R.mats.at(k)->textures)
                if (e.gtex && !e.gtex->empty() && !pinned.count(full)) pinned[full] = *e.gtex;
        for (const auto &[full, g] : R.gtex)
            if (!pinned.count(full) && full.find("/textures/default/") == std::string::npos) pinned[full] = g;
        R.gtex = pinned;
    }
    const auto dict = load_dict(opt.texdict);

    const std::vector<bg::Proto> C = bg::collection(p);
    std::vector<bg::Material3> mats3;
    std::vector<bg::Texture3> texs3;
    bg::read_materials(p, mats3, texs3);
    std::map<std::string, const bg::Material3 *> m3;
    for (const bg::Material3 &m : mats3) m3[m.name] = &m;

    const std::vector<PakTexture> ps3_textures = p.textures();
    // PS3 textures by short path (the first of a short wins)
    std::vector<std::pair<std::string, PakTexture>> ps3tex;
    std::map<std::string, size_t> ps3tex_at;
    for (const PakTexture &t : ps3_textures) {
        const std::string s = short_of(t.path);
        if (!ps3tex_at.count(s)) {
            ps3tex_at[s] = ps3tex.size();
            ps3tex.push_back({s, t});
        }
    }
    auto ps3tex_get = [&](const std::string &s) -> const PakTexture * {
        auto it = ps3tex_at.find(s);
        return it == ps3tex_at.end() ? nullptr : &ps3tex[it->second].second;
    };

    PakWriter w;
    AutoPages P(w);
    auto strs = blob("strings");
    std::map<std::string, size_t> spool;
    auto S = [&](const std::string &s) -> int64_t {
        auto it = spool.find(s);
        if (it == spool.end()) it = spool.emplace(s, strs->cstr(bg::utf8_of(s))).first;
        return int64_t(it->second);
    };
    auto raw = [&](const Bytes &data, const std::string &name = "") {
        auto b = blob("raw");
        b->raw(data);
        return P.item("RAW_DATA", b, name)->data;
    };

    // ---------------- textures
    Bytes vram;
    std::map<std::string, TexP> tex_entries;
    std::vector<TexP> tex_order;
    auto add_texture_desc = [&](const std::string &path_ndb, const Bytes &descraw, const Bytes &texels, const std::optional<Bytes> &gtex) -> TexP {
        const std::string full = ends_with(path_ndb, ".ndb") ? path_ndb.substr(0, path_ndb.size() - 4) : path_ndb;
        if (auto it = tex_entries.find(full); it != tex_entries.end()) return it->second;
        while (vram.size() % 0x100) vram.push_back(0);
        const uint64_t voff = vram.size();
        vram.insert(vram.end(), texels.begin(), texels.end());
        Bytes d(descraw.begin(), descraw.begin() + std::ptrdiff_t(std::min<size_t>(0x38, descraw.size())));
        if (d.size() < 0x14) throw Stop("struct.error: pack_into requires a buffer of at least 20 bytes");
        set64(d, 0, 0);
        set64(d, 8, voff);
        set32(d, 0x10, uint32_t(texels.size()));
        auto db = blob("vramdesc");
        db->raw(d);
        db->ptr(db, 0x40);
        db->cstr(bg::utf8_of(path_ndb));
        auto nb = blob("texname");
        nb->cstr(bg::utf8_of(after_prefix(path_ndb)));   // retail: short path incl. '/<HASH>.ndb', sid(name) == hash
        auto e = std::make_shared<TexE>();
        e->name = P.item("RAW_DATA", nb);
        e->desc = P.item("VRAM_DESC", db);
        e->full = full;
        e->path = path_ndb;
        e->hash = rd32le(d, 0x14);
        e->fmt = rd32le(d, 0x1c);
        if (gtex && !gtex->empty()) e->gtex = gtex;
        tex_entries[full] = e;
        tex_order.push_back(e);
        return e;
    };
    auto desc_bytes = [](uint32_t hsh, uint32_t type, uint32_t fmt4, uint32_t w20, uint32_t mips, uint32_t wd, uint32_t ht, uint32_t flag) {
        Bytes d;
        put64(d, 0);
        put64(d, 0);
        put32(d, 0);
        put32(d, hsh);
        put32(d, type);
        put32(d, fmt4);
        put32(d, w20);
        put32(d, mips);
        put32(d, wd);
        put32(d, ht);
        put32(d, 0);
        put32(d, flag);
        return d;
    };
    auto gtex_bytes = [](uint32_t hsh, uint32_t w4, uint32_t cls) {
        Bytes g;
        put32(g, hsh);
        put32(g, w4);
        put32(g, 0x101);
        put32(g, 0xffffffffu);
        put32(g, 0);
        put32(g, cls);
        put64(g, 0);
        return g;
    };
    auto convert_ps3_cubemap = [&](const PakTexture &t, const Bytes &texels, std::string kind, int bpb, const std::string &path_ndb, uint32_t flag) -> TexP {
        std::optional<Bytes> out;
        if (kind == "A8R8G8B8") {
            out = cube_texels_argb(t, texels, t.gcmRemapCtl & 0xffff, opt.sky);
            kind = "DXT5";
        } else
            out = cube_texels(t, texels, bpb);
        if (!out) return nullptr;
        const uint32_t fmt4 = kind == "DXT1" ? 0x48 : kind == "DXT5" ? 0x4e : 0x4b;
        const uint32_t hsh = sidu(after_prefix(path_ndb));
        const Bytes d = desc_bytes(hsh, 2, fmt4, 0x060f0000, t.mips, t.width, t.height, flag);
        return add_texture_desc(path_ndb, d, *out, gtex_bytes(hsh, 0x100, fmt4 == 0x48 ? 4 : 5));
    };
    auto convert_ps3_texture = [&](const PakTexture &t, const std::string &path_ndb, const std::string &shrt, uint32_t flag,
                                   std::optional<bool> srgb, bool invert = false) -> TexP {
        const auto fmt3 = tc::ps3_format(t);
        if (!fmt3) return nullptr;
        std::string kind = fmt3->kind;
        int bpb = fmt3->bpb;
        const bool comp = fmt3->compressed;
        Bytes texels = p.vram_bytes(t.vramOffset, t.vramSize);
        uint32_t w_ = t.width, h_ = t.height, mips_ = t.mips;
        if (t.type == 2 && (comp || kind == "A8R8G8B8")) return convert_ps3_cubemap(t, texels, kind, bpb, path_ndb, flag);
        if (!comp) {
            // uncompressed swizzled A8R8G8B8 (sky colour maps): deswizzle mip 0, rebuild the mip chain, encode BC3
            if (kind != "A8R8G8B8" || (w_ & (w_ - 1)) || (h_ & (h_ - 1))) return nullptr;
            const uint32_t rm = t.gcmRemapCtl & 0xffff;
            const tc::Img img8 = opt.sky ? tc::sky::ps3_deswizzle_argb8(texels, w_, h_, rm ? rm : 0xaae4) : tc::ps3_deswizzle_argb8(texels, w_, h_, rm ? rm : 0xaae4);
            std::vector<float> lv(img8.px.begin(), img8.px.end());
            uint32_t lw = w_, lh = h_;
            Bytes out;
            uint32_t n = 0;
            for (uint32_t k = 0; k < std::max(1u, mips_); k++) {
                tc::Img q;
                q.w = lw;
                q.h = lh;
                q.px.resize(lv.size());
                for (size_t i = 0; i < lv.size(); i++) q.px[i] = uint8_t(std::clamp(std::nearbyint(lv[i]), 0.0f, 255.0f));
                const Bytes e = opt.sky ? tc::sky::encode_bc3(q) : tc::encode_bc3(q);
                out.insert(out.end(), e.begin(), e.end());
                n++;
                if (std::min(lw, lh) < 8) break;
                const uint32_t nw = lw / 2, nh = lh / 2;
                std::vector<float> d(size_t(nw) * nh * 4);
                for (uint32_t y = 0; y < nh; y++)
                    for (uint32_t x = 0; x < nw; x++)
                        for (int c = 0; c < 4; c++) {
                            auto at = [&](uint32_t xx, uint32_t yy) { return lv[(size_t(yy) * lw + xx) * 4 + c]; };
                            d[(size_t(y) * nw + x) * 4 + c] = (((at(2 * x, 2 * y) + at(2 * x, 2 * y + 1)) + at(2 * x + 1, 2 * y)) + at(2 * x + 1, 2 * y + 1)) / 4.0f;
                        }
                lv.swap(d);
                lw = nw;
                lh = nh;
            }
            texels = out;
            mips_ = n;
            kind = "DXT5";
            bpb = 16;
        }
        const uint32_t rm_ = t.gcmRemapCtl & 0xffff;
        if (comp && rm_ != 0xaae4 && (kind == "DXT5" || kind == "DXT3")) {
            // a non-identity GCM remap (0xaae1 = R<->A on PS3 normal maps): decode every mip, remap, re-encode BC3
            Bytes out;
            uint64_t o = 0;
            for (auto [mw, mh] : tc::mip_dims(w_, h_, mips_)) {
                const uint64_t n_ = tc::ps3_linear_size(mw, mh, 1, bpb);
                const uint64_t s = std::min<uint64_t>(o, texels.size()), e = std::min<uint64_t>(o + n_, texels.size());
                tc::Img img = tc::decode(Bytes(texels.begin() + std::ptrdiff_t(s), texels.begin() + std::ptrdiff_t(std::max(s, e))), mw, mh, kind);
                o += n_;
                for (size_t i = 0; i < size_t(mw) * mh; i++) {
                    uint8_t *px = &img.px[i * 4];
                    const uint8_t argb[4] = {px[3], px[0], px[1], px[2]};
                    if (opt.sky) tc::sky::gcm_remap(argb, rm_, px);
                    else tc::gcm_remap(argb, rm_, px);
                }
                const Bytes e2 = opt.sky ? tc::sky::encode_bc3(img) : tc::encode_bc3(img);
                out.insert(out.end(), e2.begin(), e2.end());
            }
            texels = out;
            kind = "DXT5";
            bpb = 16;
        }
        Bytes tiled = tc::ps3_to_ps4_texels(texels, w_, h_, mips_, bpb);
        if (invert) {   // the sky converter: a blend map of the opposite polarity (BC1 only)
            if (kind != "DXT1") return nullptr;
            tiled = bc1_invert(tiled);
        }
        std::string role = tex_role_final(shrt).value_or("color");
        if (opt.sky && role == "color") {   // retail keeps '-height' / '-inc' maps BC1 UNORM (linear)
            const auto xr = xrole(shrt);
            if (xr == "height" || xr == "inc" || xr == "blend") role = "mask";
        }
        const bool sr = srgb ? *srgb : (role == "color" || role == "spec" || role == "detail");
        uint32_t fmt4;
        if (kind == "DXT1") fmt4 = sr ? 0x48 : 0x47;
        else if (kind == "DXT5") fmt4 = sr ? 0x4e : 0x4d;
        else if (kind == "DXT3") fmt4 = sr ? 0x4b : 0x4a;
        else throw Stop("KeyError: " + kind);
        const uint32_t hsh = sidu(after_prefix(path_ndb));
        const Bytes d = desc_bytes(hsh, 1, fmt4, sr ? 0x010f0000 : 0x01000000, mips_, w_, h_, flag);
        const uint32_t cls = (fmt4 == 0x47 || fmt4 == 0x48) ? 4 : 5;
        return add_texture_desc(path_ndb, d, tiled, gtex_bytes(hsh, starts_with(shrt, "lightmaps/") ? 0x1000 : 0x400, cls));
    };
    auto cap_raw_texture = [](const Bytes &desc, const Bytes &texels, Bytes &d_out, Bytes &t_out) {
        d_out = desc;
        t_out = texels;
        const uint64_t cap = 4u << 20;
        const uint32_t fmt = rd32le(desc, 0x1c), typ = rd32le(desc, 0x18);
        if (!(fmt == 0x1c || fmt == 0x1d || fmt == 0x57 || fmt == 0x5b) || typ != 1 || texels.size() <= cap) return;
        const uint32_t mips = rd32le(desc, 0x24), w = rd32le(desc, 0x28), h = rd32le(desc, 0x2c);
        uint64_t cut = 0;
        uint32_t n = 0;
        while (texels.size() - cut > cap && mips - n > 1) {
            const uint64_t mw = std::max(1u, n < 32 ? w >> n : 0), mh = std::max(1u, n < 32 ? h >> n : 0);
            cut += ((mw + 7) / 8 * 8) * ((mh + 7) / 8 * 8) * 4;
            n++;
        }
        if (!n) return;
        set32(d_out, 0x24, mips - n);
        set32(d_out, 0x28, std::max(1u, w >> n));
        set32(d_out, 0x2c, std::max(1u, h >> n));
        t_out = Bytes(texels.begin() + std::ptrdiff_t(std::min<uint64_t>(cut, texels.size())), texels.end());
    };
    std::function<TexP(const std::string &, const std::string &)> texture_for_full = [&](const std::string &full, const std::string &hint) -> TexP {
        if (auto it = tex_entries.find(full); it != tex_entries.end()) return it->second;
        const RTex *t = R.get(full + ".ndb");
        if (!t) t = R.get(full);
        if (t) {
            Bytes dsc, txl;
            cap_raw_texture(t->desc, t->texels, dsc, txl);
            std::optional<Bytes> g;
            if (auto gi = R.gtex.find(full); gi != R.gtex.end() && !gi->second.empty()) g = gi->second;
            else g = t->gtex;
            return add_texture_desc(t->path, dsc, txl, g);
        }
        const std::string shrt = hint.empty() ? short_of(full) : hint;
        if (const PakTexture *t3 = ps3tex_get(shrt)) return convert_ps3_texture(*t3, full + ".ndb", shrt, 3, std::nullopt);
        return nullptr;
    };
    auto no_ndb = [](const std::string &s) { return ends_with(s, ".ndb") ? s.substr(0, s.size() - 4) : s; };
    auto texture_for_short = [&](const std::string &shrt) -> TexP {
        if (const RTex *t = R.by_short(shrt)) return texture_for_full(no_ndb(t->path), "");
        const PakTexture *t3 = ps3tex_get(shrt);
        if (!t3) return nullptr;
        auto row = dict->find(shrt);
        std::string full;
        if (row != dict->end()) full = row->second.at(1);
        else {
            std::string b = t3->path.substr(t3->path.rfind('/') == std::string::npos ? 0 : t3->path.rfind('/') + 1);
            b = b.substr(0, b.find('.'));
            full = PS4_PREFIX + shrt + "/" + b;
        }
        return convert_ps3_texture(*t3, full + ".ndb", shrt, 3, std::nullopt);
    };
    std::map<std::string, TexP> flat_cache;
    auto flat_texture = [&](const std::optional<std::string> &role, const std::string &pname) -> TexP {
        std::string low = pname;
        for (char &c : low) c = char(std::tolower((unsigned char)c));
        std::string kind;
        uint32_t r_, g_, b_;
        bool srgb;
        if (has(low, "normal") || role == "normal") { kind = "normal"; r_ = 128; g_ = 128; b_ = 255; srgb = false; }
        else if (role == "mask" || role == "spec" || role == "blend") { kind = "mask"; r_ = g_ = b_ = 0; srgb = false; }
        else if (role == "trans") { kind = "trans"; r_ = g_ = b_ = 255; srgb = false; }
        else if (role == "white") { kind = "white"; r_ = g_ = b_ = 255; srgb = true; }   // the sky converter: neutral for a detail-multiply colour layer
        else if (role == "detail") { kind = "detail"; r_ = g_ = b_ = 128; srgb = false; }
        else { kind = "color"; r_ = g_ = b_ = 128; srgb = true; }
        if (auto it = flat_cache.find(kind); it != flat_cache.end()) return it->second;
        const uint32_t c = ((r_ >> 3) << 11) | ((g_ >> 2) << 5) | (b_ >> 3);
        Bytes lin;
        uint32_t w_ = 64, h_ = 64, mips_ = 0;
        while (true) {
            const uint32_t n = std::max(1u, (w_ + 3) / 4) * std::max(1u, (h_ + 3) / 4);
            for (uint32_t i = 0; i < n; i++) {
                put16(lin, c);
                put16(lin, c);
                put32(lin, 0);
            }
            mips_++;
            if (w_ == 1) break;
            w_ /= 2;
            h_ /= 2;
        }
        const std::string shrt = "art/generated/u3mp/flat-" + kind + ".tga";
        const Bytes tiled = tc::ps3_to_ps4_texels(lin, 64, 64, mips_, 8);
        const std::string path_ndb = PS4_PREFIX + shrt + "/" + u3::fmt("%016llX", (unsigned long long)sidu(shrt)) + ".ndb";
        const uint32_t hsh = sidu(after_prefix(path_ndb));
        const Bytes d = desc_bytes(hsh, 1, srgb ? 0x48 : 0x47, srgb ? 0x010f0000 : 0x01000000, mips_, 64, 64, 3);
        TexP e = add_texture_desc(path_ndb, d, tiled, gtex_bytes(hsh, 0x400, 4));
        flat_cache[kind] = e;
        return e;
    };
    // lightmap textures of the stage (resident, renamed like retail)
    std::map<std::string, TexP> lm_entries;
    for (const auto &[s, t] : ps3tex)
        if (has(s, ".geom.atgi/") || has(t.path, "/lightmap0/")) {
            const std::string base = s.substr(s.rfind('/') == std::string::npos ? 0 : s.rfind('/') + 1);
            std::string hx = t.path.substr(t.path.rfind('/') == std::string::npos ? 0 : t.path.rfind('/') + 1);
            hx = hx.substr(0, hx.find('.'));
            const std::string path_ndb = PS4_PREFIX + "lightmaps/" + stage + "/" + base + "/" + hx + ".ndb";
            lm_entries[base] = convert_ps3_texture(t, path_ndb, "lightmaps/" + stage + "/" + base, 3, false);
        }
    auto lightmap_entry = [&](const std::string &pname) -> TexP {
        const char *lm = pname == "g_tLightMap" ? "indirect.rgbmax.tga" : pname == "g_tLightMap1" ? "indirect.rgbmaxdelta.tga" : nullptr;
        if (!lm) return nullptr;
        if (auto it = lm_entries.find(lm); it != lm_entries.end() && it->second) return it->second;
        const RTex *t = R.by_short(std::string("art/general/textures/default/") + (pname == "g_tLightMap" ? "default-lightmap0.tga" : "default-lightmap1.tga"));
        if (t) return texture_for_full(no_ndb(t->path), "");
        return nullptr;
    };

    // ---------------- material records
    std::vector<std::string> names3;
    auto add_name = [&](const std::string &n) {
        if (std::find(names3.begin(), names3.end(), n) == names3.end()) names3.push_back(n);
    };
    for (const bg::Proto &c : C)
        for (const bg::Group &g : c.groups) add_name(g.mat);
    // water surfaces (WATER_DATA): their materials join the material table through one dummy prototype each
    std::vector<std::string> water_mats;
    const auto wobjs = bg::water_objects(p);
    for (const std::string &m : bg::water_materials(p))
        if (plan.get(m)) water_mats.push_back(m);
    for (const std::string &n : water_mats) add_name(n);
    // the sky converter: the last template material's spec maps and detail flag (read by exact materials after it), the
    // blend softness of vertex-colour-only blends, the PS3 fragment programs
    std::optional<std::map<std::string, std::vector<std::string>>> own_leak;
    std::optional<bool> detail_leak;
    std::map<std::string, double> soft_override;
    fp::Ps3Programs progs(p);
    std::map<std::string, MatW> matrec;
    for (const std::string &n : names3) {
        const PlanMat *pe = plan.get(n);
        if (!pe) throw Stop("KeyError: " + n);
        auto ri = R.mats.find({pe->pak, pe->key});
        if (ri == R.mats.end() || !ri->second->has_fx) throw Stop("bundle lacks material " + pe->key + " from " + pe->pak);
        const MatRec &rec0 = *ri->second;
        MatW mw;
        mw.pak = pe->pak;
        mw.rec = rec0;
        if (pe->exact) {
            mw.name = rec0.name;
            for (const MatBind &b : rec0.binds) {
                TexP e = lightmap_entry(b.param);
                // the sky converter: the spec-colour slots take the PS3 '-spec' map. As in the converter, this reads the
                // spec maps and detail flag the last template material left behind.
                if (opt.sky && !e && (b.param == "g_tNdFetchColor4Map" || b.param == "g_tNdFetchColor5Map" || b.param == "g_tNdFetchMask4Map")) {
                    if (!own_leak) throw Stop("NameError: name 'own' is not defined");
                    auto si = own_leak->find("spec3");
                    if (si != own_leak->end() && !si->second.empty()) {
                        if (!detail_leak) throw Stop("NameError: name 'detail_layer' is not defined");
                        if (!*detail_leak) e = texture_for_short(si->second[(b.param == "g_tNdFetchColor5Map" && si->second.size() > 1) ? 1 : 0]);
                    }
                }
                if (!e) e = texture_for_full(b.full, b.shrt);
                mw.binds.push_back({b.param, b.shrt, b.full, e});
            }
        } else {
            // template material re-bound to the PS3 material's textures (same role)
            std::map<std::string, std::vector<std::string>> own;
            auto add_own = [&](const std::string &r, const std::string &s3) {
                auto &v = own[r];
                if (std::find(v.begin(), v.end(), s3) == v.end()) v.push_back(s3);
            };
            if (auto mi = m3.find(n); mi != m3.end()) {
                std::vector<uint32_t> idxs = mi->second->textures;
                if (m3.size() == 1)   // single-material level (skies): every texture of the pak belongs to it
                    for (uint32_t i = 0; i < texs3.size(); i++)
                        if (std::find(idxs.begin(), idxs.end(), i) == idxs.end()) idxs.push_back(i);
                static const std::regex HASHTAIL(R"(/[0-9A-F]{16}\.ndb(\.error)?$)");
                for (uint32_t i : idxs) {
                    const std::string s3 = std::regex_replace(short_of(texs3.at(i).name), HASHTAIL, "");
                    auto r = tex_role_final(s3);
                    if (r == "spec") {
                        if (opt.sky) add_own("spec3", s3);   // kept apart for the spec-colour slots
                        r = "mask";
                    }
                    if (opt.sky && r == "color")
                        if (auto xr = xrole(s3)) r = xr;   // '-height' / '-blend2' / '-inc' maps get their own role
                    if (r) add_own(*r, s3);
                }
            }
            auto own_get = [&](const std::string &r) -> const std::vector<std::string> * {
                auto it = own.find(r);
                return it == own.end() || it->second.empty() ? nullptr : &it->second;
            };
            std::map<std::string, size_t> used_role;
            static const std::regex MASKN(R"(^g_tNdFetchMask\dMap$)"), TINTN(R"(^g_tNdFetchColorTintMask\dMap$)"), HT2(R"(/[0-9A-F]{16}(\.ndb)?$)");
            for (const MatBind &b : rec0.binds) {
                auto role = param_role(b.param);
                if (role == "spec") role = "mask";
                bool detail_layer = false;
                if (opt.sky) {
                    // a template colour layer multiplied by its Mask<n> texture: retail fills it with a '-detail' map
                    const std::string src = !b.shrt.empty() ? b.shrt : b.full;
                    if (std::regex_search(b.param, MASKN) && tex_role_final(std::regex_replace(short_of(src), HT2, "")) == "detail") {
                        role = "detail";
                        detail_layer = true;
                    }
                    detail_leak = detail_layer;
                }
                TexP e = lightmap_entry(b.param);
                // an explicit PS3 texture per template binding (the sky plans' bindmap; '' = a neutral flat texture)
                if (!e && pe->has_bindmap) {
                    auto bi = std::find_if(pe->bindmap.begin(), pe->bindmap.end(), [&](const auto &x) { return x.first == b.param; });
                    if (bi != pe->bindmap.end()) e = bi->second.empty() ? flat_texture(role, b.param) : texture_for_short(bi->second);
                }
                if (opt.sky && !e) {   // slots the role rules would leave on a black flat although the PS3 material has the texture
                    std::optional<std::string> alt;
                    const auto *blend = own_get("blend");
                    if (b.param == "g_tNdFetchBlendMap" && used_role["blend"] >= (blend ? blend->size() : 0)) alt = "height";
                    else if (std::regex_search(b.param, TINTN) && !own_get("mask")) {
                        size_t nbl = 0;
                        for (const MatBind &x : rec0.binds) nbl += x.param == "g_tNdFetchBlendMap";
                        if ((blend ? blend->size() : 0) > nbl) e = texture_for_short((*blend)[nbl]);
                    } else if (b.param == "g_tNdFetchIncandescenceMaskMap" && own_get("inc"))
                        alt = "inc";
                    if (!e && alt && own_get(*alt) && used_role[*alt] < own_get(*alt)->size()) {
                        const size_t k = used_role[*alt]++;
                        e = texture_for_short((*own_get(*alt))[k]);
                    }
                }
                if (!e && role && own_get(*role) && *role != "lightmap") {
                    const size_t k = used_role[*role]++;
                    const auto &v = *own_get(*role);
                    e = texture_for_short(v[std::min(k, v.size() - 1)]);
                }
                if ((!e && (!role || *role == "env")) || (!e && has(b.param, "LightMap"))) e = texture_for_full(b.full, b.shrt);
                if (!e) e = flat_texture(opt.sky && detail_layer ? std::optional<std::string>("white") : role, b.param);
                mw.binds.push_back({b.param, b.shrt, b.full, e});
            }
            if (opt.sky) own_leak = own;
            const auto &feats = rec0.feats;
            auto has_feat = [&](const char *f) { return std::find(feats.begin(), feats.end(), f) != feats.end(); };
            // the sky converter: blend-map polarity (PS3 program (1 - map) vs a template without INVERT_BLEND_MAP: an
            // inverted copy of the blend texture), and a blend factor the PS3 program takes from one channel of another
            // texture (a grey BC1 of that channel) or from vertex colour only (softness 1000)
            if (opt.sky && has_feat("BLEND_FACTOR_USE_BLEND_MAP_AND_VERTEX_COLOR") && !has_feat("BLEND_FACTOR_INVERT_BLEND_MAP") &&
                fp::material_polarity(progs, n) == "inv")
                for (auto &bd : mw.binds) {
                    if (bd.param != "g_tNdFetchBlendMap" || !bd.e) continue;
                    const std::string s_ = std::regex_replace(short_of(bd.e->full), HT2, "");
                    const PakTexture *t3_ = ps3tex_get(s_);
                    if (!t3_ || has(s_, "generated/u3mp/flat-")) continue;
                    const std::string sinv = ends_with(s_, ".tga") ? s_.substr(0, s_.size() - 4) + "-polinv.tga" : s_ + "-polinv";
                    const TexP einv = convert_ps3_texture(*t3_, PS4_PREFIX + sinv + "/" + u3::fmt("%016llX", (unsigned long long)sidu(sinv)) + ".ndb", sinv, 3,
                                                          std::nullopt, true);
                    if (einv) bd.e = einv;
                }
            if (opt.sky && has_feat("BLEND_FACTOR_USE_BLEND_MAP_AND_VERTEX_COLOR"))
                for (auto &bd : mw.binds) {
                    if (bd.param != "g_tNdFetchBlendMap" || !bd.e || !has(bd.e->full, "generated/u3mp/flat-")) continue;
                    const auto bs = fp::blend_source(progs, n);
                    if (!bs) continue;
                    if (bs->vc_only) {
                        if (has_feat("BLEND_FACTOR_SOFTNESS")) soft_override[n] = 1000.0;
                        continue;
                    }
                    std::vector<const PakTexture *> t3s;
                    for (const PakTexture &t : ps3_textures)
                        if (ends_with(t.path, "/" + bs->full)) t3s.push_back(&t);
                    if (t3s.size() != 1) continue;
                    const bool inv_ = (bs->pol == "inv") != has_feat("BLEND_FACTOR_INVERT_BLEND_MAP");
                    static const std::regex HT3(R"(/[0-9A-F]{16}\.ndb$)");
                    const std::string s0_ = std::regex_replace(bs->full, HT3, "");
                    std::string stem = s0_.substr(s0_.rfind('/') == std::string::npos ? 0 : s0_.rfind('/') + 1);
                    if (ends_with(stem, ".tga")) stem = stem.substr(0, stem.size() - 4);
                    const std::string sb_ = "art/generated/u3mp/b104/" + stem + "-" + bs->chan + "blend" + (bs->gamma ? "-lin" : "") + (inv_ ? "-inv" : "") + ".tga";
                    const std::string path_ = PS4_PREFIX + sb_ + "/" + u3::fmt("%016llX", (unsigned long long)sidu(sb_)) + ".ndb";
                    TexP eb_;
                    if (auto te = tex_entries.find(path_.substr(0, path_.size() - 4)); te != tex_entries.end()) eb_ = te->second;
                    else {
                        const auto dv = fp::derive_texels(p, *t3s[0], bs->chan, bs->gamma, inv_);
                        if (!dv) continue;
                        const auto &[tl_, w_, h_, mp_] = *dv;
                        const uint32_t hsh_ = sidu(after_prefix(path_));
                        eb_ = add_texture_desc(path_, desc_bytes(hsh_, 1, 0x47, 0x01000000, mp_, w_, h_, 3), tl_, gtex_bytes(hsh_, 0x400, 4));
                    }
                    bd.e = eb_;
                }
            const size_t sl = n.rfind('/');
            if (sl == std::string::npos) throw Stop("IndexError: material name without a directory");
            const std::string k3 = n.substr(0, sl) + ":" + n.substr(sl + 1);
            const std::string u8k = bg::utf8_of(k3);
            const std::string hx = md5_hex(Bytes(u8k.begin(), u8k.end()));
            mw.name = k3 + "," + hx.substr(0, 8) + "-" + hx.substr(8, 4) + "-" + hx.substr(12, 4) + "-" + hx.substr(16, 4) + "-" + hx.substr(20, 12) + "," + rec0.shader;
        }
        if (!pe->ps3params.empty()) {
            // the PS3 material's own constants overwrite the template's
            for (MatParam &mp : mw.rec.params) {
                const std::vector<double> *v3 = bg::param(pe->ps3params, mp.name);
                if (!v3) continue;
                const size_t nold = mp.vals.size() / 4;
                Bytes nv;
                for (size_t j = 0; j < nold; j++) {
                    if (j < v3->size()) put32(nv, f32b((*v3)[j]));
                    else put32(nv, quiet(rd32le(mp.vals, 4 * j)));
                }
                mp.vals = nv;
            }
        }
        if (opt.sky) {
            // a template that draws specular without a spec-map slot: the PS3 spec terms scaled by the PS3 mask mean
            const auto &feats = rec0.feats;
            bool slot = false;
            for (const auto &bd : mw.binds) slot = slot || bd.param == "g_tNdFetchColor4Map" || bd.param == "g_tNdFetchMask4Map";
            if (std::find(feats.begin(), feats.end(), "FINAL_COLOR_ADD_SPECULAR_LIGHT") != feats.end() && !slot) {
                const auto sc = fp::mask_scale(progs, n);
                if (sc && std::fabs(*sc - 1.0) > 1e-4)
                    for (MatParam &mp : mw.rec.params)
                        if (mp.name == "g_specularBrightness" || mp.name == "g_specularEnvironmentStrength" || mp.name == "g_specularSecondLobeBrightness")
                            set_first_float(mp.vals, py_round5(double(as_float(rd32le(mp.vals, 0))) * *sc));
            }
            if (auto so = soft_override.find(n); so != soft_override.end())
                for (MatParam &mp : mw.rec.params)
                    if (mp.name == "g_blendSoftness_blendOffset_linearBlendOffset") set_first_float(mp.vals, so->second);
        }
        matrec[n] = std::move(mw);
    }

    // ---------------- effects
    std::vector<std::string> order_hash;
    std::map<std::string, std::pair<std::string, const MatRec *>> eff_by_hash;
    for (const std::string &n : names3) {
        const MatRec &rec = matrec[n].rec;
        if (!eff_by_hash.count(rec.hash)) {
            eff_by_hash[rec.hash] = {matrec[n].pak, &rec};
            order_hash.push_back(rec.hash);
        }
    }
    if (order_hash.empty()) throw Stop("IndexError: no effects");
    std::string base_pak;
    {
        std::vector<std::pair<std::string, int>> cnt;   // Counter in first-seen order; most_common keeps the first of a tie
        for (const std::string &h : order_hash) {
            const std::string &pk = eff_by_hash[h].first;
            auto it = std::find_if(cnt.begin(), cnt.end(), [&](const auto &c) { return c.first == pk; });
            if (it == cnt.end()) cnt.push_back({pk, 1});
            else it->second++;
        }
        auto best = cnt.begin();
        for (auto it = cnt.begin(); it != cnt.end(); ++it)
            if (it->second > best->second) best = it;
        base_pak = best->first;
    }
    auto techs_of = [&](const std::string &pk) -> const std::vector<std::string> & {
        auto it = bundle.techs.find(pk);
        if (it == bundle.techs.end()) throw Stop("KeyError: techs of " + pk);
        return it->second;
    };
    std::vector<std::string> table = techs_of(base_pak);
    std::map<std::string, uint32_t> tpos;
    for (size_t i = 0; i < table.size(); i++) tpos[table[i]] = uint32_t(i);   // a repeated name: its last index
    std::vector<std::string> shaderfiles;
    struct FxW {
        std::shared_ptr<Blob> blob;
        int64_t eoff;
        uint32_t fx, x;
    };
    std::map<std::string, FxW> fx_items;
    for (const std::string &h : order_hash) {
        const auto &[pak, rec] = eff_by_hash[h];
        const std::vector<std::string> &src = techs_of(pak);
        std::map<uint32_t, uint32_t> mp;
        for (size_t i = 0; i < src.size(); i++) {
            if (!tpos.count(src[i])) {
                tpos[src[i]] = uint32_t(table.size());
                table.push_back(src[i]);
            }
            mp[uint32_t(i)] = tpos[src[i]];
        }
        const std::vector<FxItem> &clos = rec->fx;
        std::vector<std::shared_ptr<Blob>> blobs;
        for (size_t k = 0; k < clos.size(); k++) {
            Bytes data = clos[k].data;
            if (k == 0) {
                std::map<uint64_t, std::optional<int64_t>> ptrs;
                for (const auto &pp : clos[k].ptrs) ptrs[pp.pos] = pp.target == 0 ? std::optional<int64_t>(pp.off) : std::nullopt;
                data = remap(data, ptrs, mp);
            }
            auto b = blob("fx");
            b->raw(data);
            blobs.push_back(b);
        }
        for (size_t k = 0; k < clos.size(); k++) {
            auto &b = blobs[k];
            for (const auto &pp : clos[k].ptrs) {
                b->relocs.push_back({size_t(pp.pos), blobs.at(pp.target), pp.off});
                set64(b->b, size_t(pp.pos), 0);
            }
        }
        for (size_t k = 0; k < clos.size(); k++) P.item(clos[k].type, blobs[k], bg::utf8_of(clos[k].name), std::make_pair(size_t(0x100), size_t(clos[k].off % 0x100)));
        if (std::find(shaderfiles.begin(), shaderfiles.end(), rec->shader) == shaderfiles.end()) shaderfiles.push_back(rec->shader);
        fx_items[h] = {blobs.at(0), rec->fx_eoff, uint32_t(std::find(shaderfiles.begin(), shaderfiles.end(), rec->shader) - shaderfiles.begin()), rec->fx_x};
    }
    // EFFECT_TABLE: header, shader-file table and names at 0x20, then the string table
    auto eb = blob("effecttable");
    eb->u32(order_hash.size()).u32(7);
    auto ent = blob("fxentries");
    for (const std::string &h : order_hash) {
        const FxW &f = fx_items[h];
        ent->ptr(f.blob, f.eoff).u32(f.fx).u32(f.x).u64(std::stoull(h, nullptr, 16));
    }
    eb->ptr(ent);
    eb->ptr(eb, 0x20);
    const size_t sf_tab_size = 8 + 8 * shaderfiles.size();
    Bytes names_blob;
    std::vector<size_t> sf_offs;
    for (const std::string &s : shaderfiles) {
        sf_offs.push_back(names_blob.size());
        const std::string u = bg::utf8_of(s);
        names_blob.insert(names_blob.end(), u.begin(), u.end());
        names_blob.push_back(0);
    }
    while (names_blob.size() % 16) names_blob.push_back(0);
    const size_t tab_off = (0x20 + sf_tab_size + names_blob.size() + 15) & ~size_t(15);
    eb->ptr(eb, int64_t(tab_off));
    eb->u32(shaderfiles.size()).u32(0);
    for (size_t o : sf_offs) eb->ptr(eb, int64_t(0x20 + sf_tab_size + o));
    eb->raw(names_blob);
    while (eb->size() < tab_off) eb->u8(0);
    eb->u32(table.size()).u32(0);
    const size_t str_base = tab_off + 8 + 8 * table.size();
    Bytes sb;
    std::vector<size_t> so;
    for (const std::string &s : table) {
        so.push_back(sb.size());
        const std::string u = bg::utf8_of(s);
        sb.insert(sb.end(), u.begin(), u.end());
        sb.push_back(0);
    }
    for (size_t o : so) eb->ptr(eb, int64_t(str_base + o));
    eb->raw(sb);
    auto et_item = P.item("EFFECT_TABLE", eb);
    P.item("RAW_DATA", ent);

    // ---------------- geometry
    auto fmt_blob = blob("vtxfmt");
    std::map<std::pair<int, int>, size_t> fmt_off;
    auto fmt_entry = [&](int slot, int ty) {
        auto it = fmt_off.find({slot, ty});
        if (it == fmt_off.end()) {
            it = fmt_off.emplace(std::make_pair(slot, ty), fmt_blob->size()).first;
            fmt_blob->raw(Bytes{uint8_t(slot), 0, uint8_t(tsize(ty)), uint8_t(ty)});
        }
        return it->second;
    };
    for (const auto &[aid, m] : ATTR) fmt_entry(m.slot, m.ty);
    P.item("RAW_DATA", fmt_blob);
    std::unordered_map<Bytes, std::shared_ptr<Blob>, BytesHash> stream_cache;
    struct StreamIn {
        int slot, ty;
        Bytes data;
    };
    // stream records (0x18 each); identical vertex data blocks are shared, uniform streams share one buffer per value
    auto streams_blob = [&](const std::vector<StreamIn> &lst) {
        auto sb_ = blob("streams");
        for (const StreamIn &s : lst) {
            const size_t es = tsize(s.ty), nvv = s.data.size() / es;
            std::optional<std::pair<int, Bytes>> ukey;
            if (nvv > 1) {
                bool uni = s.data.size() == es * nvv;
                for (size_t i = es; uni && i < s.data.size(); i++) uni = s.data[i] == s.data[i % es];
                if (uni) ukey = std::make_pair(s.ty, Bytes(s.data.begin(), s.data.begin() + std::ptrdiff_t(es)));
            }
            std::shared_ptr<Blob> db;
            auto pi = ukey && !U.collecting ? U.plan.find(*ukey) : U.plan.end();
            if (ukey && !U.collecting && pi != U.plan.end() && pi->second >= nvv) {
                auto bi = U.bufs.find(*ukey);
                if (bi == U.bufs.end()) {
                    Bytes buf;
                    for (size_t i = 0; i < pi->second; i++) buf.insert(buf.end(), ukey->second.begin(), ukey->second.end());
                    bi = U.bufs.emplace(*ukey, raw(buf)).first;
                }
                db = bi->second;
                U.seen.insert(*ukey);
            } else {
                if (ukey && U.collecting) U.collect[*ukey] = std::max(U.collect[*ukey], nvv);
                auto ci = stream_cache.find(s.data);
                if (ci == stream_cache.end()) ci = stream_cache.emplace(s.data, raw(s.data)).first;
                db = ci->second;
            }
            sb_->u32(uint32_t(tsize(s.ty) << 16) | 1).u32(s.data.size()).ptr(fmt_blob, int64_t(fmt_entry(s.slot, s.ty))).ptr(db);
        }
        P.item("RAW_DATA", sb_);
        return sb_;
    };

    std::vector<Submesh> subs;
    std::vector<ProtoW> protos;
    std::vector<InstW> insts;
    for (size_t pi = 0; pi < C.size(); pi++) {
        const bg::Proto &pr = C[pi];
        const std::string pname = "Prototype" + std::to_string(pi) + "." + stage;
        const auto hdr = pr.hdr;
        const uint32_t hw0 = p.pu32(hdr.first, hdr.second);
        const uint32_t njoints = ((hw0 >> 16) & 2) ? (hw0 & 0xffff) : 0;
        const uint32_t hw14 = p.u32(p.pbase(hdr.first) + hdr.second + 0x38);
        const uint32_t hw8 = p.u32(p.pbase(hdr.first) + hdr.second + 8);
        std::optional<std::pair<uint32_t, uint32_t>> fade;
        if ((hw0 >> 16) & 4) fade = std::make_pair((hw8 >> 16) & 0xffff, hw8 & 0xffff);
        std::vector<std::pair<Ref, bg::Instance>> inst_list;
        for (const ORef &x : pr.insts) inst_list.push_back({bg::need(x), bg::instance(p, bg::need(x))});
        std::vector<Block> blocks;
        for (size_t bi = 0; bi < pr.blocks.size(); bi++) {
            std::vector<std::pair<ORef, ORef>> tabs;
            for (const auto &[x, I] : inst_list) {
                if (!I.blockdata) {
                    tabs.push_back({std::nullopt, std::nullopt});
                    continue;
                }
                const Ref bd = *I.blockdata;
                tabs.push_back({bg::res(p, bd.first, bd.second + 8 * bi), bg::res(p, bd.first, bd.second + 8 * bi + 4)});
            }
            blocks.push_back(decode_block3(p, pr.blocks[bi], tabs, opt.sky));
        }
        bool skinned = njoints > 0;
        for (const Block &b : blocks) skinned = skinned && b.has_skin;
        const Ref t = bg::need(p, hdr.first, hdr.second + 0x10);
        std::vector<uint32_t> gflags;
        for (size_t s = 0; s < pr.groups.size(); s++) gflags.push_back(p.u32(p.pbase(t.first) + t.second + 0x10 * s + 0xc));
        // PS3 prototypes with n LODs > 1 list the groups of every LOD in turn; PS4 keeps LOD0 (up to the first repeat)
        std::vector<bg::Group> groups = pr.groups;
        if ((hw0 >> 24) > 1) {
            std::set<std::string> seen;
            for (size_t i = 0; i < groups.size(); i++) {
                if (seen.count(groups[i].mat)) {
                    groups.resize(i);
                    break;
                }
                seen.insert(groups[i].mat);
            }
        }
        ProtoW P_;
        P_.name = pname;
        for (size_t gi = 0; gi < groups.size(); gi++) {
            const bg::Group &g = groups[gi];
            for (uint32_t c0 = g.first; c0 < g.first + g.count; c0 += MAX_BLOCKS) {
                Submesh rec;
                for (uint32_t b = c0; b < std::min(g.first + g.count, c0 + MAX_BLOCKS); b++) rec.chunk.push_back(b);
                std::vector<const Block *> bl;
                for (uint32_t b : rec.chunk) bl.push_back(&blocks.at(b));
                size_t nv = 0;
                for (const Block *b : bl) nv += b->nv;
                Bytes idx;
                size_t base = 0, ni = 0;
                for (const Block *b : bl) {
                    for (const auto &tr : b->tris)
                        for (uint32_t v : tr) {
                            const uint64_t x = uint64_t(v) + base;
                            if (x > 0xffff) throw Stop("OverflowError: index out of bounds for uint16");
                            put16(idx, uint32_t(x));
                            ni++;
                        }
                    base += b->nv;
                }
                // main streams in PS3 attribute order (union over blocks; missing = default)
                std::vector<int> order;
                for (const Block *b : bl)
                    for (const auto &[aid, a] : b->attrs)
                        if (std::find(order.begin(), order.end(), aid) == order.end()) order.push_back(aid);
                if (std::find(order.begin(), order.end(), 2) != order.end() && std::find(order.begin(), order.end(), 0x11) != order.end() &&
                    std::find(order.begin(), order.end(), 3) == order.end())
                    order.insert(std::find(order.begin(), order.end(), 2) + 1, 3);
                std::vector<StreamIn> st;
                std::optional<Mat> posarr;
                for (int aid : order) {
                    const AttrMap &am = *attr_map(aid);
                    std::vector<Mat> parts;
                    for (const Block *b : bl) {
                        if (const Mat *a = last_attr(*b, aid)) parts.push_back(*a);
                        else if (aid == 3) {
                            const Mat *ps = last_attr(*b, 1), *nr = last_attr(*b, 2);
                            if (!ps || !nr) throw Stop("KeyError: tangents need positions and normals");
                            Mat n3(nr->rows, 3);
                            for (size_t i = 0; i < nr->rows; i++)
                                for (size_t c = 0; c < 3; c++) n3.at(i, c) = nr->at(i, c);
                            parts.push_back(bg::gen_tangents(*ps, n3, last_attr(*b, 0x11), b->tris));
                        } else {
                            Mat d(b->nv, size_t(am.nc));
                            if (aid == 2 || aid == 0x15)
                                for (size_t i = 0; i < b->nv; i++) d.at(i, 3) = 1;
                            parts.push_back(d);
                        }
                    }
                    Mat arr = concat(parts, size_t(am.nc));
                    st.push_back({am.slot, am.ty, stream_bytes(am.ty, arr)});
                    if (aid == 1) posarr = arr;
                }
                // instance attribute defaults (prototype level), in the first block's format order
                std::vector<int> iorder;
                for (int a : bl[0]->order) {
                    bool any = false;
                    for (const Block *b : bl) any = any || b->base_inst.count(a);
                    if (inst_attr(a) && any) iorder.push_back(a);
                }
                // no lightmaps: the per-instance writer drops the lightmap-uv stream, so the defaults must too
                if (lm_entries.empty()) iorder.erase(std::remove(iorder.begin(), iorder.end(), 0x13), iorder.end());
                std::vector<StreamIn> st2;
                for (int aid : iorder) {
                    const AttrMap &am = *attr_map(aid);
                    Mat arr;
                    if (aid == 0x13) {
                        std::vector<Mat> parts;
                        for (const Block *b : bl) {
                            auto it = b->base_inst.find(aid);
                            parts.push_back(it != b->base_inst.end() ? it->second : Mat(b->nv, size_t(am.nc)));
                        }
                        arr = concat(parts, size_t(am.nc));
                    } else
                        arr = Mat(nv, size_t(am.nc));
                    st2.push_back({am.slot, am.ty, stream_bytes(am.ty, arr)});
                }
                if (skinned) {
                    std::vector<const bg::Influence *> infl;
                    for (const Block *b : bl)
                        for (const auto &x : b->skin) infl.push_back(&x);
                    Bytes cb, ob, jb, wb;
                    uint64_t sum = 0;
                    for (const auto *x : infl) {
                        if (x->size() > 0xffff) throw Stop("OverflowError: influence count");
                        rec.counts.push_back(uint16_t(x->size()));
                        put16(cb, uint32_t(x->size()));
                    }
                    for (size_t i = 0; i < infl.size(); i++) {
                        put16(ob, uint32_t(sum & 0xffff));   // the running offset, as numpy's astype('<u2') wraps it
                        sum += infl[i]->size();
                    }
                    for (const auto *x : infl)
                        for (const auto &[j, wv] : *x) {
                            if (j > 0xffff) throw Stop("OverflowError: joint index out of bounds for uint16");
                            rec.joints.push_back(uint16_t(j));
                            put16(jb, j);
                        }
                    for (const auto *x : infl)
                        for (const auto &[j, wv] : *x) {
                            const float f = float(double(wv) / 255.0);
                            rec.wts.push_back(f);
                            put32(wb, f32b(f));
                        }
                    rec.skin = true;
                    rec.skin_tot = sum;
                    rec.sk_c = raw(cb);
                    rec.sk_o = raw(ob);
                    rec.sk_j = raw(jb);
                    rec.sk_w = raw(wb);
                    rec.has_infl = true;
                }
                rec.name = stage + ".Prototype" + std::to_string(pi) + ".Submesh" + std::to_string(c0);
                rec.nv = nv;
                rec.ni = ni;
                rec.st = streams_blob(st);
                rec.st2 = st2.empty() ? nullptr : streams_blob(st2);
                rec.ns = st.size();
                rec.ns2 = st2.size();
                rec.ib = raw(idx);
                rec.mat = matrec.at(g.mat).name;
                rec.ps3mat = g.mat;
                rec.flags = 1 | ((gflags.at(gi) & 0x80) ? 0x100 : 0);
                rec.njoints = skinned ? njoints : 0;
                rec.iorder = iorder;
                if (!posarr) throw Stop("KeyError: 1");
                rec.pos = Mat(posarr->rows, 3);
                for (size_t i = 0; i < posarr->rows; i++)
                    for (size_t c = 0; c < 3; c++) rec.pos.at(i, c) = posarr->at(i, c);
                P_.subs.push_back(subs.size());
                subs.push_back(std::move(rec));
            }
        }
        // cbuffer capacity: 3 when a material of the prototype is alpha blended, else 2; one slot per distinct
        // feature set + 2, at least 8
        uint32_t cb3 = 2;
        std::set<std::set<std::string>> fsets;
        for (size_t si : P_.subs) {
            const auto &feats = matrec.at(subs[si].ps3mat).rec.feats;
            if (std::find(feats.begin(), feats.end(), "TRANSPARENCY_ALPHA_BLEND") != feats.end()) cb3 = 3;
            fsets.insert(std::set<std::string>(feats.begin(), feats.end()));
        }
        cb3 = std::max<uint32_t>({cb3, uint32_t(fsets.size() + 2), 8});
        P_.hw0 = hw0;
        P_.hw14 = hw14;
        P_.fade = fade;
        P_.skinned = skinned;
        P_.cb3 = cb3;
        const std::vector<size_t> proto_subs = P_.subs;
        protos.push_back(P_);
        // instances
        for (size_t ii = 0; ii < inst_list.size(); ii++) {
            const auto &[x, I] = inst_list[ii];
            InstW ins;
            std::copy(I.mat, I.mat + 16, ins.M);
            std::vector<double> J;   // njoints x 3 x 4
            bool hasJ = false;
            if (skinned)
                if (const ORef jr = bg::res(p, x.first, x.second + 0x10)) {
                    const uint64_t a = p.pbase(jr->first) + jr->second;
                    if (a + 0x30ull * njoints > p.d.size()) throw Stop("ValueError: joint matrices past the end");
                    for (uint64_t k = 0; k < 12ull * njoints; k++) J.push_back(double(bg::bef32(p, a + 4 * k)));
                    hasJ = true;
                }
            for (size_t si : proto_subs) {
                const Submesh &rr = subs[si];
                InstSub is;
                is.sub = si;
                const Mat &pos = rr.pos;
                if (pos.rows == 0) throw Stop("ValueError: zero-size array to reduction operation minimum");
                if (hasJ && rr.has_infl) {
                    // retail: skinned prototypes get the AABB of the posed (skinned) vertices
                    Mat posed(pos.rows, 3);
                    size_t t_ = 0;
                    for (size_t v = 0; v < pos.rows; v++)
                        for (uint32_t c = 0; c < rr.counts[v]; c++, t_++) {
                            const size_t jn = rr.joints.at(t_);
                            if (jn >= njoints) throw Stop("IndexError: joint index out of range");
                            const double ph[4] = {pos.at(v, 0), pos.at(v, 1), pos.at(v, 2), 1.0};
                            for (int i = 0; i < 3; i++) {
                                const double *Jr = &J[jn * 12 + size_t(i) * 4];
                                const double s = (Jr[0] * ph[0] + Jr[2] * ph[2]) + (Jr[1] * ph[1] + Jr[3] * ph[3]);
                                posed.at(v, size_t(i)) += s * double(rr.wts.at(t_));
                            }
                        }
                    for (int c = 0; c < 3; c++) {
                        is.mn[c] = is.mx[c] = posed.at(0, size_t(c));
                        for (size_t v = 1; v < posed.rows; v++) {
                            is.mn[c] = std::min(is.mn[c], posed.at(v, size_t(c)));
                            is.mx[c] = std::max(is.mx[c], posed.at(v, size_t(c)));
                        }
                    }
                } else
                    for (int c = 0; c < 3; c++) {   // retail: prototype-local AABB
                        is.mn[c] = is.mx[c] = pos.at(0, size_t(c));
                        for (size_t v = 1; v < pos.rows; v++) {
                            is.mn[c] = std::min(is.mn[c], pos.at(v, size_t(c)));
                            is.mx[c] = std::max(is.mx[c], pos.at(v, size_t(c)));
                        }
                    }
                std::vector<StreamIn> ist;
                for (int aid : rr.iorder) {
                    const AttrMap &am = *attr_map(aid);
                    std::vector<Mat> parts;
                    for (uint32_t b : rr.chunk) {
                        const Block &B = blocks[b];
                        const std::map<int, Mat> *d = ii < B.inst.size() ? &B.inst[ii] : nullptr;
                        if (d && d->count(aid)) parts.push_back(d->at(aid));
                        else if (B.base_inst.count(aid)) parts.push_back(B.base_inst.at(aid));
                        else parts.push_back(Mat(B.nv, size_t(am.nc)));
                    }
                    Mat arr = concat(parts, size_t(am.nc));
                    if (aid == 0x13 && lm_entries.empty()) continue;
                    ist.push_back({am.slot, am.ty, stream_bytes(am.ty, arr)});
                }
                bool lmf = false;
                for (uint32_t b : rr.chunk)
                    if (ii < blocks[b].inst.size() && blocks[b].inst_lm[ii]) lmf = true;
                // retail shadow-only flags on shadow-blocker submeshes (0x10000; 0x1010000 for ik-only), unless the PS3
                // instance word +8 has 0x10000; hidden blockers also carry +0x24 = 0x101
                uint32_t sbf = 0;
                const std::string &pm = rr.ps3mat;
                std::string pml = pm;
                for (char &ch : pml) ch = char(std::tolower((unsigned char)ch));
                if (has(pm, "ik-only")) sbf = 0x1010000;
                else if ((has(pml, "shadowblocker") || has(pml, "canopy-shadow")) && !(I.x8 & 0x10000)) sbf = 0x10000;
                is.st = ist.empty() ? nullptr : streams_blob(ist);
                is.n = ist.size();
                is.flags = ((lmf && !lm_entries.empty()) ? 0x100 : 0) | sbf;
                is.flags2 = sbf ? 0x101 : 0;
                ins.subs.push_back(is);
            }
            if (skinned)
                if (const ORef jr = bg::res(p, x.first, x.second + 0x10)) {
                    ins.nj = njoints;
                    const uint64_t a = p.pbase(jr->first) + jr->second;
                    const uint64_t e = std::min<uint64_t>(a + 0x30ull * njoints, p.d.size());
                    const Bytes be(p.d.begin() + std::ptrdiff_t(std::min<uint64_t>(a, p.d.size())), p.d.begin() + std::ptrdiff_t(std::max(e, std::min<uint64_t>(a, p.d.size()))));
                    raw(be);   // the converter adds the big-endian copy as an item too (unused)
                    Bytes le;
                    for (size_t k = 0; k + 4 <= be.size(); k += 4) put32(le, quiet(uint32_t(be[k]) << 24 | uint32_t(be[k + 1]) << 16 | uint32_t(be[k + 2]) << 8 | be[k + 3]));
                    ins.jm = raw(le);
                }
            for (int c = 0; c < 3; c++) {
                if (ins.subs.empty()) break;
                ins.mn[c] = ins.subs[0].mn[c];
                ins.mx[c] = ins.subs[0].mx[c];
                for (const InstSub &s : ins.subs) {
                    ins.mn[c] = std::min(ins.mn[c], s.mn[c]);
                    ins.mx[c] = std::max(ins.mx[c], s.mx[c]);
                }
            }
            ins.proto = pname;
            ins.id = I.x8 & 0xffff;
            ins.x8 = I.x8;
            insts.push_back(std::move(ins));
        }
    }
    // water dummy prototypes (after the PS3 prototypes, like retail)
    for (size_t wk = 0; wk < water_mats.size(); wk++) {
        const std::string &wm = water_mats[wk];
        const size_t pi = C.size() + wk;
        const std::string pname = "Prototype" + std::to_string(pi) + "." + stage;
        Submesh rec;
        rec.name = stage + ".Prototype" + std::to_string(pi) + ".Submesh0";
        rec.nv = 1;
        rec.ni = 3;
        rec.st = streams_blob({{0, 6, stream_bytes(6, Mat(1, 3))}});
        rec.ns = 1;
        rec.ib = raw(Bytes(6, 0));
        rec.mat = matrec.at(wm).name;
        rec.ps3mat = wm;
        rec.flags = 1;
        rec.pos = Mat(1, 3);
        ProtoW P_;
        P_.name = pname;
        P_.subs = {subs.size()};
        P_.cb3 = 1;
        subs.push_back(std::move(rec));
        protos.push_back(P_);
        int64_t nid = -1;
        for (const InstW &i : insts) nid = std::max<int64_t>(nid, i.id);
        nid++;
        InstW ins;
        ins.proto = pname;
        ins.id = uint32_t(nid);
        ins.x8 = uint32_t(nid);
        InstSub is;
        is.sub = P_.subs[0];
        ins.subs.push_back(is);
        for (int k = 0; k < 4; k++) ins.M[k * 5] = 1.0;
        insts.push_back(std::move(ins));
    }

    // ---------------- material / texture tables
    std::vector<TexP> gtex;
    for (const std::string &n : names3)
        for (const auto &b : matrec[n].binds)
            if (b.e && std::find(gtex.begin(), gtex.end(), b.e) == gtex.end()) gtex.push_back(b.e);
    auto texb = blob("geotextures");
    for (const TexP &e : gtex) {
        texb->ptr(strs, S(e->full));
        if (e->gtex) {
            Bytes g = *e->gtex;
            g.resize(0x20, 0);
            texb->raw(g);
        } else
            texb->u32(e->hash).u32(0x400).u32(0x101).i32(-1).u32(0).u32(4).u64(0);
    }
    auto featb = blob("features"), parb = blob("params"), valb = blob("paramvalues"), bindb = blob("bindings"), matb = blob("materials");
    std::vector<const MatW *> mat_order;
    for (const std::string &n : names3) {
        const MatW &m = matrec[n];
        bool dup = false;
        for (const MatW *x : mat_order) dup = dup || x->name == m.name;
        if (!dup) mat_order.push_back(&m);
    }
    for (const MatW *m : mat_order) {
        const MatRec &rec = m->rec;
        const size_t foff = featb->size();
        for (const std::string &f : rec.feats) featb->ptr(strs, S(f));
        const size_t poff = parb->size();
        for (const MatParam &mp : rec.params) {
            const size_t voff = valb->size();
            valb->raw(mp.vals);
            valb->pad(16);
            parb->ptr(strs, S(mp.name)).ptr(valb, int64_t(voff)).u32(mp.cnt).u32(mp.x);
        }
        const size_t boff = bindb->size();
        for (const auto &b : m->binds) {
            std::string full2 = b.full, sh = b.shrt;
            if (b.e) {
                full2 = b.e->full;
                sh = has(b.param, "LightMap") ? b.shrt : short_of(b.e->path);
            }
            bindb->ptr(strs, S(b.param));
            bindb->ptr(strs, S(sh));
            bindb->ptr(strs, S(full2)).i32(-1).u32(0).u64(0);
        }
        matb->ptr(strs, S(m->name));
        matb->ptr(strs, S(rec.shader));
        matb->ptr(rec.feats.empty() ? nullptr : featb, int64_t(foff));
        matb->ptr(strs, S(rec.hash));
        matb->ptr(rec.params.empty() ? nullptr : parb, int64_t(poff)).ptr(m->binds.empty() ? nullptr : bindb, int64_t(boff));
        matb->u32(rec.feats.size()).u32(rec.params.size()).u32(m->binds.size()).u32(rec.x3);
        matb->u32(rec.tail[0]).u32(rec.tail[1]);
    }
    for (const auto &b : {featb, parb, valb, bindb, matb, texb})
        if (b->size()) P.item("RAW_DATA", b);

    // submesh array
    auto sbk = blob("submeshes");
    for (const Submesh &r : subs) {
        sbk->u64(0);
        sbk->ptr(strs, S(r.name)).u32(r.flags).u32(0).u32(0).u32(0);
        sbk->u32(r.nv).u32(r.ni).u32(r.ns).u32(r.ns2).ptr(r.st).ptr(r.st2);
        sbk->ptr(r.ib);
        sbk->ptr(strs, S(r.mat)).i32(-1).u32(0).u32(r.njoints).u32(0);
        if (r.skin) {
            auto skb = blob("skin");
            skb->u32(0).u32(r.skin_tot).ptr(r.sk_c).ptr(r.sk_o).ptr(r.sk_j).ptr(r.sk_w);
            P.item("RAW_DATA", skb);
            sbk->ptr(skb);
        } else
            sbk->u64(0);
        sbk->raw(Bytes(0x20, 0));
    }
    P.item("RAW_DATA", sbk);
    // prototypes
    auto protob = blob("prototypes"), plist = blob("protolist"), flagsb = blob("protoflags"), snl = blob("subnames");
    for (ProtoW &pr : protos) {
        const size_t foff = flagsb->size();
        {   // 16 flag bytes: [0] skinned, [1] PS3 hdr byte1 0x04, [2] 0x08, [6] / [7] hdr+0x38 bits 0x01 / 0x02, [10] 0x20
            const uint32_t b1 = (pr.hw0 >> 16) & 0xff, b38 = pr.hw14 >> 24;
            Bytes fl(16, 0);
            fl[0] = pr.skinned ? 1 : 0;
            fl[1] = (b1 & 4) ? 1 : 0;
            fl[2] = (b1 & 8) ? 1 : 0;
            fl[6] = (b38 & 1) ? 1 : 0;
            fl[7] = (b38 & 2) ? 1 : 0;
            fl[10] = (b38 & 0x20) ? 1 : 0;
            flagsb->raw(fl);
        }
        const size_t soff = snl->size();
        for (size_t si : pr.subs) snl->ptr(strs, S(subs[si].name)).i32(-1).u32(0);
        pr.off = protob->size();
        protob->u32(3).u32(pr.subs.size()).u32(0).u32(sidu(stage + "." + pr.name));
        protob->ptr(strs, S(pr.name)).ptr(snl, int64_t(soff)).ptr(flagsb, int64_t(foff)).u64(0);
        protob->u32(pr.cb3).u32(1);
        std::shared_ptr<Blob> fb;
        if (pr.fade) {
            fb = blob("fade");
            fb->u32(f32b(double(pr.fade->first))).u32(f32b(double(pr.fade->second))).u32(0).u32(0);
            P.item("RAW_DATA", fb);
        }
        protob->raw(Bytes(0x48 - 0x38, 0));
        protob->ptr(fb);   // +0x48 fade distances (flag[1]); null when unflagged
        protob->raw(Bytes(0x78 - 0x50, 0));
    }
    for (const ProtoW &pr : protos) plist->ptr(protob, int64_t(pr.off));
    for (const auto &b : {protob, flagsb, snl, plist}) P.item("RAW_DATA", b);
    // instances
    auto instb = blob("instances"), ilist = blob("instlist"), isb = blob("instsubs");
    for (InstW &ins : insts) {
        const size_t ioff = isb->size();
        for (const InstSub &s : ins.subs) {
            isb->ptr(strs, S(subs[s.sub].name)).i32(-1).u32(0).u32(s.n).u32(0).ptr(s.st);
            isb->u32(s.flags).u32(s.flags2).u32(0x00ff0000).i32(-1);
            isb->u32(f32b(s.mn[0])).u32(f32b(s.mn[1])).u32(f32b(s.mn[2])).u32(f32b(1.0));
            isb->u32(f32b(s.mx[0])).u32(f32b(s.mx[1])).u32(f32b(s.mx[2])).u32(f32b(1.0));
        }
        ins.off = instb->size();
        instb->u32(3).u32(ins.id).ptr(strs, S(ins.proto)).i32(-1).u32(0).u32(0x01010101).u32(1);
        instb->u32(ins.subs.size()).u32(ins.nj).u64(0).ptr(isb, int64_t(ioff)).ptr(ins.jm);
        instb->raw(Bytes(0x20, 0));
        instb->u32(f32b(ins.mn[0])).u32(f32b(ins.mn[1])).u32(f32b(ins.mn[2])).u32(f32b(1.0));
        instb->u32(f32b(ins.mx[0])).u32(f32b(ins.mx[1])).u32(f32b(ins.mx[2])).u32(f32b(1.0));
        for (double m : ins.M) instb->u32(f32b(m));
    }
    for (const InstW &ins : insts) ilist->ptr(instb, int64_t(ins.off));
    for (const auto &b : {instb, isb, ilist}) P.item("RAW_DATA", b);
    P.item("RAW_DATA", strs);
    auto gb = blob("geometry");
    gb->u32(0x10).u32(0);
    gb->u64(subs.size()).ptr(sbk);
    gb->u64(protos.size()).ptr(plist);
    gb->u64(insts.size()).ptr(ilist);
    gb->u64(gtex.size()).ptr(gtex.empty() ? nullptr : texb);
    gb->u64(mat_order.size()).ptr(matb);
    gb->u64(0).u64(0);
    auto geo = P.item("GEOMETRY_1", gb);

    // ---------------- small items
    auto li = blob("levelinfo");
    uint32_t liv = 0;   // the sky converter: LEVEL_INFO_1 is the PS3 u32 flags verbatim (retail = PS3)
    if (opt.sky)
        if (const auto lif = p.items_of("LEVEL_INFO_1"); !lif.empty()) liv = p.pu32(lif[0].page, lif[0].data_off());
    li->u32(liv);
    auto level_info = P.item("LEVEL_INFO_1", li);
    auto lp = blob("levelpats");
    lp->u64(0);
    auto level_pats = P.item("LEVEL_PATS_1", lp);
    auto di = blob("debuginfo");
    di->ptr(di, 0x10).u32(PAD32).u32(PAD32);
    di->cstr(bg::utf8_of(stage));
    auto debug_info = P.item("DEBUG_INFO_1", di);
    auto af = blob("afm");
    af->u32(0).u32(PAD32).u64(0).u64(0);
    auto afm = P.item("ATTACHED_FG_MAPPING", af);
    auto tf = blob("tagfloat");
    tf->u32(0);
    auto tagf = P.item("TAG_FLOAT", tf);
    // each PS3 projector entry names its gobo texture by the PS3 texture hash; the texture is converted into the pak
    auto gobo = [&](const std::vector<uint32_t> &hashes) -> std::optional<std::vector<std::string>> {
        std::map<uint32_t, std::string> byh;
        for (const auto &[sh, t3] : ps3tex) byh.emplace(t3.hash, sh);
        for (const auto &[hx, sh] : bundle.gobo_tex) byh.emplace(uint32_t(std::stoull(hx, nullptr, 16)), sh);
        std::vector<std::string> names;
        for (uint32_t h : hashes) {
            auto it = byh.find(h);
            if (it == byh.end()) return std::nullopt;
            names.push_back(it->second);
        }
        for (const std::string &sh : std::set<std::string>(names.begin(), names.end()))
            if (!texture_for_short(sh)) return std::nullopt;
        return names;
    };
    std::shared_ptr<PakWItem> light;
    convert_light_table(p, P, gobo, light);
    // texture table
    std::vector<TexP> order = tex_order;
    std::stable_sort(order.begin(), order.end(), [](const TexP &a, const TexP &b) { return a->hash < b->hash; });
    auto namesb = blob("ttnames");
    for (const TexP &e : order) namesb->ptr(e->name->data);
    auto hashb = blob("tthash");
    for (const TexP &e : order) hashb->u32(e->hash);
    P.item("RAW_DATA", namesb);
    P.item("RAW_DATA", hashb);
    auto ttb = blob("texturetable");
    ttb->u64(order.size()).ptr(hashb).ptr(namesb).ptr(ttb, 0x20);
    for (const TexP &e : order) ttb->ptr(e->desc->data);
    auto ttab = P.item("TEXTURE_TABLE", ttb);
    // background collision (PS3 ND mesh -> Havok 2011.3 hkpBvCompressedMeshShape)
    std::vector<std::shared_ptr<PakWItem>> colls;
    for (const auto &cb : havok_bg_blobs(p, opt.sky)) colls.push_back(P.item("COLLISION_DATA_HAVOK_BG", cb));
    // occlusion (VIS_INFO_1)
    std::shared_ptr<PakWItem> vis;
    if (!p.items_of("VIS_INFO_1").empty()) {
        const auto vbs = vis_blobs(p);
        if (!vbs.empty()) {
            vis = P.item("VIS_INFO_1", vbs[0]);
            for (size_t k = 1; k < vbs.size(); k++) P.item("RAW_DATA", vbs[k]);   // a big VIS: the rest in RAW_DATA items
        }
    }
    // water surfaces (WATER_DATA)
    std::shared_ptr<PakWItem> water;
    if (!wobjs.empty() && !water_mats.empty()) {
        auto wb = water_blob(p, [&](const std::string &n) {
            const std::string k = matrec.count(n) ? matrec[n].name : ps4key(n);
            return k.substr(0, k.find(','));
        });
        if (wb) water = P.item("WATER_DATA", wb);
    }
    // LEVEL_OFFSET_1 (3 floats) kept 1:1, after LEVEL_INFO_1
    std::shared_ptr<PakWItem> loff;
    if (const auto lo3 = p.items_of("LEVEL_OFFSET_1"); !lo3.empty()) {
        if (lo3[0].size != 12) throw Stop(u3::fmt("AssertionError: LEVEL_OFFSET_1 size %#x", lo3[0].size));
        auto lb_ = blob("leveloffset");
        const uint64_t b0 = p.pbase(lo3[0].page) + lo3[0].data_off();
        for (int k = 0; k < 3; k++) lb_->u32(be32(p.d, b0 + 4 * k));
        loff = P.item("LEVEL_OFFSET_1", lb_);
    }
    if (!build) return {};
    // login page; retail order: LEVEL_INFO_1 (LEVEL_OFFSET_1) DEBUG_INFO_1 LEVEL_PATS_1 EFFECT_TABLE TEXTURE_TABLE GEOMETRY_1
    // VIS_INFO_1 LIGHT_TABLE_1 ATTACHED_FG_MAPPING COLLISION_DATA_HAVOK_BG (WATER_DATA) TAG_FLOAT
    std::vector<std::shared_ptr<PakWItem>> targets = {level_info};
    if (loff) targets.push_back(loff);
    for (const auto &x : {debug_info, level_pats, et_item, ttab, geo}) targets.push_back(x);
    if (vis) targets.push_back(vis);
    if (light) targets.push_back(light);
    targets.push_back(afm);
    for (const auto &c : colls) targets.push_back(c);
    if (water) targets.push_back(water);
    targets.push_back(tagf);
    std::vector<std::shared_ptr<PakWItem>> descs;
    for (const TexP &e : order) descs.push_back(e->desc);
    Page &p1 = w.page();
    make_login_and_vram_page(w, p1, targets, descs);
    w.vram = vram;
    Bytes data = w.build();
    for (const auto &pg : w.pages)   // break the self-reference cycles (blobs pointing into themselves)
        for (auto &part : pg->parts)
            if (auto *it = std::get_if<std::shared_ptr<PakWItem>>(&part)) (*it)->data->relocs.clear();
    return data;
}

// ------------------------------------------------------------------------------------------------ LIGHT_TABLE_1
// PS3 0x60 header -> PS4 0xa0 header: the u32 fields +0x08..+0x24 -> +0x10..+0x2c; records (+0x28, 0x3c each) -> +0x30,
// local lights (+0x30, 0x48) -> +0x40, sun (+0x34, 0x3c) -> +0x48, projector list (+0x38, 0xa0 -> 0xb0) -> +0x50, block
// (+0x3c) -> +0x58, vec4s (+0x48) -> +0x70, probe positions (+0x4c) -> +0x78, probe SH (+0x50) -> +0x80, flags (+0x04)
// after the header. Big-endian words; the word at +0x30 of records / lights / sun is 4 raw bytes. Blocks 16-aligned in
// header order, 0xaa fill.
std::shared_ptr<Blob> convert_light_table(const Pak &p, AutoPages &P, const std::function<std::optional<std::vector<std::string>>(const std::vector<uint32_t> &)> &gobo,
                                          std::shared_ptr<PakWItem> &item) {
    const auto its = p.items_of("LIGHT_TABLE_1");
    if (its.empty()) return nullptr;   // sky levels: retail PS4 sky paks carry no LIGHT_TABLE_1 either
    const PakItem &it = its[0];
    const uint32_t pg = it.page;
    const uint64_t b = it.data_off(), base = p.pbase(pg);
    auto u = [&](uint64_t o) { return p.pu32(pg, b + o); };
    std::map<uint64_t, uint64_t> fields;
    for (uint64_t o = 0; o < 0x60; o += 4)
        if (const ORef r = bg::res(p, pg, b + o); r && r->first == pg) fields[o] = r->second - b;
    std::set<uint64_t> ends;
    for (const auto &[o, t] : fields) ends.insert(t);
    ends.insert(it.size);
    auto block = [&](uint64_t o) -> std::optional<Bytes> {
        auto f = fields.find(o);
        if (f == fields.end()) return std::nullopt;
        const uint64_t t = f->second;
        auto e = ends.upper_bound(t);
        if (e == ends.end()) throw Stop("IndexError: list index out of range");
        const uint64_t s = std::min<uint64_t>(base + b + t, p.d.size()), en = std::min<uint64_t>(base + b + *e, p.d.size());
        return Bytes(p.d.begin() + std::ptrdiff_t(s), p.d.begin() + std::ptrdiff_t(std::max(s, en)));
    };
    auto head = [](const Bytes &d, size_t n) { return Bytes(d.begin(), d.begin() + std::ptrdiff_t(std::min(n, d.size()))); };
    auto words = [](Bytes d, std::optional<size_t> raw_at = std::nullopt, size_t stride = 0) {
        d.resize(d.size() / 4 * 4);
        Bytes out(d.size());
        for (size_t k = 0; k < d.size(); k += 4)
            for (int j = 0; j < 4; j++) out[k + j] = d[k + 3 - j];
        if (raw_at && int64_t(d.size()) - int64_t(*raw_at) - 3 > 0)
            for (size_t k = 0; int64_t(k) < int64_t(d.size()) - int64_t(*raw_at) - 3; k += stride)
                for (int j = 0; j < 4; j++) out[k + *raw_at + j] = d[k + *raw_at + j];
        return out;
    };
    auto sh = [](const Bytes &d) {
        Bytes out;
        for (size_t k = 0; k + 0x20 <= d.size() / 0x20 * 0x20; k += 0x20) {
            put32(out, quiet(uint32_t(d[k]) << 24 | uint32_t(d[k + 1]) << 16 | uint32_t(d[k + 2]) << 8 | d[k + 3]));
            out.insert(out.end(), d.begin() + std::ptrdiff_t(k + 4), d.begin() + std::ptrdiff_t(k + 0x20));
        }
        return out;
    };
    auto cat = [](Bytes a, const Bytes &c) { a.insert(a.end(), c.begin(), c.end()); return a; };
    const uint32_t nflags = u(0), cnt8 = u(8), nlocal = u(0x10);
    uint32_t n18 = u(0x18);
    std::map<uint64_t, Bytes> parts;
    if (auto d = block(0x28); d && cnt8) parts[0x30] = words(head(*d, 0x3cull * cnt8), 0x30, 0x3c);
    if (auto d = block(0x30); d && nlocal) parts[0x40] = words(head(*d, 0x48ull * nlocal), 0x30, 0x48);
    if (auto d = block(0x34)) parts[0x48] = cat(words(head(*d, 0x3c), 0x30, 0x3c), Bytes(4, 0xaa));
    std::vector<std::string> gobo_names;
    std::vector<size_t> gobo_str;
    if (auto d = block(0x38); n18 && d && d->size() >= 0xa0ull * n18) {
        // projector (gobo) lights: PS3 0xa0 -> PS4 0xb0 {0x3c words (raw +0x30), pad, char* gobo texture, u64 0, 0x60
        // words}; one name string per entry after the entries (retail)
        std::vector<uint32_t> hashes;
        for (uint32_t k = 0; k < n18; k++) hashes.push_back(be32(*d, 0xa0ull * k + 0x3c));
        if (auto names = gobo(hashes); names && !names->empty()) {
            Bytes ent;
            for (uint32_t k = 0; k < n18; k++) {
                const Bytes e3(d->begin() + std::ptrdiff_t(0xa0ull * k), d->begin() + std::ptrdiff_t(0xa0ull * k + 0xa0));
                ent = cat(ent, words(head(e3, 0x3c), 0x30, 0x3c));
                ent = cat(ent, Bytes(4, 0xaa));
                ent = cat(ent, Bytes(16, 0));
                ent = cat(ent, words(Bytes(e3.begin() + 0x40, e3.end())));
            }
            for (const std::string &nm : *names) {
                gobo_str.push_back(ent.size());
                const std::string u8 = bg::utf8_of(nm);
                ent.insert(ent.end(), u8.begin(), u8.end());
                ent.push_back(0);
                while (ent.size() % 16) ent.push_back(0xaa);
            }
            parts[0x50] = ent;
            gobo_names = *names;
        }
    }
    if (n18 && gobo_names.empty()) n18 = 0;
    if (auto d = block(0x3c)) parts[0x58] = words(*d);
    if (auto d = block(0x48)) parts[0x70] = words(head(*d, 16ull * u(0x24)));
    if (auto d = block(0x4c)) parts[0x78] = words(head(*d, 12ull * u(0x1c)));
    if (auto d = block(0x50)) parts[0x80] = sh(head(*d, 0x20ull * u(0x1c)));
    auto lb = blob("lighttable");
    lb->u32(nflags).u32(PAD32);
    if (nflags) lb->ptr(lb, 0xa0);
    else lb->u64(0);
    lb->u32(cnt8).u32(u(0xc)).u32(nlocal).u32(u(0x14)).u32(n18).u32(u(0x1c)).u32(u(0x20)).u32(u(0x24));
    size_t off = 0xa0 + (nflags ? ((nflags + 15) & ~15u) : 0);
    std::map<uint64_t, size_t> pos;
    for (const auto &[o4, d] : parts) {
        pos[o4] = off;
        off = (off + d.size() + 15) & ~size_t(15);
    }
    for (uint64_t o4 = 0x30; o4 < 0xa0; o4 += 8) {
        if (pos.count(o4)) lb->ptr(lb, int64_t(pos[o4]));
        else lb->u64(0);
    }
    if (lb->size() != 0xa0) throw Stop("AssertionError: light table header");
    if (nflags) {
        const Ref fl = bg::need(p, pg, b + 4);
        const uint64_t a = p.pbase(fl.first) + fl.second;
        const uint64_t s = std::min<uint64_t>(a, p.d.size()), e = std::min<uint64_t>(a + nflags, p.d.size());
        lb->raw(Bytes(p.d.begin() + std::ptrdiff_t(s), p.d.begin() + std::ptrdiff_t(std::max(s, e))));
        while (lb->size() % 16) lb->u8(0xaa);
    }
    for (const auto &[o4, d] : parts) {
        while (lb->size() < pos[o4]) lb->u8(0xaa);
        lb->raw(d);
    }
    while (lb->size() % 16) lb->u8(0xaa);
    for (size_t k = 0; k < gobo_names.size(); k++) lb->relocs.push_back({pos[0x50] + 0xb0 * k + 0x40, lb, int64_t(pos[0x50] + gobo_str[k])});
    item = P.item("LIGHT_TABLE_1", lb);
    return lb;
}

// ------------------------------------------------------------------------------------------------ WATER_DATA
// PS3 item {u32 5, u32 n, ptr objects} -> PS4 {u32 5, u32 n, ptr}; object 0xf0 -> 0x108: words 0..19 swapped (w1 =
// sid of the PS4 material key), +0x50 verts*, +0x58 null, +0x60 idx*, words 23..49, pad, 0, 0, words 51..59, 0; data
// blocks after the objects (16-aligned, verts then idx): grid water (4-byte texels) raw + 8 swapped u32; mesh water
// (40-byte vertices, per-word byte orders) + swapped u16 triangle indices.
std::shared_ptr<Blob> water_blob(const Pak &p, const std::function<std::string(const std::string &)> &matkey) {
    const auto obs = bg::water_objects(p);
    if (obs.empty()) return nullptr;
    static const char *VPERM[] = {"3210", "3210", "3210", "3210", "1032", "3210", "0123", "0132", "1032", "1032"};
    auto B = blob("water");
    B->u32(5).u32(obs.size()).ptr(B, 0x10);
    struct Body {
        Bytes out, verts, idx;
    };
    std::vector<Body> body;
    for (const auto &[o, mname] : obs) {
        const auto [pg, off] = o;
        const uint64_t base = p.pbase(pg) + off;
        auto w = [&](int i) {
            const uint64_t a = base + 4ull * i;
            if (a + 4 > p.d.size()) throw Stop("struct.error: water object past the end");
            return Bytes(p.d.begin() + std::ptrdiff_t(a), p.d.begin() + std::ptrdiff_t(a + 4));
        };
        auto u = [&](int i) { return be32(p.d, base + 4ull * i); };
        Body bd;
        auto rev = [&](int i) { const Bytes x = w(i); bd.out.insert(bd.out.end(), x.rbegin(), x.rend()); };
        for (int i = 0; i < 20; i++) rev(i);
        if (mname && !mname->empty()) set32(bd.out, 4, sidu(matkey(*mname)));
        const Ref rv = bg::need(p, pg, off + 0x50), ri = bg::need(p, pg, off + 0x58);
        bd.out.resize(bd.out.size() + 24, 0);   // +0x50 verts, +0x58 null, +0x60 idx
        for (int i = 23; i < 50; i++) rev(i);
        put32(bd.out, PAD32);   // PS4 w53: pad (techniques pointer dropped); w54..w55 0
        bd.out.resize(bd.out.size() + 8, 0);
        for (int i = 51; i < 60; i++) rev(i);
        put32(bd.out, 0);
        if (bd.out.size() != 0x108) throw Stop("AssertionError: water object size");
        const uint32_t nv = u(18), nt = u(19);
        const ORef vend = bg::res(p, pg, off + 0x54);
        std::optional<uint64_t> vsize;
        if (vend && vend->first == rv.first) vsize = vend->second - rv.second;
        const uint64_t stride = (nv && vsize && *vsize && *vsize / nv >= 40) ? 40 : 4;
        const uint64_t va = p.pbase(rv.first) + rv.second;
        const uint64_t vs = std::min<uint64_t>(va, p.d.size()), ve = std::min<uint64_t>(va + uint64_t(nv) * stride, p.d.size());
        const Bytes vb(p.d.begin() + std::ptrdiff_t(vs), p.d.begin() + std::ptrdiff_t(std::max(vs, ve)));
        if (stride == 4)
            bd.verts = vb;
        else
            for (uint32_t k = 0; k < nv; k++)
                for (int j = 0; j < 10; j++)
                    for (int c = 0; c < 4; c++) bd.verts.push_back(vb.at(40ull * k + 4 * j + uint64_t(VPERM[j][c] - '0')));
        const uint64_t ib = p.pbase(ri.first) + ri.second;
        if (stride == 4)
            for (int k = 0; k < 8; k++)
                for (int c = 3; c >= 0; c--) bd.idx.push_back(p.d.at(ib + 4ull * k + c));
        else
            for (uint64_t k = 0; k < 3ull * nt; k++) {
                bd.idx.push_back(p.d.at(ib + 2 * k + 1));
                bd.idx.push_back(p.d.at(ib + 2 * k));
            }
        body.push_back(std::move(bd));
    }
    const size_t objstart = B->size();
    for (const Body &bd : body) B->raw(bd.out);
    while (B->size() % 16) B->u32(PAD32);
    for (size_t k = 0; k < body.size(); k++) {
        B->pad(16);
        const size_t vo = B->size();
        B->raw(body[k].verts);
        const size_t io = B->size();
        B->raw(body[k].idx);
        const size_t o = objstart + 0x108 * k;
        set64(B->b, o + 0x50, 0);
        set64(B->b, o + 0x60, 0);
        B->relocs.push_back({o + 0x50, B, int64_t(vo)});
        B->relocs.push_back({o + 0x60, B, int64_t(io)});
    }
    return B;
}

// ------------------------------------------------------------------------------------------------ paramfix ('mb')
// neutralise the foreign constants a fallback template brings: for params the PS3 material does not set itself, on
// non-exact prime-shader materials (not sky, not a same-file template): colour tints / brightness 1, uv scale 1, uv
// offset / scroll 0, distortion 0, facing / border tint 1, rim 0, incandescence 0 (unless emissive by name). A value
// array shared with a material that must keep it is not touched.
Bytes paramfix(Bytes d, const Plan &plan) {
    const Pak q(d, "stage");
    const auto gs = q.items_of("GEOMETRY_1");
    if (gs.empty()) return d;
    const uint32_t pg = gs[0].page;
    const uint64_t b = gs[0].data_off();
    auto S = [&](uint32_t a, uint64_t o) {
        const ORef r = bg::res(q, a, o);
        return r ? q.pstr(r->first, r->second) : std::string();
    };
    const uint64_t nmat = q.u64(q.pbase(pg) + b + 0x48);
    const ORef mr = bg::res(q, pg, b + 0x50);
    auto digit_end = [](const std::string &s, const std::string &pre) { return s.size() == pre.size() + 1 && starts_with(s, pre) && std::isdigit((unsigned char)s.back()); };
    auto rule = [&](const std::string &pn) -> std::optional<float> {
        if (digit_end(pn, "g_colorTint") || digit_end(pn, "g_colorBrightness") || digit_end(pn, "g_uvScale")) return 1.0f;
        if (digit_end(pn, "g_uvOffsetU") || digit_end(pn, "g_uvOffsetV") || digit_end(pn, "g_timeScrollU") || digit_end(pn, "g_timeScrollV")) return 0.0f;
        if (digit_end(pn, "g_distortionIntensity") || pn == "g_distortionStrength") return 0.0f;
        if (digit_end(pn, "g_facingTintIn") || digit_end(pn, "g_facingTintOut") || pn == "g_diffuseColorBorderTint") return 1.0f;
        if (digit_end(pn, "g_rim") || pn == "g_incandescenceStrength") return 0.0f;
        return std::nullopt;
    };
    static const std::regex EMISSIVE("lamp|light|fire|glow|neon|lava|candle|torch|screen|incand|emissive|bulb|sign|flame|ember|sun");
    std::set<uint64_t> keep_off;
    struct Todo {
        uint64_t off;
        uint32_t cnt;
        float val;
    };
    std::vector<Todo> todo;
    for (uint64_t i = 0; i < (mr ? nmat : 0); i++) {
        const uint32_t mp = mr->first;
        const uint64_t mo = mr->second + 0x48 * i;
        const std::string name = S(mp, mo), shader = S(mp, mo + 8);
        const uint32_t npar = rd32le(q.d, q.pbase(mp) + mo + 0x34);
        const ORef pr = bg::res(q, mp, mo + 0x20);
        std::string k = name.substr(0, name.find(','));
        std::string n3 = k;
        if (k.find(':') != std::string::npos) std::replace(n3.begin(), n3.end(), ':', '/');
        const PlanMat *e = plan.get(n3);
        const size_t sl = n3.rfind('/');
        const bool samefile = e && e->key.substr(0, e->key.find(':')) == (sl == std::string::npos ? n3 : n3.substr(0, sl));
        std::string kl = k;
        for (char &c : kl) c = char(std::tolower((unsigned char)c));
        const bool frozen = shader != "prime-shader.fx" || kl.find("sky") != std::string::npos || (e && e->exact) || samefile;
        std::set<std::string> own;
        if (e)
            for (const auto &[pk, pv] : e->ps3params) own.insert(pk);
        std::string em = n3;
        if (n3.find('/') != std::string::npos) {   // n3.rsplit('/', 2)[-2]
            const size_t a = n3.rfind('/');
            const size_t a2 = a == 0 ? std::string::npos : n3.rfind('/', a - 1);
            em = a2 == std::string::npos ? n3.substr(0, a) : n3.substr(a2 + 1, a - a2 - 1);
        }
        const bool emissive = std::regex_search(em, EMISSIVE);
        for (uint32_t kk = 0; kk < (pr ? npar : 0); kk++) {
            const uint64_t po = pr->second + 0x18 * kk;
            const std::string pn = S(pr->first, po);
            const ORef vr = bg::res(q, pr->first, po + 8);
            const uint32_t cnt = rd32le(q.d, q.pbase(pr->first) + po + 0x10);
            if (!(vr && cnt > 0 && cnt < 64)) continue;
            const uint64_t off = q.pbase(vr->first) + vr->second;
            std::optional<float> val;
            if (!frozen && !own.count(pn)) val = rule(pn);
            if (pn == "g_incandescenceStrength" && emissive) val.reset();
            if (!val) keep_off.insert(off);
            else todo.push_back({off, cnt, *val});
        }
    }
    bool changed = false;
    Bytes out = d;
    for (const Todo &t : todo) {
        if (keep_off.count(t.off)) continue;
        std::vector<uint32_t> old(t.cnt), nw(t.cnt);
        bool diff = false;
        for (uint32_t j = 0; j < t.cnt; j++) {
            old[j] = rd32le(out, t.off + 4 * j);
            float ov;
            std::memcpy(&ov, &old[j], 4);
            const float v = j < 3 ? t.val : ov;
            nw[j] = j < 3 ? f32b(double(t.val)) : quiet(old[j]);
            if (j < 3 && std::fabs(double(ov) - double(v)) > 1e-6) diff = true;
        }
        if (diff) {
            for (uint32_t j = 0; j < t.cnt; j++) set32(out, t.off + 4 * j, nw[j]);
            changed = true;
        }
    }
    return changed ? out : d;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ cube maps
std::optional<Bytes> cube_texels(const PakTexture &t3, const Bytes &data, int bpb) {
    const uint32_t w = t3.width, h = t3.height, mips = t3.mips;
    const uint64_t face = tc::ps3_linear_size(w, h, mips, bpb), stride = (face + 127) / 128 * 128;
    if (data.size() < stride * 5 + face) return std::nullopt;
    Bytes out;
    uint64_t o = 0;
    for (auto [mw, mh] : tc::mip_dims(w, h, mips)) {
        auto [bw, bh] = tc::blocks(mw, mh, true);
        std::vector<Bytes> tiles;
        for (int k = 0; k < 6; k++) {
            const uint64_t a = k * stride + o, n = uint64_t(bw) * bh * bpb;
            const uint64_t s = std::min<uint64_t>(a, data.size()), e = std::min<uint64_t>(a + n, data.size());
            tiles.push_back(tc::tile_mip(data.data() + s, e > s ? e - s : 0, bw, bh, bpb));
        }
        for (int k = 0; k < 8; k++) out.insert(out.end(), tiles[std::min(k, 5)].begin(), tiles[std::min(k, 5)].end());
        o += uint64_t(bw) * bh * bpb;
    }
    return out;
}

std::optional<Bytes> cube_texels_argb(const PakTexture &t3, const Bytes &data, uint32_t remap, bool sky) {
    const uint32_t w = t3.width, h = t3.height, mips = t3.mips;
    std::vector<uint64_t> sizes;
    for (uint32_t m = 0; m < mips; m++) sizes.push_back(uint64_t(std::max(1u, m < 32 ? w >> m : 0)) * std::max(1u, m < 32 ? h >> m : 0) * 4);
    uint64_t face = 0;
    for (uint64_t s : sizes) face += s;
    const uint64_t stride = (face + 127) / 128 * 128;
    if (data.size() < stride * 5 + face) return std::nullopt;
    Bytes out;
    uint64_t mo = 0;
    for (uint32_t m = 0; m < mips; m++) {
        const uint32_t mw = std::max(1u, m < 32 ? w >> m : 0), mh = std::max(1u, m < 32 ? h >> m : 0);
        const uint32_t bw = (mw + 3) / 4, bh = (mh + 3) / 4;
        std::vector<Bytes> tiles;
        for (int k = 0; k < 6; k++) {
            const uint64_t a = k * stride + mo;
            const Bytes face_d(data.begin() + std::ptrdiff_t(std::min<uint64_t>(a, data.size())), data.begin() + std::ptrdiff_t(std::min<uint64_t>(a + sizes[m], data.size())));
            const Bytes bc = sky ? tc::sky::encode_bc3(tc::sky::ps3_deswizzle_argb8(face_d, mw, mh, remap)) : tc::encode_bc3(tc::ps3_deswizzle_argb8(face_d, mw, mh, remap));
            tiles.push_back(tc::tile_mip(bc.data(), bc.size(), bw, bh, 16));
        }
        for (int k = 0; k < 8; k++) out.insert(out.end(), tiles[std::min(k, 5)].begin(), tiles[std::min(k, 5)].end());
        mo += sizes[m];
    }
    return out;
}

namespace {

// cubefix ('mb'): descs copied from a retail template keep the retail cube texels and +0x20 0x060f0000 but type 1:
// set type 2; then every resident type-1 desc whose short path is a PS3 cube map gets retail-layout cube texels
Bytes cubefix(Bytes d, const fs::path &ps3) {
    {
        const Pak q(d, "stage");
        bool fixed = false;
        for (const PakTexture &t : q.textures())
            if (t.type == 1 && (t.unk20 >> 24) == 6) {
                set32(d, q.pbase(t.page) + t.off + 0x20 + 0x18, 2);
                fixed = true;
            }
        (void)fixed;
    }
    const Pak q4(d, "stage"), q3(ps3);
    std::map<std::string, PakTexture> cubes3;
    for (const PakTexture &t : q3.textures())
        if (t.type == 2) cubes3[short_of(t.path)] = t;
    Bytes extra;
    const uint64_t vbase_len = q4.h[8];
    bool any = false;
    for (const PakTexture &t : q4.textures()) {
        if (t.type != 1 || (t.unk34 != 2 && t.unk34 != 3)) continue;
        auto ci = cubes3.find(short_of(t.path));
        if (ci == cubes3.end()) continue;
        const PakTexture &t3 = ci->second;
        const auto f = tc::ps3_format(t3);
        if (!f) continue;
        std::string kind = f->kind;
        std::optional<Bytes> tex;
        if (f->compressed) tex = cube_texels(t3, q3.vram_bytes(t3.vramOffset, t3.vramSize), f->bpb);
        else if (kind == "A8R8G8B8") {
            tex = cube_texels_argb(t3, q3.vram_bytes(t3.vramOffset, t3.vramSize), t3.gcmRemapCtl & 0xffff);
            kind = "DXT5";
        } else
            continue;
        if (!tex) continue;
        while ((vbase_len + extra.size()) % 0x100) extra.push_back(0);
        const uint64_t voff = vbase_len + extra.size();
        extra.insert(extra.end(), tex->begin(), tex->end());
        const uint64_t db = q4.pbase(t.page) + t.off + 0x20;
        const uint32_t fmt4 = kind == "DXT1" ? 0x48 : kind == "DXT5" ? 0x4e : 0x4b;
        set64(d, db + 8, voff);
        set32(d, db + 0x10, uint32_t(tex->size()));
        const uint32_t vals[6] = {2, fmt4, 0x060f0000, t3.mips, t3.width, t3.height};
        for (int k = 0; k < 6; k++) set32(d, db + 0x18 + 4 * k, vals[k]);
        // the GEOMETRY_1 texture entry {hash, size class, 0x101, -1, 0, cls, 0}
        const uint8_t hb[4] = {uint8_t(t.hash), uint8_t(t.hash >> 8), uint8_t(t.hash >> 16), uint8_t(t.hash >> 24)};
        for (auto it = std::search(d.begin(), d.end(), hb, hb + 4); it != d.end(); it = std::search(it + 1, d.end(), hb, hb + 4)) {
            const size_t i = size_t(it - d.begin());
            if (i + 16 > d.size()) throw Stop("struct.error: texture entry past the end");
            if (rd32le(d, i + 8) == 0x101 && rd32le(d, i + 12) == 0xffffffffu && int64_t(i) < q4.vram_base) {
                set32(d, i + 4, 0x100);
                set32(d, i + 0x14, fmt4 == 0x48 ? 4 : 5);
            }
        }
        any = true;
    }
    if (!any) return d;
    d.insert(d.end(), extra.begin(), extra.end());
    set32(d, 0x20, uint32_t(vbase_len + extra.size()));
    return d;
}

}  // namespace

namespace {

// ps4stage.convert: pass 1 measures the longest length of every uniform per-vertex value, pass 2 writes the pak
Bytes convert_two_pass(const fs::path &ps3, const std::string &lv, const Plan &plan, const Bundle &bundle, const Options &o) {
    const Pak p(ps3);
    Uni U;
    U.collecting = true;
    convert_once(p, lv, plan, bundle, o, U, false);
    U.plan = U.collect;
    U.collecting = false;
    U.bufs.clear();
    U.seen.clear();
    Bytes data = convert_once(p, lv, plan, bundle, o, U, true);
    for (auto &[k, b] : U.bufs) b->relocs.clear();
    return data;
}

}  // namespace

Bytes stage_convert(const fs::path &ps3, const Options &o) {
    Plan plan = make_plan(ps3, o.res);
    const std::string lv = plan.stage;
    fs::create_directories(o.plan_dir);
    {
        const std::string pj = plan_json(plan);
        write_text(o.plan_dir / upath(lv + ".plan.json"), pj);   // as json.dump wrote it (text mode)
    }
    Bundle bundle = extract(o.pak_dir, plan);
    if (o.gobo && o.gobo->kind == json::Value::Object) {
        // gobo textures of another level (co-op Syria): the retail PS4 copy goes into the bundle; 'gobo_tex' maps the
        // PS3 hash the projector light names to it
        for (const auto &[pk, m] : o.gobo->fields) {
            const Pak q(o.pak_dir / upath(pk));
            std::map<std::string, PakTexture> tx;
            for (const PakTexture &t : q.textures()) tx[short_of(t.path)] = t;
            for (const auto &[hx, s_] : m.fields) {
                auto ti = tx.find(s_.s);
                if (ti == tx.end()) throw Stop("KeyError: gobo texture " + s_.s);
                const PakTexture &t = ti->second;
                const uint64_t b_ = q.pbase(t.page) + t.off + 0x20;
                BundleTex bt;
                bt.pak = pk;
                bt.path = t.path;
                bt.desc = Bytes(q.d.begin() + std::ptrdiff_t(b_), q.d.begin() + std::ptrdiff_t(std::min<uint64_t>(b_ + 0x40, q.d.size())));
                bt.texels = q.vram_bytes(t.vramOffset, t.vramSize);
                bundle.tex.push_back(std::move(bt));
                auto gi = std::find_if(bundle.gobo_tex.begin(), bundle.gobo_tex.end(), [&](const auto &g) { return g.first == hx; });
                if (gi == bundle.gobo_tex.end()) bundle.gobo_tex.push_back({hx, s_.s});
                else gi->second = s_.s;
            }
        }
    }
    Bytes data = convert_two_pass(ps3, lv, plan, bundle, o);
    if (o.mapbuild) {
        data = paramfix(std::move(data), plan);
        data = cubefix(std::move(data), ps3);
    }
    return data;
}

}  // namespace u3::stage

namespace u3::stage {
namespace {

// build_skies.py: the retail sky-clouds.fx templates and, per PS3 sky material, the template and the PS3 texture
// (by the material's texture order) for each template binding
const std::map<std::string, std::pair<std::string, std::string>> &sky_templates() {
    static const std::map<std::string, std::pair<std::string, std::string>> T = {
        {"SUN", {"sky-chateau-multi.pak", "levels/sky/-big3/sky-chateau-multi:default"}},
        {"PLAIN", {"sky-highrise-multi.pak", "levels/sky/-big3/mp-coop/sky-highrise-multi:default"}},
        {"UNDER", {"sky-grave-03-docks.pak", "levels/sky/-big3/sky-grave-03-docks-undersky:default"}},
        {"DIG", {"sky-dig-coop.pak", "levels/sky/-big3/mp-coop/sky-dig-coop:default"}},
        {"LW", {"sky-london-wide.pak", "levels/sky/-big3/sky-london-wide:default"}},
        {"LWM", {"sky-london-wide.pak", "levels/sky/-big3/sky-london-wide-moon:default"}},
        {"GR", {"sky-grave-03-docks.pak", "levels/sky/-big3/sky-grave-03-docks:default"}},
        {"HC", {"sky-horse-chase.pak", "levels/sky/-big3/sky-horse-chase:default"}},
        {"SYN", {"sky-syria.pak", "levels/sky/-big3/sky-syria-night:default"}}};
    return T;
}
struct SkyMapEntry {
    std::string lv, tail, t;
    std::vector<std::pair<std::string, int>> bm;
};
const std::vector<SkyMapEntry> &sky_map() {
    const std::string S = "g_tSkyColorMap", R = "g_tNdFetchSkyColorReflectionMap", CM = "g_tNdFetchCloudMaskMap", CM2 = "g_tNdFetchCloudMask2Map",
                      CC = "g_tNdFetchCloudColorMap", CC2 = "g_tNdFetchCloudColor2Map", F = "g_tFadeMaskMap", FL = "g_tNdFetchFlowTextureMap",
                      DM = "g_tNdDistortionMaskMap", MC = "g_tNdFetchMoonColorMap", MM = "g_tNdFetchMoonMaskMap", CYL = "g_tNdFetchCylinderCloudMaskMap",
                      NOI = "g_tNdFetchUvNoiseColorMap";
    const std::vector<std::pair<std::string, int>> LWB = {{S, 0}, {CM, 1}, {CM2, 2}, {F, 3}, {R, 4}}, SR = {{S, 0}, {R, 1}};
    static const std::vector<SkyMapEntry> M = {
        {"sky-air-multi", "sky-airport-multi", "SUN", SR},
        {"sky-cave-multi-2", "sky-cave-multi", "LW", LWB},
        {"sky-fort-coop-b", "sky-fort-coop-b", "SUN", SR},
        {"sky-fort-multi", "sky-fort-multi", "DIG", {{S, 0}, {CC, 1}, {CM, 2}, {CC2, 1}, {CM2, 3}, {F, 4}, {R, 5}}},
        {"sky-gompa-mp", "sky-gompa-multi-3", "LWM", {{S, 0}, {CM, 1}, {CM2, 2}, {F, 3}, {R, 4}, {MC, 5}, {MM, 6}}},
        {"sky-gompa-mp", "sky-gompa-multi-moon", "LWM", {{S, 0}, {CM, 1}, {CM2, 2}, {F, 3}, {MC, 4}, {MM, 5}, {R, 6}}},
        {"sky-grave-multi", "sky-grave-multi", "GR", {{S, 0}, {CM, 1}, {CM2, 2}, {F, 3}, {FL, 4}, {R, 5}}},
        {"sky-grave-multi", "sky-grave-multi-undersky", "UNDER", SR},
        {"sky-ldn-streets-multi", "sky-ldn-streets-multi", "LW", LWB},
        {"sky-mon-coop-b", "sky-mon-coop-b", "SUN", SR},
        {"sky-oasis-multi", "sky-oasis-multi", "HC", {{S, 0}, {CM, 1}, {CM2, 1}, {F, 2}, {FL, 3}, {DM, 4}, {R, 5}}},
        {"sky-ruins-multi", "sky-ruins-multi", "PLAIN", SR},
        {"sky-sham-multi", "sky-sham-multi", "LW", LWB},
        {"sky-temple-multi", "sky-temple-multi", "SUN", SR},
        {"sky-train-wreck-multi", "sky-train-wreck-multi", "LW", LWB},
        {"sky-village-multi", "sky-village-multi-2", "LW", LWB},
        {"sky-warzone-multi", "sky-warzone-multi", "LW", LWB},
        {"sky-yemen-mp", "sky-yemen-multi", "DIG", {{S, 0}, {CC, 1}, {CM, 2}, {CC2, 1}, {CM2, 3}, {F, 4}, {R, 6}}},
        {"sky-drydock", "sky-drydock-special", "GR", {{S, 0}, {CM, 1}, {CM2, 2}, {F, 3}, {FL, 4}, {R, 5}}},
        {"sky-old-quarter", "sky-old-quarter", "SYN", {{CM, 0}, {CM2, 1}, {F, 2}, {CYL, 3}, {NOI, 4}, {R, 5}}}};
    return M;
}

std::mutex g_sky_lock;
std::map<std::string, std::shared_ptr<const Bundle>> g_sky_bundles;   // the one shared template bundle, per PS4 pak dir

std::shared_ptr<const Bundle> sky_bundle(const fs::path &pak_dir) {
    std::lock_guard<std::mutex> lock(g_sky_lock);
    auto &b = g_sky_bundles[utf8(pak_dir)];
    if (!b) {
        std::vector<std::pair<std::string, std::string>> mats;
        std::vector<std::string> techs;
        for (const SkyMapEntry &e : sky_map()) {
            const auto &t = sky_templates().at(e.t);
            if (std::find(mats.begin(), mats.end(), t) == mats.end()) mats.push_back(t);
            if (std::find(techs.begin(), techs.end(), t.first) == techs.end()) techs.push_back(t.first);
        }
        b = std::make_shared<const Bundle>(extract_req(pak_dir, mats, {}, techs));
    }
    return b;
}

std::vector<std::string> slash_parts(const std::string &s) {
    std::vector<std::string> v;
    size_t a = 0;
    for (size_t k = 0;; k++)
        if (k == s.size() || s[k] == '/') {
            v.push_back(s.substr(a, k - a));
            if (k == s.size()) break;
            a = k + 1;
        }
    return v;
}

}  // namespace

Bytes sky_convert(const fs::path &ps3, const std::string &lv, const Options &o, const fs::path &water_templates) {
    Plan P = make_plan(ps3, o.res, water_templates);
    {   // skyplan: the level's sky materials re-pointed to their template, a PS3 texture per binding
        const Pak p(ps3);
        std::vector<bg::Material3> m3;
        std::vector<bg::Texture3> t3;
        bg::read_materials(p, m3, t3);
        static const std::regex HT(R"(/[0-9A-F]{16}\.ndb(\.error)?$)");
        for (const SkyMapEntry &e : sky_map()) {
            if (e.lv != lv) continue;
            const bg::Material3 *m = nullptr;
            for (const auto &x : m3) {
                const std::vector<std::string> parts = slash_parts(x.name);
                if (parts.size() < 2) throw Stop("IndexError: list index out of range");
                if (parts[parts.size() - 2] == e.tail) {
                    m = &x;
                    break;
                }
            }
            if (!m) throw Stop("IndexError: no sky material " + e.tail);
            std::vector<std::string> sh;
            for (uint32_t i : m->textures) sh.push_back(std::regex_replace(short_of(t3.at(i).name), HT, ""));
            auto pi = std::find_if(P.mats.begin(), P.mats.end(), [&](const auto &x) { return x.first == m->name; });
            if (pi == P.mats.end()) throw Stop("KeyError: " + m->name);
            PlanMat &pe = pi->second;
            const auto &t = sky_templates().at(e.t);
            pe.key = t.second;
            pe.pak = t.first;
            pe.exact = false;
            pe.has_bindmap = true;
            pe.bindmap.clear();
            for (const auto &[b, i] : e.bm)
                if (size_t(i) < sh.size()) {
                    auto bi = std::find_if(pe.bindmap.begin(), pe.bindmap.end(), [&](const auto &x) { return x.first == b; });
                    if (bi == pe.bindmap.end()) pe.bindmap.push_back({b, sh[size_t(i)]});
                    else bi->second = sh[size_t(i)];
                }
        }
    }
    fs::create_directories(o.plan_dir);
    write_text(o.plan_dir / upath(lv + ".plan.json"), plan_json(P));
    // the ONE shared sky bundle (all templates), plus the level's other materials from their retail paks
    Bundle B = *sky_bundle(o.pak_dir);
    std::vector<std::pair<std::string, std::string>> need;
    std::set<std::string> paks;
    for (const auto &[n, e] : P.mats)
        if (!e.has_bindmap) {
            need.push_back({e.pak, e.key});
            paks.insert(e.pak);
        }
    if (!need.empty()) {
        Bundle X = extract_req(o.pak_dir, need, {}, std::vector<std::string>(paks.begin(), paks.end()));
        if (lv == "sky-drydock") {
            // its exact materials were taken from the release pak itself, so their technique table is that pak's:
            // techs(the sky template donor) + the material pak's own; the EFFECT string indices re-indexed to it
            const std::vector<std::string> D = pak_techs(o.pak_dir / "ldn-atwood-station.pak");
            for (auto &[k, rec] : X.mats) {
                const std::vector<std::string> src = X.techs.at(k.first);
                std::vector<std::string> T = D;
                for (const std::string &s : src)
                    if (std::find(D.begin(), D.end(), s) == D.end()) T.push_back(s);
                std::map<uint32_t, uint32_t> mp;
                for (size_t i = 0; i < src.size(); i++) mp[uint32_t(i)] = uint32_t(std::find(T.begin(), T.end(), src[i]) - T.begin());
                if (rec.fx.empty()) throw Stop("IndexError: no effect closure");
                std::map<uint64_t, std::optional<int64_t>> ptrs;
                for (const auto &pp : rec.fx[0].ptrs) ptrs[pp.pos] = pp.target == 0 ? std::optional<int64_t>(pp.off) : std::nullopt;
                rec.fx[0].data = remap(rec.fx[0].data, ptrs, mp);
                X.techs[k.first] = T;
            }
        }
        for (const auto &[k, v] : X.techs) B.techs[k] = v;
        for (auto &m : X.mats) B.mats.push_back(std::move(m));
        for (auto &t : X.tex) B.tex.push_back(std::move(t));
    }
    Options so = o;
    so.sky = true;
    so.mapbuild = false;
    Bytes data = convert_two_pass(ps3, lv, P, B, so);
    return paramfix(std::move(data), P);
}

}  // namespace u3::stage

namespace u3::stage {

Bytes cubefix_pak(const Bytes &d, const fs::path &ps3) { return cubefix(d, ps3); }

}  // namespace u3::stage
