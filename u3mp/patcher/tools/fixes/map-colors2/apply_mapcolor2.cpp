// Recipe step map-colors2: PS3 normal-map blends, gloss and stripes on the overlay-r3 level paks, using effects already in
// each pak. Replaces mk_p3idx.py, survey_ov.py, detect.py, apply_mapcolor2.py and their helpers (lib.py mats4, p3prog.py,
// sm.py, fxin.py, tools/fp.py, tools/bc1inv.py).
//  * survey: the materials of every overlay-r3 level pak (no ingame12): name, shader, hash, features, constants, bindings;
//  * detect: for each prime-shader material with a PS3 twin (the PS3 pak from pak23.psarc, goty-dlc, dlc-patch13,
//    dlc-patch18; the last archive that has it), the PS3 main-pass fragment program says where the blend factor comes
//    from (the term t + M / (2s + 0.01) traced back to a texture channel), which texture units it reads and which
//    textures the PS4 conversion dropped;
//  * plan, per material:
//      Zflat  PS3 blends from the normal map's blue channel, PS4's BlendMap is a flat mask: a grey BC1 copy of that blue
//             (inverted when the material inverts the blend map) is appended and BlendMap bound to it;
//      Zvc    the same source, but the PS4 effect is vertex-colour only: the material is also moved to a sibling
//             material's effect that blends with a map and adds specular (its streams must cover the effect inputs);
//      stripe a dropped PS3 hazard / striped / decal overlay on a blend-map material: moved to the pak's uv2 overlay
//             effect of the same polarity, the overlay bound (appended from PS3 when the pak lacks it) with a white
//             tint mask;
//  * build: the retail pages are kept, new textures, tables, constants and bindings go to one new page;
//  * the lightmap-flag fix of map-colors (its manifest offsets) must still hold in the result, else the pak is skipped.
//   u3fix map-colors2 --project P --job J --ps3-disc D --ps3-update U [--apply | --revert] [--out DIR] [--concrete-stripes]
// Dry run by default (built in memory). --apply backs up to work/backup-mapcolor2 and writes the overlay, with
// backup-mapcolor2/manifest.json; J/plan-result.json(.concrete) records the plan either way.
#include "ndpak.hpp"
#include "ps3bg.hpp"
#include "ps3fp.hpp"
#include "ps4writer.hpp"
#include "psarc.hpp"
#include "stageplan.hpp"
#include "texcodec.hpp"
#include "u3fix.hpp"

#include <cstring>
#include <regex>

