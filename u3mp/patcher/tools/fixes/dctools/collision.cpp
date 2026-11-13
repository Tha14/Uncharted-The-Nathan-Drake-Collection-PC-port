// dctools/collision.cpp: see collision.hpp.
#include "collision.hpp"

#include "ps3bg.hpp"
#include "u3fix.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <set>

#if defined(__clang__)
#pragma clang fp contract(off)   // float results must match numpy's
#endif

namespace u3 {
namespace {

using V3 = std::array<double, 3>;
using F3 = std::array<float, 3>;

struct Ps3Coll {
    uint32_t version = 0, count = 0;
    Bytes misc;
    std::vector<uint32_t> arr;
    bool has_mesh = false;
    std::vector<std::array<V3, 3>> tris;
    std::vector<uint32_t> weld, mat;
    std::vector<std::pair<float, uint32_t>> mats;   // (friction, flags)
};

uint32_t qbits(float f) {   // a float32 as struct.pack gives it after a double (a signalling NaN comes out quiet)
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return ((v & 0x7f800000u) == 0x7f800000u && (v & 0x7fffffu)) ? v | 0x400000u : v;
}

std::optional<Ps3Coll> ps3_collision(const Pak &p, size_t index) {
    const auto items = p.items_of("COLLISION_DATA_HAVOK_BG");
    if (items.size() <= index) return std::nullopt;
    const PakItem &it = items[index];
    const uint32_t pg = it.page;
    const uint64_t b = it.data_off(), B = p.pbase(pg) + b;
    Ps3Coll c;
    c.version = be32(p.d, B);
    be32(p.d, B + 4);
    be32(p.d, B + 8);
    c.count = be32(p.d, B + 12);
    c.misc = Bytes(p.d.begin() + std::ptrdiff_t(std::min<uint64_t>(B + 0x10, p.d.size())), p.d.begin() + std::ptrdiff_t(std::min<uint64_t>(B + 0x24, p.d.size())));
    if (const ORef r = bg::res(p, pg, b + 0x30); r && c.count)
        for (uint32_t i = 0; i < c.count; i++) c.arr.push_back(be32(p.d, p.pbase(r->first) + r->second + 4ull * i));
    const ORef r = bg::res(p, pg, b + 0x40);
    if (!r) return c;
    c.has_mesh = true;
    const auto [mpg, mo] = *r;
    const uint64_t MB = p.pbase(mpg) + mo;
    for (int k = 0; k < 6; k++) bg::bef32(p, MB + 4 * k);
    const uint32_t nchunk = be32(p.d, MB + 0x1c), nmat = be32(p.d, MB + 0x24);
    be32(p.d, MB + 0x18);
    be32(p.d, MB + 0x20);
    auto at = [&](uint64_t off) {
        const Ref rr = bg::need(p, mpg, mo + off);
        return std::make_pair(rr, p.pbase(rr.first) + rr.second);
    };
    const auto [crr, chunks] = at(0x34);
    const uint64_t qbox = at(0x38).second, mats_b = at(0x3c).second;
    for (uint32_t i = 0; i < nchunk; i++) {
        const uint64_t rec = chunks + 16ull * i;
        if (rec + 16 > p.d.size()) throw Stop("struct.error: chunk record past the end");
        const ORef dr = bg::res(p, crr.first, crr.second + 16ull * i);
        const uint32_t nidx = bg::be16(p, rec + 8), nv = bg::be16(p, rec + 10), mi = be32(p.d, rec + 12);
        if (!dr || nidx == 0) continue;
        const uint64_t D = p.pbase(dr->first) + dr->second;
        const uint64_t ib = (uint64_t(nidx) + 20) / 21 * 16, vb = (uint64_t(nv) * 6 + 15) & ~uint64_t(15);
        if (D + ib > p.d.size()) throw Stop("ValueError: chunk indices past the end");
        std::vector<uint32_t> idx;
        for (uint64_t g = 0; g < ib; g += 16)
            for (int k = 0; k < 21; k++) {   // 6-bit fields from bit 127 down
                const int top = 127 - 6 * k;   // the field's highest bit
                uint32_t v = 0;
                for (int bit = top; bit > top - 6; bit--) {
                    const int byte = 15 - bit / 8, sh = bit % 8;
                    v = v << 1 | ((p.d[D + g + uint64_t(byte)] >> sh) & 1);
                }
                idx.push_back(v);
            }
        idx.resize(nidx);
        std::vector<V3> V(nv);
        double box[6];
        for (int k = 0; k < 6; k++) box[k] = double(bg::bef32(p, qbox + 24ull * i + 4 * k));
        for (uint32_t v = 0; v < nv; v++)
            for (int c3 = 0; c3 < 3; c3++) {
                const double q = double(bg::be16(p, D + ib + 6ull * v + 2 * c3));
                V[v][c3] = box[c3] + (box[3 + c3] - box[c3]) * q / 65536.0;
            }
        const uint32_t nt = nidx / 3;
        for (uint32_t t = 0; t < nt; t++) {
            const uint32_t a = idx[3 * t], b2 = idx[3 * t + 1], cc = idx[3 * t + 2];
            if (std::max({a, b2, cc}) >= nv) throw Stop(u3::fmt("ValueError: chunk %u: index out of range", i));
            c.tris.push_back({V[a], V[b2], V[cc]});
            c.weld.push_back(bg::be16(p, D + ib + vb + 2ull * t));
            c.mat.push_back(mi);
        }
    }
    for (uint32_t k = 0; k < nmat; k++) {
        be32(p.d, mats_b + 12ull * k);
        const uint32_t fr = be32(p.d, mats_b + 12ull * k + 4), fl = be32(p.d, mats_b + 12ull * k + 8);
        const uint32_t bits = (fr & 0xffff) << 16;
        float f;
        std::memcpy(&f, &bits, 4);
        c.mats.push_back({f, fl});
    }
    return c;
}

// ------------------------------------------------------------------------------------------------ hkcdStaticMeshTree
// conservative Codec3Axis byte triple + the decoded box (as the runtime decodes it)
void enc_box(const F3 &cmin, const F3 &cmax, const F3 &pmin, const F3 &pmax, uint8_t xyz[3], F3 &dmin, F3 &dmax) {
    const float inv = float(1.0 / 226.0);
    for (int a = 0; a < 3; a++) {
        const float span = pmax[a] - pmin[a];
        int lo = 0, hi = 0;
        const float tlo = cmin[a] - 1e-6f * std::max(1.0f, std::fabs(cmin[a]));
        const float thi = cmax[a] + 1e-6f * std::max(1.0f, std::fabs(cmax[a]));
        for (int k = 15; k >= 0; k--) {
            const float v = pmin[a] + (span * float(k * k)) * inv;
            if (v <= tlo) { lo = k; break; }
        }
        for (int k = 15; k >= 0; k--) {
            const float v = pmax[a] - (span * float(k * k)) * inv;
            if (v >= thi) { hi = k; break; }
        }
        xyz[a] = uint8_t((lo << 4) | hi);
        dmin[a] = pmin[a] + (span * float(lo * lo)) * inv;
        dmax[a] = pmax[a] - (span * float(hi * hi)) * inv;
    }
}

struct Node {
    bool leaf;
    size_t id;
    F3 mn, mx;
    std::unique_ptr<Node> l, r;
    size_t nleaves = 1;
};

// median split along the longest axis of the centroid bounds
std::unique_ptr<Node> build_tree(std::vector<size_t> ids, const std::vector<F3> &bmin, const std::vector<F3> &bmax, const std::vector<F3> &cent) {
    auto n = std::make_unique<Node>();
    if (ids.size() == 1) {
        n->leaf = true;
        n->id = ids[0];
        n->mn = bmin[ids[0]];
        n->mx = bmax[ids[0]];
        return n;
    }
    F3 cmn = cent[ids[0]], cmx = cent[ids[0]];
    for (size_t i : ids)
        for (int a = 0; a < 3; a++) {
            cmn[a] = std::min(cmn[a], cent[i][a]);
            cmx[a] = std::max(cmx[a], cent[i][a]);
        }
    int ax = 0;
    float best = cmx[0] - cmn[0];
    for (int a = 1; a < 3; a++)
        if (cmx[a] - cmn[a] > best) { best = cmx[a] - cmn[a]; ax = a; }
    std::stable_sort(ids.begin(), ids.end(), [&](size_t x, size_t y) { return cent[x][ax] < cent[y][ax]; });
    const size_t h = ids.size() / 2;
    n->leaf = false;
    n->l = build_tree(std::vector<size_t>(ids.begin(), ids.begin() + std::ptrdiff_t(h)), bmin, bmax, cent);
    n->r = build_tree(std::vector<size_t>(ids.begin() + std::ptrdiff_t(h), ids.end()), bmin, bmax, cent);
    for (int a = 0; a < 3; a++) {
        n->mn[a] = std::min(n->l->mn[a], n->r->mn[a]);
        n->mx[a] = std::max(n->l->mx[a], n->r->mx[a]);
    }
    n->nleaves = n->l->nleaves + n->r->nleaves;
    return n;
}

// depth-first node bytes; 4 bytes per node (Codec3Axis4) or 5 (Codec3Axis5); leaf id -> node index
Bytes emit_tree(const Node &root, const F3 &pmin, const F3 &pmax, int nbytes, std::map<size_t, size_t> &leafnode) {
    std::vector<Bytes> out;
    std::function<void(const Node &, const F3 &, const F3 &)> rec = [&](const Node &n, const F3 &pn, const F3 &px) {
        uint8_t xyz[3];
        F3 dmin, dmax;
        enc_box(n.mn, n.mx, pn, px, xyz, dmin, dmax);
        const size_t idx = out.size();
        out.push_back({});
        Bytes b(xyz, xyz + 3);
        if (n.leaf) {
            const size_t v = n.id;
            leafnode[n.id] = idx;
            if (nbytes == 4) {
                if (v >= 128) throw Stop("AssertionError: section leaf");
                b.push_back(uint8_t(v << 1));
            } else {
                if (v >= 0x8000) throw Stop("AssertionError: top leaf");
                b.push_back(uint8_t(v >> 8));
                b.push_back(uint8_t(v));
            }
            out[idx] = b;
        } else {
            const size_t nl = n.l->nleaves;
            if (nbytes == 4) {
                if (nl >= 128) throw Stop("AssertionError: section node");
                b.push_back(uint8_t((nl << 1) | 1));
            } else {
                if (nl >= 0x8000) throw Stop("AssertionError: top node");
                b.push_back(uint8_t(0x80 | (nl >> 8)));
                b.push_back(uint8_t(nl));
            }
            out[idx] = b;
            rec(*n.l, dmin, dmax);
            rec(*n.r, dmin, dmax);
        }
    };
    rec(root, pmin, pmax);
    Bytes all;
    for (const Bytes &x : out) all.insert(all.end(), x.begin(), x.end());
    return all;
}

void put16(Bytes &o, uint32_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); }
void put32(Bytes &o, uint32_t v) { for (int k = 0; k < 4; k++) o.push_back(uint8_t(v >> (8 * k))); }
void put64(Bytes &o, uint64_t v) { for (int k = 0; k < 8; k++) o.push_back(uint8_t(v >> (8 * k))); }
void set32(Bytes &o, size_t at, uint32_t v) { for (int k = 0; k < 4; k++) o.at(at + k) = uint8_t(v >> (8 * k)); }
void set64(Bytes &o, size_t at, uint64_t v) { for (int k = 0; k < 8; k++) o.at(at + k) = uint8_t(v >> (8 * k)); }
uint32_t fbits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }

