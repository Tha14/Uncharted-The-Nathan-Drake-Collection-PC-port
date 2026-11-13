// Recipe steps wall-emblem and wall-emblem-actors: the Airstrip wall emblems as separate actors. Replaces
// mkemblem_v2.py / mkemblem.py, prep_p3.py, apply_air_emblem.py and actor33/install_emblem_actors.py.
//
// The PS3 air-multi.pak draws the team emblems as background patches (materials with 'emblem' in the name). PS4 needs
// them as foreground actors so the helper can set the emblem texture per instance:
//   * every instance of every emblem material group of the PS3 PM_COLLECTION_2 becomes one patch: its blocks moved to
//     world space by the instance matrix (normals / tangents by the matrix scaled to determinant 1), clipped along
//     uv1 = 0 and 1 (pieces outside get uv1 (0, 0), so nothing relies on the overlay sampler's wrap mode);
//   * each patch becomes an actor pak air-multi-emblem-<wall>[-<instance>].pak built on the retail loading-menu-cloth
//     effects (its first two materials) and the 1-joint NdSimpleObject skeleton of cargo-plane-int-moving-multi: a
//     single prototype, positions centred, a shared map with a flat normal and blue 166 (inside the retail cloth range,
//     so layer 4 stays), white overlay and transparency, the wall's own colour map from the converted air-multi.pak,
//     and an Overlay4 base in the wall's mean tone;
//   * air-multi-ingame12.pak is converted again with one spawner per actor (NdSimpleObject at the patch centre);
//   * the emblem submeshes of the converted air-multi.pak get all their indices set to 0 (hidden), and pak23_ps4.txt
//     lists the actors after 'actor air-multi-wind-socks' in the air-multi block.
// Everything is built from the current files in the same run, so no recorded hashes are needed: the state of the
// overlay tells what is left to do.
//
//   u3fix wall-emblem --project P --job J --ps4 PS4 --ps3-disc D [--apply | --restore]
//       the whole fix. Backups (once) in work/backup-wall-emblem/<path>; --restore puts them back and deletes the actors.
//   u3fix wall-emblem-actors --project P --job J --ps4 PS4 --ps3-disc D [--apply]
//       only the actor paks: an actor that is not the build (or the build with the cbuffer capacity 3 of emblem-cap3)
//       is written again.
// Dry run without --apply.
#include "ndpak.hpp"
#include "ps3bg.hpp"
#include "ps4actor.hpp"
#include "ps4ingame.hpp"
#include "ps4writer.hpp"
#include "psarc.hpp"
#include "texcodec.hpp"
#include "u3fix.hpp"

#include <array>
#include <cmath>
#include <cstring>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace u3 {