namespace u3 {
namespace {

using BlobP = std::shared_ptr<Blob>;
using ItemP = std::shared_ptr<PakWItem>;
const std::string PFX = "d:/dev/CP9/Data/U3/CookData/ND/build/texture2/";
const std::string BLENDP = "BLEND_FACTOR_USE_BLEND_MAP_AND_VERTEX_COLOR", INV = "BLEND_FACTOR_INVERT_BLEND_MAP";
const uint32_t CAP = 256;

std::shared_ptr<PtrTarget> unowned(PtrTarget &t) { return std::shared_ptr<PtrTarget>(std::shared_ptr<PtrTarget>(), &t); }
bool has(const std::vector<std::string> &v, const std::string &x) { return std::find(v.begin(), v.end(), x) != v.end(); }
bool has(const std::string &s, const std::string &x) { return s.find(x) != std::string::npos; }
bool starts(const std::string &s, const std::string &x) { return s.rfind(x, 0) == 0; }
bool ends(const std::string &s, const std::string &x) { return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0; }
std::string before(const std::string &s, char c) { return s.substr(0, s.find(c)); }
std::string last_part(const std::string &s) { return s.substr(s.rfind('/') == std::string::npos ? 0 : s.rfind('/') + 1); }
std::string pstr_need(const Pak &q, const ORef &r) {   // q.pstr(*q.resolve(...)): a null pointer stops (Python's TypeError)
    if (!r) throw Stop("TypeError: no pointer");
    return q.pstr(r->first, r->second);
}

// ---- survey (lib.py mats4) --------------------------------------------------------------------------------------
struct Param {
    std::string name;
    uint32_t cnt;
    std::optional<std::array<float, 4>> vals;
};
struct Mat4 {
    uint64_t i;
    std::optional<std::string> n, s, h;
    std::vector<std::string> f;
    std::vector<Param> p;
    std::vector<std::pair<std::string, std::string>> b;
    std::string base() const { return before(n ? *n : "", ','); }
};
std::vector<Mat4> mats4(const Pak &q) {
    std::vector<Mat4> out;
    for (const PakItem &g : q.items_of("GEOMETRY_1")) {
        const uint32_t pg = g.page;
        const uint64_t b = g.data_off();
        const uint64_t n = q.u64(q.pbase(pg) + b + 0x48);
        const ORef r = bg::res(q, pg, b + 0x50);
        if (!r) continue;
        for (uint64_t i = 0; i < n; i++) {
            const uint64_t o = r->second + i * 0x48;
            auto S = [&](uint64_t off) -> std::optional<std::string> {
                const ORef x = bg::res(q, r->first, o + off);
                return x ? std::optional<std::string>(q.pstr(x->first, x->second)) : std::nullopt;
            };
            const uint64_t cb = q.pbase(r->first) + o + 0x30;
            const uint32_t nf = rd32le(q.d, cb), npar = rd32le(q.d, cb + 4), nb = rd32le(q.d, cb + 8);
            Mat4 m;
            m.i = i, m.n = S(0), m.s = S(8), m.h = S(0x18);
            if (const ORef fr = bg::res(q, r->first, o + 0x10))
                for (uint32_t k = 0; k < nf; k++) m.f.push_back(pstr_need(q, bg::res(q, fr->first, fr->second + 8ull * k)));
            if (const ORef pr = bg::res(q, r->first, o + 0x20))
                for (uint32_t k = 0; k < npar; k++) {
                    const uint64_t po = pr->second + 0x18ull * k;
                    Param p;
                    p.name = pstr_need(q, bg::res(q, pr->first, po));
                    p.cnt = q.pu32(pr->first, po + 0x10);
                    if (const ORef vr = bg::res(q, pr->first, po + 8)) {
                        std::array<float, 4> v;
                        std::memcpy(v.data(), &q.d.at(q.pbase(vr->first) + vr->second + 15) - 15, 16);
                        p.vals = v;
                    }
                    m.p.push_back(p);
                }
            if (const ORef br = bg::res(q, r->first, o + 0x28))
                for (uint32_t k = 0; k < nb; k++) {
                    const uint64_t bo = br->second + 0x28ull * k;
                    m.b.push_back({pstr_need(q, bg::res(q, br->first, bo)), pstr_need(q, bg::res(q, br->first, bo + 8))});
                }
            out.push_back(std::move(m));
        }
    }
    return out;
}

// ---- PS3 program facts (p3prog.py) --------------------------------------------------------------------------------
struct Ins {
    std::string op, d;
    std::vector<std::string> a;
};
std::optional<Ins> parse(std::string l) {
    for (size_t k; (k = l.find("  (end)")) != std::string::npos;) l.erase(k, 7);
    static const std::regex rx(R"(^(\w+) (\S+), (.*)$)");
    std::smatch m;
    if (!std::regex_search(l, m, rx)) return std::nullopt;
    Ins t{m[1], m[2], {}};
    const std::string rest = m[3];
    // split on ', ' that is not inside a {...} constant
    size_t s = 0;
    for (size_t k = 0; k + 1 < rest.size(); k++) {
        if (rest[k] != ',' || rest[k + 1] != ' ') continue;
        bool inside = false;
        for (size_t j = k + 2; j < rest.size(); j++) {
            if (rest[j] == '{') break;
            if (rest[j] == '}') {
                inside = true;
                break;
            }
        }
        if (inside) continue;
        t.a.push_back(rest.substr(s, k - s));
        s = k + 2;
        k++;
    }
    t.a.push_back(rest.substr(s));
    for (auto &x : t.a) {   // strip()
        size_t b = 0;
        while (b < x.size() && py_space((unsigned char)x[b])) b++;
        x = py_rstrip(x.substr(b));
    }
    return t;
}
std::string lstrip_set(std::string x, const std::string &set) {
    size_t b = 0;
    while (b < x.size() && set.find(x[b]) != std::string::npos) b++;
    return x.substr(b);
}
std::string rstrip_set(std::string x, const std::string &set) {
    while (!x.empty() && set.find(x.back()) != std::string::npos) x.pop_back();
    return x;
}
std::string split_at(const std::string &x, char c, size_t i) {   // x.split(c)[i]
    size_t s = 0;
    for (size_t k = 0; k < i; k++) {
        s = x.find(c, s);
        if (s == std::string::npos) throw Stop("IndexError: list index out of range");
        s++;
    }
    const size_t e = x.find(c, s);
    return x.substr(s, e == std::string::npos ? std::string::npos : e - s);
}
std::string base(const std::string &x) { return rstrip_set(split_at(lstrip_set(x, "-|"), '.', 0), "|"); }
std::string swz(const std::string &x0) {
    const std::string x = rstrip_set(lstrip_set(x0, "-|"), "|");
    return x.find('.') != std::string::npos ? split_at(x, '.', 1) : "xyzw";
}
std::pair<std::string, std::string> wr(const std::string &d) {
    return {split_at(d, '.', 0), d.find('.') != std::string::npos ? split_at(d, '.', 1) : "xyzw"};
}
int tex_unit(const std::string &a) {   // int(a[3:])
    const std::string s = a.size() > 3 ? a.substr(3) : "";
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) throw Stop("ValueError: invalid literal for int() with base 10: '" + s + "'");
    return std::stoi(s);
}
struct Src {   // ('tex', unit, chan, coord) or (op, None, None, None) or ('?',)
    std::string kind;
    std::optional<int> unit;
    std::string chan, coord;
};
std::optional<Src> trace(const std::vector<std::optional<Ins>> &P, size_t i, const std::string &reg, char c, int depth = 0) {
    for (size_t j = i; j-- > 0;) {
        if (!P[j]) continue;
        const Ins &t = *P[j];
        const auto [rn, mk] = wr(t.d);
        if (rn != reg || mk.find(c) == std::string::npos) continue;
        if (starts(t.op, "TEX")) return Src{"tex", tex_unit(t.a.back()), std::string(1, c), t.a.at(0)};
        if (starts(t.op, "MOV") && depth < 6 && !t.a.empty() && !starts(t.a[0], "{") && !starts(t.a[0], "f[")) {
            const size_t k = mk.find(c);
            const std::string s = swz(t.a[0]);
            char s2;
            if (s.size() == mk.size() || (s.size() == 4 && mk.size() == 4)) s2 = s.at(k);
            else if (s == "xyzw") s2 = c;
            else s2 = s.at(std::min(std::string("xyzw").find(c), s.size() - 1));
            return trace(P, j, base(t.a[0]), s2, depth + 1);
        }
        return Src{before(t.op, '_'), std::nullopt, "", ""};
    }
    return std::nullopt;
}
std::optional<Src> blend_source(const std::vector<std::string> &L) {
    std::vector<std::optional<Ins>> P;
    for (const auto &l : L) P.push_back(parse(l));
    std::optional<std::pair<std::string, char>> k2, rc;
    auto traced = [&](size_t i, const std::string &x) -> std::optional<Src> {
        const std::string s = swz(x);
        auto r = trace(P, i, base(x), s.at(0));
        return r ? r : Src{"?", std::nullopt, "", ""};
    };
    for (size_t i = 0; i < P.size(); i++) {
        if (!P[i]) continue;
        const Ins &t = *P[i];
        if (starts(t.op, "MAD") && has(L[i], "0.0100021")) {
            k2 = std::make_pair(wr(t.d).first, wr(t.d).second.at(0));
            rc.reset();
            continue;
        }
        if (k2 && starts(t.op, "DIV") && t.a.size() >= 2 && base(t.a[1]) == k2->first) return traced(i, t.a[0]);
        if (k2 && starts(t.op, "RCP") && base(t.a.at(0)) == k2->first) {
            rc = std::make_pair(wr(t.d).first, wr(t.d).second.at(0));
            continue;
        }
        if (rc && (starts(t.op, "MUL") || starts(t.op, "MAD"))) {
            const size_t n2 = std::min<size_t>(2, t.a.size());
            bool any = false;
            for (size_t k = 0; k < n2; k++) any = any || base(t.a[k]) == rc->first;
            if (!any) continue;
            std::vector<std::string> o;
            for (size_t k = 0; k < n2; k++)
                if (base(t.a[k]) != rc->first) o.push_back(t.a[k]);
            if (starts(t.op, "MAD") && starts(t.a.at(0), "-") && t.a.at(1) == t.a.at(2)) o = {t.a[0]};
            if (!o.empty()) return traced(i, o[0]);
        }
    }
    return std::nullopt;
}
std::map<int, std::vector<std::pair<std::string, std::string>>> tex_use(const std::vector<std::string> &L) {
    std::map<int, std::vector<std::pair<std::string, std::string>>> out;
    for (const auto &l : L) {
        const auto t = parse(l);
        if (t && (starts(t->op, "TEX") || starts(t->op, "TXP"))) out[tex_unit(t->a.back())].push_back({t->a.at(0), wr(t->d).second});
    }
    return out;
}
struct Facts {
    bool zproof = false;
    std::vector<std::pair<int, std::string>> units;   // in unit order
    std::optional<Src> src;
    std::map<int, std::vector<std::pair<std::string, std::string>>> use;
    bg::Params params;
};
std::map<std::string, Facts> facts(const Pak &p) {
    std::map<std::string, Facts> out;
    const auto mts = p.items_of("MATERIAL_TABLE_1");
    if (mts.empty()) return out;
    const uint32_t pg = mts[0].page;
    const uint64_t b = mts[0].data_off();
    const uint32_t nmat = p.pu32(pg, b + 0xc), ntex = p.pu32(pg, b + 4);
    const ORef ma = bg::res(p, pg, b + 0x20), th = bg::res(p, pg, b + 0x18), nm = bg::res(p, pg, b + 0x30), sr = bg::res(p, pg, b + 0x1c);
    const auto PAR = bg::all_material_params(p);
    std::map<uint32_t, std::vector<std::string>> cache;
    auto tn = [&](const ORef &rr) -> std::optional<std::string> {
        if (rr && th && rr->first == th->first && th->second <= rr->second && rr->second < th->second + 4ull * ntex) {
            const std::string s = pstr_need(p, bg::res(p, bg::need(nm).first, bg::need(nm).second + 4 * ((rr->second - th->second) / 4)));
            std::vector<std::string> parts;
            size_t a = 0;
            for (size_t k; (k = s.find('/', a)) != std::string::npos; a = k + 1) parts.push_back(s.substr(a, k - a));
            parts.push_back(s.substr(a));
            if (parts.size() < 2) throw Stop("IndexError: list index out of range");
            return parts[parts.size() - 2];
        }
        return std::nullopt;
    };
    for (uint32_t i = 0; i < nmat; i++) {
        const Ref m = bg::need(p, bg::need(ma).first, bg::need(ma).second + 4ull * i);
        const std::string name = pstr_need(p, bg::res(p, m.first, m.second));
        std::optional<std::tuple<std::vector<std::string>, Ref, uint32_t>> best;
        for (const bg::WalkNode &w : bg::walk_passes(p, m)) {
            if (w.k > 2) continue;
            const uint32_t pid = bg::be16(p, p.pbase(w.node.first) + w.node.second);
            const ORef t = bg::res(p, bg::need(sr).first, bg::need(sr).second + 0x10ull * pid + 12);
            if (!t) continue;
            const uint64_t B = p.pbase(t->first) + t->second;
            if (!cache.count(pid)) {
                try {
                    const uint32_t size = be32(p.d, B), off = be32(p.d, B + 16);
                    const uint64_t s = std::min<uint64_t>(B + off, p.d.size()), e = std::min<uint64_t>(B + uint64_t(off) + size, p.d.size());
                    cache[pid] = fp::dis(Bytes(p.d.begin() + std::ptrdiff_t(s), p.d.begin() + std::ptrdiff_t(std::max(s, e))));
                } catch (const std::exception &) {
                    cache[pid] = {};
                }
            }
            if (w.e == 0 && (!best || cache[pid].size() > std::get<0>(*best).size())) best = std::make_tuple(cache[pid], w.node, pid);
        }
        if (!best) continue;
        const auto &[L, node, pid] = *best;
        Facts f;
        if (const ORef bb = bg::res(p, node.first, node.second + 0x10)) {
            if (auto x = tn(bg::res(p, bb->first, bb->second + 0x10))) f.units.push_back({0, *x});
            for (int u = 1; u < 12; u++)
                if (auto x = tn(bg::res(p, bb->first, bb->second + 0x40 + 0x10ull * uint64_t(u)))) f.units.push_back({u, *x});
        }
        const auto U = tex_use(L);
        const auto sc = blend_source(L);
        bool zp = false;
        if (sc && sc->kind == "tex" && sc->chan == "z") {
            auto ui = U.find(*sc->unit);
            if (ui != U.end())
                for (const auto &[c, mk] : ui->second)
                    if (has(mk, "x") && has(mk, "y") && !has(mk, "w")) zp = true;
            bool dp = false;
            for (const auto &l : L) dp = dp || starts(l, "DP2") || starts(l, "DP3");
            zp = zp && dp;
        }
        f.zproof = zp, f.src = sc, f.use = U;
        auto pi = PAR.find(name);
        if (pi != PAR.end()) f.params = pi->second;
        out[name] = std::move(f);
    }
    return out;
}

// ---- rows (detect.py) and plan ------------------------------------------------------------------------------------
struct Row {
    std::string pak, mat;
    bool vc_only, blendmap, shared;
    std::optional<std::tuple<int, std::optional<std::string>, std::string, std::string>> src;   // unit, texture, chan, coord
    bg::Params ps3p;
    std::vector<std::tuple<int, std::string, std::optional<std::string>>> dropped;            // unit, texture, role
};
struct Job {
    std::string cls, mat, nrm, ovl;
    const Row *row;
};

std::vector<Row> rows_of(const std::string &pak, const std::vector<Mat4> &mats, const std::map<std::string, Facts> &F) {
    static const std::regex Z("missingtexture|indirect|lightmap", std::regex::icase);
    std::vector<Row> out;
    for (const Mat4 &m : mats) {
        const std::string key = m.base();
        const size_t c = key.rfind(':');
        const std::string pn = (c == std::string::npos ? "" : key.substr(0, c)) + "/" + (c == std::string::npos ? key : key.substr(c + 1));
        auto gi = F.find(pn);
        if (gi == F.end() || !m.s || *m.s != "prime-shader.fx") continue;
        const Facts &g = gi->second;
        std::set<std::string> bn;
        for (const auto &b : m.b) bn.insert(last_part(b.second));
        Row r;
        r.pak = pak, r.mat = key, r.ps3p = g.params;
        r.vc_only = has(m.f, "BLEND_FACTOR_USE_VERTEX_COLOR_ONLY");
        r.blendmap = has(m.f, "BLEND_FACTOR_USE_BLEND_MAP_AND_VERTEX_COLOR");
        r.shared = has(m.f, "BLEND_FACTOR_USE_SHARED_TEXTURE_1_B");
        auto unit = [&](int u) -> std::optional<std::string> {
            for (const auto &[k, s] : g.units)
                if (k == u) return s;
            return std::nullopt;
        };
        if (g.src && g.src->kind == "tex") r.src = std::make_tuple(*g.src->unit, unit(*g.src->unit), g.src->chan, g.src->coord);
        for (const auto &[u, s] : g.units) {
            if (std::regex_search(s, Z) || bn.count(s)) continue;
            r.dropped.push_back({u, s, stage::tex_role_final("x/" + s)});
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<Job> plan(const std::vector<Row> &R, const std::vector<Mat4> &mats, bool concrete) {
    static const std::regex hz("hazard|striped|decal"), mr(R"(-(hazard|striped)\b|multi-red)");
    std::vector<Job> out;
    for (const Row &r : R) {
        const Mat4 *m = nullptr;
        for (const Mat4 &x : mats)
            if (x.base() == r.mat) {
                m = &x;
                break;
            }
        if (!m) throw Stop("StopIteration: " + r.mat);
        std::vector<std::string> ov;
        for (const auto &[u, s, role] : r.dropped)
            if (role && *role == "color" && std::regex_search(s, hz)) ov.push_back(s);
        if (r.src && std::get<2>(*r.src) == "z" && !r.shared) {
            std::optional<std::string> nrm;
            const auto &s1 = std::get<1>(*r.src);
            if (s1 && !s1->empty() && has(*s1, "normal")) nrm = *s1;
            if (!nrm)
                for (const auto &b : m->b)
                    if (has(b.first, "Normal1")) {
                        nrm = last_part(b.second);
                        break;
                    }
            std::optional<std::string> blend;
            for (const auto &b : m->b)
                if (has(b.first, "BlendMap")) {
                    blend = b.second;
                    break;
                }
            if (nrm && !nrm->empty() && r.vc_only) out.push_back({"Zvc", r.mat, *nrm, "", &r});
            else if (nrm && !nrm->empty() && blend && !blend->empty() && has(*blend, "generated/u3mp/flat")) out.push_back({"Zflat", r.mat, *nrm, "", &r});
        }
        if (!ov.empty() && r.blendmap && std::regex_search(r.mat, mr) && (concrete || !has(r.mat, "concrete"))) out.push_back({"stripe", r.mat, "", ov[0], &r});
    }
    return out;
}

// ---- textures -------------------------------------------------------------------------------------------------------
// DXT1 / DXT5 colour blocks -> DXT1 blocks whose colour is the grey of the blue channel (as retail '-blend' maps)
Bytes blueblocks(const Bytes &lin, int bpb) {
    const size_t n = lin.size() / size_t(bpb);
    Bytes out(n * 8);
    auto g = [](uint32_t c) { return (c & 31) << 11 | (((c & 31) << 1) | ((c & 31) >> 4)) << 5 | (c & 31); };
    for (size_t k = 0; k < n; k++) {
        const size_t o = k * size_t(bpb) + (bpb == 16 ? 8 : 0);
        const uint32_t c0 = uint32_t(lin[o] | lin[o + 1] << 8), c1 = uint32_t(lin[o + 2] | lin[o + 3] << 8);
        uint32_t ix = rd32le(&lin[o + 4]);
        uint32_t a = g(c0), b = g(c1);
        if (c0 > c1 || bpb == 16) {   // 4-colour block: a > b
            if (a < b) std::swap(a, b), ix ^= 0x55555555;
            else if (a == b) {
                if (a == 0) a = 1;
                else b = a - 1;
                ix = 0;
            }
        } else if (a > b) {   // 3-colour block: a <= b
            std::swap(a, b);
            uint32_t ni = 0;
            static const uint32_t MAP[4] = {1, 0, 2, 3};
            for (int q = 0; q < 16; q++) ni |= MAP[(ix >> (2 * q)) & 3] << (2 * q);
            ix = ni;
        }
        out[k * 8] = uint8_t(a), out[k * 8 + 1] = uint8_t(a >> 8), out[k * 8 + 2] = uint8_t(b), out[k * 8 + 3] = uint8_t(b >> 8);
        wr32le(&out[k * 8 + 4], ix);
    }
    return out;
}
// BC1 value-wise inversion (v -> 1 - v): 4-colour blocks exactly (endpoints NOT-ed and swapped, indices ^ 1), 3-colour
// blocks re-fitted as 4-colour blocks of the inverted colours (transparent black becomes white)
Bytes bc1_invert(const Bytes &blob) {
    Bytes out = blob;
    auto d565 = [](uint32_t c) { return std::array<int, 3>{int((c >> 11) & 31) * 255 / 31, int((c >> 5) & 63) * 255 / 63, int(c & 31) * 255 / 31}; };
    auto e565 = [](int r, int g, int b) { return uint32_t((r * 31 + 127) / 255) << 11 | uint32_t((g * 63 + 127) / 255) << 5 | uint32_t((b * 31 + 127) / 255); };
    for (size_t o = 0; o + 7 < blob.size(); o += 8) {
        const uint32_t c0 = uint32_t(blob[o] | blob[o + 1] << 8), c1 = uint32_t(blob[o + 2] | blob[o + 3] << 8), ix = rd32le(&blob[o + 4]);
        auto put = [&](uint32_t a, uint32_t b, uint32_t i) {
            out[o] = uint8_t(a), out[o + 1] = uint8_t(a >> 8), out[o + 2] = uint8_t(b), out[o + 3] = uint8_t(b >> 8);
            wr32le(&out[o + 4], i);
        };
        if (c0 > c1) {
            put(c1 ^ 0xffff, c0 ^ 0xffff, ix ^ 0x55555555);
            continue;
        }
        const auto p0 = d565(c0), p1 = d565(c1);
        std::array<std::array<int, 3>, 4> pal = {p0, p1, {(p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2, (p0[2] + p1[2]) / 2}, {0, 0, 0}};
        std::array<std::array<int, 3>, 16> px;
        for (int k = 0; k < 16; k++) {
            const auto &c = pal[(ix >> (2 * k)) & 3];
            px[size_t(k)] = {255 - c[0], 255 - c[1], 255 - c[2]};
        }
        std::array<int, 3> lo = px[0], hi = px[0];
        for (const auto &c : px)
            for (int j = 0; j < 3; j++) lo[size_t(j)] = std::min(lo[size_t(j)], c[size_t(j)]), hi[size_t(j)] = std::max(hi[size_t(j)], c[size_t(j)]);
        uint32_t e0 = e565(hi[0], hi[1], hi[2]), e1 = e565(lo[0], lo[1], lo[2]);
        if (e0 == e1) {
            put(e0, e1, 0);
            continue;
        }
        if (e0 < e1) std::swap(e0, e1);
        const auto q0 = d565(e0), q1 = d565(e1);
        std::array<std::array<int, 3>, 4> q = {q0, q1, std::array<int, 3>{(2 * q0[0] + q1[0]) / 3, (2 * q0[1] + q1[1]) / 3, (2 * q0[2] + q1[2]) / 3},
                                               std::array<int, 3>{(q0[0] + 2 * q1[0]) / 3, (q0[1] + 2 * q1[1]) / 3, (q0[2] + 2 * q1[2]) / 3}};
        uint32_t ni = 0;
        for (int k = 0; k < 16; k++) {
            int best = 0, bd = -1;
            for (int i = 0; i < 4; i++) {
                int dd = 0;
                for (int j = 0; j < 3; j++) dd += (px[size_t(k)][size_t(j)] - q[size_t(i)][size_t(j)]) * (px[size_t(k)][size_t(j)] - q[size_t(i)][size_t(j)]);
                if (bd < 0 || dd < bd) bd = dd, best = i;
            }
            ni |= uint32_t(best) << (2 * k);
        }
        put(e0, e1, ni);
    }
    return out;
}

const PakTexture *ps3_tex(const std::vector<PakTexture> &T, const std::string &base_) {
    for (const PakTexture &t : T) {
        const std::string &s = t.path;
        const size_t e = s.rfind('/');
        if (e == std::string::npos) continue;
        const size_t b = s.rfind('/', e - 1);
        if (s.substr(b == std::string::npos ? 0 : b + 1, e - (b == std::string::npos ? 0 : b + 1)) == base_) return &t;
    }
    return nullptr;
}
struct Chain {
    Bytes lin;
    uint32_t w, h, m;
    std::string kind;
    int bpb;
};
Chain chain(const Pak &p, const PakTexture &t) {   // the mips from the first one within 256 pixels
    const auto f = tc::ps3_format(t);
    if (!f || !f->compressed || (f->kind != "DXT1" && f->kind != "DXT5")) throw Stop("ValueError: format " + (f ? f->kind : std::string("None")));
    Bytes d = p.vram_bytes(t.vramOffset, t.vramSize);
    uint32_t w = t.width, h = t.height, m = t.mips;
    size_t s = 0;
    while (std::max(w, h) > CAP && m > 1) s += tc::ps3_linear_size(w, h, 1, f->bpb), w = std::max(1u, w >> 1), h = std::max(1u, h >> 1), m--;
    const size_t e = std::min<size_t>(d.size(), s + tc::ps3_linear_size(w, h, m, f->bpb));
    return {Bytes(d.begin() + std::ptrdiff_t(std::min(s, d.size())), d.begin() + std::ptrdiff_t(std::max(e, std::min(s, d.size())))), w, h, m, f->kind, f->bpb};
}

// ---- the pak's effect inputs (fxin.py) and submesh stream signatures (sm.py) ------------------------------------------
std::map<std::string, std::set<uint32_t>> effect_inputs(const Pak &q) {   // effect hash -> the input semantics
    std::map<std::string, std::set<uint32_t>> out;
    const auto ets = q.items_of("EFFECT_TABLE");
    if (ets.empty()) throw Stop("IndexError: no EFFECT_TABLE");
    const PakItem &et = ets[0];
    const uint32_t n = q.pu32(et.page, et.data_off());
    const Ref arr = bg::need(q, et.page, et.data_off() + 8);
    for (uint32_t k = 0; k < n; k++) {
        const uint64_t o = arr.second + 0x18ull * k;
        const Ref ep = bg::need(q, arr.first, o);
        const std::string h = u3::fmt("%016llX", (unsigned long long)q.u64(q.pbase(arr.first) + o + 0x10));
        const Ref pp = bg::need(q, ep.first, ep.second);
        const uint32_t np_ = q.pu32(ep.first, ep.second + 8);
        std::set<uint32_t> S;
        for (uint32_t j = 0; j < np_; j++) {
            const uint64_t po = pp.second + 0xd0ull * j;
            const ORef ir = bg::res(q, pp.first, po + 0x10);
            const uint32_t ni = q.pu32(pp.first, po + 0x18);
            for (uint32_t i = 0; i < (ir ? ni : 0); i++) S.insert(rd32le(q.d, q.pbase(ir->first) + ir->second + 16ull * i + 4));
        }
        out[h] = S;
    }
    return out;
}
std::map<std::string, std::vector<std::set<uint32_t>>> stream_slots(const Pak &q) {   // material -> per submesh, its stream slots
    std::map<std::string, std::vector<std::set<uint32_t>>> out;
    const PakItem g = q.items_of("GEOMETRY_1").at(0);
    const uint64_t n = q.u64(q.pbase(g.page) + g.data_off() + 8);
    const Ref sr = bg::need(q, g.page, g.data_off() + 0x10);
    for (uint64_t si = 0; si < n; si++) {
        const uint64_t so = sr.second + 0x88 * si;
        const ORef mr = bg::res(q, sr.first, so + 0x48);
        if (!mr) continue;
        std::set<uint32_t> have;
        for (const auto &[which, co] : std::vector<std::pair<uint64_t, uint64_t>>{{0x30, 0x28}, {0x38, 0x2c}}) {
            const uint32_t ns = q.pu32(sr.first, so + co);
            const ORef st = bg::res(q, sr.first, so + which);
            for (uint32_t k = 0; k < (st ? ns : 0); k++) {
                const uint64_t e = st->second + 0x18ull * k;
                const Ref f = bg::need(q, st->first, e + 8);
                have.insert(q.d.at(q.pbase(f.first) + f.second));
            }
        }
        out[before(q.pstr(mr->first, mr->second), ',')].push_back(have);
    }
    return out;
}

using Values = std::vector<std::pair<std::string, std::vector<double>>>;   // name -> floats, in insertion order
const std::vector<double> *vget(const Values &v, const std::string &k) {
    for (const auto &e : v)
        if (e.first == k) return &e.second;
    return nullptr;
}

struct NewTex {
    std::string shrt, full;
    uint32_t hash, fmt, cls;
    ItemP desc, name;
};
using NewP = std::shared_ptr<NewTex>;

struct Built {
    std::optional<Bytes> data;
    std::vector<std::vector<std::string>> log;
};

Built build(const std::string &OV, const std::string &pak, const std::vector<Job> &rows, const Pak &p3) {
    Built res;
    const Bytes raw = read_file(upath(OV + pak));
    const Pak q(raw, pak);
    const std::vector<Mat4> M = mats4(q);
    std::map<std::string, const Mat4 *> byn;
    for (const Mat4 &m : M) byn[m.base()] = &m;
    const auto SU = stream_slots(q);
    const auto FX = effect_inputs(q);
    const auto T3 = p3.textures();
    PakWriter w;
    w.flags = uint16_t(q.h[0] >> 16);
    std::map<uint32_t, std::vector<std::pair<uint64_t, size_t>>> byp;
    for (const Pak::Fixup &f : q.fixups) byp[f.src].push_back({f.off, f.dst});
    std::vector<RawPage *> raws;
    for (uint32_t pi = 0; pi < q.pages.size(); pi++)
        raws.push_back(&w.raw_page(Bytes(q.d.begin() + std::ptrdiff_t(q.pbase(pi)), q.d.begin() + std::ptrdiff_t(q.pbase(pi) + q.pages[pi].size)), byp[pi]));
    auto rp = [&](uint32_t pg) { return unowned(*raws.at(pg)); };
    Page &newp = w.page();
    auto strs = std::make_shared<Blob>("strs2");
    Bytes vram(q.d.begin() + q.vram_base, q.d.end());
    std::map<std::string, PakTexture> TB;
    for (const PakTexture &t : q.textures()) {
        const size_t e = t.path.rfind('/');
        const size_t b = e == std::string::npos ? std::string::npos : t.path.rfind('/', e - 1);
        if (e != std::string::npos) TB[t.path.substr(b == std::string::npos ? 0 : b + 1, e - (b == std::string::npos ? 0 : b + 1))] = t;
    }
    std::vector<std::pair<std::string, NewP>> nw;   // short -> new texture, in creation order
    auto addtex = [&](const std::string &shrt, const Bytes &texels, uint32_t fmt, uint32_t mips, uint32_t w_, uint32_t h_, uint32_t cls) {
        for (const auto &[k, v] : nw)
            if (k == shrt) return v;
        const std::string sn = shrt + "/" + u3::fmt("%016llX", (unsigned long long)sid(shrt)) + ".ndb", pn = PFX + sn;
        const uint32_t hsh = sid(sn);
        while (vram.size() % 0x100) vram.push_back(0);
        const uint64_t voff = vram.size();
        vram.insert(vram.end(), texels.begin(), texels.end());
        auto db = std::make_shared<Blob>("vd");
        db->u64(0).u64(voff).u32(uint32_t(texels.size())).u32(hsh).u32(1).u32(fmt).u32(fmt == 0x48 || fmt == 0x4e ? 0x010f0000 : 0x01000000);
        db->u32(mips).u32(w_).u32(h_).u32(0).u32(3);
        db->ptr(db, 0x40);
        db->cstr(pn);
        auto nb = std::make_shared<Blob>("tn");
        nb->cstr(sn);
        auto e = std::make_shared<NewTex>();
        e->name = newp.item("RAW_DATA", nb);
        e->desc = newp.item("VRAM_DESC", db);
        e->shrt = shrt, e->full = pn.substr(0, pn.size() - 4), e->hash = hsh, e->fmt = fmt, e->cls = cls;
        nw.push_back({shrt, e});
        return e;
    };
    auto blendtex = [&](const std::string &nrm, bool invert) {
        const PakTexture *t3 = ps3_tex(T3, nrm);
        if (!t3) throw Stop("ValueError: no PS3 normal " + nrm);
        const Chain c = chain(p3, *t3);
        Bytes gb = blueblocks(c.lin, c.bpb);
        if (invert) gb = bc1_invert(gb);
        return addtex("art/generated/u3mp/zblend/" + nrm.substr(0, nrm.size() - 4) + (invert ? "-zblendinv.tga" : "-zblend.tga"), tc::ps3_to_ps4_texels(gb, c.w, c.h, c.m, 8), 0x47, c.m, c.w,
                      c.h, 4);
    };
    auto donor_ok = [&](const Mat4 &d, const std::string &mat) -> std::pair<bool, std::vector<uint32_t>> {
        auto fi = FX.find(d.h ? *d.h : "");
        if (fi == FX.end()) throw Stop("KeyError: " + (d.h ? *d.h : std::string("None")));
        std::set<uint32_t> need = fi->second;
        need.erase(15);
        auto si = SU.find(mat);
        if (si != SU.end())
            for (const auto &have : si->second) {
                std::vector<uint32_t> miss;
                for (uint32_t x : need)
                    if (!have.count(x)) miss.push_back(x);
                if (!miss.empty()) return {false, miss};
            }
        return {true, {}};
    };
    struct Ed {
        const Mat4 *m, *d;
        std::vector<std::pair<std::string, std::pair<std::string, std::string>>> ov;   // binding -> (short, full)
        Values pv;
    };
    std::vector<Ed> edits;
    auto ps3p = [](const Row &r) {
        Values v;
        for (const auto &[k, x] : r.ps3p) v.push_back({k, x});
        return v;
    };
    for (const Job &r : rows) {
        auto mi = byn.find(r.mat);
        if (mi == byn.end()) {
            res.log.push_back({"skip", r.mat, "not in pak"});
            continue;
        }
        const Mat4 &m = *mi->second;
        try {
            if (r.cls == "Zflat") {
                const NewP e = blendtex(r.nrm, has(m.f, INV));
                edits.push_back({&m, nullptr, {{"g_tNdFetchBlendMap", {e->shrt, e->full}}}, {}});
                res.log.push_back({"Zflat", r.mat, e->shrt});
            } else if (r.cls == "Zvc") {
                const size_t c = r.mat.find(':');
                if (c == std::string::npos || r.mat.find(':', c + 1) != std::string::npos)
                    throw Stop(std::string("ValueError: ") + (c == std::string::npos ? "not enough values to unpack (expected 2, got 1)" : "too many values to unpack (expected 2)"));
                const std::string stem = r.mat.substr(0, c), var = r.mat.substr(c + 1);
                const Mat4 *d = nullptr;
                for (const Mat4 &x : M)
                    if (x.n && starts(*x.n, stem + "-") && ends(x.base(), ":" + var) && has(x.f, BLENDP) && has(x.f, "FINAL_COLOR_ADD_SPECULAR_LIGHT") && donor_ok(x, r.mat).first) {
                        d = &x;
                        break;
                    }
                if (!d) {
                    res.log.push_back({"skip", r.mat, "no sibling effect with compatible streams"});
                    continue;
                }
                const NewP e = blendtex(r.nrm, has(d->f, INV));
                Values pv;
                for (const auto &[k, v] : r.row->ps3p)
                    if (!starts(k, "g_specular")) pv.push_back({k, v});   // the sibling's specular is already converted
                edits.push_back({&m, d, {{"g_tNdFetchBlendMap", {e->shrt, e->full}}}, pv});
                res.log.push_back({"Zvc", r.mat, d->h ? *d->h : "None", e->shrt});
            } else {
                const Mat4 *d = nullptr;
                for (const Mat4 &x : M) {
                    bool all = true;
                    for (const char *f : {"COLOR_3_APPLY_OVERLAY_TEXTURE", "COLOR_3_OVERLAY_USE_UV2", "COLOR_3_OVERLAY_MASK_OVERLAY_TEXTURE_A", "COLOR_1_BLEND_COLOR_2",
                                          "BLEND_FACTOR_SOFTNESS", "COLOR_2_APPLY_COLOR_TINT"})
                        all = all && has(x.f, f);
                    if (all && has(x.f, BLENDP) && has(x.f, INV) == has(m.f, INV)) {
                        d = &x;
                        break;
                    }
                }
                if (!d) {
                    res.log.push_back({"skip", r.mat, "no overlay effect of this polarity in pak"});
                    continue;
                }
                const auto [ok, miss] = donor_ok(*d, r.mat);
                if (!ok) {
                    std::string ms = "[";
                    for (size_t k = 0; k < miss.size(); k++) ms += (k ? ", " : "") + std::to_string(miss[k]);
                    res.log.push_back({"skip", r.mat, "streams lack " + ms + "]"});
                    continue;
                }
                std::pair<std::string, std::string> oe;
                auto ti = TB.find(r.ovl);
                if (ti == TB.end()) {
                    const PakTexture *t3 = ps3_tex(T3, r.ovl);
                    if (!t3) throw Stop("TypeError: no PS3 overlay texture " + r.ovl);
                    const Chain c = chain(p3, *t3);
                    if (c.kind != "DXT5" || (t3->gcmRemapCtl & 0xffff) != 0xaae4) {
                        res.log.push_back({"skip", r.mat, "overlay has no plain alpha"});
                        continue;
                    }
                    const NewP e = addtex("art/generated/u3mp/zblend/" + r.ovl, tc::ps3_to_ps4_texels(c.lin, c.w, c.h, c.m, 16), 0x4e, c.m, c.w, c.h, 5);
                    oe = {e->shrt, e->full};
                } else {
                    const std::string &pth = ti->second.path;
                    const size_t k = pth.find("texture2/");
                    if (k == std::string::npos) throw Stop("IndexError: list index out of range");
                    const std::string after = pth.substr(k + 9);
                    oe = {after.substr(0, after.rfind('/')), pth.substr(0, pth.size() - 4)};
                }
                const Bytes white = {0xff, 0xff, 0xfe, 0xff, 0, 0, 0, 0};
                const NewP wh = addtex("art/generated/u3mp/flat-white-u3mc.tga", tc::ps3_to_ps4_texels(white, 4, 4, 1, 8), 0x47, 1, 4, 4, 4);
                Values pv = ps3p(*r.row);
                for (const auto &[k, v] : std::vector<std::pair<std::string, double>>{{"g_uvOffsetU3", 0.0}, {"g_uvOffsetV3", 0.0}, {"g_uvScale3", 1.0}}) {
                    bool found = false;
                    for (auto &e : pv)
                        if (e.first == k) e.second = {v}, found = true;
                    if (!found) pv.push_back({k, {v}});
                }
                edits.push_back({&m, d, {{"g_tNdFetchOverlay3Map", oe}, {"g_tNdFetchColorTintMask2Map", {wh->shrt, wh->full}}}, pv});
                res.log.push_back({"stripe", r.mat, d->h ? *d->h : "None", r.ovl});
            }
        } catch (const Stop &ex) {
            const std::string w_ = ex.what();
            if (!starts(w_, "ValueError: ")) throw;
            res.log.push_back({"skip", r.mat, w_.substr(12)});
        }
    }
    if (edits.empty()) return res;
    const PakItem g = q.items_of("GEOMETRY_1").at(0);
    const uint32_t gpg = g.page;
    const uint64_t gb = g.data_off();
    ItemP vitem;
    if (!nw.empty()) {
        const PakItem tt = q.items_of("TEXTURE_TABLE").at(0);
        const uint32_t tp = tt.page;
        const uint64_t tb = tt.data_off();
        const uint64_t n0 = q.u64(q.pbase(tp) + tb);
        const Ref hp = bg::need(q, tp, tb + 8), npp = bg::need(q, tp, tb + 0x10), dp = bg::need(q, tp, tb + 0x18);
        struct T {
            uint32_t hash;
            ORef name, desc;
            NewP e;
        };
        std::vector<T> allt;
        for (uint64_t i = 0; i < n0; i++) allt.push_back({q.pu32(hp.first, hp.second + 4 * i), bg::res(q, npp.first, npp.second + 8 * i), bg::res(q, dp.first, dp.second + 8 * i), nullptr});
        for (const auto &[k, e] : nw) allt.push_back({e->hash, std::nullopt, std::nullopt, e});
        std::stable_sort(allt.begin(), allt.end(), [](const T &a, const T &b) { return a.hash < b.hash; });
        auto hb = std::make_shared<Blob>("h2"), nb = std::make_shared<Blob>("n2"), db = std::make_shared<Blob>("d2");
        for (const T &e : allt) {
            hb->u32(e.hash);
            if (!e.e) nb->ptr(rp(bg::need(e.name).first), int64_t(e.name->second)), db->ptr(rp(bg::need(e.desc).first), int64_t(e.desc->second));
            else nb->ptr(e.e->name->data), db->ptr(e.e->desc->data);
        }
        for (const auto &bl : {hb, nb, db}) newp.item("RAW_DATA", bl);
        raws[tp]->set_u64(tb, allt.size());
        raws[tp]->redirect(tb + 8, hb);
        raws[tp]->redirect(tb + 0x10, nb);
        raws[tp]->redirect(tb + 0x18, db);
        std::map<std::pair<uint32_t, uint64_t>, std::pair<uint32_t, uint64_t>> vmap;
        for (const auto &[pg, off] : q.vram_desc_table())
            if (pg) vmap[{*pg, off + 0x20}] = {*pg, off};
        auto vb = std::make_shared<Blob>("vt2");
        vb->u32(uint32_t(allt.size())).u32(0);
        for (const T &e : allt) {
            if (!e.e) {
                auto vi = vmap.find({e.desc->first, e.desc->second});
                if (vi == vmap.end()) throw Stop("KeyError: VRAM desc not in the table");
                vb->ptr(rp(vi->second.first), int64_t(vi->second.second));
            } else
                vb->ptr(e.e->desc->data, -0x20);
        }
        vitem = newp.item("VRAM_DESC_TABLE", vb, "", false);
        const uint32_t ntex = q.pu32(gpg, gb + 0x38);
        const Ref gtp = bg::need(q, gpg, gb + 0x40);
        auto geob = std::make_shared<Blob>("gt2");
        for (uint32_t i = 0; i < ntex; i++) {
            const uint64_t e = gtp.second + 0x28ull * i;
            const Ref s = bg::need(q, gtp.first, e);
            geob->ptr(rp(s.first), int64_t(s.second));
            const uint64_t a = q.pbase(gtp.first) + e;
            geob->raw(Bytes(q.d.begin() + std::ptrdiff_t(a + 8), q.d.begin() + std::ptrdiff_t(a + 0x28)));
        }
        for (const auto &[k, e] : nw) {
            geob->ptr(strs, int64_t(strs->cstr(e->full)));
            geob->u32(e->hash).u32(0x400).u32(0x101).i32(-1).u32(0).u32(e->cls).u64(0);
        }
        newp.item("RAW_DATA", geob);
        raws[gpg]->set_u32(gb + 0x38, ntex + uint32_t(nw.size()));
        raws[gpg]->redirect(gb + 0x40, geob);
    }
    const Ref mref = bg::need(q, gpg, gb + 0x50);
    const uint32_t mp = mref.first;
    const uint64_t mo = mref.second;
    static const std::regex unit_rx("^(g_color(Tint|Brightness)|g_uvScale)");
    for (const Ed &ed : edits) {
        const Mat4 &m = *ed.m;
        const uint64_t o = mo + m.i * 0x48;
        const Mat4 &src = ed.d ? *ed.d : m;
        const uint64_t so = mo + src.i * 0x48;
        if (ed.d) {
            const Ref fr = bg::need(q, mp, so + 0x10), hr = bg::need(q, mp, so + 0x18);
            raws[mp]->redirect(o + 0x10, rp(fr.first), int64_t(fr.second));
            raws[mp]->redirect(o + 0x18, rp(hr.first), int64_t(hr.second));
            const Ref pr = bg::need(q, mp, so + 0x20);
            auto pb = std::make_shared<Blob>("par2"), vb_ = std::make_shared<Blob>("val2");
            for (size_t k = 0; k < ed.d->p.size(); k++) {
                const Param &dp_ = ed.d->p[k];
                const uint64_t po = pr.second + 0x18ull * k;
                const Ref nr = bg::need(q, pr.first, po);
                std::vector<double> v;
                bool got = false;
                if (const auto *x = vget(ed.pv, dp_.name); x && !x->empty()) v = *x, got = true;
                if (!got)
                    for (const Param &op : m.p)   // dict: the last of a name
                        if (op.name == dp_.name) {
                            if (op.vals) v.assign(op.vals->begin(), op.vals->end()), got = true;
                            else got = false, v.clear();
                        }
                if (!got) {
                    if (std::regex_search(dp_.name, unit_rx)) v = {1.0, 1.0, 1.0, 1.0}, got = true;
                    else if (dp_.vals) v.assign(dp_.vals->begin(), dp_.vals->end()), got = true;
                    else throw Stop("TypeError: 'NoneType' object is not iterable");
                }
                const size_t vo = vb_->size();
                float f4[4] = {0, 0, 0, 0};
                for (size_t j = 0; j < 4 && j < v.size(); j++) f4[j] = float(v[j]);
                Bytes vb4(16);
                std::memcpy(vb4.data(), f4, 16);
                vb_->raw(vb4);
                pb->ptr(rp(nr.first), int64_t(nr.second));
                pb->ptr(vb_, int64_t(vo));
                const uint64_t a = q.pbase(pr.first) + po;
                pb->raw(Bytes(q.d.begin() + std::ptrdiff_t(a + 0x10), q.d.begin() + std::ptrdiff_t(a + 0x18)));
            }
            newp.item("RAW_DATA", vb_);
            newp.item("RAW_DATA", pb);
            raws[mp]->redirect(o + 0x20, pb);
            std::memcpy(&raws[mp]->data.at(o + 0x30), &q.d.at(q.pbase(mp) + so + 0x30), 0x10);
        }
        const Ref br = bg::need(q, mp, so + 0x28), ot = bg::need(q, mp, o + 0x28);
        std::map<std::string, uint64_t> tgt;
        for (size_t k = 0; k < m.b.size(); k++) tgt[m.b[k].first] = ot.second + 0x28 * k;
        auto bb = std::make_shared<Blob>("bnd2");
        for (size_t k = 0; k < src.b.size(); k++) {
            const std::string &nm = src.b[k].first;
            const uint64_t e = br.second + 0x28 * k;
            const Ref nr = bg::need(q, br.first, e);
            bb->ptr(rp(nr.first), int64_t(nr.second));
            auto oi = std::find_if(ed.ov.begin(), ed.ov.end(), [&](const auto &x) { return x.first == nm; });
            if (oi != ed.ov.end()) {
                bb->ptr(strs, int64_t(strs->cstr(oi->second.first)));
                bb->ptr(strs, int64_t(strs->cstr(oi->second.second)));
            } else {
                const auto ti = tgt.find(nm);
                const uint32_t pg2 = ti != tgt.end() ? ot.first : br.first;
                const uint64_t e2 = ti != tgt.end() ? ti->second : e;
                const Ref a = bg::need(q, pg2, e2 + 8), c = bg::need(q, pg2, e2 + 0x10);
                bb->ptr(rp(a.first), int64_t(a.second));
                bb->ptr(rp(c.first), int64_t(c.second));
            }
            const uint64_t ab = q.pbase(br.first) + e;
            bb->raw(Bytes(q.d.begin() + std::ptrdiff_t(ab + 0x18), q.d.begin() + std::ptrdiff_t(ab + 0x28)));
        }
        newp.item("RAW_DATA", bb);
        raws[mp]->redirect(o + 0x28, bb);
    }
    newp.item("RAW_DATA", strs);
    w.login_page = std::make_pair(size_t(q.h[2]), size_t(q.h[3]));
    w.vram_table_loc = std::make_pair(size_t(q.h[9]), size_t(q.h[10]));
    if (vitem) w.vram_table_item = vitem;   // its place is known after the layout
    w.vram = vram;
    Bytes data = w.build();
    if (const size_t rem = newp.size % 16) {
        strs->raw(Bytes(16 - rem, 0));
        data = w.build();
        if (newp.size % 16) throw Stop("AssertionError: new page size");
    }
    res.data = data;
    return res;
}

// the lightmap-flag fix of map-colors (its manifest) still holds in the result
bool lm_locked(const std::string &W, const std::string &OV, const std::string &pak, const Bytes &data) {
    const fs::path mf = upath(W + "backup-mapcolor/manifest.json");
    if (!fs::exists(mf)) throw Stop("FileNotFoundError: " + utf8(mf));
    const json::Value man = read_json(mf);
    const json::Value *e = nullptr;
    for (const auto &[k, v] : man.fields)
        if (k == pak) e = &v;
    if (!e) return true;
    const Pak o(read_file(upath(OV + pak)), pak), n(data, pak);
    for (const auto &[k, v] : e->fields) {
        if (k != "edits") continue;
        for (const json::Value &ed : v.items) {
            uint64_t off = 0;
            for (const auto &[kk, vv] : ed.fields)
                if (kk == "off") off = uint64_t(vv.i);
            bool found = false;
            for (uint32_t pi = 0; pi < o.pages.size() && pi < n.pages.size(); pi++) {
                const uint64_t po = o.pbase(pi), ps = o.pages[pi].size, po2 = n.pbase(pi);
                if (po <= off && off < po + ps) {
                    found = true;
                    if (std::memcmp(&o.d.at(off), &n.d.at(po2 + off - po), 4) != 0 || (rd32le(n.d, po2 + off - po) & 0x100)) return false;
                    break;
                }
            }
            if (!found) throw Stop("ValueError: " + std::to_string(off));
        }
    }
    return true;
}

std::string crlf(const std::string &s) {
    std::string o;
    for (char c : s) o += c == 10 ? std::string{char(13), char(10)} : std::string(1, c);
    return o;
}

}  // namespace

int map_colors2(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply", "--revert", "--concrete-stripes"}, {"--project", "--job", "--ps3-disc", "--ps3-update", "--out"});
    const bool apply = a.has("--apply"), conc = a.has("--concrete-stripes");
    const std::string W = a.need("--project") + "/work/", OV = W + "overlay-r3/u3data/build/main/pak23/ps4/", BK = W + "backup-mapcolor2/";
    const std::string out = a.get("--out", "");
    if ((apply || a.has("--revert")) && game_running()) throw Stop("game running: stop it first");
    if (a.has("--revert")) {
        for (const auto &e : fs::directory_iterator(upath(BK))) {
            const std::string f = utf8(e.path().filename());
            if (!ends(f, ".pak")) continue;
            copy2(e.path(), upath(OV + f));
            std::printf("restored %s\n", f.c_str());
        }
        return 0;
    }
    // the PS3 paks: pak23.psarc, goty-dlc, dlc-patch13, dlc-patch18; the last one that has a name wins
    const std::string D = a.need("--ps3-disc"), U = a.need("--ps3-update");
    std::vector<std::unique_ptr<Psarc>> arcs;
    std::map<std::string, std::pair<size_t, size_t>> idx;   // basename -> (archive, entry)
    for (const std::string &f : {D + "/build/main/pak23.psarc", D + "/goty-dlc.psarc", U + "/dlc-patch13.psarc", U + "/dlc-patch18.psarc"}) {
        arcs.push_back(std::make_unique<Psarc>(upath(f)));
        for (size_t i = 0; i < arcs.back()->names.size(); i++) idx[last_part(arcs.back()->names[i])] = {arcs.size() - 1, i + 1};
    }
    // survey + detect
    std::vector<std::string> names;
    for (const auto &e : fs::directory_iterator(upath(OV))) names.push_back(utf8(e.path().filename()));
    std::sort(names.begin(), names.end());
    std::map<std::string, std::vector<Mat4>> O;
    std::vector<Row> R;
    for (const std::string &fn : names) {
        if (!ends(fn, ".pak") || ends(fn, "-ingame12.pak")) continue;
        std::vector<Mat4> mats;
        try {
            mats = mats4(Pak(read_file(upath(OV + fn)), fn));
        } catch (const std::exception &) {
            continue;   // the survey records an error for it: no materials
        }
        if (mats.empty() || !idx.count(fn)) {
            O[fn] = std::move(mats);
            continue;
        }
        const auto [ai, ei] = idx[fn];
        std::map<std::string, Facts> F;
        try {
            F = facts(Pak(arcs[ai]->read(ei), fn));
        } catch (const std::exception &e) {
            std::printf("ERR %s %.100s\n", fn.c_str(), e.what());
            O[fn] = std::move(mats);
            continue;
        }
        const auto rows = rows_of(fn, mats, F);
        R.insert(R.end(), rows.begin(), rows.end());
        O[fn] = std::move(mats);
    }
    // plan, per pak in name order
    std::map<std::string, std::vector<Job>> by;
    for (const Row &r : R) {
        const auto jobs = plan(std::vector<Row>{r}, O[r.pak], conc);
        for (Job j : jobs) {
            j.row = &r;
            by[r.pak].push_back(j);
        }
    }
    json::Value res = json::Value::object();
    for (const auto &[pak, rows] : by) {
        const auto [ai, ei] = idx.at(pak);
        const Pak p3(arcs[ai]->read(ei), pak);
        Built b = build(OV, pak, rows, p3);
        for (const auto &l : b.log) {
            std::string s = pak;
            for (const auto &x : l) s += " " + x;
            std::printf("%s\n", s.c_str());
        }
        if (!b.data) continue;
        if (!lm_locked(W, OV, pak, *b.data)) {
            std::printf("%s SKIP: lightmap-flag lock fails (base pak differs from the recorded state)\n", pak.c_str());
            continue;
        }
        const Bytes orig = read_file(upath(OV + pak));
        json::Value e = json::Value::object();
        e.fields.push_back({"md5_before", json::Value::text(md5_hex(orig))});
        e.fields.push_back({"md5_after", json::Value::text(md5_hex(*b.data))});
        json::Value ed;
        ed.kind = json::Value::Array;
        for (const auto &l : b.log) {
            if (l[0] == "skip") continue;
            json::Value x;
            x.kind = json::Value::Array;
            for (const auto &s : l) x.items.push_back(json::Value::text(s));
            ed.items.push_back(x);
        }
        e.fields.push_back({"edits", ed});
        const long long grew = (long long)b.data->size() - (long long)orig.size();
        e.fields.push_back({"grew", json::Value::integer(grew)});
        res.fields.push_back({pak, e});
        std::printf("%s OK +%lld bytes\n", pak.c_str(), grew);
        std::fflush(stdout);
        if (!out.empty()) {
            fs::create_directories(upath(out));
            write_file_replace(upath(out) / upath(pak), *b.data);
        }
        if (apply) {
            fs::create_directories(upath(BK));
            copy2(upath(OV + pak), upath(BK + pak));
            write_file_replace(upath(OV + pak), *b.data);
        }
    }
    const std::string js = crlf(json_dump_indent1(res));
    write_file_replace(upath(a.need("--job") + "/plan-result.json" + (conc ? ".concrete" : "")), Bytes(js.begin(), js.end()));
    if (apply) write_file_replace(upath(BK + "manifest.json"), Bytes(js.begin(), js.end()));
    std::printf("%s\n", apply ? "APPLIED" : "DRY RUN: overlay not written");
    return 0;
}

}  // namespace u3