double round5(double x) { return std::nearbyint(x * 100000.0) / 100000.0; }

// tris (N, 3, 3); weld codes; user-data palette index; filter palette index. Returns the __data__ section and its
// local fixups (source offset, destination offset).
Bytes build_bvcm(const std::vector<std::array<V3, 3>> &tris, const std::vector<uint32_t> &weld, const std::vector<uint32_t> &uidx,
                 const std::vector<uint32_t> &userpal, const std::vector<uint32_t> &filterpal, const Bytes &header,
                 const std::vector<uint32_t> &fidx, std::vector<std::pair<uint32_t, uint32_t>> &loc) {
    const size_t N = tris.size();
    // sections: spatial split into <= 127 triangles / <= 255 unique vertices (vertices merged on 5 decimals)
    std::vector<V3> cent(N);
    for (size_t t = 0; t < N; t++)
        for (int c = 0; c < 3; c++) cent[t][c] = ((tris[t][0][c] + tris[t][1][c]) + tris[t][2][c]) / 3.0;
    std::map<std::array<double, 3>, size_t> vid;
    std::vector<std::array<size_t, 3>> tv(N);
    for (size_t t = 0; t < N; t++)
        for (int k = 0; k < 3; k++) {
            std::array<double, 3> key;
            for (int c = 0; c < 3; c++) {
                key[c] = round5(tris[t][k][c]);
                if (key[c] == 0) key[c] = 0;   // -0.0 and 0.0 are one key
            }
            auto it = vid.find(key);
            if (it == vid.end()) it = vid.emplace(key, vid.size()).first;
            tv[t][k] = it->second;
        }
    std::vector<std::vector<size_t>> secs;
    std::function<void(std::vector<size_t>)> srec = [&](std::vector<size_t> ids) {
        if (ids.size() <= 127) {
            std::set<size_t> vs;
            for (size_t t : ids)
                for (int k = 0; k < 3; k++) vs.insert(tv[t][k]);
            if (vs.size() <= 255) {
                secs.push_back(ids);
                return;
            }
        }
        V3 mn = cent[ids[0]], mx = cent[ids[0]];
        for (size_t t : ids)
            for (int c = 0; c < 3; c++) {
                mn[c] = std::min(mn[c], cent[t][c]);
                mx[c] = std::max(mx[c], cent[t][c]);
            }
        int ax = 0;
        double best = mx[0] - mn[0];
        for (int c = 1; c < 3; c++)
            if (mx[c] - mn[c] > best) { best = mx[c] - mn[c]; ax = c; }
        std::stable_sort(ids.begin(), ids.end(), [&](size_t a, size_t b) { return cent[a][ax] < cent[b][ax]; });
        const size_t h = ids.size() / 2;
        srec(std::vector<size_t>(ids.begin(), ids.begin() + std::ptrdiff_t(h)));
        srec(std::vector<size_t>(ids.begin() + std::ptrdiff_t(h), ids.end()));
    };
    std::vector<size_t> all(N);
    std::iota(all.begin(), all.end(), 0);
    srec(all);
    F3 dmin, dmax;
    for (int c = 0; c < 3; c++) {
        double mn = tris[0][0][c], mx = mn;
        for (const auto &t : tris)
            for (const V3 &v : t) {
                mn = std::min(mn, v[c]);
                mx = std::max(mx, v[c]);
            }
        dmin[c] = float(mn);
        dmax[c] = float(mx);
    }
    // vertices referenced by more than one section become shared (crack-free borders)
    std::map<size_t, std::set<size_t>> vsec;
    for (size_t si = 0; si < secs.size(); si++)
        for (size_t t : secs[si])
            for (int k = 0; k < 3; k++) vsec[tv[t][k]].insert(si);
    std::map<size_t, V3> vpos;
    for (size_t t = 0; t < N; t++)
        for (int k = 0; k < 3; k++) vpos.emplace(tv[t][k], tris[t][k]);
    std::vector<size_t> shared_ids;
    if (vsec.size() <= 0xffff)
        for (const auto &[v, s] : vsec) shared_ids.push_back(v);   // everything shared: 21/21/22 bits over the mesh
    else {
        for (const auto &[v, s] : vsec)
            if (s.size() > 1) shared_ids.push_back(v);
        if (shared_ids.size() > 0xffff) shared_ids.resize(0xffff);
    }
    std::map<size_t, size_t> shared_index;
    for (size_t i = 0; i < shared_ids.size(); i++) shared_index[shared_ids[i]] = i;
    double span[3];
    for (int c = 0; c < 3; c++) {
        span[c] = double(dmax[c] - dmin[c]);
        if (span[c] == 0) span[c] = 1.0;
    }
    const double SQ[3] = {double(0x1fffff), double(0x1fffff), double(0x3fffff)};
    auto enc_shared = [&](const V3 &p) {
        uint64_t q[3];
        for (int c = 0; c < 3; c++) {
            double x = std::nearbyint((p[c] - double(dmin[c])) / span[c] * SQ[c]);
            x = std::min(std::max(x, 0.0), SQ[c]);
            q[c] = uint64_t(int64_t(x));
        }
        return q[0] | (q[1] << 21) | (q[2] << 42);
    };
    auto dec_shared = [&](uint64_t v) {
        V3 o;
        const double fr[3] = {double(v & 0x1fffff) / double(0x1fffff), double((v >> 21) & 0x1fffff) / double(0x1fffff),
                              double((v >> 42) & 0x3fffff) / double(0x3fffff)};
        for (int c = 0; c < 3; c++) o[c] = double(dmin[c]) + span[c] * fr[c];
        return o;
    };
    std::vector<uint64_t> sharedV;
    for (size_t v : shared_ids) sharedV.push_back(enc_shared(vpos.at(v)));
    std::vector<uint32_t> packedV;
    std::vector<std::array<uint8_t, 4>> prims;
    std::vector<uint16_t> svi;
    struct Run {
        uint32_t v;
        uint8_t i, c;
    };
    std::vector<Run> runs;
    struct SRec {
        Bytes nodes;
        F3 smin, smax, off, scale;
        uint32_t fpv, sh, pr, dr;
        uint8_t npv, nsi;
    };
    std::vector<SRec> srecs;
    std::vector<F3> sec_min, sec_max;
    for (size_t si = 0; si < secs.size(); si++) {
        const auto &ids = secs[si];
        std::vector<size_t> vl;
        std::set<size_t> vlset;
        for (size_t t : ids)
            for (int k = 0; k < 3; k++)
                if (vlset.insert(tv[t][k]).second) vl.push_back(tv[t][k]);
        std::vector<size_t> loc_sh, loc_pk;
        for (size_t v : vl) (shared_index.count(v) ? loc_sh : loc_pk).push_back(v);
        F3 pmin{0, 0, 0}, pmax{0, 0, 0};
        if (!loc_pk.empty())
            for (int c = 0; c < 3; c++) {
                double mn = vpos.at(loc_pk[0])[c], mx = mn;
                for (size_t v : loc_pk) {
                    mn = std::min(mn, vpos.at(v)[c]);
                    mx = std::max(mx, vpos.at(v)[c]);
                }
                pmin[c] = float(mn);
                pmax[c] = float(mx);
            }
        F3 scale;
        const double DIV[3] = {2047.0, 2047.0, 1023.0}, QM[3] = {2047, 2047, 1023};
        for (int c = 0; c < 3; c++) scale[c] = float(std::max(double(pmax[c] - pmin[c]) / DIV[c], 1e-7));
        std::map<size_t, V3> dec;
        const uint32_t fpv = uint32_t(packedV.size());
        for (size_t v : loc_pk) {
            int64_t q[3];
            V3 d;
            for (int c = 0; c < 3; c++) {
                double x = std::nearbyint((vpos.at(v)[c] - double(pmin[c])) / double(scale[c]));
                x = std::min(std::max(x, 0.0), QM[c]);
                q[c] = int64_t(x);
                d[c] = double(pmin[c]) + double(scale[c]) * double(q[c]);
            }
            packedV.push_back(uint32_t(q[0] | (q[1] << 11) | (q[2] << 22)));
            dec[v] = d;
        }
        const uint32_t so = uint32_t(svi.size());
        for (size_t v : loc_sh) {
            svi.push_back(uint16_t(shared_index.at(v)));
            dec[v] = dec_shared(sharedV[shared_index.at(v)]);
        }
        std::map<size_t, size_t> lidx;
        for (size_t i = 0; i < loc_pk.size(); i++) lidx[loc_pk[i]] = i;
        for (size_t i = 0; i < loc_sh.size(); i++) lidx[loc_sh[i]] = loc_pk.size() + i;
        const uint32_t po = uint32_t(prims.size());
        std::vector<F3> tb_min, tb_max;
        for (size_t t : ids) {
            size_t a[3];
            for (int k = 0; k < 3; k++) a[k] = lidx.at(tv[t][k]);
            if (a[0] > 255 || a[1] > 255 || a[2] > 255) throw Stop("ValueError: bytes must be in range(0, 256)");
            prims.push_back({uint8_t(a[0]), uint8_t(a[1]), uint8_t(a[2]), uint8_t(a[2])});
            F3 mn, mx;
            for (int c = 0; c < 3; c++) {
                double lo = dec.at(tv[t][0])[c], hi = lo;
                for (int k = 1; k < 3; k++) {
                    lo = std::min(lo, dec.at(tv[t][k])[c]);
                    hi = std::max(hi, dec.at(tv[t][k])[c]);
                }
                mn[c] = float(lo);
                mx[c] = float(hi);
            }
            tb_min.push_back(mn);
            tb_max.push_back(mx);
        }
        F3 smin = tb_min[0], smax = tb_max[0];
        for (size_t i = 0; i < tb_min.size(); i++)
            for (int c = 0; c < 3; c++) {
                smin[c] = std::min(smin[c], tb_min[i][c]);
                smax[c] = std::max(smax[c], tb_max[i][c]);
            }
        sec_min.push_back(smin);
        sec_max.push_back(smax);
        // data runs
        const uint32_t ro = uint32_t(runs.size());
        std::vector<uint32_t> vals;
        for (size_t t : ids) vals.push_back(fidx[t] | (uidx[t] << 8) | (weld[t] << 16));
        for (size_t j = 0; j < vals.size();) {
            size_t k = j;
            while (k + 1 < vals.size() && vals[k + 1] == vals[j] && k + 1 - j < 255) k++;
            runs.push_back({vals[j], uint8_t(j), uint8_t(k - j + 1)});
            j = k + 1;
        }
        // section tree
        std::vector<F3> tcent(ids.size());
        for (size_t i = 0; i < ids.size(); i++)
            for (int c = 0; c < 3; c++) tcent[i][c] = (tb_min[i][c] + tb_max[i][c]) * 0.5f;
        std::vector<size_t> local(ids.size());
        std::iota(local.begin(), local.end(), 0);
        const auto root = build_tree(local, tb_min, tb_max, tcent);
        std::map<size_t, size_t> ln;
        SRec r;
        r.nodes = emit_tree(*root, smin, smax, 4, ln);
        r.smin = smin;
        r.smax = smax;
        r.off = pmin;
        r.scale = scale;
        r.fpv = fpv;
        r.sh = (so << 8) | uint32_t(loc_pk.size());
        r.pr = (po << 8) | uint32_t(ids.size());
        r.dr = (ro << 8) | uint32_t(runs.size() - ro);
        if (loc_pk.size() > 255 || loc_sh.size() > 255 || ids.size() > 127 || runs.size() - ro > 255) throw Stop("AssertionError: section limits");
        r.npv = uint8_t(loc_pk.size());
        r.nsi = uint8_t(loc_sh.size());
        srecs.push_back(std::move(r));
    }
    // top tree over the sections
    std::vector<F3> scent(secs.size());
    for (size_t i = 0; i < secs.size(); i++)
        for (int c = 0; c < 3; c++) scent[i][c] = (sec_min[i][c] + sec_max[i][c]) * 0.5f;
    std::vector<size_t> sids(secs.size());
    std::iota(sids.begin(), sids.end(), 0);
    const auto troot = build_tree(sids, sec_min, sec_max, scent);
    std::map<size_t, size_t> leafnode;
    const Bytes top = emit_tree(*troot, dmin, dmax, 5, leafnode);
    uint64_t maxkey = 0;
    for (size_t si = 0; si < secs.size(); si++) maxkey = std::max<uint64_t>(maxkey, (uint64_t(si) << 8) | (uint64_t(secs[si].size() - 1) << 1));
    // serialize
    Bytes D(header.begin(), header.begin() + std::ptrdiff_t(std::min<size_t>(0x30, header.size())));
    D.resize(0xf0, 0);
    auto al = [&] { while (D.size() % 16) D.push_back(0); };
    auto arr = [&](size_t at, uint32_t n) {
        set64(D, at, 0);
        set32(D, at + 8, n);
        set32(D, at + 12, 0x80000000u | n);
    };
    al();
    loc.push_back({0x30, uint32_t(D.size())});
    arr(0x30, uint32_t(filterpal.size()));
    for (uint32_t f : filterpal) put32(D, f);
    al();
    loc.push_back({0x40, uint32_t(D.size())});
    arr(0x40, uint32_t(userpal.size()));
    for (uint32_t f : userpal) put32(D, f);
    al();
    loc.push_back({0x50, uint32_t(D.size())});
    arr(0x50, uint32_t(top.size() / 5));
    D.insert(D.end(), top.begin(), top.end());
    al();
    for (int c = 0; c < 3; c++) set32(D, 0x60 + 4 * c, fbits(dmin[c]));
    set32(D, 0x6c, fbits(1.0f));
    for (int c = 0; c < 3; c++) set32(D, 0x70 + 4 * c, fbits(dmax[c]));
    set32(D, 0x7c, fbits(1.0f));
    int bl = 0;
    for (uint64_t k = maxkey; k; k >>= 1) bl++;
    set32(D, 0x80, uint32_t(N));
    set32(D, 0x84, uint32_t(std::max(1, bl)));
    set32(D, 0x88, uint32_t(maxkey));
    const size_t secbase = D.size();
    loc.push_back({0x90, uint32_t(secbase)});
    arr(0x90, uint32_t(secs.size()));
    D.resize(D.size() + 0x60 * secs.size(), 0);
    for (size_t si = 0; si < srecs.size(); si++) {
        const SRec &s = srecs[si];
        const size_t o = secbase + 0x60 * si;
        const uint32_t nn = uint32_t(s.nodes.size() / 4);
        set64(D, o, 0);
        set32(D, o + 8, nn);
        set32(D, o + 12, 0x80000000u | nn);
        for (int c = 0; c < 3; c++) set32(D, o + 0x10 + 4 * c, fbits(s.smin[c]));
        set32(D, o + 0x1c, fbits(1.0f));
        for (int c = 0; c < 3; c++) set32(D, o + 0x20 + 4 * c, fbits(s.smax[c]));
        set32(D, o + 0x2c, fbits(1.0f));
        for (int c = 0; c < 3; c++) set32(D, o + 0x30 + 4 * c, fbits(s.off[c]));
        for (int c = 0; c < 3; c++) set32(D, o + 0x3c + 4 * c, fbits(s.scale[c]));
        set32(D, o + 0x48, s.fpv);
        set32(D, o + 0x4c, s.sh);
        set32(D, o + 0x50, s.pr);
        set32(D, o + 0x54, s.dr);
        D[o + 0x58] = s.npv;
        D[o + 0x59] = s.nsi;
        const size_t lnode = leafnode.at(si);
        if (lnode > 0xffff) throw Stop("struct.error: leaf node index");
        D[o + 0x5a] = uint8_t(lnode);
        D[o + 0x5b] = uint8_t(lnode >> 8);
        D[o + 0x5c] = D[o + 0x5d] = D[o + 0x5e] = D[o + 0x5f] = 0;
    }
    for (size_t si = 0; si < srecs.size(); si++) {
        al();
        loc.push_back({uint32_t(secbase + 0x60 * si), uint32_t(D.size())});
        D.insert(D.end(), srecs[si].nodes.begin(), srecs[si].nodes.end());
    }
    Bytes pr, sv, pv, shv, rn;
    for (const auto &x : prims) pr.insert(pr.end(), x.begin(), x.end());
    for (uint16_t x : svi) put16(sv, x);
    for (uint32_t x : packedV) put32(pv, x);
    for (uint64_t x : sharedV) put64(shv, x);
    for (const Run &x : runs) {
        put32(rn, x.v);
        rn.push_back(x.i);
        rn.push_back(x.c);
        put16(rn, 0);
    }
    const std::pair<size_t, std::pair<const Bytes *, size_t>> tabs[] = {{0xa0, {&pr, prims.size()}}, {0xb0, {&sv, svi.size()}}, {0xc0, {&pv, packedV.size()}},
                                                                     {0xd0, {&shv, sharedV.size()}}, {0xe0, {&rn, runs.size()}}};
    for (const auto &[at, dn] : tabs) {
        al();
        arr(at, uint32_t(dn.second));
        if (dn.second) {
            loc.push_back({uint32_t(at), uint32_t(D.size())});
            D.insert(D.end(), dn.first->begin(), dn.first->end());
        }
    }
    al();
    return D;
}