namespace {

using namespace u3::actor;
using BlobP = std::shared_ptr<Blob>;
using ItemP = std::shared_ptr<PakWItem>;

// ---- the PS3 patches -------------------------------------------------------------------------------------------
// vertex data of a patch, float64 as decoded; uv0 / uv1 2 columns, nrm 3, tan 4
struct Verts {
    std::vector<std::array<double, 3>> pos, nrm;
    std::vector<std::array<double, 2>> uv0, uv1;
    std::vector<std::array<double, 4>> tan;
    size_t size() const { return pos.size(); }
    void push_from(const Verts &s, size_t i) {
        pos.push_back(s.pos[i]), nrm.push_back(s.nrm[i]), uv0.push_back(s.uv0[i]), uv1.push_back(s.uv1[i]), tan.push_back(s.tan[i]);
    }
};
using Tri = std::array<uint32_t, 3>;

struct Patch {
    size_t proto, inst;
    std::string mat;
    Verts v;
    std::vector<Tri> tris;
    size_t nout = 0;
    bool blackboard = false;
};

// a*b+c rounded once (the BLAS kernels the reference used multiply-add in one step)
double fma_(double a, double b, double c) { return std::fma(a, b, c); }

// |det| of a 3x3 matrix as numpy gives it: LU with partial pivoting, then exp(sum of log |pivot|)
double abs_det3(double a[3][3]) {
    double logdet = 0;
    for (int c = 0; c < 3; c++) {
        int p = c;
        for (int r = c + 1; r < 3; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (p != c) std::swap(a[p], a[c]);
        if (a[c][c] == 0) return 0;
        const double inv = 1.0 / a[c][c];
        for (int r = c + 1; r < 3; r++) {
            a[r][c] *= inv;
            for (int k = c + 1; k < 3; k++) a[r][k] = fma_(-a[r][c], a[c][k], a[r][k]);
        }
        logdet += std::log(std::fabs(a[c][c]));
    }
    return std::exp(logdet);
}

// a PS3 background block (bggeom.decode_block without instance data): positions, normals, tangents, uv0, uv1, triangles
struct BgBlock {
    uint32_t nv = 0;
    std::map<int, Mat> a;   // by attribute id, the last of an id wins
    std::vector<Tri> tris;
};
BgBlock decode_bg_block(const Pak &p, const Ref &blk) {
    const auto [pg, bo] = blk;
    BgBlock r;
    const Ref cnt = bg::need(p, pg, bo + 0x24);
    r.nv = p.pu32(cnt.first, cnt.second);
    const Ref f = bg::need(p, pg, bo + 0x28), s = bg::need(p, pg, bo);
    const std::vector<bg::Attr> attrs = bg::read_format(p, f.first, f.second);
    for (size_t ai = 0; ai < attrs.size(); ai++) {
        const bg::Attr &at = attrs[ai];
        if (at.id == 0x4c || at.id == 0x4e) continue;   // per-instance data
        const ORef rr = bg::res(p, s.first, s.second + 8 * ai);
        int sw = 0;
        for (int c = 0; c < at.n(); c++) sw += at.w[c];
        if (sw && !rr) throw Stop("ValueError: missing stream");
        r.a[at.id] = bg::decode_attr(p, f.first, f.second, at, rr ? rr->first : 0, rr ? rr->second : 0, r.nv);
    }
    const Ref ii = bg::need(p, pg, bo + 4);
    const uint64_t ib = p.pbase(ii.first) + ii.second;
    const uint32_t ntris = bg::be16(p, ib), isz = bg::be16(p, ib + 4);
    const Ref idat = bg::need(p, ii.first, ii.second + 8);
    const uint64_t bi = p.pbase(idat.first) + idat.second;
    if (bi + isz > p.d.size()) throw Stop("struct.error: index data past the end");
    r.tris = bg::decode_indices(p.d.data() + bi, isz, ntris);
    return r;
}
const Mat &attr(const BgBlock &b, int id, const char *name) {
    auto it = b.a.find(id);
    if (it == b.a.end()) throw Stop(std::string("KeyError: '") + name + "'");
    return it->second;
}

// split on uv1 = 0 / 1; pieces outside get their own vertices with uv1 (0, 0)
void clip(Verts &V, std::vector<Tri> &tris, size_t &nout) {
    auto lerp = [&](uint32_t a, uint32_t b, double t) {
        auto mix = [&](auto &vec) {
            auto x = vec[a], y = vec[b];
            for (size_t k = 0; k < x.size(); k++) {
                const double p = x[k] * (1 - t), q = y[k] * t;
                x[k] = p + q;
            }
            vec.push_back(x);
        };
        mix(V.pos), mix(V.uv0), mix(V.uv1), mix(V.nrm), mix(V.tan);
        return uint32_t(V.size() - 1);
    };
    std::vector<std::pair<Tri, bool>> cur;
    for (const Tri &t : tris) cur.push_back({t, true});
    const struct {
        int ax;
        double lim;
        int keep;
    } planes[] = {{0, 0.0, 1}, {0, 1.0, -1}, {1, 0.0, 1}, {1, 1.0, -1}};
    for (const auto &pl : planes) {
        std::vector<std::pair<Tri, bool>> nxt;
        for (const auto &[t, ins] : cur) {
            if (!ins) {
                nxt.push_back({t, false});
                continue;
            }
            double d[3];
            for (int j = 0; j < 3; j++) d[j] = pl.keep * (V.uv1[t[size_t(j)]][size_t(pl.ax)] - pl.lim);
            if (std::min({d[0], d[1], d[2]}) >= -1e-5) {
                nxt.push_back({t, true});
                continue;
            }
            if (std::max({d[0], d[1], d[2]}) <= 1e-5) {
                nxt.push_back({t, false});
                continue;
            }
            std::vector<uint32_t> pi, po;
            for (int j = 0; j < 3; j++) {
                const uint32_t a = t[size_t(j)], b = t[size_t((j + 1) % 3)];
                const double da = d[j], db = d[(j + 1) % 3];
                (da >= 0 ? pi : po).push_back(a);
                if ((da >= 0) != (db >= 0)) {
                    const uint32_t m = lerp(a, b, da / (da - db));
                    pi.push_back(m), po.push_back(m);
                }
            }
            for (int side = 0; side < 2; side++) {
                const auto &L = side == 0 ? pi : po;
                for (size_t k = 1; k + 1 < L.size(); k++) nxt.push_back({Tri{L[0], L[k], L[k + 1]}, side == 0});
            }
        }
        cur = std::move(nxt);
    }
    tris.clear();
    nout = 0;
    for (const auto &[t, ins] : cur) {
        if (ins) {
            tris.push_back(t);
            continue;
        }
        Tri nt;
        for (int j = 0; j < 3; j++) {
            V.push_from(V, t[size_t(j)]);
            V.uv1.back() = {0.0, 0.0};
            nt[size_t(j)] = uint32_t(V.size() - 1);
        }
        tris.push_back(nt);
        nout++;
    }
}

std::vector<Patch> patches(const Pak &P3) {
    std::vector<Patch> res;
    const std::vector<bg::Proto> col = bg::collection(P3);
    for (size_t pi = 0; pi < col.size(); pi++) {
        const bg::Proto &c = col[pi];
        for (const bg::Group &g : c.groups) {
            if (g.mat.find("emblem") == std::string::npos) continue;
            for (size_t ii = 0; ii < c.insts.size(); ii++) {
                const bg::Instance I = bg::instance(P3, bg::need(c.insts[ii]));
                double M[4][3], R[3][3], Rd[3][3];
                for (int r = 0; r < 4; r++)
                    for (int k = 0; k < 3; k++) M[r][k] = I.mat[r * 4 + k];
                for (int r = 0; r < 3; r++)
                    for (int k = 0; k < 3; k++) R[r][k] = Rd[r][k] = I.mat[r * 4 + k];
                const double cb = std::cbrt(abs_det3(Rd));
                double Rn[3][3];
                for (int r = 0; r < 3; r++)
                    for (int k = 0; k < 3; k++) Rn[r][k] = R[r][k] / cb;
                Patch pt;
                pt.proto = pi, pt.inst = ii, pt.mat = g.mat, pt.blackboard = g.mat.find("blackboard") != std::string::npos;
                uint32_t base = 0;
                for (uint32_t k = g.first; k < g.first + g.count; k++) {
                    const BgBlock d = decode_bg_block(P3, c.blocks.at(k));
                    const Mat &pos = attr(d, 1, "pos"), &nrm = attr(d, 2, "normal"), &tan = attr(d, 3, "tangent");
                    const Mat &uv0 = attr(d, 0x11, "uv0"), &uv1 = attr(d, 0x12, "uv1");
                    for (uint32_t i = 0; i < d.nv; i++) {
                        const double ph[4] = {pos.at(i, 0), pos.at(i, 1), pos.at(i, 2), 1.0};
                        std::array<double, 3> w{};
                        for (size_t j = 0; j < 3; j++) {   // [x y z 1] @ M, the 4th column unused
                            double acc = ph[0] * M[0][j];
                            for (int q = 1; q < 4; q++) acc = fma_(ph[q], M[q][j], acc);
                            w[j] = acc;
                        }
                        auto rot = [&](const Mat &m) {
                            std::array<double, 3> o{};
                            for (size_t j = 0; j < 3; j++) {
                                double acc = m.at(i, 0) * Rn[0][j];
                                for (int q = 1; q < 3; q++) acc = fma_(m.at(i, size_t(q)), Rn[q][j], acc);
                                o[j] = acc;
                            }
                            return o;
                        };
                        pt.v.pos.push_back(w);
                        pt.v.nrm.push_back(rot(nrm));
                        const auto t3 = rot(tan);
                        pt.v.tan.push_back({t3[0], t3[1], t3[2], tan.cols > 3 ? tan.at(i, 3) : 1.0});
                        pt.v.uv0.push_back({uv0.at(i, 0), uv0.at(i, 1)});
                        pt.v.uv1.push_back({uv1.at(i, 0), uv1.at(i, 1)});
                    }
                    for (const Tri &t : d.tris) pt.tris.push_back({t[0] + base, t[1] + base, t[2] + base});
                    base += d.nv;
                }
                clip(pt.v, pt.tris, pt.nout);
                res.push_back(std::move(pt));
            }
        }
    }
    return res;
}

// ---- the actor pak -------------------------------------------------------------------------------------------
struct Tex {
    uint32_t hash = 0, fmt = 0;
    std::string shrt, full;
    ItemP desc, name;
};
using TexP = std::shared_ptr<Tex>;

std::string bindfull(const Tex &e) { return e.full.size() >= 4 && e.full.compare(e.full.size() - 4, 4, ".ndb") == 0 ? e.full.substr(0, e.full.size() - 4) : e.full; }

struct Inputs {
    Pak T, SK, MAP;
    std::vector<TMat> tm;                     // loading-menu-cloth's materials
    std::map<std::string, TMat> mapm;         // air-multi's materials by name before ','
    std::map<std::string, PakTexture> maptx;  // air-multi's textures by path
    uint32_t cb3 = 8, cb4 = 1;
};

struct Built {
    std::string art, color;
    Bytes pak;
    std::array<double, 3> center;
    size_t verts, tris, nout;
};

Built build_actor(const Inputs &in, const std::string &art, const Patch &pt) {
    PakWriter w;
    auto [span, sit] = extract_effect_span(in.T);
    AutoPages p0(w, std::max<int64_t>(1, (int64_t(sit[0].off) - 0x14) / 16));
    span->modulo = uint64_t(1) << 40;
    span->phase = sit[0].off;
    p0.span(span);
    Bytes vram;
    std::vector<TexP> tex;
    auto add = [&](const std::string &shrt, uint32_t h, uint32_t fmt, uint32_t tw, uint32_t th, uint32_t mips, const Bytes &tiled, uint32_t flags = 3,
                   std::optional<uint32_t> unk20 = std::nullopt) {
        while (vram.size() % 0x100) vram.push_back(0);
        const uint64_t off = vram.size();
        vram.insert(vram.end(), tiled.begin(), tiled.end());
        auto nb = std::make_shared<Blob>("texname");
        nb->cstr(shrt);
        auto e = std::make_shared<Tex>();
        e->name = p0.item("RAW_DATA", nb);
        e->full = PS4_COOK_PREFIX + shrt;
        auto db = std::make_shared<Blob>("vramdesc");
        db->u64(0).u64(off).u32(uint32_t(tiled.size())).u32(h);
        db->u32(1).u32(fmt).u32(unk20 ? *unk20 : (fmt == 0x48 || fmt == 0x4b || fmt == 0x4e) ? 0x010f0000 : 0x01000000);
        db->u32(mips).u32(tw).u32(th).u32(0).u32(flags);
        db->ptr(db, 0x40);
        db->cstr(e->full);
        e->hash = h, e->shrt = shrt, e->fmt = fmt;
        e->desc = p0.item("VRAM_DESC", db);
        tex.push_back(e);
        return e;
    };
    auto flat = [&](const std::string &tag, const uint32_t rgb[3], uint32_t fmt = 0x47) {
        const auto [lin, m] = bc1_flat(64, 64, rgb);
        const Bytes ti = tc::ps3_to_ps4_texels(lin, 64, 64, m, 8);
        const std::string sh = u3::fmt("art/generated/u3mp/emblem-%s.tga/%016X.ndb", tag.c_str(), sid(tag));
        return add(sh, sid(sh), fmt, 64, 64, m, ti);
    };
    const size_t sl = pt.mat.rfind('/');
    const std::string key = pt.mat.substr(0, sl) + ":" + pt.mat.substr(sl + 1);
    auto mmi = in.mapm.find(key);
    if (mmi == in.mapm.end()) throw Stop("KeyError: '" + key + "' (air-multi.pak material)");
    const TMat &mm = mmi->second;
    auto f32 = [](const Bytes &d, size_t i) {
        float f;
        std::memcpy(&f, &d.at(4 * i), 4);
        return double(f);
    };
    std::map<std::string, std::array<double, 3>> pv3;
    std::map<std::string, double> pv;
    for (const auto &p : mm.params) {
        if (p.cnt == 3) pv3[p.name] = {f32(p.vals, 0), f32(p.vals, 1), f32(p.vals, 2)};
        pv[p.name] = f32(p.vals, 0);
    }
    const TMat::Bind *cb = nullptr;
    for (const auto &b : mm.binds)
        if (b.pname == "g_tNdFetchColor1Map") {
            cb = &b;
            break;
        }
    if (!cb) throw Stop("IndexError: no colour map bound to " + key);
    auto ti = in.maptx.find(cb->full);
    if (ti == in.maptx.end()) ti = in.maptx.find(cb->full + ".ndb");
    if (ti == in.maptx.end()) throw Stop("TypeError: colour map " + cb->full + " not in air-multi.pak");
    const PakTexture &t = ti->second;
    const std::string full = t.path;
    const size_t pk = full.find(PS4_COOK_PREFIX);
    const std::string shrt = pk != std::string::npos ? full.substr(pk + PS4_COOK_PREFIX.size()) : full;
    const Bytes tvram = in.MAP.vram_bytes(t.vramOffset, t.vramSize);
    TexP ecol = add(shrt, t.hash, t.format, t.width, t.height, t.mips, tvram, t.unk34, t.unk20);
    ecol->full = full;
    const uint32_t c_shared[3] = {128, 128, 166}, c_white[3] = {255, 255, 255};
    TexP eshared = flat("shared-flat-b166", c_shared);
    TexP ewhite = flat("white", c_white);
    // layer 4 has no colour map: its base (Overlay4Map, on uv1) gets the wall's mean tone
    std::string kind;
    int bpb;
    switch (t.format) {
    case 0x47: case 0x48: kind = "BC1", bpb = 8; break;
    case 0x4a: case 0x4b: kind = "BC2", bpb = 16; break;
    case 0x4d: case 0x4e: kind = "BC3", bpb = 16; break;
    default: throw Stop(u3::fmt("KeyError: colour map format %u", t.format));
    }
    const tc::Img img = tc::decode(tc::ps4_mip0_linear(tvram, t.width, t.height, bpb), t.width, t.height, kind);
    double sum[3] = {0, 0, 0};
    const size_t npx = size_t(img.w) * img.h;
    for (size_t i = 0; i < npx; i++)
        for (int c = 0; c < 3; c++) sum[c] += img.px[i * 4 + size_t(c)];
    const std::array<double, 3> one = {1, 1, 1};
    const std::array<double, 3> tint = pv3.count("g_colorTint1") ? pv3["g_colorTint1"] : pv3.count("g_colorTint2") ? pv3["g_colorTint2"] : one;
    uint32_t tone[3];
    for (int c = 0; c < 3; c++) {
        const double mean = sum[c] / double(npx) / 255.0;
        const double x = mean * std::pow(tint[size_t(c)], 1 / 2.2) * 255;
        tone[c] = uint32_t(int(std::min(std::max(x, 0.0), 255.0)));
    }
    TexP etone = flat("tone-" + art, tone, 0x48);

    // two materials: the cloth's first (the emblem) with the wall's tint, brightness and specular, and its second
    const std::vector<TMat> &tm = in.tm;
    if (tm.size() < 2) throw Stop("IndexError: loading-menu-cloth has fewer than 2 materials");
    auto pack4 = [](double a, double b, double c, double d) {
        Bytes o(16);
        const float f[4] = {float(a), float(b), float(c), float(d)};
        std::memcpy(o.data(), f, 16);
        return o;
    };
    auto get = [&](const std::string &k, double def) { return pv.count(k) ? pv[k] : def; };
    auto ov = [&](const std::string &nm, const Bytes &vals) -> Bytes {
        auto starts = [&](const char *s) { return nm.rfind(s, 0) == 0; };
        if (nm == "g_ambientOcclusionTint" || starts("g_facingTintIn") || starts("g_facingTintOut")) return pack4(1, 1, 1, 0);
        if (nm == "g_colorTint1") return pack4(tint[0], tint[1], tint[2], 0);
        if (nm == "g_colorBrightness1") return pack4(get("g_colorBrightness1", get("g_colorBrightness2", 1.0)), 0, 0, 0);
        if (nm == "g_normalStrength1" || nm == "g_specularMetalness" || nm == "g_uvOffsetU2" || nm == "g_uvOffsetV2") return pack4(0, 0, 0, 0);
        if (nm == "g_specularBrightness") return pack4(get("g_specularBrightness", 0.0), 0, 0, 0);
        if (nm == "g_specularExponentMax") return pack4(get("g_specularExponentMax", 10.0), 0, 0, 0);
        if (starts("g_uvScale")) return pack4(1, 0, 0, 0);
        return vals;
    };
    const std::map<std::string, TexP> b0 = {{"g_tNdFetchSharedTexture1Map", eshared}, {"g_tNdFetchColor1Map", ecol}, {"g_tNdFetchOverlay1Map", ewhite},
                                            {"g_tNdFetchOverlay4Map", etone},      {"g_tNdFetchTransparencyMap", ewhite}};
    const std::string hx = md5_hex(Bytes(art.begin(), art.end()));
    const std::string guid = hx.substr(0, 8) + "-" + hx.substr(8, 4) + "-" + hx.substr(12, 4) + "-" + hx.substr(16, 4) + "-" + hx.substr(20, 12);
    struct Rec {
        std::string name;
        const TMat *tm;
        std::vector<std::tuple<std::string, uint32_t, Bytes>> params;
        std::vector<std::pair<std::string, TexP>> binds;
    };
    std::vector<Rec> recs(2);
    recs[0].name = "objects/levels/multiplayer/" + art + ":default," + guid + "," + tm[0].shader;
    recs[0].tm = &tm[0];
    for (const auto &p : tm[0].params) recs[0].params.push_back({p.name, p.cnt, ov(p.name, p.vals)});
    for (const auto &b : tm[0].binds) {
        auto it = b0.find(b.pname);
        if (it == b0.end()) throw Stop("KeyError: '" + b.pname + "'");
        recs[0].binds.push_back({b.pname, it->second});
    }
    recs[1].name = tm[1].name;
    recs[1].tm = &tm[1];
    for (const auto &p : tm[1].params) recs[1].params.push_back({p.name, p.cnt, p.vals});
    for (const auto &b : tm[1].binds) recs[1].binds.push_back({b.pname, b.pname.find("Color") != std::string::npos ? ewhite : eshared});

    auto ttab = p0.item("TEXTURE_TABLE");
    std::vector<TexP> order = tex;
    std::stable_sort(order.begin(), order.end(), [](const TexP &a, const TexP &b) { return a->hash < b->hash; });
    auto nb = std::make_shared<Blob>("ttnames");
    for (const auto &e : order) nb->ptr(e->name->data);
    auto nit = p0.item("RAW_DATA", nb);
    auto hb = std::make_shared<Blob>("tthash");
    for (const auto &e : order) hb->u32(e->hash);
    auto hit = p0.item("RAW_DATA", hb);
    BlobP tb = ttab->data;
    tb->u64(order.size()).ptr(hit->data).ptr(nit->data);
    tb->ptr(tb, 0x20);
    for (const auto &e : order) tb->ptr(e->desc->data);
    auto strs = std::make_shared<Blob>("geostrings");
    std::map<std::string, size_t> sp;
    auto S = [&](const std::string &s) {
        auto it = sp.find(s);
        if (it != sp.end()) return it->second;
        return sp[s] = strs->cstr(s);
    };
    auto raw = [&](const Bytes &d) {
        auto b = std::make_shared<Blob>("raw");
        b->raw(d);
        return p0.item("RAW_DATA", b)->data;
    };
    const Verts &v = pt.v;
    const size_t nv = v.size();
    std::array<double, 3> c{};
    for (int k = 0; k < 3; k++) {
        double lo = v.pos[0][size_t(k)], hi = lo;
        for (const auto &p : v.pos) lo = std::min(lo, p[size_t(k)]), hi = std::max(hi, p[size_t(k)]);
        c[size_t(k)] = (lo + hi) / 2;
    }
    std::vector<std::array<float, 3>> pos(nv);
    float rad = 0;
    for (size_t i = 0; i < nv; i++) {
        for (int k = 0; k < 3; k++) pos[i][size_t(k)] = float(v.pos[i][size_t(k)] - c[size_t(k)]);
        const float sx = pos[i][0] * pos[i][0], sy = pos[i][1] * pos[i][1], sz = pos[i][2] * pos[i][2];
        const float n2 = (sx + sy) + sz;
        rad = std::max(rad, std::sqrt(n2));
    }
    auto fmtb = std::make_shared<Blob>("vtxfmt");
    std::map<int, size_t> fo;
    const std::vector<std::pair<int, std::array<uint8_t, 4>>> fmts = {{0, {0, 0, 12, 6}},    {1, {1, 0, 4, 0x22}}, {12, {12, 0, 4, 0x22}},
                                                                      {2, {2, 0, 8, 0x0a}}, {3, {3, 0, 8, 0x0a}}, {4, {4, 0, 8, 0x0a}}};
    for (const auto &[s_, f_] : fmts) {
        fo[s_] = fmtb->size();
        fmtb->raw(Bytes(f_.begin(), f_.end()));
    }
    p0.item("RAW_DATA", fmtb);
    auto h16 = [](Bytes &o, double x) { const uint16_t h = f16b(x); o.push_back(uint8_t(h)), o.push_back(uint8_t(h >> 8)); };
    Bytes sp0, s1, s12, s2, s3, s4;
    for (size_t i = 0; i < nv; i++) {
        const uint8_t *pb = reinterpret_cast<const uint8_t *>(pos[i].data());
        sp0.insert(sp0.end(), pb, pb + 12);
        for (int k = 0; k < 2; k++) h16(s1, v.uv0[i][size_t(k)]), h16(s12, v.uv1[i][size_t(k)]);
        for (int k = 0; k < 3; k++) h16(s2, v.nrm[i][size_t(k)]);
        h16(s2, 1.0);
        for (int k = 0; k < 4; k++) h16(s3, v.tan[i][size_t(k)]), h16(s4, 1.0);
    }
    const std::vector<std::tuple<int, uint32_t, const Bytes *>> streams = {{0, 12, &sp0}, {1, 4, &s1}, {12, 4, &s12}, {2, 8, &s2}, {3, 8, &s3}, {4, 8, &s4}};
    auto st = std::make_shared<Blob>("streams");
    for (const auto &[s_, sd, d] : streams) {
        auto db = raw(*d);
        st->u32((sd << 16) | 1).u32(uint32_t(d->size())).ptr(fmtb, int64_t(fo[s_])).ptr(db);
    }
    auto sti = p0.item("RAW_DATA", st);
    Bytes ibd, cnts, offs, js, ws;
    for (const Tri &t : pt.tris)
        for (uint32_t x : t) ibd.push_back(uint8_t(x)), ibd.push_back(uint8_t(x >> 8));
    auto ib = raw(ibd);
    const uint32_t ni = uint32_t(pt.tris.size() * 3);
    for (size_t i = 0; i < nv; i++) {
        cnts.push_back(1), cnts.push_back(0);
        offs.push_back(uint8_t(i)), offs.push_back(uint8_t(i >> 8));
        js.push_back(0), js.push_back(0);
        const float one_f = 1.0f;
        const uint8_t *ob = reinterpret_cast<const uint8_t *>(&one_f);
        ws.insert(ws.end(), ob, ob + 4);
    }
    auto bc = raw(cnts), bo = raw(offs), bj = raw(js), bw = raw(ws);
    auto skb = std::make_shared<Blob>("skin");
    skb->u32(0).u32(uint32_t(nv)).ptr(bc).ptr(bo).ptr(bj).ptr(bw);
    auto ski = p0.item("RAW_DATA", skb);
    const Pak &SK = in.SK;
    const auto jhs = SK.items_of("JOINT_HIERARCHY"), fgs = SK.items_of("FOREGROUND_SECTION_2");
    if (jhs.empty() || fgs.empty()) throw Stop("IndexError: cargo-plane-int-moving-multi.pak without its skeleton");
    const PakItem &jh = jhs[0], &fgS = fgs[0];
    const uint32_t skel = SK.pu32(fgS.page, fgS.data_off() + 8), hier = SK.pu32(fgS.page, fgS.data_off() + 12);
    const Ref jr = bg::need(SK, fgS.page, fgS.data_off() + 0x10);
    const PakItem jit = SK.item_at(jr.first, uint32_t(jr.second - 0x20));
    const uint64_t jb = SK.pbase(jr.first) + jr.second;
    const Bytes jnames(SK.d.begin() + std::ptrdiff_t(jb), SK.d.begin() + std::ptrdiff_t(std::min<uint64_t>(jb + jit.size, SK.d.size())));
    auto subs = std::make_shared<Blob>("submeshes");
    const std::string sname = art + ".Prototype0.Submesh0";
    subs->u64(0).ptr(strs, int64_t(S(sname))).u32(1).u32(0).u32(0).u32(0).u32(uint32_t(nv)).u32(ni).u32(uint32_t(streams.size())).u32(0).ptr(sti->data).u64(0).ptr(ib);
    subs->ptr(strs, int64_t(S(recs[0].name))).i32(-1).u32(0).u32(1).u32(0).ptr(ski->data).raw(Bytes(0x20, 0));
    p0.item("RAW_DATA", subs);
    const std::string pname = "Prototype0." + art;
    auto fl = std::make_shared<Blob>("pf");
    fl->raw(Bytes{1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0});
    auto sn = std::make_shared<Blob>("sn");
    sn->ptr(strs, int64_t(S(sname))).i32(-1).u32(0);
    auto pb = std::make_shared<Blob>("prototypes");
    pb->u32(3).u32(1).u32(0).u32(0x5791a421).ptr(strs, int64_t(S(pname))).ptr(sn).ptr(fl).u64(0).u32(in.cb3).u32(in.cb4).raw(Bytes(0x78 - 0x38, 0));
    auto pl = std::make_shared<Blob>("pl");
    pl->ptr(pb);
    for (const auto &b : {pb, fl, sn, pl}) p0.item("RAW_DATA", b);
    std::vector<TexP> gt;
    for (const Rec &m : recs)
        for (const auto &[n, e] : m.binds)
            if (std::find(gt.begin(), gt.end(), e) == gt.end()) gt.push_back(e);
    auto texb = std::make_shared<Blob>("gt");
    for (const auto &e : gt) texb->ptr(strs, int64_t(S(bindfull(*e)))).u32(e->hash).u32(0x200).u32(0x101).i32(-1).u32(0).u32(4).u64(0);
    auto fe = std::make_shared<Blob>("f"), pa = std::make_shared<Blob>("p"), va = std::make_shared<Blob>("v"), bi = std::make_shared<Blob>("b"),
         ma = std::make_shared<Blob>("m");
    for (const Rec &m : recs) {
        const TMat &tmm = *m.tm;
        const size_t fo_ = fe->size();
        for (const auto &f : tmm.feats) fe->ptr(strs, int64_t(S(f)));
        const size_t po = pa->size();
        for (const auto &[n_, c_, vals] : m.params) {
            const size_t vo = va->size();
            va->raw(vals);
            pa->ptr(strs, int64_t(S(n_))).ptr(va, int64_t(vo)).u32(c_).u32(0);
        }
        const size_t bo_ = bi->size();
        for (const auto &[pn, e] : m.binds) {
            const std::string sh = e->shrt.size() >= 4 && e->shrt.compare(e->shrt.size() - 4, 4, ".ndb") == 0 ? e->shrt.substr(0, e->shrt.rfind('/')) : e->shrt;
            bi->ptr(strs, int64_t(S(pn))).ptr(strs, int64_t(S(sh))).ptr(strs, int64_t(S(bindfull(*e)))).i32(-1).u32(0).u64(0);
        }
        ma->ptr(strs, int64_t(S(m.name))).ptr(strs, int64_t(S(tmm.shader))).ptr(fe, int64_t(fo_)).ptr(strs, int64_t(S(tmm.hash))).ptr(pa, int64_t(po)).ptr(bi, int64_t(bo_));
        ma->u32(uint32_t(tmm.feats.size())).u32(uint32_t(m.params.size())).u32(uint32_t(m.binds.size())).u32(tmm.x3).u32(tmm.tail).u32(0);
    }
    for (const auto &b : {fe, pa, va, bi, ma, texb}) p0.item("RAW_DATA", b);
    p0.item("RAW_DATA", strs);
    auto gb = std::make_shared<Blob>("geometry");
    gb->u32(0x10).u32(1).u64(1).ptr(subs).u64(1).ptr(pl).u64(0).ptr(nullptr).u64(gt.size()).ptr(texb).u64(2).ptr(ma).u64(0).u64(0);
    auto geo = p0.item("GEOMETRY_1", gb);
    auto jn = raw(jnames);
    auto fb = std::make_shared<Blob>("fg");
    fb->ptr(fb, 0x1c).u32(skel).u32(hier).ptr(jn);
    fb->u32(0);   // 0.0f
    fb->cstr(pname);
    auto fg = p0.item("FOREGROUND_SECTION_2", fb, art);
    auto ji = p0.item("JOINT_HIERARCHY", copy_item_blob(SK, jh), jh.name);
    auto tv = std::make_shared<Blob>("t");
    {
        const float f[4] = {0, 0, 0, rad};
        Bytes b(16);
        std::memcpy(b.data(), f, 16);
        tv->raw(b);
    }
    auto tv_ = p0.item("TAG_VEC4", tv, "vis_sphere");
    auto tiB = std::make_shared<Blob>("t");
    tiB->i32(0);
    auto ti_ = p0.item("TAG_INT", tiB, "vis_joint_index");
    const uint32_t keys[2] = {sid(jh.name), sid("geo." + art)};
    const int nb_ = 7, n = 2;
    auto ab = std::make_shared<Blob>("artgroup");
    ab->u32(n).u32(1).ptr(ab, 0x50).ptr(ab, 0x30).u64(0).u64(0).u32(PAD32).u32(PAD32);
    const int64_t tbl = 0x50 + 16 * n, x = tbl + 16 * nb_;
    ab->u64(0).u32(1).u32(0x12345).ptr(ab, x).u32(PAD32).u32(PAD32);
    ab->ptr(ji->data, -0x20).u64(0);
    ab->u64(0).u64(0);
    std::optional<std::pair<uint32_t, int>> bk[nb_];
    for (int k = 0; k < 2; k++) {
        int i = int(keys[k] % nb_);
        while (bk[i]) i = (i + 1) % nb_;
        bk[i] = std::make_pair(keys[k], k);
    }
    for (const auto &e : bk) {
        if (!e) ab->u32(0).u32(PAD32).u64(0);
        else ab->u32(e->first).u32(PAD32).ptr(ab, 0x50 + 16 * e->second);
    }
    ab->u32(0).u32(PAD32).u64(0).u64(0).u32(0).u32(nb_).ptr(ab, tbl);
    auto ag = p0.item("ARTGROUP_DATA", ab, art);
    auto lp = std::make_shared<Blob>("lp");
    lp->u64(0);
    auto lpi = p0.item("LEVEL_PATS_1", lp);
    auto pkb = std::make_shared<Blob>("pk");
    pkb->ptr(pkb, 0x20).u32(0).u32(1).ptr(pkb, int64_t(0x20 + ((art.size() + 1 + 15) / 16) * 16)).u32(PAD32).u32(PAD32);
    pkb->cstr(art);
    pkb->pad(16, 0xaa);
    pkb->u32(sid(art)).u32(PAD32).ptr(ag->data, -0x20);
    auto pki = p0.item("PACKAGE_ITEM_LOOKUP", pkb);
    Page &p1 = w.page();
    std::vector<ItemP> descs;
    for (const auto &e : tex) descs.push_back(e->desc);
    make_login_and_vram_page(w, p1, {nullptr, nullptr, nullptr}, descs);
    w.vram = vram;
    auto span_item = std::make_shared<PakWItem>();   // the EFFECT_TABLE inside the copied span
    span_item->type = "EFFECT_TABLE";
    span_item->span_ref = span;
    for (const PakItem &i : sit)
        if (i.type == "EFFECT_TABLE") {
            span_item->span_pos = i.off - sit[0].off;
            break;
        }
    w.login_targets = {lpi, ag, span_item, ttab, geo, ji, fg, pki, tv_, ti_};
    Bytes lb(8 + 8 * w.login_targets.size(), 0);
    lb[0] = uint8_t(w.login_targets.size());
    w.login_item->data->b = lb;
    Built r;
    r.art = art;
    r.pak = w.build();
    r.center = c;
    r.verts = nv, r.tris = pt.tris.size(), r.nout = pt.nout;
    const std::string dir = shrt.substr(0, shrt.rfind('/'));
    r.color = dir.substr(dir.rfind('/') + 1);
    return r;
}

const std::vector<std::pair<std::string, std::string>> SHORT = {{"ground-concrete", "ground"},      {"metal-diamond", "diamond"}, {"corrugated-main-blue", "corr-blue"},
                                                                {"corrugated-main", "corr"},        {"control-tower", "tower"},   {"stucco-brown", "stucco"},
                                                                {"blackboard", "board"}};

struct Job {
    std::string P, main;
    fs::path retail;
    Bytes p3map, p3ingame;
    fs::path levels_data;
};

Job job_of(const Args &a) {
    Job j;
    j.P = a.need("--project");
    j.main = j.P + "/work/overlay-r3/u3data/build/main/";
    j.retail = upath(a.need("--ps4")) / "u3data" / "build" / "main" / "actor33" / "ps4";
    j.levels_data = upath(a.need("--job")) / ".." / "pak23-levels" / "data" / "conv_anim_actors.json";
    Psarc A(upath(a.need("--ps3-disc")) / "build" / "main" / "pak23.psarc");   // the PS3 levels the conversion used
    for (const char *n : {"air-multi.pak", "air-multi-ingame12.pak"}) {
        bool found = false;
        for (size_t i = 0; i < A.names.size() && !found; i++) {
            std::string x = "/" + A.names[i];
            for (char &ch : x)
                if (ch == '\\') ch = '/';
            const std::string want = std::string("/") + n;
            if (x.size() >= want.size() && x.compare(x.size() - want.size(), want.size(), want) == 0) {
                (std::string(n) == "air-multi.pak" ? j.p3map : j.p3ingame) = A.read(i + 1);
                found = true;
            }
        }
        if (!found) throw Stop(std::string(n) + " not in pak23.psarc");
    }
    return j;
}

std::vector<Built> build_actors(const Job &j, std::vector<Patch> &pts) {
    const fs::path mp = upath(j.main + "pak23/ps4/air-multi.pak");
    if (!fs::exists(mp)) throw Stop("missing " + j.main + "pak23/ps4/air-multi.pak (the converted level; convert-levels-r3 first)");
    Inputs in{Pak(j.retail / "loading-menu-cloth.pak"), Pak(j.retail / "cargo-plane-int-moving-multi.pak"), Pak(mp), {}, {}, {}};
    in.tm = tmpl_materials(in.T);
    for (const PakTexture &t : in.MAP.textures()) in.maptx[t.path] = t;
    for (const TMat &m : tmpl_materials(in.MAP)) in.mapm[m.name.substr(0, m.name.find(','))] = m;
    for (const PakItem &i : in.T.items)   // the cloth prototypes' cbuffer capacities (at least 8 / 1)
        if (i.type == "RAW_DATA" && i.size >= 0x38) {
            const uint64_t o = in.T.pbase(i.page) + i.data_off();
            if (rd32le(in.T.d, o) == 3 && rd32le(in.T.d, o + 12) == 0x5791a421)
                in.cb3 = std::max(in.cb3, rd32le(in.T.d, o + 0x30)), in.cb4 = std::max(in.cb4, rd32le(in.T.d, o + 0x34));
        }
    const Pak P3(j.p3map, "air-multi.pak");
    pts = patches(P3);
    std::vector<Built> out;
    for (const Patch &pt : pts) {
        std::string s;
        for (const auto &[k, v] : SHORT)
            if (pt.mat.find(k) != std::string::npos) {
                s = v;
                break;
            }
        if (s.empty()) throw Stop("IndexError: no short name for " + pt.mat);
        const std::string art = "air-multi-emblem-" + s + (pt.blackboard ? "-" + std::to_string(pt.inst) : "");
        out.push_back(build_actor(in, art, pt));
        const Built &r = out.back();
        std::printf("%s %zu %zu outside %zu %s %zu %#x\n", art.c_str(), r.verts, r.tris, r.nout, r.color.c_str(), r.pak.size(), sid(art + "-spawner"));
    }
    return out;
}

// the build with emblem-cap3's change: the prototype's cbuffer capacity 3 (all but the ground pak)
Bytes with_cap3(const Built &b) {
    if (b.art == "air-multi-emblem-ground") return b.pak;
    Bytes d = b.pak;
    const uint8_t sig[16] = {3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0x21, 0xa4, 0x91, 0x57};
    for (size_t o = 0; o + 0x38 <= d.size(); o += 16)
        if (std::memcmp(&d[o], sig, 16) == 0) {
            d[o + 0x30] = 3, d[o + 0x31] = d[o + 0x32] = d[o + 0x33] = 0;
            return d;
        }
    return d;
}

}  // namespace

int wall_emblem_actors(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply"}, {"--project", "--job", "--ps4", "--ps3-disc"});
    const Job j = job_of(a);
    std::vector<Patch> pts;
    const std::vector<Built> bs = build_actors(j, pts);
    const std::string dst = j.main + "actor33/ps4/";
    int todo = 0;
    for (const Built &b : bs) {
        const fs::path f = upath(dst + b.art + ".pak");
        if (fs::exists(f)) {
            const Bytes cur = read_file(f);
            if (cur == b.pak || cur == with_cap3(b)) continue;
        }
        todo++;
        std::printf("install %s %s\n", b.art.c_str(), sha256_hex(b.pak).substr(0, 16).c_str());
        if (a.has("--apply")) {
            fs::create_directories(upath(dst));
            write_file_replace(f, b.pak);
        }
    }
    if (!todo) std::printf("emblem actors already installed (%zu)\n", bs.size());
    else std::printf("%s\n", a.has("--apply") ? "done" : "dry run: nothing written (use --apply)");
    return 0;
}

int wall_emblem(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply", "--restore"}, {"--project", "--job", "--ps4", "--ps3-disc"});
    const bool apply = a.has("--apply");
    const std::string P = a.need("--project");
    const std::string MAIN = P + "/work/overlay-r3/u3data/build/main/", BAK = P + "/work/backup-wall-emblem/";
    const std::vector<std::string> files = {"pak23/ps4/air-multi.pak", "pak23/ps4/air-multi-ingame12.pak", "pak23_ps4.txt"};
    if (a.has("--restore")) {
        for (const std::string &rel : files) {
            std::printf("restore %s\n", rel.c_str());
            if (!fs::exists(upath(BAK + rel))) throw Stop("no backup: " + BAK + rel);
            if (apply) copy2(upath(BAK + rel), upath(MAIN + rel));
        }
        for (const auto &e : fs::directory_iterator(upath(MAIN + "actor33/ps4"))) {
            const std::string n = utf8(e.path().filename());
            if (n.rfind("air-multi-emblem-", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".pak") == 0) {
                std::printf("delete actor %s\n", n.substr(0, n.size() - 4).c_str());
                if (apply) fs::remove(e.path());
            }
        }
        std::printf("%s\n", apply ? "restored" : "dry run: nothing written (use --apply)");
        return 0;
    }
    const Job j = job_of(a);
    std::vector<Patch> pts;
    const std::vector<Built> bs = build_actors(j, pts);
    // the level paks: the ingame12 converted again with the spawners, the emblem submeshes of air-multi.pak hidden
    const std::set<std::string> anim = anim_actor_names(j.retail, j.levels_data);
    const Pak p3ig(j.p3ingame, "air-multi-ingame12.pak");
    std::vector<ExtraSpawner> extra;
    for (const Built &b : bs) extra.push_back({b.art + "-spawner", b.art, {b.center[0], b.center[1], b.center[2]}});
    const Bytes ingame = ps4ingame_convert(p3ig, anim, extra);
    Bytes map = read_file(upath(MAIN + files[0]));
    const Bytes map0 = map;
    {
        const Pak M(map0, "air-multi.pak");
        const auto gs = M.items_of("GEOMETRY_1");
        if (gs.empty()) throw Stop("IndexError: air-multi.pak without GEOMETRY_1");
        const uint32_t pg = gs[0].page;
        const uint64_t gb = gs[0].data_off();
        const uint64_t ns = M.u64(M.pbase(pg) + gb + 8);
        const Ref sr = bg::need(M, pg, gb + 0x10);
        std::vector<std::pair<std::string, uint32_t>> hidden;
        for (uint64_t i = 0; i < ns; i++) {
            const uint64_t o = sr.second + 0x88 * i;
            const Ref mr = bg::need(M, sr.first, o + 0x48);
            const std::string mat = M.pstr(mr.first, mr.second);
            if (mat.find("emblem") == std::string::npos) continue;
            const uint32_t ni = M.pu32(sr.first, o + 0x24);
            const Ref ib = bg::need(M, sr.first, o + 0x40);
            const uint64_t f = M.pbase(ib.first) + ib.second;
            if (ni % 3) throw Stop("AssertionError: emblem submesh index count");
            if (f + 2ull * ni > map.size()) throw Stop("struct.error: emblem indices past the end");
            std::fill(map.begin() + std::ptrdiff_t(f), map.begin() + std::ptrdiff_t(f + 2ull * ni), uint8_t(0));
            const Ref nr = bg::need(M, sr.first, o + 8);
            hidden.push_back({M.pstr(nr.first, nr.second), ni});
        }
        std::printf("hidden");
        for (const auto &[s, n] : hidden) std::printf(" %s:%u", s.c_str(), n);
        std::printf("\n");
    }
    const bool map_done = map == map0;
    const Bytes cur_ig = fs::exists(upath(MAIN + files[1])) ? read_file(upath(MAIN + files[1])) : Bytes();
    const bool ig_done = cur_ig == ingame;
    const Bytes txt = read_file(upath(MAIN + files[2]));
    const std::string ts(txt.begin(), txt.end());
    const std::string nl = ts.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const bool lines = ts.find("air-multi-emblem") != std::string::npos;
    std::string newtxt = ts;
    if (!lines) {
        const std::string head = "level-name air-multi" + nl, anchor = "actor air-multi-wind-socks" + nl;
        const size_t i = ts.find(head);
        size_t k = i == std::string::npos ? i : ts.find(anchor, i);
        const size_t nxt = i == std::string::npos ? i : ts.find("level-name ", i + head.size());
        if (i == std::string::npos || k == std::string::npos || (nxt != std::string::npos && nxt < k)) throw Stop("air-multi level block not found");
        k += anchor.size();
        std::string add;
        for (const Built &b : bs) add += "actor " + b.art + nl;
        newtxt = ts.substr(0, k) + add + ts.substr(k);
    }
    std::vector<const Built *> acts;   // actor paks that are missing (an existing one is the build or a later step's edit of it)
    for (const Built &b : bs)
        if (!fs::exists(upath(MAIN + "actor33/ps4/" + b.art + ".pak"))) acts.push_back(&b);
    if (map_done && ig_done && lines && acts.empty()) {
        std::printf("already installed: %zu actors, ingame12 with their spawners, background patches hidden, registry lines present\n", bs.size());
        return 0;
    }
    std::printf("install: %zu actor paks, %s, %s, %s\n", acts.size(), ig_done ? "ingame12 already rebuilt" : "ingame12 rebuilt with the spawners",
                map_done ? "patches already hidden" : "background patches hidden", lines ? "registry lines present" : "the actor lines added to the air-multi block");
    if (!apply) {
        std::printf("dry run: nothing written (use --apply)\n");
        return 0;
    }
    for (const std::string &rel : files)   // the first state is kept: an earlier pass's backup stays
        if (!fs::exists(upath(BAK + rel))) {
            fs::create_directories(upath(BAK + rel).parent_path());
            copy2(upath(MAIN + rel), upath(BAK + rel));
        }
    fs::create_directories(upath(MAIN + "actor33/ps4"));
    for (const Built *b : acts) write_file_replace(upath(MAIN + "actor33/ps4/" + b->art + ".pak"), b->pak);
    if (!ig_done) write_file_replace(upath(MAIN + files[1]), ingame);
    if (!map_done) write_file_replace(upath(MAIN + files[0]), map);
    if (!lines) write_file_replace(upath(MAIN + files[2]), Bytes(newtxt.begin(), newtxt.end()));
    std::printf("done; backups in %s\n", BAK.c_str());
    return 0;
}

}  // namespace u3
