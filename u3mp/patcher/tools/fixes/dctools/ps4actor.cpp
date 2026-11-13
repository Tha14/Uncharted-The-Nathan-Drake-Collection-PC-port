// dctools/ps4actor.cpp: see ps4actor.hpp.
#include "ps4actor.hpp"

#include "anim.hpp"
#include "fgcoll.hpp"
#include "ps3bg.hpp"
#include "ps4writer.hpp"
#include "skel.hpp"
#include "texcodec.hpp"
#include "u3fix.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <regex>
#include <set>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace u3::actor {
namespace {

const uint32_t PAD32 = 0xaaaaaaaa;
const size_t PAGE_LIMIT = 0x180000;   // (a margin: tables filled after item() grow past the estimate)
const std::string PS4_COOK_PREFIX = "d:/dev/CP9/Data/U3/CookData/ND/build/texture2/";
const std::string PS3_COOK_MARK = "texture1/";

uint32_t quiet(uint32_t v) { return ((v & 0x7f800000u) == 0x7f800000u && (v & 0x7fffffu)) ? v | 0x400000u : v; }
uint32_t f32b(double d) {
    const float f = float(d);
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return quiet(v);
}
uint16_t f16b(double v);   // numpy float64 -> float16 (below)

std::string u8(const std::string &latin1) { return bg::utf8_of(latin1); }
uint32_t sidu(const std::string &latin1) { return sid(u8(latin1)); }
bool has(const std::string &s, const std::string &x) { return s.find(x) != std::string::npos; }
bool ends_with(const std::string &s, const std::string &e) { return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; }
std::string before(const std::string &s, const std::string &sep) { return s.substr(0, s.find(sep)); }   // s.split(sep)[0]
std::string rsplit1_head(const std::string &s) {   // s.rsplit('/', 1)[0]
    const size_t k = s.rfind('/');
    return k == std::string::npos ? s : s.substr(0, k);
}
std::string rsplit1_tail(const std::string &s) {   // s.rsplit('/', 1)[1] (IndexError without a '/')
    const size_t k = s.rfind('/');
    if (k == std::string::npos) throw Stop("IndexError: list index out of range");
    return s.substr(k + 1);
}
std::string ps4key(const std::string &mn) { return rsplit1_head(mn) + ":" + rsplit1_tail(mn); }
std::string lower(std::string s) {
    for (char &c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

void put16(Bytes &o, uint32_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); }
void put32(Bytes &o, uint32_t v) { for (int k = 0; k < 4; k++) o.push_back(uint8_t(v >> (8 * k))); }
void set32(Bytes &o, size_t at, uint32_t v) { for (int k = 0; k < 4; k++) o.at(at + k) = uint8_t(v >> (8 * k)); }
void set64(Bytes &o, size_t at, uint64_t v) { for (int k = 0; k < 8; k++) o.at(at + k) = uint8_t(v >> (8 * k)); }

// ------------------------------------------------------------------------------------------------ PS3 side
std::vector<std::string> ps3_block_materials(const Pak &p, const Ref &proto) {
    const auto [pg, po] = proto;
    const uint32_t nmat = p.u16(p.pbase(pg) + po + 4), nblk = p.u16(p.pbase(pg) + po + 6);
    const Ref t = bg::need(p, pg, po + 0x10);
    std::vector<std::optional<std::string>> out(nblk);
    for (uint32_t s = 0; s < nmat; s++) {
        const uint64_t e = t.second + 0x10ull * s;
        const Ref sd = bg::need(p, t.first, e);
        const uint32_t first = bg::be16(p, p.pbase(t.first) + e + 4), cnt = bg::be16(p, p.pbase(t.first) + e + 6);
        const Ref m = bg::need(p, sd.first, sd.second);
        const Ref n = bg::need(p, m.first, m.second);
        const std::string nm = p.pstr(n.first, n.second);
        for (uint32_t k = first; k < first + cnt; k++) {
            if (k >= out.size()) throw Stop("IndexError: list assignment index out of range");
            out[k] = nm;
        }
    }
    std::vector<std::string> r;
    for (const auto &x : out) {
        if (!x) throw Stop("AssertionError: a block without a material");
        r.push_back(*x);
    }
    return r;
}

struct Tag {
    std::string type, name;
    Bytes data;
};
std::vector<Tag> read_tags(const Pak &p) {
    std::vector<Tag> out;
    for (const PakItem &it : p.items)
        if (it.type == "TAG_FLOAT" || it.type == "TAG_INT" || it.type == "TAG_VEC4") {
            const uint64_t b = p.pbase(it.page) + it.data_off();
            out.push_back({it.type, it.name, Bytes(p.d.begin() + std::ptrdiff_t(std::min<uint64_t>(b, p.d.size())), p.d.begin() + std::ptrdiff_t(std::min<uint64_t>(b + it.size, p.d.size())))});
        }
    return out;
}

struct Artgroup {
    std::string name;
    std::vector<std::optional<PakItem>> targets;
    uint32_t nb;
};
Artgroup ps3_artgroup(const Pak &p) {
    const auto ags = p.items_of("ARTGROUP_DATA");
    if (ags.empty()) throw Stop("IndexError: no ARTGROUP_DATA");
    const PakItem &it = ags[0];
    const uint32_t pg = it.page;
    const uint64_t b = it.data_off();
    const uint32_t n = p.pu32(pg, b);
    const uint64_t ents = bg::need(p, pg, b + 8).second;
    Artgroup a;
    a.name = it.name;
    for (uint32_t k = 0; k < n; k++) {
        const ORef r = bg::res(p, pg, ents + 8ull * k);
        a.targets.push_back(r ? std::optional<PakItem>(p.item_at(r->first, uint32_t(r->second))) : std::nullopt);
    }
    const Ref h2 = bg::need(p, pg, b + 0xc), x = bg::need(p, h2.first, h2.second + 0xc);
    a.nb = p.u32(p.pbase(x.first) + x.second + 0x10);
    return a;
}

// ------------------------------------------------------------------------------------------------ the PS4 template
struct TMat {
    std::string name, shader, hash;
    std::vector<std::string> feats;
    struct Param {
        std::string name;
        uint32_t cnt;
        Bytes vals;
    };
    std::vector<Param> params;
    struct Bind {
        std::string pname, shrt, full;
    };
    std::vector<Bind> binds;
    uint32_t x3, tail;
};
std::vector<TMat> tmpl_materials(const Pak &q) {
    const auto gs = q.items_of("GEOMETRY_1");
    if (gs.empty()) throw Stop("IndexError: no GEOMETRY_1");
    const uint32_t pg = gs[0].page;
    const uint64_t b = gs[0].data_off();
    const uint32_t n = q.pu32(pg, b + 0x48);
    const Ref mr = bg::need(q, pg, b + 0x50);
    const uint32_t mp = mr.first;
    std::vector<TMat> out;
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t o = mr.second + uint64_t(i) * 0x48;
        auto S = [&](uint64_t off) { const Ref r = bg::need(q, mp, o + off); return q.pstr(r.first, r.second); };
        const uint64_t mb = q.pbase(mp) + o;
        const uint32_t nf = rd32le(q.d, mb + 0x30), npar = rd32le(q.d, mb + 0x34), nb = rd32le(q.d, mb + 0x38);
        TMat m;
        m.x3 = rd32le(q.d, mb + 0x3c);
        m.tail = q.pu32(mp, o + 0x40);
        const ORef fr = bg::res(q, mp, o + 0x10);
        for (uint32_t k = 0; k < nf; k++) {
            const Ref f = bg::need(q, bg::need(fr).first, bg::need(fr).second + 8ull * k);
            m.feats.push_back(q.pstr(f.first, f.second));
        }
        const ORef pr = bg::res(q, mp, o + 0x20);
        for (uint32_t k = 0; k < npar; k++) {
            const uint64_t po = bg::need(pr).second + 0x18ull * k;
            const Ref vr = bg::need(q, pr->first, po + 8);
            const Ref nm = bg::need(q, pr->first, po);
            const uint64_t va = q.pbase(vr.first) + vr.second;
            m.params.push_back({q.pstr(nm.first, nm.second), q.pu32(pr->first, po + 0x10),
                                Bytes(q.d.begin() + std::ptrdiff_t(std::min<uint64_t>(va, q.d.size())), q.d.begin() + std::ptrdiff_t(std::min<uint64_t>(va + 16, q.d.size())))});
        }
        const ORef br = bg::res(q, mp, o + 0x28);
        for (uint32_t k = 0; k < nb; k++) {
            const uint64_t bo = bg::need(br).second + 0x28ull * k;
            auto s = [&](uint64_t off) { const Ref r = bg::need(q, br->first, bo + off); return q.pstr(r.first, r.second); };
            m.binds.push_back({s(0), s(8), s(0x10)});
        }
        m.name = S(0);
        m.shader = S(8);
        m.hash = S(0x18);
        out.push_back(std::move(m));
    }
    return out;
}

struct FgInfo {
    uint32_t skel, hier, njoints;
    Bytes names;
};
// am: the joint count is read from the page the submesh array is on (the newer rules read GEOMETRY_1's page)
FgInfo tmpl_fg_info(const Pak &q, bool am = false) {
    const auto fs_ = q.items_of("FOREGROUND_SECTION_2");
    if (fs_.empty()) throw Stop("IndexError: no FOREGROUND_SECTION_2");
    const uint32_t pg = fs_[0].page;
    const uint64_t b = fs_[0].data_off();
    FgInfo f;
    f.skel = rd32le(q.d, q.pbase(pg) + b + 8);
    f.hier = rd32le(q.d, q.pbase(pg) + b + 12);
    const Ref jr = bg::need(q, pg, b + 0x10);
    const PakItem jitem = q.item_at(jr.first, uint32_t(jr.second - 0x20));
    const uint64_t a = q.pbase(jr.first) + jr.second;
    f.names = Bytes(q.d.begin() + std::ptrdiff_t(std::min<uint64_t>(a, q.d.size())), q.d.begin() + std::ptrdiff_t(std::min<uint64_t>(a + jitem.size, q.d.size())));
    const auto gs = q.items_of("GEOMETRY_1");
    if (gs.empty()) throw Stop("IndexError: no GEOMETRY_1");
    const Ref sm = bg::need(q, gs[0].page, gs[0].data_off() + 0x10);
    f.njoints = q.pu32(am ? sm.first : gs[0].page, sm.second + 0x58);
    return f;
}

// material name -> [(slot, stride, format bytes)] of the first template submesh that uses it
std::map<std::string, std::vector<std::tuple<uint32_t, uint32_t, Bytes>>> template_streams(const Pak &q) {
    const auto gs = q.items_of("GEOMETRY_1");
    if (gs.empty()) throw Stop("IndexError: no GEOMETRY_1");
    const uint32_t pg = gs[0].page;
    const uint64_t b = gs[0].data_off();
    const uint64_t nsub = q.u64(q.pbase(pg) + b + 8);
    const Ref sr = bg::need(q, pg, b + 0x10);
    std::map<std::string, std::vector<std::tuple<uint32_t, uint32_t, Bytes>>> out;
    for (uint64_t i = 0; i < nsub; i++) {
        const uint64_t o = sr.second + 0x88 * i;
        const Ref mr = bg::need(q, sr.first, o + 0x48);
        const std::string mat = q.pstr(mr.first, mr.second);
        const uint32_t ns = q.pu32(sr.first, o + 0x28);
        const Ref st = bg::need(q, sr.first, o + 0x30);
        std::vector<std::tuple<uint32_t, uint32_t, Bytes>> sl;
        for (uint32_t k = 0; k < ns; k++) {
            const uint64_t so = st.second + 0x18ull * k;
            const Ref f = bg::need(q, st.first, so + 8);
            const uint64_t fb = q.pbase(f.first) + f.second;
            if (fb + 4 > q.d.size()) throw Stop("IndexError: stream format past the end");
            sl.push_back({q.d[fb], q.pu32(st.first, so) >> 16, Bytes(q.d.begin() + std::ptrdiff_t(fb), q.d.begin() + std::ptrdiff_t(fb + 4))});
        }
        out.emplace(mat, sl);   // setdefault: the first submesh of a material
    }
    return out;
}

// retail material names (path -> 'path,GUID,shader') of every PS4 actor next to the template ('mp-*', 'hero*')
std::mutex g_rn_lock;
std::map<std::string, std::shared_ptr<std::map<std::string, std::string>>> g_retail_names;
std::shared_ptr<std::map<std::string, std::string>> retail_names(const fs::path &dir) {
    std::lock_guard<std::mutex> lock(g_rn_lock);
    auto &v = g_retail_names[utf8(dir)];
    if (v) return v;
    v = std::make_shared<std::map<std::string, std::string>>();
    std::vector<std::string> names;
    for (const auto &e : fs::directory_iterator(dir)) {
        const std::string fn = utf8(e.path().filename());
        if (ends_with(fn, ".pak") && (fn.rfind("mp-", 0) == 0 || fn.rfind("hero", 0) == 0)) names.push_back(fn);
    }
    std::sort(names.begin(), names.end());
    for (const std::string &fn : names) {
        try {
            for (const TMat &m : tmpl_materials(Pak(dir / upath(fn)))) v->emplace(before(m.name, ","), m.name);
        } catch (const std::exception &) {
        }
    }
    return v;
}

// ------------------------------------------------------------------------------------------------ helpers
// (name suffix, the bindings that take it), in matching order; am: the older list (hair alpha first, no hair shift)
const std::vector<std::pair<std::string, std::vector<std::string>>> &tex_kinds(bool am = false) {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> K = {
        {"-detail-normal", {"g_tNdFetchDetail3Map", "g_tNdFetchDetail1Map", "g_tNdFetchDetail2Map"}},
        {"-spec-mask", {"g_tNdFetchMask4Map"}},
        {"-tint-mask", {"g_tNdFetchColorTintMask1Map"}},
        {"-normal", {"g_tNdFetchNormal1Map", "g_tNdFetchNormal2Map"}},
        {"-color", {"g_tNdFetchColor1Map", "g_tNdFetchColor2Map"}},
        {"-shift", {"g_tNdFetchNoiseMap"}},     // hair anisotropic shift
        {"-trans", {"g_tNdFetchHairAlphaMap"}}};   // hair alpha (often missing on PS3: see donors)
    static const std::vector<std::pair<std::string, std::vector<std::string>>> KA = {
        {"-trans", {"g_tNdFetchHairAlphaMap"}},
        {"-detail-normal", {"g_tNdFetchDetail3Map", "g_tNdFetchDetail1Map", "g_tNdFetchDetail2Map"}},
        {"-spec-mask", {"g_tNdFetchMask4Map"}},
        {"-tint-mask", {"g_tNdFetchColorTintMask1Map"}},
        {"-normal", {"g_tNdFetchNormal1Map", "g_tNdFetchNormal2Map"}},
        {"-color", {"g_tNdFetchColor1Map", "g_tNdFetchColor2Map"}}};
    return am ? KA : K;
}
// am also knows the eye / teeth naming x_c / x_clr (colour) and x_n / x_nmt (normal)
std::optional<std::string> tex_kind(const std::string &shrt, bool am = false) {
    const std::string base = before(before(shrt, ".tga"), ".dds");
    for (const auto &[suf, ps] : tex_kinds(am))
        if (ends_with(base, suf) || has(base, suf + "-")) return suf;
    if (am && (ends_with(base, "_c") || ends_with(base, "_clr"))) return "-color";
    if (am && (ends_with(base, "_n") || ends_with(base, "_nmt"))) return "-normal";
    return std::nullopt;
}


std::string rsplit1_last(const std::string &s) {   // s.rsplit('/', 1)[-1]
    const size_t k = s.rfind('/');
    return k == std::string::npos ? s : s.substr(k + 1);
}

// ------------------------------------------------------------------------------------------------ am helpers
// The PS3 MATERIAL_TABLE_1 as the actor-materials rules read it: every pointer followed to its own page, up to six
// binding groups per material (group 0 is the material's own), groups of more than 32 bindings skipped, each texture
// once and missing textures left out.
struct AmTable {
    std::vector<std::pair<std::string, std::vector<uint32_t>>> mats;   // name, texture indices
    std::vector<std::pair<uint32_t, std::string>> texs;                 // hash, name
};
AmTable am_read_materials(const Pak &p) {
    AmTable t;
    const auto mts = p.items_of("MATERIAL_TABLE_1");
    if (mts.empty()) return t;
    const uint32_t pg = mts[0].page;
    const uint64_t b = mts[0].data_off();
    if (p.pu32(pg, b) != 0x79) throw Stop("ValueError: version");
    const uint32_t ntex = p.pu32(pg, b + 4), nmat = p.pu32(pg, b + 0xc);
    const Ref th = bg::need(p, pg, b + 0x18), ma = bg::need(p, pg, b + 0x20), na = bg::need(p, pg, b + 0x30);
    for (uint32_t i = 0; i < ntex; i++) {
        const Ref r = bg::need(p, na.first, na.second + 4ull * i);
        t.texs.push_back({p.pu32(th.first, th.second + 4ull * i), p.pstr(r.first, r.second)});
    }
    for (uint32_t i = 0; i < nmat; i++) {
        const Ref a = bg::need(p, ma.first, ma.second + 4ull * i);
        const Ref n = bg::need(p, a.first, a.second);
        std::pair<std::string, std::vector<uint32_t>> rec{p.pstr(n.first, n.second), {}};
        p.pu32(a.first, a.second + 0x18);
        p.pu32(a.first, a.second + 0xc);
        if (const ORef r = bg::res(p, a.first, a.second + 0x10)) {
            std::set<uint32_t> seen;
            for (uint32_t slot = 0; slot < 6; slot++) {
                const ORef node = bg::res(p, r->first, r->second + 4ull * slot);
                if (!node) continue;
                const ORef bb = bg::res(p, node->first, node->second + 0x10);
                if (!bb) continue;
                const uint32_t nb = p.pu32(bb->first, bb->second + 4);
                if (nb > 32) continue;
                for (uint32_t k = 0; k < nb; k++) {
                    const ORef rr = bg::res(p, bb->first, bb->second + 0x50 + 0x10ull * k);
                    if (!rr || rr->first != th.first || rr->second < th.second || rr->second >= th.second + 4ull * ntex) continue;
                    const uint32_t ti = uint32_t((rr->second - th.second) / 4);
                    if (!seen.count(ti) && !has(t.texs[ti].second, "missingtexture")) {
                        seen.insert(ti);
                        rec.second.push_back(ti);
                    }
                }
            }
        }
        t.mats.push_back(std::move(rec));
    }
    return t;
}
// material name -> the hashes of its textures (a repeated name: the last)
std::map<std::string, std::set<uint32_t>> am_material_textures(const Pak &p) {
    const AmTable t = am_read_materials(p);
    std::map<std::string, std::set<uint32_t>> out;
    for (const auto &[name, idx] : t.mats) {
        std::set<uint32_t> h;
        for (uint32_t i : idx) h.insert(t.texs[i].first);
        out[name] = h;
    }
    return out;
}

// float16 bits -> double
double h2d(uint16_t h) {
    const int e = (h >> 10) & 0x1f, m = h & 0x3ff;
    double v;
    if (e == 0) v = std::ldexp(double(m), -24);
    else if (e == 31) v = m ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity();
    else v = std::ldexp(double(m | 0x400), e - 25);
    return (h & 0x8000) ? -v : v;
}

// A block that lost its normals and tangents (the Yeti mask teeth): area-weighted triangle normals and UV-derived
// tangents from the float32 positions, float16 UVs and the triangles, no welding; unreferenced vertices get a unit
// frame; degenerate input stops the build. The result goes through float16 like the converted streams.
void rebuild_basis(bg::FgBlock &b, const std::map<std::pair<uint32_t, size_t>, std::string> &targets, std::set<std::pair<uint32_t, size_t>> &seen) {
    const bool no_n = !b.get("normal"), no_t = !b.get("tangent");
    if (no_n != no_t) throw Stop("ValueError: only one of the normal / tangent streams is missing");
    if (!no_n) return;
    const std::pair<uint32_t, size_t> sig{b.nverts, b.tris.size()};
    if (!targets.count(sig)) throw Stop(u3::fmt("ValueError: unexpected mesh without a basis (%u, %zu)", sig.first, sig.second));
    const Mat *pos = b.get("pos"), *uv0 = b.get("uv0");
    if (!pos || !uv0) throw Stop("KeyError: 'uv0'");
    const size_t nv = b.nverts;
    if (!nv || pos->rows != nv || uv0->rows != nv || b.tris.empty()) throw Stop("ValueError: invalid position / UV / triangle buffer");
    std::vector<std::array<double, 3>> P(nv);
    std::vector<std::array<double, 2>> U(nv);
    for (size_t i = 0; i < nv; i++) {
        for (int k = 0; k < 3; k++) P[i][k] = double(float(pos->at(i, k)));
        for (int k = 0; k < 2; k++) U[i][k] = h2d(f16b(uv0->at(i, k)));
        for (int k = 0; k < 3; k++)
            if (!std::isfinite(P[i][k])) throw Stop("ValueError: non-finite position");
        for (int k = 0; k < 2; k++)
            if (!std::isfinite(U[i][k])) throw Stop("ValueError: non-finite UV");
    }
    std::vector<char> referenced(nv, 0);
    for (const auto &tr : b.tris)
        for (uint32_t v : tr) {
            if (v > 0xffff || v >= nv) throw Stop("ValueError: invalid triangle index");
            referenced[v] = 1;
        }
    using V3 = std::array<double, 3>;
    auto cross = [](const V3 &a, const V3 &c) { return V3{a[1] * c[2] - a[2] * c[1], a[2] * c[0] - a[0] * c[2], a[0] * c[1] - a[1] * c[0]}; };
    auto dot = [](const V3 &a, const V3 &c) { return (a[0] * c[0] + a[1] * c[1]) + a[2] * c[2]; };
    std::vector<V3> n(nv, V3{0, 0, 0}), t(nv, V3{0, 0, 0}), bt(nv, V3{0, 0, 0});
    struct Tri {
        std::array<uint32_t, 3> v;
        V3 face, tan, bit;
        double area;
    };
    std::vector<Tri> good;
    for (const auto &tr : b.tris) {
        V3 e1, e2;
        for (int k = 0; k < 3; k++) e1[k] = P[tr[1]][k] - P[tr[0]][k], e2[k] = P[tr[2]][k] - P[tr[0]][k];
        const V3 face = cross(e1, e2);
        const double area = std::sqrt(dot(face, face));
        if (!(area > 1e-12)) continue;
        const double d1[2] = {U[tr[1]][0] - U[tr[0]][0], U[tr[1]][1] - U[tr[0]][1]}, d2[2] = {U[tr[2]][0] - U[tr[0]][0], U[tr[2]][1] - U[tr[0]][1]};
        const double det = d1[0] * d2[1] - d1[1] * d2[0];
        if (!(std::fabs(det) > 1e-12)) throw Stop("ValueError: degenerate UV triangle");
        Tri x{tr, face, {}, {}, area};
        for (int k = 0; k < 3; k++) {
            x.tan[k] = (e1[k] * d2[1] - e2[k] * d1[1]) / det;
            x.bit[k] = (e2[k] * d1[0] - e1[k] * d2[0]) / det;
        }
        good.push_back(x);
    }
    for (const Tri &x : good)   // np.add.at: triangle by triangle, vertex by vertex
        for (uint32_t v : x.v)
            for (int k = 0; k < 3; k++) n[v][k] += x.face[k];
    for (const Tri &x : good)
        for (uint32_t v : x.v)
            for (int k = 0; k < 3; k++) t[v][k] += x.tan[k] * x.area;
    for (const Tri &x : good)
        for (uint32_t v : x.v)
            for (int k = 0; k < 3; k++) bt[v][k] += x.bit[k] * x.area;
    for (size_t i = 0; i < nv; i++)
        if (!referenced[i]) n[i] = {0, 0, 1}, t[i] = {1, 0, 0}, bt[i] = {0, 1, 0};
    for (int pass = 0; pass < 2; pass++) {
        std::vector<V3> &vals = pass ? t : n;
        for (size_t i = 0; i < nv; i++) {
            if (pass) {
                const double d = dot(n[i], t[i]);
                for (int k = 0; k < 3; k++) t[i][k] -= n[i][k] * d;
            }
            const double len = std::sqrt(dot(vals[i], vals[i]));
            if (!std::isfinite(len) || !(len > 1e-12)) throw Stop("ValueError: degenerate basis or vertex without nondegenerate triangles");
            for (int k = 0; k < 3; k++) vals[i][k] /= len;
        }
    }
    Mat nm(nv, 3), tm(nv, 4);
    for (size_t i = 0; i < nv; i++) {
        const double hand = dot(cross(n[i], t[i]), bt[i]);
        if (!std::isfinite(hand) || !(std::fabs(hand) > 1e-12)) throw Stop("ValueError: ambiguous tangent handedness");
        for (int k = 0; k < 3; k++) {
            nm.at(i, k) = h2d(f16b(n[i][k]));
            tm.at(i, k) = h2d(f16b(t[i][k]));
        }
        tm.at(i, 3) = hand < 0 ? -1.0 : 1.0;
    }
    b.attr["normal"] = nm;
    b.attr["tangent"] = tm;
    seen.insert(sig);
}

// the actor-materials choice of a template material for a PS3 material without an exact match: by role (eyes,
// mouth, hair, skin of head / arms, cloth) and then the most texture kinds in common, fewest missing
const std::vector<std::pair<std::string, std::regex>> &am_special() {
    static const std::vector<std::pair<std::string, std::regex>> R = {
        {"eyelashes", std::regex("eyelash")}, {"brows", std::regex("brows")}, {"eyeoverlay", std::regex("eyeoverlay")},
        {"eyes", std::regex("(^|[/:-])eyes?([/:-]|$)")}, {"mouth", std::regex("mouth|teeth|tongue")}, {"hair", std::regex("hair")}};
    return R;
}
const std::vector<std::pair<std::string, std::regex>> &am_skin() {
    static const std::vector<std::pair<std::string, std::regex>> R = {{"head", std::regex("head|neck|face")}, {"arms", std::regex("arms|hands?|skin")}};
    return R;
}
std::string am_role(const std::string &name) {
    const std::string low = lower(name);
    if (has(low, "goggles")) return "cloth";
    for (const auto &[r, x] : am_special())
        if (std::regex_search(low, x)) return r;
    const size_t c = low.rfind("characters/");
    const std::string tail = c == std::string::npos ? low : low.substr(c + 11);
    for (const auto &[r, x] : am_skin())
        if (std::regex_search(tail, x)) return r;
    return "cloth";
}
std::vector<const TMat *> am_pool(const std::vector<TMat> &tmats, const std::string &r) {
    std::vector<const TMat *> main, out;
    for (const TMat &m : tmats)
        if (!has(m.name, "shadowblocker")) main.push_back(&m);
    for (const auto &[sr, x] : am_special())
        if (sr == r) {
            for (const TMat *m : main)
                if (std::regex_search(lower(before(m->name, ",")), x)) out.push_back(m);
            return out;
        }
    bool skin = false;
    for (const auto &[sr, x] : am_skin()) skin = skin || sr == r;
    if (skin) {
        for (const TMat *m : main)
            if (am_role(before(m->name, ",")) == r) out.push_back(m);
        if (!out.empty()) return out;
    }
    for (const TMat *m : main)
        if (am_role(before(m->name, ",")) == "cloth") out.push_back(m);
    return out;
}

// BC1 texels (PS3 linear order, all mips) of a constant colour
std::pair<Bytes, uint32_t> bc1_flat(uint32_t w, uint32_t h, const uint32_t rgb[3]) {
    const uint32_t c = ((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3);
    Bytes out;
    uint32_t mips = 0;
    while (true) {
        const uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
        for (uint32_t i = 0; i < bw * bh; i++) {
            put16(out, c);
            put16(out, c);
            put32(out, 0);
        }
        mips++;
        if (w == 1 && h == 1) break;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    return {out, mips};
}

// a Page.item / span look-alike that starts a new page when the current one fills up; the effect span is pinned to
// the template's page-0 offset, so page 0 may not get a longer entry table than the room before it
struct AutoPages {
    PakWriter &w;
    Page *cur;
    size_t size = 0x200;
    std::optional<size_t> max_entries;
    size_t nent = 0, limit;
    AutoPages(PakWriter &w_, std::optional<size_t> me, size_t lim = PAGE_LIMIT) : w(w_), cur(&w_.page()), max_entries(me), limit(lim) {}
    void new_page() {
        cur = &w.page();
        size = 0x200;
        max_entries.reset();
        nent = 0;
    }
    void room(size_t n, size_t ne) {
        if (size + n > limit && size > 0x200) new_page();
        if (max_entries && nent + ne > *max_entries) new_page();
        size += n;
        nent += ne;
    }
    std::shared_ptr<PakWItem> item(const std::string &type, const std::shared_ptr<Blob> &data = nullptr, const std::string &name = "", bool listed = true) {
        size_t est = (data ? data->size() : 0) + 0x40 + type.size() + name.size();
        if (!data || data->size() < 0x100) est += 0x400;   // items filled in later (tables) stay small
        room((est + 15) & ~size_t(15), listed ? 1 : 0);
        return cur->item(type, data, name, listed);
    }
    void span(const std::shared_ptr<RawSpan> &s) {
        room(s->data.size() + 0x1000, s->entries.size());
        cur->span(s);
    }
};

}  // namespace

// EFFECT_TABLE and every item it reaches (EFFECTs, RAW_DATA), as one RawSpan (convert_net.extract_effect_span)
std::pair<std::shared_ptr<RawSpan>, std::vector<PakItem>> extract_effect_span(const Pak &ref) {
    const auto ets = ref.items_of("EFFECT_TABLE");
    if (ets.empty()) throw Stop("IndexError: no EFFECT_TABLE");
    const PakItem et = ets[0];
    const uint32_t pg = et.page;
    std::vector<PakItem> items;
    for (const PakItem &i : ref.items)
        if (i.page == pg) items.push_back(i);
    std::stable_sort(items.begin(), items.end(), [](const PakItem &a, const PakItem &b) { return a.off < b.off; });
    auto container = [&](uint64_t off) -> const PakItem * {
        for (const PakItem &it : items)
            if (it.off <= off && off < uint64_t(it.off) + 0x20 + it.size) return &it;
        return nullptr;
    };
    std::map<uint32_t, PakItem> seen;
    std::vector<PakItem> todo = {et};
    while (!todo.empty()) {
        const PakItem it = todo.back();
        todo.pop_back();
        if (seen.count(it.off)) continue;
        seen[it.off] = it;
        for (uint64_t o = it.data_off(); o < uint64_t(it.data_off()) + it.size; o += 8) {
            const ORef r = bg::res(ref, pg, o);
            if (!r) continue;
            if (r->first != pg) throw Stop("ValueError: effect data points into another page");
            const PakItem *c = container(r->second);
            if (!c) throw Stop(u3::fmt("ValueError: effect pointer to loose data %#llx", (unsigned long long)r->second));
            todo.push_back(*c);
        }
    }
    std::vector<PakItem> span_items;
    for (const auto &[o, it] : seen) span_items.push_back(it);
    const uint64_t start = span_items.front().off;
    const PakItem &last = span_items.back();
    uint64_t end = uint64_t(last.data_off()) + last.size;
    const uint64_t base = ref.pbase(pg);
    auto find0 = [&](uint64_t from) -> uint64_t {   // ref.d.find(b'\0', from) (-1 when none)
        for (uint64_t k = from; k < ref.d.size(); k++)
            if (!ref.d[k]) return k;
        throw Stop("ValueError: no NUL after the effect span");
    };
    end = find0(base + end) - base + 1;   // the type string
    end = find0(base + end) - base + 1;   // the name string
    std::vector<uint32_t> inside, want;
    for (const PakItem &i : items)
        if (start <= i.off && i.off < end) inside.push_back(i.off);
    for (const PakItem &i : span_items) want.push_back(i.off);
    if (inside != want) throw Stop("ValueError: effect closure is not contiguous");
    auto span = std::make_shared<RawSpan>();
    span->data = Bytes(ref.d.begin() + std::ptrdiff_t(base + start), ref.d.begin() + std::ptrdiff_t(base + end));
    for (const Pak::Fixup &f : ref.fixups) {
        if (f.src != pg || f.off < start || f.off >= end) continue;
        if (f.dst != pg) throw Stop("ValueError: cross page pointer inside effect span");
        const uint64_t tgt = ref.pptr(pg, f.off);
        if (!(start <= tgt && tgt < end)) throw Stop("ValueError: effect pointer leaves span");
        span->internal.push_back({size_t(f.off - start), tgt - start});
    }
    for (const PakItem &i : span_items) span->headers.push_back({size_t(i.off - start), i.type});
    // retail ResItem name / type pointers are page relative -> span relative
    for (const auto &[ipos, ty] : span->headers) {
        set64(span->data, ipos, rd64le(span->data, ipos) - start);
        set64(span->data, ipos + 8, rd64le(span->data, ipos + 8) - start);
    }
    for (const auto &[pos, tgt] : span->internal) set64(span->data, pos, 0);
    for (const PakItem &i : span_items) span->entries.push_back({size_t(i.off - start), size_t(rd64le(span->data, i.off - start))});
    span->modulo = 0x1000;
    span->phase = start % 0x1000;
    return {span, span_items};
}

namespace {

// template item data as a Blob; its pointers must stay inside the item (JOINT_HIERARCHY / ANIM / collision of props)
std::shared_ptr<Blob> copy_item_blob(const Pak &T, const PakItem &it) {
    auto b = std::make_shared<Blob>(it.type);
    const uint64_t base = T.pbase(it.page);
    Bytes data(T.d.begin() + std::ptrdiff_t(base + it.data_off()), T.d.begin() + std::ptrdiff_t(std::min<uint64_t>(base + it.data_off() + it.size, T.d.size())));
    // the pointer map in Python's dict order: the first fixup of an address decides the position, the last its page
    std::vector<std::pair<uint32_t, uint32_t>> order;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> dst;
    for (const Pak::Fixup &f : T.fixups) {
        if (!dst.count({f.src, f.off})) order.push_back({f.src, f.off});
        dst[{f.src, f.off}] = f.dst;
    }
    std::vector<std::pair<size_t, int64_t>> relocs;
    for (const auto &k : order) {
        const auto [pg, o] = k;
        if (pg != it.page || o < it.data_off() || o >= uint64_t(it.data_off()) + it.size) continue;
        const uint64_t tgt = T.pptr(pg, o);
        if (dst[k] != it.page || !(it.data_off() <= tgt && tgt < uint64_t(it.data_off()) + it.size))
            throw Stop("ValueError: " + it.type + " " + it.name + " points outside itself");
        relocs.push_back({size_t(o - it.data_off()), int64_t(tgt - it.data_off())});
        set64(data, o - it.data_off(), 0);
    }
    b->raw(data);
    for (const auto &[pos, toff] : relocs) b->relocs.push_back({pos, b, toff});
    return b;
}

}  // namespace

// numpy float64 -> float16 (round half to even, overflow to inf, NaN payload kept)
namespace {
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
    return uint16_t(h_sgn + h_exp + uint16_t(d_sig >> 42));
}

struct TexE {
    uint32_t hash = 0, fmt = 0;
    std::string shrt, full;
    std::shared_ptr<PakWItem> desc, name;
    std::optional<std::string> kind;
};
using TexP = std::shared_ptr<TexE>;
struct Ps3Tex {   // a PS3 texture whose VRAM_DESC is made the first time a material binds it
    std::string shrt, full;
    uint32_t hash, fmt4, w, h, mips;
    std::optional<std::string> kind;
    Bytes tiled;
    TexP real;
};
// a binding's texture: a made texture, a lazy PS3 one, or none
struct BindTex {
    TexP e;
    Ps3Tex *lazy = nullptr;
    bool none() const { return !e && !lazy; }
};

}  // namespace

Bytes convert(const fs::path &ps3_path, const fs::path &tmpl_path, const Kw &kw, const Options &opt) {
    const Pak src(ps3_path), T(tmpl_path);
    if (!src.ps3 || T.ps3) throw Stop("AssertionError: expected a PS3 pak and a PS4 template");
    Artgroup ag = ps3_artgroup(src);
    const std::string art_old = ag.name, art = kw.rename ? *kw.rename : art_old;
    auto ren = [&](const std::string &s) {
        if (!kw.rename) return s;
        std::string r = s;
        if (!art_old.empty())
            for (size_t k = 0; (k = r.find(art_old, k)) != std::string::npos; k += art.size()) r.replace(k, art_old.size(), art);
        return r;
    };
    const std::vector<TMat> tmats = tmpl_materials(T);
    // the constant-buffer table capacities of the template prototypes (u32 at +0x30 / +0x34)
    uint32_t cb3_cap = 2, cb4_cap = 1;
    for (const PakItem &it : T.items)
        if (it.type == "RAW_DATA" && it.size >= 0x38) {
            const uint64_t o = T.pbase(it.page) + it.data_off();
            if (rd32le(T.d, o) == 3 && rd32le(T.d, o + 12) == 0x5791a421) {
                cb3_cap = std::max(cb3_cap, rd32le(T.d, o + 0x30));
                cb4_cap = std::max(cb4_cap, rd32le(T.d, o + 0x34));
            }
        }
    cb3_cap = std::max<uint32_t>(cb3_cap, uint32_t(opt.cb3_min));
    if (opt.am) cb3_cap = 2, cb4_cap = 1;   // am: the fixed retail-style 2 / 1 (cb3fix raises them afterwards)
    const auto rnames = opt.am ? std::make_shared<std::map<std::string, std::string>>() : retail_names(fs::absolute(tmpl_path).parent_path());
    FgInfo fi = kw.skel_tmpl ? tmpl_fg_info(Pak(*kw.skel_tmpl)) : tmpl_fg_info(T, opt.am);
    uint32_t skel_sid = fi.skel, hier_id = fi.hier, njoints = fi.njoints;
    Bytes joint_names = fi.names;
    const auto tstreams = kw.match_streams ? template_streams(T) : std::map<std::string, std::vector<std::tuple<uint32_t, uint32_t, Bytes>>>{};
    bool graft = kw.graft;
    std::optional<uint32_t> ps3_skel, t_skel;
    if (graft) {   // a grafted skeleton: the FG sections reference the PS3 skeleton id (patched into the copies)
        const auto jh3 = src.items_of("JOINT_HIERARCHY"), jh4 = T.items_of("JOINT_HIERARCHY");
        if (!jh3.empty() && !jh4.empty()) {
            const uint64_t b3 = src.pbase(jh3[0].page) + jh3[0].data_off(), b4 = T.pbase(jh4[0].page) + jh4[0].data_off();
            const uint32_t n3 = src.u32(b3 + 4), n4 = rd32le(T.d, b4 + 4);
            if (n3 != n4) throw Stop(u3::fmt("AssertionError: joint count PS3 %u != template %u", n3, n4));
            ps3_skel = src.u32(b3 + 0x1c);
            t_skel = rd32le(T.d, b4 + 0x30);
            if (src.u32(b3 + 0x24) != hier_id) throw Stop("AssertionError: hierarchy id differs from template");
            skel_sid = *ps3_skel;
        }
    }
    std::optional<skel::JH> own_jh;
    if (kw.own_skel) {
        graft = true;   // the same login / artgroup handling as grafted props
        auto jh3 = src.items_of("JOINT_HIERARCHY");
        const auto fg3 = src.items_of("FOREGROUND_SECTION_2");
        std::optional<Pak> jsrc_own;
        const Pak *jsrc = &src;
        if (jh3.empty() && opt.skel3) {   // LOD / part paks drawn on another actor's skeleton: take it from that PS3 pak
            jsrc_own.emplace(*opt.skel3);
            jsrc = &*jsrc_own;
            jh3 = jsrc->items_of("JOINT_HIERARCHY");
        }
        if (!jh3.empty()) {
            own_jh = skel::read_jh(*jsrc, jh3[0]);
            hier_id = be32(own_jh->blob, 4);
            joint_names.clear();
            for (const auto &[s, n] : own_jh->names) put32(joint_names, s);
            njoints = own_jh->nj;
        } else {
            hier_id = 0;
            joint_names.clear();
            njoints = 0;
        }
        skel_sid = fg3.empty() ? 0 : src.pu32(fg3[0].page, fg3[0].data_off() + 8);
        ps3_skel.reset();
    }
    std::vector<const TMat *> sb_tmpl, main_tmpl;
    for (const TMat &m : tmats) (has(m.name, "shadowblocker") ? sb_tmpl : main_tmpl).push_back(&m);

    // ---------------- geometry
    const std::vector<bg::FgProto> protos = bg::fg_prototypes(src);
    std::vector<PakItem> fg_items = src.items_of("FOREGROUND_SECTION_2");
    std::vector<ORef> proto_list;   // unique prototypes in PM_FG_PROTOTYPES order
    {
        const auto pms = src.items_of("PM_FG_PROTOTYPES");
        if (pms.empty()) throw Stop("IndexError: no PM_FG_PROTOTYPES");
        const uint32_t n = src.pu32(pms[0].page, pms[0].data_off());
        for (uint32_t k = 0; k < n; k++) proto_list.push_back(bg::res(src, pms[0].page, pms[0].data_off() + 4 + 4ull * k));
    }
    if (kw.lod0_only) {
        static const std::regex LOD(R"(-lod\d+$)");
        std::vector<PakItem> keep_items;
        for (const PakItem &it : fg_items)
            if (!std::regex_search(it.name, LOD)) keep_items.push_back(it);
        fg_items = keep_items;
        std::vector<ORef> keep;
        for (const PakItem &it : fg_items) keep.push_back(bg::res(src, it.page, it.data_off()));
        std::vector<ORef> pl;
        for (const ORef &pr : proto_list)
            if (std::find(keep.begin(), keep.end(), pr) != keep.end()) pl.push_back(pr);
        proto_list = pl;
        std::vector<std::optional<PakItem>> tg;
        for (const auto &t : ag.targets)
            if (!t || !std::regex_search(t->name, LOD)) tg.push_back(t);
        ag.targets = tg;
        if (ag.targets.size() <= 2) ag.nb = 7;
    }
    std::map<ORef, size_t> pindex;
    for (size_t i = 0; i < proto_list.size(); i++) pindex[proto_list[i]] = i;   // a repeated prototype: its last index
    struct Sub {
        std::string mat;
        size_t first;
        std::vector<bg::FgBlock> blocks;
    };
    std::vector<std::vector<Sub>> proto_out;
    std::vector<std::string> used_mats;
    std::set<std::pair<uint32_t, size_t>> basis_seen;   // am: the rebuilt blocks (vertices, triangles)
    for (size_t pi = 0; pi < proto_list.size(); pi++) {
        const ORef &pr = proto_list[pi];
        const bg::FgProto *info = nullptr;
        for (const auto &v : protos)
            if (pr && v.proto == *pr) { info = &v; break; }
        if (!info) throw Stop("IndexError: list index out of range");
        const std::vector<std::string> mnames = ps3_block_materials(src, *pr);
        std::vector<bg::FgBlock> blocks;
        for (const Ref &b : info->blocks) blocks.push_back(bg::decode_fg_block(src, b.first, b.second));
        if (!opt.basis_targets.empty())
            for (bg::FgBlock &b : blocks) rebuild_basis(b, opt.basis_targets, basis_seen);
        std::vector<Sub> subs;
        for (size_t bi = 0; bi < std::min(blocks.size(), mnames.size()); bi++) {   // consecutive blocks of one material merge
            if (!subs.empty() && subs.back().mat == mnames[bi]) subs.back().blocks.push_back(std::move(blocks[bi]));
            else subs.push_back({mnames[bi], bi, {}}), subs.back().blocks.push_back(std::move(blocks[bi]));
        }
        if (!opt.drop_mat.empty()) {
            bool any = false;
            for (const Sub &s : subs) any = any || !has(s.mat, opt.drop_mat);
            if (any) subs.erase(std::remove_if(subs.begin(), subs.end(), [&](const Sub &s) { return has(s.mat, opt.drop_mat); }), subs.end());
        }
        if (opt.sb_drop && !opt.sb_exact) {   // PS3 shadow-blocker submeshes (positions only) dropped
            bool any = false;
            for (const Sub &s : subs) any = any || !has(s.mat, "shadowblocker");
            if (any) subs.erase(std::remove_if(subs.begin(), subs.end(), [&](const Sub &s) { return has(s.mat, "shadowblocker"); }), subs.end());
        }
        for (const Sub &s : subs)
            if (std::find(used_mats.begin(), used_mats.end(), s.mat) == used_mats.end()) used_mats.push_back(s.mat);
        proto_out.push_back(std::move(subs));
    }
    if (!opt.basis_targets.empty()) {
        std::set<std::pair<uint32_t, size_t>> want;
        for (const auto &[k, v] : opt.basis_targets) want.insert(k);
        if (basis_seen != want) throw Stop("ValueError: basis repair coverage differs");
    }

    // ---------------- textures
    const std::vector<PakTexture> texs = src.textures();
    PakWriter w;
    auto [span, span_items] = extract_effect_span(T);
    // the effects first, at the template's exact page offset: EFFECT data holds page-relative offsets that are no fixups
    // (am: no entry cap, the older page limit, the span after the texture table at the template's offset modulo 0x1000)
    AutoPages p0(w, opt.am ? std::nullopt : std::optional<size_t>(std::max<int64_t>(1, (int64_t(span_items[0].off) - 0x14) / 16)), opt.am ? 0x1c0000 : PAGE_LIMIT);
    if (!opt.am) {
        span->modulo = uint64_t(1) << 40;
        span->phase = span_items[0].off;
        p0.span(span);
    }
    Bytes vram;
    std::vector<TexP> tex_entries;
    auto add_texture = [&](const std::string &shrt, uint32_t hash_, uint32_t fmt4, uint32_t w_, uint32_t h_, uint32_t mips, const Bytes &tiled,
                           std::optional<uint32_t> flags, std::optional<uint32_t> unk20) {
        while (vram.size() % 0x100) vram.push_back(0);
        const uint64_t voff = vram.size();
        vram.insert(vram.end(), tiled.begin(), tiled.end());
        auto nameb = std::make_shared<Blob>("texname");
        nameb->cstr(u8(shrt));
        auto e = std::make_shared<TexE>();
        e->name = p0.item("RAW_DATA", nameb);
        const std::string full4 = PS4_COOK_PREFIX + shrt;
        auto db = std::make_shared<Blob>("vramdesc");
        db->u64(0).u64(voff).u32(uint32_t(tiled.size())).u32(hash_);
        db->u32(1).u32(fmt4);
        db->u32(unk20 ? *unk20 : ((fmt4 == 0x48 || fmt4 == 0x4b || fmt4 == 0x4e) ? 0x010f0000 : 0x01000000));
        db->u32(mips).u32(w_).u32(h_).u32(0).u32(flags ? *flags : uint32_t(kw.stream_flags));
        db->ptr(db, 0x40);
        db->cstr(u8(full4));
        e->desc = p0.item("VRAM_DESC", db);
        e->hash = hash_;
        e->shrt = shrt;
        e->full = full4;
        e->fmt = fmt4;
        e->kind = tex_kind(shrt, opt.am);
        tex_entries.push_back(e);
        return e;
    };
    std::vector<Ps3Tex> ps3tex;
    ps3tex.reserve(texs.size());
    for (const PakTexture &t : texs) {
        const std::string &full3 = t.path;
        const size_t mk = full3.find(PS3_COOK_MARK);
        const std::string shrt = mk != std::string::npos ? full3.substr(mk + PS3_COOK_MARK.size()) : full3;
        const auto f3 = tc::ps3_format(t);
        if (!f3 || !f3->compressed) continue;
        Bytes texels = src.vram_bytes(t.vramOffset, t.vramSize);
        uint32_t tw = t.width, th = t.height, tm = t.mips;
        // PS4 kit parts keep only small resident mips: a PS3 texture is fully resident, so drop its top mips to max_tex
        // (am: 2D textures only)
        while (kw.max_tex && (!opt.am || t.type == 1) && std::max(tw, th) > uint32_t(kw.max_tex) && tm > 1) {
            const auto [bw, bh] = tc::blocks(tw, th, true);
            texels.erase(texels.begin(), texels.begin() + std::ptrdiff_t(std::min<uint64_t>(uint64_t(bw) * bh * f3->bpb, texels.size())));
            tw = std::max(1u, tw >> 1);
            th = std::max(1u, th >> 1);
            tm--;
        }
        Ps3Tex x;
        x.tiled = tc::ps3_to_ps4_texels(texels, tw, th, tm, f3->bpb);
        x.kind = tex_kind(shrt, opt.am);
        const bool col = x.kind == "-color";
        if (f3->kind == "DXT1") x.fmt4 = col ? 0x48 : 0x47;
        else if (f3->kind == "DXT5") x.fmt4 = col ? 0x4e : 0x4d;
        else if (f3->kind == "DXT3") x.fmt4 = col ? 0x4b : 0x4a;
        else throw Stop("NotImplementedError: " + f3->kind);
        x.shrt = shrt;
        x.hash = t.hash;
        x.full = PS4_COOK_PREFIX + shrt;
        x.w = tw;
        x.h = th;
        x.mips = tm;
        ps3tex.push_back(std::move(x));
    }
    auto use = [&](const BindTex &b) -> TexP {   // a PS3 texture's VRAM_DESC is made the first time a material binds it
        if (b.e) return b.e;
        if (!b.lazy) return nullptr;
        if (!b.lazy->real) b.lazy->real = add_texture(b.lazy->shrt, b.lazy->hash, b.lazy->fmt4, b.lazy->w, b.lazy->h, b.lazy->mips, b.lazy->tiled, std::nullopt, std::nullopt);
        return b.lazy->real;
    };
    std::map<std::string, TexP> flat_cache;
    auto flat_texture = [&](const std::string &kind) -> TexP {
        if (auto it = flat_cache.find(kind); it != flat_cache.end()) return it->second;
        static const std::map<std::string, std::array<uint32_t, 3>> RGB = {
            {"-spec-mask", {40, 40, 40}}, {"-normal", {128, 128, 255}}, {"-color", {128, 128, 128}}, {"-tint-mask", {0, 0, 0}}, {"-detail-normal", {128, 128, 255}}};
        auto rgb = RGB.find(kind);
        static const std::array<uint32_t, 3> WHITE = {255, 255, 255};   // am: the hair alpha
        if (rgb == RGB.end() && !(opt.am && kind == "-trans")) throw Stop("KeyError: '" + kind + "'");
        const auto [lin, mips] = bc1_flat(64, 64, rgb == RGB.end() ? WHITE.data() : rgb->second.data());
        const Bytes tiled = tc::ps3_to_ps4_texels(lin, 64, 64, mips, 8);
        const std::string shrt = "art/generated/u3mp/flat" + kind + ".tga/" + u3::fmt("%016llX", (unsigned long long)sidu(kind)) + ".ndb";
        TexP e = add_texture(shrt, sidu(shrt), opt.am && kind == "-color" ? 0x48 : 0x47, 64, 64, mips, tiled, std::nullopt, std::nullopt);   // am: colour sRGB
        flat_cache[kind] = e;
        return e;
    };
    std::map<std::string, PakTexture> ttex;
    for (const PakTexture &t : T.textures()) ttex[t.path] = t;
    std::map<std::string, TexP> tcache;
    std::map<std::string, bool> tcache_none;
    // donor paks: textures the PS3 pak lacks (its cooked '-trans' hair alpha is usually a missingtexture) are copied from
    // the retail pak that has the same texture, instead of the template's unrelated one
    std::vector<std::pair<std::unique_ptr<Pak>, std::map<std::string, PakTexture>>> donors;
    for (const fs::path &dp : opt.donors) {
        try {
            auto D = std::make_unique<Pak>(dp);
            std::map<std::string, PakTexture> idx;
            for (const PakTexture &t : D->textures()) {
                const size_t k = t.path.find("texture2/");
                std::string s = k == std::string::npos ? t.path : t.path.substr(k + 9);
                idx[before(s, ".tga")] = t;
            }
            donors.push_back({std::move(D), idx});
        } catch (const std::exception &) {
        }
    }
    std::function<TexP(std::string, const Pak *, const PakTexture *, bool)> tmpl_texture = [&](std::string tfull, const Pak *srcp, const PakTexture *t, bool force) -> TexP {
        if (opt.no_tmpl_tex && !force) return nullptr;
        if (auto it = tcache.find(tfull); it != tcache.end()) return it->second;
        if (tcache_none.count(tfull)) return nullptr;
        const Pak &P = srcp ? *srcp : T;
        PakTexture tt;
        if (!t) {
            auto it = ttex.find(tfull);
            if (it == ttex.end()) it = ttex.find(tfull + ".ndb");
            if (it != ttex.end()) {
                tt = it->second;
                t = &tt;
            }
        }
        if (t) tfull = t->path;
        if (!t) {
            tcache_none[tfull] = true;
            return nullptr;
        }
        const size_t k = tfull.find(PS4_COOK_PREFIX);
        const std::string shrt = k != std::string::npos ? tfull.substr(k + PS4_COOK_PREFIX.size()) : tfull;
        TexP e = add_texture(shrt, t->hash, t->format, t->width, t->height, t->mips, P.vram_bytes(t->vramOffset, t->vramSize), t->unk34, t->unk20);
        e->full = tfull;
        tcache[tfull] = e;
        tcache_none.erase(tfull);
        return e;
    };
    auto donor_texture = [&](const std::string &short_noext) -> TexP {
        for (const auto &[D, idx] : donors)
            if (auto it = idx.find(short_noext); it != idx.end()) return tmpl_texture(it->second.path, D.get(), &it->second, false);
        return nullptr;
    };
    auto ttab = p0.item("TEXTURE_TABLE");   // filled after the materials

    // ---------------- materials (template copies, re-bound)
    // am: the PS3 material table's own textures per material (by hash)
    const std::map<std::string, std::set<uint32_t>> mattex = opt.am ? am_material_textures(src) : std::map<std::string, std::set<uint32_t>>{};
    auto has_mattex = [&](const std::string &mn) { auto it = mattex.find(mn); return it != mattex.end() && !it->second.empty(); };
    auto pick_template = [&](const std::string &mname) -> const TMat * {
        const std::string key = ps4key(mname);
        for (const TMat &m : tmats)
            if (before(m.name, ",") == key) return &m;
        if (has(mname, "shadowblocker") && !sb_tmpl.empty()) return sb_tmpl[0];
        if (opt.am)
            if (auto c = opt.mat_choice.find(mname); c != opt.mat_choice.end()) {
                for (const TMat &m : tmats)
                    if (has(m.name, c->second)) return &m;
                throw Stop("IndexError: list index out of range");
            }
        if (kw.mat_by_file)
            for (const TMat *m : main_tmpl)
                if (before(m->name, ":") == rsplit1_head(mname)) return m;
        if (opt.spread_mats) {
            // hair cards need the hair (alpha) shader, hats / masks / goggles must not get it
            std::vector<const TMat *> hair, other;
            for (const TMat *m : main_tmpl) (std::find(m->feats.begin(), m->feats.end(), "GLOBAL_HAIR") != m->feats.end() ? hair : other).push_back(m);
            // mname.rsplit('/', 2)[-2]
            const size_t a = mname.rfind('/');
            std::string seg;
            if (a == std::string::npos) throw Stop("IndexError: list index out of range");
            const size_t a2 = a == 0 ? std::string::npos : mname.rfind('/', a - 1);
            seg = a2 == std::string::npos ? mname.substr(0, a) : mname.substr(a2 + 1, a - a2 - 1);
            if (has(lower(seg), "hair")) {
                if (!hair.empty()) return hair[0];
            } else if (!other.empty())
                return other[0];
        }
        if (main_tmpl.empty()) throw Stop("IndexError: list index out of range");
        return main_tmpl[0];
    };
    auto textures_for = [&](const std::string &mname) {   // the PS3 textures of a material
        std::vector<Ps3Tex *> own;
        if (opt.am)
            if (auto mt = mattex.find(mname); mt != mattex.end()) {
                // am: the material table's own textures plus their siblings (x-color -> x-normal, x-spec-mask)
                for (Ps3Tex &e : ps3tex)
                    if (mt->second.count(e.hash)) own.push_back(&e);
                if (!own.empty()) {
                    auto base_of = [](const Ps3Tex &e) { return rsplit1_last(before(before(e.shrt, ".tga"), *e.kind)); };
                    std::set<std::string> bases;
                    for (const Ps3Tex *e : own)
                        if (e->kind) bases.insert(base_of(*e));
                    for (Ps3Tex &e : ps3tex)
                        if (std::find(own.begin(), own.end(), &e) == own.end() && e.kind && bases.count(base_of(e))) own.push_back(&e);
                    return own;
                }
            }
        // name heuristics
        const std::string stem = rsplit1_last(rsplit1_head(mname));
        for (Ps3Tex &e : ps3tex)
            if (has(e.shrt, stem)) own.push_back(&e);
        if (own.empty()) {
            auto toks_of = [](const std::string &s, const char *seps) {
                std::set<std::string> t;
                size_t a = 0;
                for (size_t i = 0;; i++)
                    if (i == s.size() || std::strchr(seps, s[i])) {
                        t.insert(s.substr(a, i - a));
                        if (i == s.size()) break;
                        a = i + 1;
                    }
                return t;
            };
            std::set<std::string> toks = toks_of(stem, "-/");
            for (const char *x : {"mp", "a", "b", "c", "00", "01"}) toks.erase(x);
            std::vector<std::pair<int, Ps3Tex *>> scored;
            for (Ps3Tex &e : ps3tex) {
                int n = 0;
                for (const std::string &x : toks_of(e.shrt, "-/.")) n += int(toks.count(x));
                scored.push_back({-n, &e});
            }
            std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
            for (size_t i = 0; i < std::min<size_t>(4, scored.size()); i++) own.push_back(scored[i].second);
        }
        return own;
    };
    // one material per template material, in the template's order (the copied effects belong to them by position);
    // am: one material per PS3 material
    std::map<std::string, size_t> slot_of;
    if (!opt.am)
        for (const std::string &mn : used_mats) slot_of[mn] = size_t(pick_template(mn) - tmats.data());
    if (opt.spread_mats) {
        // PS3 materials that fell back onto a slot another PS3 material uses get a free (non-shadowblocker) slot
        std::set<size_t> taken, usedslots;
        for (const auto &[k, v] : slot_of) usedslots.insert(v);
        std::vector<size_t> free;
        for (size_t i = 0; i < tmats.size(); i++)
            if (!has(tmats[i].name, "shadowblocker") && !usedslots.count(i)) free.push_back(i);
        for (const std::string &mn : used_mats) {
            if (taken.count(slot_of[mn]) && !free.empty() && !has(mn, "shadowblocker")) {
                slot_of[mn] = free.front();
                free.erase(free.begin());
            }
            taken.insert(slot_of[mn]);
        }
    }
    std::map<std::string, Ps3Tex *> by_path;
    for (Ps3Tex &x : ps3tex) by_path[rsplit1_head(x.shrt)] = &x;
    struct MBind {
        std::string pname;
        TexP e;
        std::string tshort, tfull;
    };
    auto binds_for = [&](const std::optional<std::string> &mn, const TMat &tm) {
        std::vector<Ps3Tex *> own;
        if (mn) own = textures_for(*mn);
        const bool exact = mn && before(tm.name, ",") == ps4key(*mn);
        std::vector<MBind> binds;
        for (const auto &b : tm.binds) {
            std::vector<std::string> want;
            for (const auto &[suf, ps] : tex_kinds(opt.am))
                if (std::find(ps.begin(), ps.end(), b.pname) != ps.end()) want.push_back(suf);
            BindTex e;
            if (exact || !mn) {   // the same material on PS4 (or a template material no PS3 mesh uses): its texture
                e.e = tmpl_texture(b.full, nullptr, nullptr, false);
                if (!e.e)
                    if (auto it = by_path.find(b.shrt); it != by_path.end()) e.lazy = it->second;
            }
            if (!e.none()) {
            } else if (auto it = by_path.find(b.shrt); it != by_path.end())
                e.lazy = it->second;
            else if (!want.empty()) {
                Ps3Tex *c = nullptr;
                for (Ps3Tex *x : own)
                    if (x->kind == want[0]) { c = x; break; }
                TexP de;
                if (!c && !donors.empty()) {
                    Ps3Tex *col = nullptr;
                    for (Ps3Tex *x : own)
                        if (x->kind == "-color") { col = x; break; }
                    if (col) {
                        const std::string stem = before(col->shrt, ".tga");
                        if (has(stem, "-color")) de = donor_texture(stem.substr(0, stem.rfind("-color")) + want[0]);
                    }
                }
                if (c) e.lazy = c;
                else {
                    // am: a material with known PS3 textures does not borrow another character's map for a kind it lacks
                    e.e = opt.am && mn && has_mattex(*mn) ? flat_texture(want[0]) : de;
                    if (!e.e) e.e = tmpl_texture(b.full, nullptr, nullptr, false);
                    if (!e.e) e.e = flat_texture(want[0]);
                }
            } else   // environment maps / cornea / hair noise etc.: the template's texture
                e.e = tmpl_texture(b.full, nullptr, nullptr, true);
            binds.push_back({b.pname, use(e), b.shrt, b.full});
        }
        return binds;
    };
    struct MatRec {
        std::string name;
        const TMat *tm;
        std::vector<MBind> binds;
        std::string ps3;   // am: the PS3 material
    };
    std::vector<MatRec> mat_recs;
    if (opt.am)
        for (const std::string &mn : used_mats) {
            const TMat &tm = *pick_template(mn);
            std::vector<MBind> binds = binds_for(mn, tm);
            const std::string name = ps4key(mn) + "," + u3::fmt("00000000-0000-0000-0000-%012x", sidu(mn)) + "," + tm.shader;
            mat_recs.push_back({ren(name), &tm, std::move(binds), mn});
        }
    for (size_t ti = 0; ti < (opt.am ? 0 : tmats.size()); ti++) {
        const TMat &tm = tmats[ti];
        std::vector<std::string> users;
        for (const std::string &mn : used_mats)
            if (slot_of[mn] == ti) users.push_back(mn);
        // several PS3 materials on one template slot: the name and textures follow the exact match if any
        std::stable_sort(users.begin(), users.end(), [&](const std::string &a, const std::string &b) {
            return (before(tm.name, ",") != ps4key(a)) < (before(tm.name, ",") != ps4key(b));
        });
        const std::optional<std::string> mn = users.empty() ? std::nullopt : std::optional<std::string>(users[0]);
        std::vector<MBind> binds = binds_for(mn, tm);
        const std::string tpath = before(tm.name, ",");
        const std::string up = mn ? ps4key(*mn) : tpath;
        std::string ps4name;
        if (rnames->count(up) && up != tpath) {
            const std::string &rn = rnames->at(up);
            ps4name = rn.substr(0, rn.rfind(',')) + "," + tm.shader;
        } else if (up == tpath || has(tpath, "lib-shadowblocker") || !mn)
            ps4name = tm.name;   // retail material GUIDs identify the material asset: the same path keeps it
        else {
            const std::string k8 = u8(up);
            const std::string hx = md5_hex(Bytes(k8.begin(), k8.end()));
            ps4name = up + "," + hx.substr(0, 8) + "-" + hx.substr(8, 4) + "-" + hx.substr(12, 4) + "-" + hx.substr(16, 4) + "-" + hx.substr(20, 12) + "," + tm.shader;
        }
        mat_recs.push_back({ps4name, &tm, std::move(binds), ""});
    }
    auto rec_of = [&](const std::string &mat) -> const MatRec & {   // the material record of a PS3 material
        if (!opt.am) return mat_recs[slot_of[mat]];
        for (const MatRec &m : mat_recs)
            if (m.ps3 == mat) return m;
        throw Stop("IndexError: list index out of range");
    };

    // the texture table body (all textures)
    std::vector<TexP> order = tex_entries;
    std::stable_sort(order.begin(), order.end(), [](const TexP &a, const TexP &b) { return a->hash < b->hash; });
    auto namesb = std::make_shared<Blob>("ttnames");
    for (const TexP &e : order) namesb->ptr(e->name->data);
    auto names_item = p0.item("RAW_DATA", namesb);
    auto hashb = std::make_shared<Blob>("tthash");
    for (const TexP &e : order) hashb->u32(e->hash);
    auto hash_item = p0.item("RAW_DATA", hashb);
    {
        auto tb = ttab->data;
        tb->u64(order.size()).ptr(hash_item->data).ptr(names_item->data);
        tb->ptr(tb, 0x20);
        for (const TexP &e : order) tb->ptr(e->desc->data);
    }
    if (opt.am) p0.span(span);   // am: the effects here

    // ---------------- geometry buffers
    auto strs = std::make_shared<Blob>("geostrings");
    std::map<std::string, size_t> spool;
    auto S = [&](const std::string &s) -> int64_t {
        auto it = spool.find(s);
        if (it == spool.end()) it = spool.emplace(s, strs->cstr(u8(s))).first;
        return int64_t(it->second);
    };
    auto raw = [&](const Bytes &data, const std::string &name = "") {
        auto b = std::make_shared<Blob>("raw");
        b->raw(data);
        return p0.item("RAW_DATA", b, name)->data;
    };
    auto fmtb = std::make_shared<Blob>("vtxfmt");
    static const uint8_t FMT[5][4] = {{0, 0, 0x0c, 0x06}, {1, 0, 0x04, 0x22}, {2, 0, 0x08, 0x0a}, {3, 0, 0x08, 0x0a}, {4, 0, 0x08, 0x0a}};
    std::map<uint32_t, size_t> fmt_off;
    for (uint32_t slot = 0; slot < (opt.am ? 4u : 5u); slot++) {   // am: no vertex colour format
        fmt_off[slot] = fmtb->size();
        fmtb->raw(Bytes(FMT[slot], FMT[slot] + 4));
    }
    p0.item("RAW_DATA", fmtb);
    auto subs_blob = std::make_shared<Blob>("submeshes");
    struct SubRec {
        std::string name, mat;
        size_t nv, ni, ns;
        std::shared_ptr<PakWItem> st, skin;
        std::shared_ptr<Blob> ib;
        uint32_t flags;
        bool sb;
    };
    std::vector<SubRec> sub_records;
    struct ProtoRec {
        std::string name;
        std::vector<std::string> subnames;
        size_t off = 0;
    };
    std::vector<ProtoRec> proto_records;
    auto f16row = [](const Mat &m, size_t cols, Bytes &out) {
        for (size_t i = 0; i < m.rows; i++)
            for (size_t c = 0; c < cols; c++) put16(out, f16b(m.at(i, c)));
    };
    for (size_t pi = 0; pi < proto_out.size(); pi++) {
        const std::vector<Sub> &subs = proto_out[pi];
        ProtoRec prr;
        prr.name = "Prototype" + std::to_string(pi) + "." + art;
        for (const Sub &s : subs) {
            size_t nv = 0;
            for (const auto &b : s.blocks) nv += b.nverts;
            auto all_have = [&](const char *k) {
                for (const auto &b : s.blocks)
                    if (!b.get(k)) return false;
                return true;
            };
            auto cat = [&](const char *k) {
                std::vector<const Mat *> parts;
                size_t cols = 0, rows = 0;
                for (const auto &b : s.blocks) {
                    const Mat *m = b.get(k);
                    if (!m) throw Stop(std::string("KeyError: '") + k + "'");
                    if (!parts.empty() && m->cols != cols) throw Stop("ValueError: all the input array dimensions must match");
                    cols = m->cols;
                    rows += m->rows;
                    parts.push_back(m);
                }
                Mat o(rows, cols);
                size_t r = 0;
                for (const Mat *m : parts) {
                    std::copy(m->v.begin(), m->v.end(), o.v.begin() + std::ptrdiff_t(r * cols));
                    r += m->rows;
                }
                return o;
            };
            const Mat pos = cat("pos");
            Bytes posb;
            for (double x : pos.v) put32(posb, f32b(x));
            Bytes idx;
            size_t base = 0, ni = 0;
            for (const auto &b : s.blocks) {
                for (const auto &tr : b.tris)
                    for (uint32_t v : tr) {
                        const uint64_t x = uint64_t(v) + base;
                        if (x > 0xffff) throw Stop("OverflowError: index out of bounds for uint16");
                        put16(idx, uint32_t(x));
                        ni++;
                    }
                base += b.nverts;
            }
            struct St {
                uint32_t slot, stride;
                Bytes data;
            };
            std::vector<St> streams = {{0, 12, posb}};
            const bool sbm = has(s.mat, "shadowblocker");
            auto tile = [&](std::initializer_list<double> v) {
                Bytes o;
                for (size_t i = 0; i < nv; i++)
                    for (double x : v) put16(o, f16b(x));
                return o;
            };
            if (all_have("uv0")) {
                Bytes d;
                const Mat uv = cat("uv0");
                f16row(uv, uv.cols, d);
                streams.push_back({1, 4, d});
            } else if (opt.fill_streams && !opt.am && !sbm)
                streams.push_back({1, 4, Bytes(nv * 4, 0)});
            if (all_have("normal")) {
                const Mat n = cat("normal");
                Mat n4(n.rows, n.cols + 1);
                for (size_t i = 0; i < n.rows; i++) {
                    for (size_t c = 0; c < n.cols; c++) n4.at(i, c) = n.at(i, c);
                    n4.at(i, n.cols) = 1.0;
                }
                Bytes d;
                f16row(n4, n4.cols, d);
                streams.push_back({2, 8, d});
            } else if (opt.fill_streams && !opt.am && !sbm)
                streams.push_back({2, 8, tile({0, 0, 1, 1})});
            if (all_have("tangent")) {
                Mat tg = cat("tangent");
                if (tg.cols == 3) {
                    Mat t4(tg.rows, 4);
                    for (size_t i = 0; i < tg.rows; i++) {
                        for (size_t c = 0; c < 3; c++) t4.at(i, c) = tg.at(i, c);
                        t4.at(i, 3) = 1.0;
                    }
                    tg = t4;
                }
                Bytes d;
                f16row(tg, tg.cols, d);
                streams.push_back({3, 8, d});
            } else if (opt.fill_streams && !opt.am && !sbm)
                streams.push_back({3, 8, tile({1, 0, 0, 1})});
            if (kw.match_streams) {
                auto ws = tstreams.find(rec_of(s.mat).tm->name);
                if (ws != tstreams.end() && !ws->second.empty()) {
                    std::vector<std::pair<uint32_t, St>> have;
                    for (const St &x : streams) {
                        auto hi = std::find_if(have.begin(), have.end(), [&](const auto &h) { return h.first == x.slot; });
                        if (hi == have.end()) have.push_back({x.slot, x});
                        else hi->second = x;
                    }
                    std::vector<St> nw;
                    for (const auto &[sl, stride, fb] : ws->second) {
                        auto hi = std::find_if(have.begin(), have.end(), [&](const auto &h) { return h.first == sl; });
                        if (hi != have.end()) {
                            nw.push_back(hi->second);
                            have.erase(hi);
                        } else if (sl == 1) nw.push_back({sl, stride, Bytes(nv * 4, 0)});
                        else if (sl == 2) nw.push_back({sl, stride, tile({0, 0, 1, 1})});
                        else if (sl == 3) nw.push_back({sl, stride, tile({1, 0, 0, 1})});
                        else if (sl == 4) nw.push_back({sl, stride, tile({1, 1, 1, 1})});
                        else throw Stop(u3::fmt("NotImplementedError: template stream slot %u", sl));
                    }
                    for (auto &h : have) nw.push_back(h.second);
                    streams = nw;
                }
            }
            // skin
            std::shared_ptr<Blob> sk_c, sk_o, sk_j, sk_w;
            uint64_t skin_total = 0;
            bool skin = !s.blocks.empty();
            for (const auto &b : s.blocks) skin = skin && b.has_skin;
            if (skin) {
                std::vector<const bg::Influence *> infl;
                for (const auto &b : s.blocks)
                    for (const auto &x : b.skin) infl.push_back(&x);
                Bytes cb, ob, jb, wb;
                uint64_t sum = 0;
                for (const auto *x : infl) {
                    if (x->size() > 0xffff) throw Stop("OverflowError: influence count");
                    put16(cb, uint32_t(x->size()));
                }
                for (const auto *x : infl) {
                    put16(ob, uint32_t(sum & 0xffff));
                    sum += x->size();
                }
                for (const auto *x : infl)
                    for (const auto &[j, wv] : *x) {
                        if (j > 0xffff) throw Stop("OverflowError: joint index out of bounds for uint16");
                        put16(jb, j);
                    }
                for (const auto *x : infl)
                    for (const auto &[j, wv] : *x) put32(wb, f32b(double(float(double(wv) / 255.0))));
                skin_total = sum;
                sk_c = raw(cb);
                sk_o = raw(ob);
                sk_j = raw(jb);
                sk_w = raw(wb);
            }
            auto ib = raw(idx);
            auto st = std::make_shared<Blob>("streams");
            for (const St &x : streams) {
                auto db = raw(x.data);
                auto fo = fmt_off.find(x.slot);
                if (fo == fmt_off.end()) throw Stop(u3::fmt("KeyError: %u", x.slot));
                st->u32((x.stride << 16) | 1).u32(uint32_t(x.data.size())).ptr(fmtb, int64_t(fo->second)).ptr(db);
            }
            auto st_item = p0.item("RAW_DATA", st);
            std::shared_ptr<PakWItem> sk_item;
            if (skin) {
                auto skb = std::make_shared<Blob>("skin");
                skb->u32(0).u32(uint32_t(skin_total)).ptr(sk_c).ptr(sk_o).ptr(sk_j).ptr(sk_w);
                sk_item = p0.item("RAW_DATA", skb);
            }
            const MatRec &mrec = rec_of(s.mat);
            const std::string sname = art + ".Prototype" + std::to_string(pi) + ".Submesh" + std::to_string(s.first);
            prr.subnames.push_back(sname);
            const bool last_lod = pi == proto_out.size() - 1;
            const uint32_t flags = sbm ? 0x01000100 : (last_lod ? 0 : 0x00010000);
            sub_records.push_back({sname, mrec.name, nv, ni, streams.size(), st_item, sk_item, ib, flags, sbm});
        }
        proto_records.push_back(prr);
    }
    for (const SubRec &r : sub_records) {
        auto &b = subs_blob;
        b->u64(0);
        b->ptr(strs, S(r.name)).u32(1).u32(r.flags).u32(r.sb ? 1 : 0).u32(0);
        b->u32(uint32_t(r.nv)).u32(uint32_t(r.ni)).u32(uint32_t(r.ns)).u32(0).ptr(r.st->data).u64(0).ptr(r.ib);
        b->ptr(strs, S(r.mat)).i32(-1).u32(0).u32(njoints).u32(0);
        b->ptr(r.skin ? r.skin->data : nullptr);
        b->raw(Bytes(0x20, 0));
    }
    p0.item("RAW_DATA", subs_blob);
    // prototypes
    auto protob = std::make_shared<Blob>("prototypes"), plist = std::make_shared<Blob>("protolist"), flagsb = std::make_shared<Blob>("protoflags"),
         snl = std::make_shared<Blob>("subnames");
    for (ProtoRec &pr : proto_records) {
        const size_t foff = flagsb->size();
        flagsb->raw(Bytes{1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0});
        const size_t soff = snl->size();
        for (const std::string &n : pr.subnames) snl->ptr(strs, S(n)).i32(-1).u32(0);
        pr.off = protob->size();
        protob->u32(3).u32(uint32_t(pr.subnames.size())).u32(0).u32(0x5791a421);
        protob->ptr(strs, S(pr.name)).ptr(snl, int64_t(soff)).ptr(flagsb, int64_t(foff)).u64(0);
        // +0x30: the capacity of the instance's scope-3 cbuffer table (registered without a bounds check in game)
        protob->u32(cb3_cap).u32(cb4_cap);
        protob->raw(Bytes(0x78 - 0x38, 0));
    }
    for (const ProtoRec &pr : proto_records) plist->ptr(protob, int64_t(pr.off));
    for (const auto &b : {protob, flagsb, snl, plist}) p0.item("RAW_DATA", b);
    // the textures bound by materials (GEOMETRY_1 texture entries)
    auto texb = std::make_shared<Blob>("geotextures");
    std::vector<TexP> gtex;
    for (const MatRec &m : mat_recs)
        for (const MBind &b : m.binds)
            if (b.e && std::find(gtex.begin(), gtex.end(), b.e) == gtex.end()) gtex.push_back(b.e);
    auto bindfull = [](const TexE &e) { return ends_with(e.full, ".ndb") ? e.full.substr(0, e.full.size() - 4) : e.full; };
    for (const TexP &e : gtex) texb->ptr(strs, S(bindfull(*e))).u32(e->hash).u32(0x200).u32(0x101).i32(-1).u32(0).u32(4).u64(0);
    // materials
    auto featb = std::make_shared<Blob>("features"), parb = std::make_shared<Blob>("params"), valb = std::make_shared<Blob>("paramvalues"),
         bindb = std::make_shared<Blob>("bindings"), matb = std::make_shared<Blob>("materials");
    for (const MatRec &m : mat_recs) {
        const TMat &tm = *m.tm;
        const size_t foff = featb->size();
        for (const std::string &f : tm.feats) featb->ptr(strs, S(f));
        const size_t poff = parb->size();
        for (const auto &pp : tm.params) {
            const size_t voff = valb->size();
            valb->raw(pp.vals);
            parb->ptr(strs, S(pp.name)).ptr(valb, int64_t(voff)).u32(pp.cnt).u32(0);
        }
        const size_t boff = bindb->size();
        for (const MBind &b : m.binds) {
            std::string shrt, full;
            if (b.e) {
                shrt = ends_with(b.e->shrt, ".ndb") ? rsplit1_head(b.e->shrt) : b.e->shrt;
                full = bindfull(*b.e);
            } else {
                shrt = b.tshort;
                full = b.tfull;
            }
            bindb->ptr(strs, S(b.pname));
            bindb->ptr(strs, S(shrt));
            bindb->ptr(strs, S(full)).i32(-1).u32(0).u64(0);
        }
        matb->ptr(strs, S(m.name));
        matb->ptr(strs, S(tm.shader));
        matb->ptr(tm.feats.empty() ? nullptr : featb, int64_t(foff));
        matb->ptr(strs, S(tm.hash));
        matb->ptr(tm.params.empty() ? nullptr : parb, int64_t(poff)).ptr(m.binds.empty() ? nullptr : bindb, int64_t(boff));
        matb->u32(uint32_t(tm.feats.size())).u32(uint32_t(tm.params.size())).u32(uint32_t(m.binds.size())).u32(tm.x3);
        matb->u32(tm.tail).u32(0);
    }
    for (const auto &b : {featb, parb, valb, bindb, matb, texb})
        if (b->size()) p0.item("RAW_DATA", b);
    p0.item("RAW_DATA", strs);
    auto gb = std::make_shared<Blob>("geometry");
    gb->u32(0x10).u32(1);
    gb->u64(sub_records.size()).ptr(subs_blob);
    gb->u64(proto_records.size()).ptr(plist);
    gb->u64(0).ptr(nullptr);
    gb->u64(gtex.size()).ptr(gtex.empty() ? nullptr : texb);
    gb->u64(mat_recs.size()).ptr(matb);
    gb->u64(0).u64(0);
    auto geo = p0.item("GEOMETRY_1", gb);

    // ---------------- FOREGROUND_SECTION_2 (+ joint names)
    std::vector<std::shared_ptr<PakWItem>> fg_out;
    for (const PakItem &it : fg_items) {
        const ORef pr = bg::res(src, it.page, it.data_off());
        const uint32_t lodbits = be32(src.d, src.pbase(it.page) + it.data_off() + 0xc);
        auto pi = pindex.find(pr);
        if (pi == pindex.end()) throw Stop("KeyError: prototype");
        auto jn = raw(joint_names, "debugMeshJointNameLoc");
        auto fb = std::make_shared<Blob>("fg");
        fb->ptr(fb, 0x1c).u32(skel_sid).u32(hier_id).ptr(jn);
        fb->u32(quiet(lodbits));
        if (fb->size() != 0x1c) throw Stop("AssertionError: FG section header");
        fb->cstr(u8(proto_records[pi->second].name));
        fg_out.push_back(p0.item("FOREGROUND_SECTION_2", fb, u8(ren(it.name))));
    }

    // ---------------- tags
    std::vector<std::shared_ptr<PakWItem>> tag_items;
    std::vector<Tag> tags = read_tags(src);
    if (kw.lod0_only) {
        std::set<std::pair<std::string, std::string>> seen;
        std::vector<Tag> kept;
        for (const Tag &t : tags) {
            if (t.type == "TAG_FLOAT") continue;
            if (!seen.insert({t.type, t.name}).second) continue;
            kept.push_back(t);
        }
        tags = kept;
    }
    for (const Tag &t : tags) {
        auto tb_ = std::make_shared<Blob>("tag");
        const size_t n = t.type == "TAG_VEC4" ? 16 : 4;
        if (t.data.size() != n) throw Stop("struct.error: unpack requires a buffer of " + std::to_string(n) + " bytes");
        for (size_t k = 0; k < n; k += 4) {
            const uint32_t v = be32(t.data, k);
            tb_->u32(t.type == "TAG_INT" ? v : quiet(v));
        }
        tag_items.push_back(p0.item(t.type, tb_, u8(t.name)));
    }

    // ---------------- grafted / converted skeleton, anims, collision and sound banks
    std::vector<std::pair<std::string, std::vector<std::shared_ptr<PakWItem>>>> graft_by_type;
    auto add_graft = [&](const std::string &ty, const std::shared_ptr<PakWItem> &gi) {
        auto it = std::find_if(graft_by_type.begin(), graft_by_type.end(), [&](const auto &x) { return x.first == ty; });
        if (it == graft_by_type.end()) graft_by_type.push_back({ty, {gi}});
        else it->second.push_back(gi);
    };
    std::vector<std::shared_ptr<PakWItem>> graft_items;
    if (kw.own_skel) {
        for (const auto &tgt : ag.targets) {
            if (!tgt) continue;
            std::shared_ptr<Blob> blob;
            bool listed_jh = false;
            for (const PakItem &x : src.items) listed_jh = listed_jh || (x.page == tgt->page && x.off == tgt->off);
            if (tgt->type == "JOINT_HIERARCHY" && listed_jh) blob = skel::build_jh(*own_jh);
            else if (tgt->type == "ANIM") {
                const anim::Anim a = anim::read_ps3_anim(src, *tgt);
                if (a.clip && anim::is_streamed(*a.clip)) throw Stop("NotImplementedError: streamed clip " + tgt->name);
                blob = anim::build_ps4_anim(a);
            } else if (tgt->type == "COLLISION_DATA_FOREGROUND")
                blob = fgcoll::fg_blob(src, *tgt);
            else
                continue;
            if (!blob) throw Stop("AttributeError: 'NoneType' object");
            auto gi = p0.item(tgt->type, blob, u8(ren(tgt->name)));
            add_graft(tgt->type, gi);
            graft_items.push_back(gi);
        }
    }
    if (graft && !kw.own_skel) {
        std::map<std::string, std::vector<PakItem>> t_by;
        for (const PakItem &it : T.items)
            if (it.type == "JOINT_HIERARCHY" || it.type == "ANIM" || it.type == "COLLISION_DATA_FOREGROUND") t_by[it.type].push_back(it);
        for (const auto &tgt : ag.targets) {
            if (!tgt || !(tgt->type == "JOINT_HIERARCHY" || tgt->type == "ANIM" || tgt->type == "COLLISION_DATA_FOREGROUND")) continue;
            std::vector<PakItem> cands = t_by[tgt->type];
            if (tgt->type == "ANIM") {
                std::vector<PakItem> c2;
                for (const PakItem &c : cands)
                    if (c.name == tgt->name) c2.push_back(c);
                cands = c2;
            }
            if (cands.empty()) throw Stop("ValueError: template has no " + tgt->type + " '" + tgt->name + "' to graft");
            auto blob = copy_item_blob(T, cands[0]);
            if (ps3_skel) {
                const std::optional<size_t> off = tgt->type == "JOINT_HIERARCHY" ? std::optional<size_t>(0x30) : tgt->type == "ANIM" ? std::optional<size_t>(0x20) : std::nullopt;
                if (off) {
                    if (rd32le(blob->b, *off) != *t_skel) throw Stop("AssertionError: template skeleton id");
                    set32(blob->b, *off, *ps3_skel);
                }
            }
            auto gi = p0.item(tgt->type, blob, u8(ren(tgt->name)));
            add_graft(tgt->type, gi);
            graft_items.push_back(gi);
        }
    }
    if (graft) {
        const auto sbt = src.items_of("SOUND_BANK_TABLE");
        if (!sbt.empty()) {
            const PakItem &it = sbt[0];
            std::vector<std::string> banks;
            for (uint64_t o = it.data_off(); o < uint64_t(it.data_off()) + it.size; o += 4)
                if (const ORef r = bg::res(src, it.page, o)) banks.push_back(src.pstr(r->first, r->second));
            auto sb = std::make_shared<Blob>("soundbanks");
            sb->u32(uint32_t(banks.size())).u32(PAD32);
            Blob strs_("banknames");
            std::vector<size_t> offs;
            for (const std::string &n : banks) offs.push_back(strs_.cstr(u8(n)));
            for (size_t o : offs) sb->ptr(sb, int64_t(8 + 8 * banks.size() + o));
            sb->raw(strs_.b);
            auto gi = p0.item("SOUND_BANK_TABLE", sb);
            auto e = std::find_if(graft_by_type.begin(), graft_by_type.end(), [](const auto &x) { return x.first == "SOUND_BANK_TABLE"; });
            if (e == graft_by_type.end()) graft_by_type.push_back({"SOUND_BANK_TABLE", {gi}});
            else e->second = {gi};
        }
    }

    // ---------------- artgroup
    std::vector<std::optional<uint32_t>> keys;
    for (const auto &tgt : ag.targets) {
        if (!tgt) keys.push_back(std::nullopt);
        else if (tgt->type == "FOREGROUND_SECTION_2") keys.push_back(sidu("geo." + ren(tgt->name)));
        else keys.push_back(sidu(ren(tgt->name)));
    }
    const uint32_t nb = ag.nb;
    auto ab = std::make_shared<Blob>("artgroup");
    const size_t n = keys.size();
    ab->u32(uint32_t(n)).u32(1).ptr(ab, 0x50).ptr(ab, 0x30).u64(0).u64(0).u32(PAD32).u32(PAD32);
    const size_t tbl_off = 0x50 + 16 * n, x_off = tbl_off + 16ull * nb;
    ab->u64(0).u32(1).u32(0x12345).ptr(ab, int64_t(x_off)).u32(PAD32).u32(PAD32);
    if (ab->size() != 0x50) throw Stop("AssertionError: artgroup header");
    // the entries point at the item headers of the skeleton / collision / anims (retail: FG entries null)
    std::map<std::pair<std::string, std::string>, std::shared_ptr<PakWItem>> by_tn;
    for (const auto &gi : graft_items) by_tn[{gi->type, gi->name}] = gi;
    for (const auto &tgt : ag.targets) {
        std::shared_ptr<PakWItem> gi;
        if (tgt)
            if (auto it = by_tn.find({tgt->type, u8(ren(tgt->name))}); it != by_tn.end()) gi = it->second;
        if (gi) ab->ptr(gi->data, -0x20).u64(0);
        else ab->u64(0).u64(0);
    }
    std::vector<std::optional<std::pair<uint32_t, size_t>>> buckets(nb);
    for (size_t k = 0; k < keys.size(); k++) {
        if (!keys[k]) continue;
        if (!nb) throw Stop("ZeroDivisionError: integer modulo by zero");
        size_t bi = *keys[k] % nb;
        while (buckets[bi]) bi = (bi + 1) % nb;
        buckets[bi] = std::make_pair(*keys[k], k);
    }
    for (const auto &bk : buckets) {
        if (!bk) ab->u32(0).u32(PAD32).u64(0);
        else ab->u32(bk->first).u32(PAD32).ptr(ab, int64_t(0x50 + 16 * bk->second));
    }
    ab->u32(0).u32(PAD32).u64(0).u64(0).u32(0).u32(nb).ptr(ab, int64_t(tbl_off));
    auto artgroup = p0.item("ARTGROUP_DATA", ab, u8(art));
    auto lp = std::make_shared<Blob>("levelpats");
    lp->u64(0);
    auto levelpats = p0.item("LEVEL_PATS_1", lp);
    auto pl = std::make_shared<Blob>("pkglookup");
    const std::string art8 = u8(art);
    pl->ptr(pl, 0x20).u32(0).u32(1).ptr(pl, int64_t(0x20 + ((art.size() + 1 + 15) / 16) * 16)).u32(PAD32).u32(PAD32);
    pl->cstr(art8);
    pl->pad(16, 0xaa);
    pl->u32(sid(art8)).u32(PAD32).ptr(artgroup->data, -0x20);
    auto pkglookup = p0.item("PACKAGE_ITEM_LOOKUP", pl);

    // ---------------- page 1 + login
    Page &p1 = w.page();
    auto span_item = std::make_shared<PakWItem>();   // the EFFECT_TABLE inside the copied span
    span_item->type = "EFFECT_TABLE";
    span_item->span_ref = span;
    for (const PakItem &i : span_items)
        if (i.type == "EFFECT_TABLE") {
            span_item->span_pos = i.off - span_items[0].off;
            break;
        }
    std::vector<std::shared_ptr<PakWItem>> targets;
    if (graft) {
        // the template's login order (retail props: LEVEL_PATS, SOUND_BANK_TABLE, ARTGROUP, EFFECT_TABLE, TEXTURE_TABLE,
        // GEOMETRY_1, JOINT_HIERARCHY, ANIM*, FG*, COLLISION, LOOKUP, TAG*)
        auto by = graft_by_type;
        auto set_by = [&](const std::string &ty, std::vector<std::shared_ptr<PakWItem>> v) {
            auto it = std::find_if(by.begin(), by.end(), [&](const auto &x) { return x.first == ty; });
            if (it == by.end()) by.push_back({ty, v});
            else it->second = v;
        };
        set_by("ARTGROUP_DATA", {artgroup});
        set_by("LEVEL_PATS_1", {levelpats});
        set_by("EFFECT_TABLE", {span_item});
        set_by("TEXTURE_TABLE", {ttab});
        set_by("GEOMETRY_1", {geo});
        set_by("FOREGROUND_SECTION_2", fg_out);
        set_by("PACKAGE_ITEM_LOOKUP", {pkglookup});
        std::set<std::string> used;
        bool tags_done = false;
        for (const auto &[pg_, off_] : T.login) {
            const std::string ty = T.item_at(pg_, off_).type;
            if (ty.rfind("TAG_", 0) == 0) {
                if (!tags_done) {
                    targets.insert(targets.end(), tag_items.begin(), tag_items.end());
                    tags_done = true;
                }
                continue;
            }
            if (!used.count(ty)) {
                used.insert(ty);
                for (const auto &[t2, v] : by)
                    if (t2 == ty) targets.insert(targets.end(), v.begin(), v.end());
            }
        }
        for (const auto &[ty, its] : by)
            if (!used.count(ty)) targets.insert(targets.end(), its.begin(), its.end());
        if (!tags_done) targets.insert(targets.end(), tag_items.begin(), tag_items.end());
    } else {
        targets = {artgroup, levelpats, span_item, ttab, geo};
        targets.insert(targets.end(), fg_out.begin(), fg_out.end());
        targets.push_back(pkglookup);
        targets.insert(targets.end(), tag_items.begin(), tag_items.end());
    }
    std::vector<std::shared_ptr<PakWItem>> descs;
    for (const TexP &e : tex_entries) descs.push_back(e->desc);
    make_login_and_vram_page(w, p1, targets, descs);
    w.vram = vram;
    Bytes data = w.build();
    for (const auto &pg : w.pages)   // break the self-reference cycles
        for (auto &part : pg->parts)
            if (auto *it = std::get_if<std::shared_ptr<PakWItem>>(&part)) (*it)->data->relocs.clear();
    return data;
}

// ------------------------------------------------------------------------------------------------ post passes
Bytes cb3fix(const Bytes &d0, uint32_t cap_min) {
    const Pak q(d0, "actor");
    Bytes d = d0;
    const auto gs = q.items_of("GEOMETRY_1");
    if (gs.empty()) return d;
    const uint32_t pg = gs[0].page;
    const uint64_t b = gs[0].data_off();
    auto S = [&](uint32_t a, uint64_t o) -> std::optional<std::string> {
        const ORef r = bg::res(q, a, o);
        if (!r) return std::nullopt;
        return q.pstr(r->first, r->second);
    };
    const uint64_t nmat = q.u64(q.pbase(pg) + b + 0x48);
    const ORef mr = bg::res(q, pg, b + 0x50);
    std::map<std::optional<std::string>, std::set<std::string>> feats;
    for (uint64_t i = 0; i < (mr ? nmat : 0); i++) {
        const uint32_t mp = mr->first;
        const uint64_t mo = mr->second + 0x48 * i;
        const uint32_t nf = rd32le(q.d, q.pbase(mp) + mo + 0x30);
        const ORef fr = bg::res(q, mp, mo + 0x10);
        std::set<std::string> fs_;
        for (uint32_t k = 0; k < (fr ? nf : 0); k++) {
            const Ref f = bg::need(q, fr->first, fr->second + 8ull * k);
            fs_.insert(q.pstr(f.first, f.second));
        }
        feats[S(mp, mo)] = fs_;
    }
    const uint64_t nsub = q.u64(q.pbase(pg) + b + 8);
    const ORef sr = bg::res(q, pg, b + 0x10);
    std::map<std::optional<std::string>, std::optional<std::string>> submat;
    std::map<std::optional<std::string>, uint32_t> subnv;
    for (uint64_t k = 0; k < nsub; k++) {
        const Ref s = bg::need(sr);
        submat[S(s.first, s.second + 0x88 * k + 8)] = S(s.first, s.second + 0x88 * k + 0x48);
        subnv[S(s.first, s.second + 0x88 * k + 8)] = q.pu32(s.first, s.second + 0x88 * k + 0x20);
    }
    const ORef pl = bg::res(q, pg, b + 0x20);
    const uint64_t np = q.u64(q.pbase(pg) + b + 0x18);
    for (uint64_t i = 0; i < np; i++) {
        const Ref r = bg::need(q, bg::need(pl).first, pl->second + 8 * i);
        const uint32_t n = q.pu32(r.first, r.second + 4);
        const ORef snr = bg::res(q, r.first, r.second + 0x18);
        std::vector<std::optional<std::string>> sn;
        for (uint32_t k = 0; k < n; k++) sn.push_back(S(bg::need(snr).first, snr->second + 0x10ull * k));
        std::set<std::optional<std::set<std::string>>> fs_;
        for (const auto &x : sn) {
            std::optional<std::set<std::string>> f;
            if (auto sm = submat.find(x); sm != submat.end())
                if (auto fi = feats.find(sm->second); fi != feats.end()) f = fi->second;
            fs_.insert(f);
        }
        bool dummy = !sn.empty();
        for (const auto &x : sn) dummy = dummy && subnv.count(x) && subnv[x] == 1;
        const uint64_t at = q.pbase(r.first) + r.second + 0x30;
        const uint32_t cap = q.pu32(r.first, r.second + 0x30), need = std::max<uint32_t>(cap_min, uint32_t(fs_.size() + 2));
        if (!dummy && cap < need) set32(d, at, need);
    }
    return d;
}

Bytes vdesc_fix(const Bytes &d0, const std::string &what) {
    const Pak q(d0, "actor");
    Bytes d = d0;
    for (const PakItem &it : q.items_of("VRAM_DESC")) {
        const uint64_t db = q.pbase(it.page) + it.data_off();
        const uint32_t typ = rd32le(d, db + 0x18), fmt = rd32le(d, db + 0x1c), u20 = rd32le(d, db + 0x20);
        if (what == "flatfix") {   // the generated flat-color.tga VRAM_DESC 0x47 -> sRGB 0x48, +0x20 0x010f0000
            const ORef r = bg::res(q, it.page, it.data_off() + 0x38);
            const std::string path = r ? q.pstr(r->first, r->second) : "";
            if (has(path, "u3mp/flat-color.tga/") && fmt == 0x47 && u20 == 0x01000000) {
                set32(d, db + 0x1c, 0x48);
                set32(d, db + 0x20, 0x010f0000);
            }
        } else if (typ == 1 && (u20 >> 24) == 6)
            set32(d, db + 0x18, 2);
    }
    return d;
}

// ------------------------------------------------------------------------------------------------ rename
Bytes rename(const fs::path &ps4, const std::string &old_name, const std::string &new_name) {
    const Pak p(ps4);
    if (p.ps3) throw Stop("AssertionError: expected a PS4 pak");
    auto tok_char = [](char c) { return std::isalnum((unsigned char)c) || c == '-'; };
    // OLD as a whole token: not after a letter, digit or '-'; followed by the end, '.' or '-lod'
    auto ren = [&](const std::string &s) -> std::optional<std::string> {
        if (s.find('/') != std::string::npos || s.find(',') != std::string::npos) return std::nullopt;
        std::string t;
        size_t i = 0;
        while (i < s.size()) {
            const size_t e = i + old_name.size();
            if (!old_name.empty() && s.compare(i, old_name.size(), old_name) == 0 && !(i && tok_char(s[i - 1])) &&
                (e == s.size() || s[e] == '.' || s.compare(e, 4, "-lod") == 0)) {
                t += new_name;
                i = e;
            } else
                t += s[i++];
        }
        if (t == s) return std::nullopt;
        return t;
    };
    std::vector<Bytes> pages;
    for (const auto &pg : p.pages) pages.emplace_back(p.d.begin() + pg.file, p.d.begin() + pg.file + pg.size);
    std::vector<std::map<uint64_t, size_t>> fix(p.pages.size());   // page -> {offset: destination page}
    for (const auto &f : p.fixups) fix.at(f.src)[f.off] = f.dst;
    std::map<std::pair<size_t, std::string>, uint64_t> appended;
    auto put = [&](size_t pg, const std::string &s) {
        auto it = appended.find({pg, s});
        if (it != appended.end()) return it->second;
        Bytes &d = pages[pg];
        while (d.size() % 16) d.push_back(0);
        const uint64_t at = d.size();
        appended[{pg, s}] = at;
        d.insert(d.end(), s.begin(), s.end());
        d.push_back(0);
        return at;
    };
    auto cstr = [&](size_t pg, uint64_t off) -> std::optional<std::string> {   // ASCII only
        const Bytes &d = pages[pg];
        if (off >= d.size()) return std::string();
        uint64_t e = off;
        while (e < d.size() && d[e]) e++;
        if (e == d.size()) e = d.size() - 1;   // no NUL: up to the last byte
        std::string s(d.begin() + std::ptrdiff_t(off), d.begin() + std::ptrdiff_t(std::max(off, e)));
        for (char c : s)
            if ((unsigned char)c >= 0x80) return std::nullopt;
        return s;
    };
    std::map<uint32_t, uint32_t> sid_map;
    // 1. page entry names and item header names (page relative)
    for (size_t pi = 0; pi < pages.size(); pi++) {
        const uint32_t n = rd16le(pages[pi], 0x12);
        for (uint32_t j = 0; j < n; j++) {
            const size_t eo = 0x14 + 16 * size_t(j);
            const auto s = cstr(pi, rd64le(pages[pi], eo));
            const auto t = s && !s->empty() ? ren(*s) : std::nullopt;
            if (!t) continue;
            const uint32_t io = rd32le(pages[pi], eo + 8);
            set64(pages[pi], eo, put(pi, *t));
            const auto hs = cstr(pi, rd64le(pages[pi], io));
            if (const auto ht = hs && !hs->empty() ? ren(*hs) : std::nullopt) set64(pages[pi], io, put(pi, *ht));
            sid_map[sid(*s)] = sid(*t);
        }
    }
    // 2. pointers to a renamable string (sound bank tables keep the campaign bank name)
    std::vector<std::tuple<uint32_t, uint64_t, uint64_t>> skip;
    for (const PakItem &it : p.items)
        if (it.type == "SOUND_BANK_TABLE" || it.type == "VRAM_DESC" || it.type == "TEXTURE_TABLE" || it.type == "EFFECT" || it.type == "EFFECT_TABLE")
            skip.push_back({it.page, it.data_off(), uint64_t(it.data_off()) + it.size});
    for (size_t pi = 0; pi < pages.size(); pi++)
        for (const auto &[off, dp] : fix[pi]) {
            bool skipped = false;
            for (const auto &[pg, lo, hi] : skip) skipped = skipped || (pg == pi && lo <= off && off < hi);
            if (skipped) continue;
            const uint64_t v = rd64le(pages[pi], off);
            if (v >= p.d.size() || v >= p.pages.at(dp).size) continue;
            const auto s = cstr(dp, v);
            if (!s || s->empty() || s->size() < old_name.size()) continue;
            if (const auto t = ren(*s)) {
                set64(pages[pi], off, put(dp, *t));
                sid_map.emplace(sid(*s), sid(*t));
            }
        }
    // the artgroup keys of the items ('geo.' + the FG section name, and the name)
    for (const PakItem &it : p.items)
        if (const auto t = ren(it.name)) {
            sid_map[sidu("geo." + it.name)] = sidu("geo." + *t);
            sid_map[sidu(it.name)] = sidu(*t);
        }
    // 3. the PACKAGE_ITEM_LOOKUP id
    const uint32_t old_sid = sid(old_name);
    for (const PakItem &it : p.items_of("PACKAGE_ITEM_LOOKUP"))
        for (uint64_t o = it.data_off(); o + 3 < uint64_t(it.data_off()) + it.size; o += 4) {
            const uint32_t k = rd32le(pages[it.page], o);
            if (k == old_sid && sid_map.count(k)) set32(pages[it.page], o, sid_map[k]);
        }
    // 4. the artgroup buckets, re-hashed (linear probing)
    for (const PakItem &it : p.items_of("ARTGROUP_DATA")) {
        const uint32_t pg = it.page;
        Bytes &d = pages[pg];
        const uint64_t b = it.data_off();
        auto x = fix[pg].find(b + 0x40);
        if (x == fix[pg].end() || x->second != pg) throw Stop("AssertionError: artgroup table on another page");
        const uint64_t xo = rd64le(d, b + 0x40);
        const uint32_t nb = rd32le(d, xo + 0x1c);
        const uint64_t tbl = rd64le(d, xo + 0x20);
        std::vector<std::pair<uint32_t, uint64_t>> ents;
        for (uint32_t k = 0; k < nb; k++) {
            const uint64_t o = tbl + 16ull * k;
            const uint32_t key = rd32le(d, o);
            if (fix[pg].erase(o + 8)) ents.push_back({sid_map.count(key) ? sid_map[key] : key, rd64le(d, o + 8)});
            set32(d, o, 0);
            set32(d, o + 4, PAD32);
            set64(d, o + 8, 0);
        }
        std::vector<std::optional<std::pair<uint32_t, uint64_t>>> slots(nb);
        for (const auto &e : ents) {
            size_t bi = e.first % nb;
            while (slots[bi]) bi = (bi + 1) % nb;
            slots[bi] = e;
        }
        for (uint32_t k = 0; k < nb; k++)
            if (slots[k]) {
                const uint64_t o = tbl + 16ull * k;
                set32(d, o, slots[k]->first);
                set32(d, o + 4, PAD32);
                set64(d, o + 8, slots[k]->second);
                fix[pg][o + 8] = pg;
            }
    }
    PakWriter w;
    for (size_t pi = 0; pi < pages.size(); pi++) {
        Bytes &d = pages[pi];
        while (d.size() % 16 && pi != pages.size() - 1) d.push_back(0);
        set32(d, 0xc, uint32_t(d.size()));
        w.raw_page(d, std::vector<std::pair<uint64_t, size_t>>(fix[pi].begin(), fix[pi].end()));
    }
    w.login_page = std::make_pair(size_t(p.h[2]), size_t(p.h[3]));
    w.vram_table_loc = std::make_pair(size_t(p.h[9]), size_t(p.h[10]));
    w.vram = Bytes(p.d.begin() + p.vram_base, p.d.end());
    return w.build();
}


// ------------------------------------------------------------------------------------------------ am material choice
std::map<std::string, std::string> am_choice(const fs::path &ps3, const fs::path &tmpl, bool tint, bool overlay) {
    const Pak p(ps3), tq(tmpl);
    const std::vector<TMat> tm = tmpl_materials(tq);
    std::set<std::string> used;
    for (const bg::FgProto &v : bg::fg_prototypes(p))
        for (const std::string &m : ps3_block_materials(p, v.proto)) used.insert(m);
    const AmTable t = am_read_materials(p);
    std::map<std::string, std::set<std::string>> kinds;   // PS3 material -> the kinds of its textures (a repeated name: the last)
    for (const auto &[name, idx] : t.mats) {
        std::set<std::string> ks;
        for (uint32_t i : idx) {
            const std::string &tn = t.texs[i].second;
            const size_t a = tn.rfind("art/");
            if (auto k = tex_kind(a == std::string::npos ? tn : tn.substr(a + 4), true)) ks.insert(*k);
        }
        kinds[name] = ks;
    }
    std::set<std::string> keys;
    for (const TMat &m : tm) keys.insert(before(m.name, ","));
    auto kinds_of_binds = [](const TMat &m) {
        std::set<std::string> s;
        for (const auto &b : m.binds)
            for (const auto &[suf, ps] : tex_kinds(true))
                if (std::find(ps.begin(), ps.end(), b.pname) != ps.end()) s.insert(suf);
        return s;
    };
    auto feat = [](const TMat &m, const std::string &f) { return std::find(m.feats.begin(), m.feats.end(), f) != m.feats.end(); };
    std::map<std::string, std::string> choice;
    for (const std::string &mn : used) {   // sorted
        if (has(mn, "shadowblocker") || keys.count(ps4key(mn))) continue;
        std::vector<const TMat *> c = am_pool(tm, am_role(mn));
        if (c.empty()) c = am_pool(tm, "cloth");
        const std::set<std::string> ks = kinds.count(mn) ? kinds[mn] : std::set<std::string>{};
        std::vector<const TMat *> c2 = c;
        if (tint) {   // the tint mask decides: a PS3 material with one gets a template that reads it, one without does not
            const bool want = ks.count("-tint-mask") != 0;
            std::vector<const TMat *> f;
            for (const TMat *m : c)
                if (feat(*m, "COLOR_1_APPLY_INSTANCE_TINT") == want) f.push_back(m);
            if (!f.empty()) c2 = f;
        }
        if (overlay) {   // overlay / UV_3 variants need a second UV set the converted mesh does not have
            std::vector<const TMat *> f;
            for (const TMat *m : c2) {
                bool ov = false;
                for (const std::string &x : m->feats) ov = ov || has(x, "OVERLAY") || has(x, "UV_3");
                if (!ov) f.push_back(m);
            }
            if (!f.empty()) c2 = f;
        }
        if (c2.empty()) throw Stop("ValueError: max() arg is an empty sequence");
        const TMat *best = nullptr;
        long best_score = 0;
        for (const TMat *m : c2) {
            const std::set<std::string> kb = kinds_of_binds(*m);
            long common = 0, extra = 0;
            for (const std::string &k : kb) (ks.count(k) ? common : extra)++;
            if (!best || common - extra > best_score) best = m, best_score = common - extra;
        }
        choice[mn] = best->name;
    }
    return choice;
}

}  // namespace u3::actor