const uint8_t CLASSNAMES[] = {
    0xf6, 0x5e, 0x58, 0x75, 0x09, 'h', 'k', 'C', 'l', 'a', 's', 's', 0,
    0xc2, 0xa4, 0x7e, 0x5c, 0x09, 'h', 'k', 'C', 'l', 'a', 's', 's', 'M', 'e', 'm', 'b', 'e', 'r', 0,
    0xcf, 0x09, 0x36, 0x8a, 0x09, 'h', 'k', 'C', 'l', 'a', 's', 's', 'E', 'n', 'u', 'm', 0,
    0x6c, 0x8a, 0x6f, 0xce, 0x09, 'h', 'k', 'C', 'l', 'a', 's', 's', 'E', 'n', 'u', 'm', 'I', 't', 'e', 'm', 0,
    0x27, 0x3f, 0x3b, 0xb6, 0x09, 'h', 'k', 'p', 'B', 'v', 'C', 'o', 'm', 'p', 'r', 'e', 's', 's', 'e', 'd', 'M', 'e', 's', 'h', 'S', 'h', 'a', 'p', 'e', 0,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
// the retail shape header (vtable / refcount / hkpShape / hkpBvTreeShape / flags), from ldn-train-stage
const uint8_t SHAPE_HDR[0x30] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0};

Bytes hexb(const char *h) {
    Bytes o;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) o.push_back(uint8_t(std::stoi(std::string(h + i, 2), nullptr, 16)));
    return o;
}

// a __data__ section in a Havok 2011.3 64-bit little-endian packfile (the retail layout)
Bytes packfile(const Bytes &data, const std::vector<std::pair<uint32_t, uint32_t>> &loc) {
    Bytes out = hexb("57e0e05710c0c010000000000900000008010101030000000200000000000000000000004b000000");
    const char tag[] = "hk_2011.3.0-r1";
    out.insert(out.end(), tag, tag + 14);
    out.push_back(0);
    out.push_back(0xff);
    const Bytes t2 = hexb("00000000ffffffff");
    out.insert(out.end(), t2.begin(), t2.end());
    Bytes lf;
    for (const auto &[s, d] : loc) {
        put32(lf, s);
        put32(lf, d);
    }
    while (lf.size() % 16) lf.push_back(0xff);
    Bytes vf;
    put32(vf, 0);
    put32(vf, 0);
    put32(vf, 0x4b);
    for (int k = 0; k < 4; k++) vf.push_back(0xff);
    const uint32_t start_cn = 0xd0, ncn = sizeof(CLASSNAMES), start_data = start_cn + ncn, dsz = uint32_t(data.size());
    const uint32_t L = uint32_t(lf.size()), V = uint32_t(vf.size());
    const std::pair<const char *, std::pair<uint32_t, std::array<uint32_t, 6>>> secs[] = {
        {"__classnames__", {start_cn, {ncn, ncn, ncn, ncn, ncn, ncn}}},
        {"__types__", {start_data, {0, 0, 0, 0, 0, 0}}},
        {"__data__", {start_data, {dsz, dsz + L, dsz + L, dsz + L + V, dsz + L + V, dsz + L + V}}}};
    for (const auto &[tg, so] : secs) {
        Bytes t(19, 0);
        std::memcpy(t.data(), tg, std::strlen(tg));
        out.insert(out.end(), t.begin(), t.end());
        out.push_back(0xff);
        put32(out, so.first);
        for (uint32_t x : so.second) put32(out, x);
    }
    if (out.size() != start_cn) throw Stop("AssertionError: packfile header");
    out.insert(out.end(), CLASSNAMES, CLASSNAMES + ncn);
    out.insert(out.end(), data.begin(), data.end());
    out.insert(out.end(), lf.begin(), lf.end());
    out.insert(out.end(), vf.begin(), vf.end());
    return out;
}

// the retail per-primitive collision filter info from the ND material flags: layer 2 for water (0x4000), 13 for
// 0x400, else 1; 0x100 -> 0x4000000, 0x200 -> 0x8000000, 0x2000 -> 0x20000000, 0x10000 -> 0x40000000
uint32_t collision_filter(uint32_t flags) {
    uint32_t f = (flags & 0x4000) ? 2 : (flags & 0x400) ? 13 : 1;
    if (flags & 0x100) f |= 0x4000000;
    if (flags & 0x200) f |= 0x8000000;
    if (flags & 0x2000) f |= 0x20000000;
    if (flags & 0x10000) f |= 0x40000000;
    return f;
}

std::shared_ptr<Blob> havok_bg_blob(const Pak &p, size_t index) {
    const std::optional<Ps3Coll> c = ps3_collision(p, index);
    if (!c || !c->has_mesh || c->tris.empty()) return nullptr;
    const auto &mats = c->mats;
    std::vector<uint32_t> flags_used, uidx, filt, fpal, fidx;
    for (uint32_t m : c->mat) {
        const uint32_t f = mats.at(m).second;
        if (std::find(flags_used.begin(), flags_used.end(), f) == flags_used.end()) flags_used.push_back(f);
    }
    for (uint32_t m : c->mat) uidx.push_back(uint32_t(std::find(flags_used.begin(), flags_used.end(), mats.at(m).second) - flags_used.begin()));
    for (uint32_t m : c->mat) filt.push_back(collision_filter(mats.at(m).second));
    for (uint32_t f : filt)
        if (std::find(fpal.begin(), fpal.end(), f) == fpal.end()) fpal.push_back(f);
    std::stable_sort(fpal.begin(), fpal.end(), [](uint32_t a, uint32_t b) {   // the plain layer-1 entry first
        return std::make_pair((a & 0x1f) != 1, a) < std::make_pair((b & 0x1f) != 1, b);
    });
    for (uint32_t f : filt) fidx.push_back(uint32_t(std::find(fpal.begin(), fpal.end(), f) - fpal.begin()));
    std::vector<std::pair<uint32_t, uint32_t>> loc;
    const Bytes data = build_bvcm(c->tris, c->weld, uidx, flags_used, fpal, Bytes(SHAPE_HDR, SHAPE_HDR + 0x30), fidx, loc);
    const Bytes pf = packfile(data, loc);
    auto B = std::make_shared<Blob>("havokbg");
    B->u32(c->version).u32(0xaaaaaaaa).u64(0).u64(0);   // +08/+10: PS3 +04/+08 (zero)
    B->u32(c->count);                                   // +18
    B->raw(Bytes(0x34 - B->size(), 0));                 // PS3 +10..+20 are zero in every sample
    Bytes misc4(4, 0);
    for (size_t k = 0; k < 4 && 0x10 + k < c->misc.size(); k++) misc4[k] = c->misc[0x10 + k];
    if (c->misc.size() < 0x14) misc4.resize(c->misc.size() > 0x10 ? c->misc.size() - 0x10 : 0);
    B->raw(misc4);                                      // +34..+38 = PS3 +20..+24
    B->u64(0).u64(0);                                   // +38/+40 runtime
    B->u64(0);                                          // +48 sid list (retail: null)
    const size_t arr_pos = B->size();
    B->u64(0);                                          // +50 -> u32[count]
    B->u64(0).u64(0).u64(0);                            // +58/+60 runtime, +68 null
    const size_t tr_pos = B->size();
    B->u64(0);                                          // +70 -> trailer
    B->u32(0xaaaaaaaa).u32(0xaaaaaaaa);
    if (B->size() != 0x80) throw Stop("AssertionError: collision header");
    const size_t a_off = B->size();
    for (uint32_t x : c->arr) B->u32(x);
    B->raw(Bytes((16 - B->size() % 16) % 16, 0));
    if (c->arr.empty()) B->raw(Bytes(0x100, 0));
    while (B->size() < 0x180) B->raw(Bytes(16, 0));
    B->pad(16, 0xaa);
    const size_t pf_off = B->size();
    B->raw(pf);
    B->pad(16, 0xaa);
    const size_t m_off = B->size();
    for (const auto &[fr, fl] : mats) B->u32(1).u32(fr != 0 ? qbits(fr) : fbits(1.0f)).u32(fl);
    B->pad(8, 0xaa);
    const size_t t_off = B->size();
    B->u64(0).u64(0).u32(pf.size()).u32(mats.size());
    B->relocs.push_back({arr_pos, B, int64_t(a_off)});
    B->relocs.push_back({tr_pos, B, int64_t(t_off)});
    B->relocs.push_back({t_off, B, int64_t(m_off)});
    B->relocs.push_back({t_off + 8, B, int64_t(pf_off)});
    return B;
}

}  // namespace

std::vector<std::shared_ptr<Blob>> havok_bg_blobs(const Pak &p) {
    std::vector<std::shared_ptr<Blob>> out;
    const size_t n = p.items_of("COLLISION_DATA_HAVOK_BG").size();
    for (size_t k = 0; k < n; k++)
        if (auto b = havok_bg_blob(p, k)) out.push_back(b);
    return out;
}

}  // namespace u3
