// dctools/ps3bg.cpp: see ps3bg.hpp.
#include "ps3bg.hpp"

#include "u3fix.hpp"

#include <cmath>
#include <cstring>
#include <functional>
#include <set>

#if defined(__clang__)
#pragma clang fp contract(off)   // float64 results must match numpy's
#endif

namespace u3::bg {

ORef res(const Pak &p, uint32_t pg, uint64_t off) {
    if (off > 0xffffffffull) return std::nullopt;
    return p.resolve(pg, uint32_t(off));
}
Ref need(const ORef &r) {
    if (!r) throw Stop("TypeError: cannot unpack non-iterable NoneType object");
    return *r;
}
Ref need(const Pak &p, uint32_t pg, uint64_t off) { return need(res(p, pg, off)); }

uint8_t u8(const Pak &p, uint64_t o) {
    if (o >= p.d.size()) throw Stop("IndexError: index out of range");
    return p.d[o];
}
uint16_t be16(const Pak &p, uint64_t o) {
    if (o + 2 > p.d.size()) throw Stop("struct.error: unpack_from requires a buffer of more bytes");
    return uint16_t(p.d[o] << 8 | p.d[o + 1]);
}
float bef32(const Pak &p, uint64_t o) {
    const uint32_t v = be32(p.d, o);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

std::string utf8_of(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x80) o += char(c);
        else { o += char(0xc0 | c >> 6); o += char(0x80 | (c & 0x3f)); }
    }
    return o;
}

bool py_printable(const std::string &s) {
    for (unsigned char c : s)
        if (c < 0x20 || (c >= 0x7f && c <= 0xa0) || c == 0xad) return false;   // Cc, NBSP (Zs), soft hyphen (Cf)
    return true;
}

std::vector<Attr> read_format(const Pak &p, uint32_t pg, uint64_t fmt) {
    const uint64_t b = p.pbase(pg);
    std::vector<Attr> attrs;
    uint64_t o = fmt + 8;
    std::optional<uint64_t> first;
    while (!first || o < fmt + *first) {
        Attr a;
        a.id = u8(p, b + o);
        a.fmt = u8(p, b + o + 1);
        a.off = be16(p, b + o + 2);
        if (b + o + 8 > p.d.size()) throw Stop("IndexError: vertex format past the end");
        for (int k = 0; k < 4; k++) a.w[k] = p.d[b + o + 4 + k];
        attrs.push_back(a);
        if ((a.fmt & 0xf0) == 0x40 || (a.fmt & 0xf0) == 0x60) first = first ? std::min<uint64_t>(*first, a.off) : a.off;
        o += 8;
        if (o >= fmt + 8 + 8 * 16) break;
    }
    return attrs;
}

namespace {

// ps3geom._bits_be: count records of big-endian bit fields (MSB first)
std::vector<uint64_t> bits_be(const uint8_t *buf, size_t len, const std::vector<int> &widths, uint32_t count) {
    std::vector<uint64_t> out(size_t(count) * widths.size(), 0);
    if (count == 0 || widths.empty()) return out;
    uint64_t W = 0;
    for (int w : widths) W += uint64_t(w);
    if (uint64_t(count) * W > uint64_t(len) * 8) throw Stop("ValueError: negative shift count");
    uint64_t pos = 0;
    for (uint32_t i = 0; i < count; i++)
        for (size_t c = 0; c < widths.size(); c++) {
            const int w = widths[c];
            if (w > 63) throw Stop("OverflowError: vertex field wider than 63 bits");
            uint64_t v = 0;
            for (int k = 0; k < w; k++, pos++) v = v << 1 | ((buf[pos >> 3] >> (7 - (pos & 7))) & 1);
            out[size_t(i) * widths.size() + c] = v;
        }
    return out;
}

}  // namespace

Mat decode_attr(const Pak &p, uint32_t pg, uint64_t fmt, const Attr &a, uint32_t data_pg, uint64_t data_off, uint32_t nv, int prec) {
    const uint64_t b = p.pbase(pg);
    const int n = a.n();
    if (n > 4) throw Stop("IndexError: vertex attribute with more than 4 components");
    std::vector<int> w(a.w, a.w + n);
    int sw = 0;
    for (int x : w) sw += x;
    Mat V(nv, size_t(n));
    if (sw) {
        const uint64_t db = p.pbase(data_pg);
        const uint64_t s = std::min<uint64_t>(db + data_off, p.d.size());
        const uint64_t e = std::min<uint64_t>(std::max(s, db + data_off + (uint64_t(nv) * sw + 7) / 8), p.d.size());
        const std::vector<uint64_t> bits = bits_be(p.d.data() + s, e - s, w, nv);
        for (size_t i = 0; i < bits.size(); i++) V.v[i] = double(int64_t(bits[i]));
    }
    Mat out(nv, size_t(n));
    const int kind = a.fmt & 0xf0;
    if (kind == 0x40 || kind == 0x60) {
        double A[4], B[4], I[4] = {};
        for (int c = 0; c < n; c++) {
            A[c] = double(bef32(p, b + fmt + a.off + 4 * c));
            B[c] = double(bef32(p, b + fmt + a.off + 4 * (n + c)));
            if (kind == 0x60) I[c] = double(be32(p.d, b + fmt + a.off + 8 * n + 4 * c));
        }
        for (uint32_t i = 0; i < nv; i++)
            for (int c = 0; c < n; c++) {
                const double v = V.at(i, c);
                out.at(i, c) = kind == 0x40 ? A[c] * (1.0 + v * 0x1p-21) + B[c] : A[c] * (1.0 + (4.0 * v + I[c]) * 0x1p-23) + B[c];
            }
        return out;
    }
    if (kind == 0x00) {
        for (int c = 0; c < n; c++) {
            const bool sign = prec >= 0 ? (w[c] == 1 && c == 3) : w[c] == 1;
            const double div = prec >= 0 ? double((1ll << prec) - 1) : (w[c] < 8 ? 255.0 : double((1ull << w[c]) - 1));
            for (uint32_t i = 0; i < nv; i++) out.at(i, c) = sign ? V.at(i, c) * 2.0 - 1.0 : V.at(i, c) * 2.0 / div - 1.0;
        }
        return out;
    }
    throw Stop(u3::fmt("ValueError: attr fmt %#x", a.fmt));
}

std::map<int, int> block_prec(const std::vector<Attr> &attrs) {
    static const std::map<int, int> TYPE_BITS = {{2, 10}, {3, 10}, {0x15, 8}};
    int P = 0;
    for (const Attr &a : attrs) {
        if ((a.fmt & 0xf0) != 0 || a.id == 0x15 || a.id == 0x4c || a.id == 0x4e) continue;
        int m = 0;
        for (int k = 0; k < std::min(a.n(), 4); k++)
            if (a.w[k] > 1) m = std::max<int>(m, a.w[k]);
        P = std::max(P, m);
    }
    if (!P) P = 8;   // colour-only block: bytes
    std::map<int, int> out;
    for (const Attr &a : attrs)
        if ((a.fmt & 0xf0) == 0) {
            auto t = TYPE_BITS.find(a.id);
            out[a.id] = std::max(1, std::min(t == TYPE_BITS.end() ? 8 : t->second, P));
        }
    for (const Attr &a : attrs)   // instance constant (iconst, 0x4e): its own widest field
        if (a.id == 0x4e && (a.fmt & 0xf0) == 0) {
            int m = 0;
            bool any = false;
            for (int k = 0; k < std::min(a.n(), 4); k++) { m = any ? std::max<int>(m, a.w[k]) : a.w[k]; any = true; }
            out[0x4e] = std::max(1, any ? m : 1);
        }
    return out;
}

std::vector<std::array<uint32_t, 3>> decode_indices(const uint8_t *raw, size_t len, uint32_t ntris) {
    if (len < 8) throw Stop("struct.error: index header");
    const uint32_t bits = uint32_t(raw[0] << 8 | raw[1]), nexp = uint32_t(raw[2] << 8 | raw[3]), nslots = uint32_t(raw[4] << 8 | raw[5]),
                   start = uint32_t(raw[6] << 8 | raw[7]);
    const uint64_t nb = (len - 8) * 8;   // the bit string after the header
    auto bit = [&](uint64_t i) { return (raw[8 + (i >> 3)] >> (7 - (i & 7))) & 1u; };
    // int(bs[a:b], 2): the bits that exist (a slice past the end is shorter; an empty one is an error)
    auto field = [&](uint64_t a, uint64_t n) -> uint64_t {
        const uint64_t e = std::min(a + n, nb);
        if (a >= e) throw Stop("ValueError: invalid literal for int() with base 2: ''");
        uint64_t v = 0;
        for (uint64_t i = a; i < e; i++) v = v << 1 | bit(i);
        return v;
    };
    auto align16 = [](uint64_t n) { return (n + 15) / 16 * 16; };
    std::vector<uint32_t> vals(nexp);
    for (uint32_t i = 0; i < nexp; i++) vals[i] = uint32_t(field(uint64_t(i) * bits, bits));
    uint64_t p = align16(uint64_t(nexp) * bits);
    std::vector<uint32_t> cfg(ntris);
    for (uint32_t i = 0; i < ntris; i++) cfg[i] = uint32_t(field(p + 2ull * i, 2));
    p += align16(uint64_t(ntris) * 2);
    const uint64_t nflags = p < nb ? std::min<uint64_t>(nslots, nb - p) : 0;
    uint32_t vi = 0, si = 0, counter = start;
    auto take = [&]() -> uint32_t {
        if (si >= nflags) throw Stop("IndexError: string index out of range");
        const unsigned f = bit(p + si);
        si++;
        if (f) {
            if (vi >= vals.size()) throw Stop("IndexError: list index out of range");
            return vals[vi++];
        }
        return counter++;
    };
    std::vector<std::array<uint32_t, 3>> tris;
    tris.reserve(ntris);
    for (uint32_t i = 0; i < ntris; i++) {
        const uint32_t c = cfg[i];
        if (i == 0 || c == 3) {
            const uint32_t a = take(), b = take(), d = take();
            tris.push_back({a, b, d});
        } else {
            const auto pv = tris.back();
            if (c == 0) tris.push_back({pv[0], pv[2], take()});
            else if (c == 1) tris.push_back({pv[2], pv[1], take()});
            else throw Stop(fmt("ValueError: index config 2 not seen yet (tri %u)", i));
        }
    }
    if (vi != nexp || si != nslots) throw Stop("AssertionError: index stream counts");
    return tris;
}

std::vector<Influence> decode_skin(const Pak &p, uint32_t page, uint64_t info, uint32_t nverts) {
    const uint64_t b = p.pbase(page);
    uint32_t sz[6];
    for (int k = 0; k < 6; k++) sz[k] = be16(p, b + info + 2 * k);
    std::optional<uint64_t> ptrs[4];
    for (int k = 0; k < 4; k++) {
        const ORef r = res(p, page, info + 0xc + 4 * k);
        if (r) ptrs[k] = r->second;
    }
    const uint64_t rng = need(p, page, info + 0x20).second;
    std::vector<uint32_t> pal;
    for (uint32_t k = 0; k < sz[4]; k++) {
        const uint32_t j0 = be16(p, b + rng + 4 * k), n = be16(p, b + rng + 4 * k + 2);
        for (uint32_t j = j0; j < j0 + n; j++) pal.push_back(j);
    }
    auto bytes_of = [&](int k) {   // p.d[b + ptr : b + ptr + size] (a short slice when the file ends)
        if (!sz[k] || !ptrs[k]) return Bytes();
        const uint64_t s = std::min<uint64_t>(b + *ptrs[k], p.d.size()), e = std::min<uint64_t>(b + *ptrs[k] + sz[k], p.d.size());
        return Bytes(p.d.begin() + s, p.d.begin() + std::max(s, e));
    };
    auto shorts_of = [&](int k) {
        std::vector<uint32_t> v;
        if (!sz[k] || !ptrs[k]) return v;
        for (uint32_t i = 0; i < sz[k] / 2; i++) v.push_back(be16(p, b + *ptrs[k] + 2 * i));
        return v;
    };
    const Bytes S1 = bytes_of(0), S4 = bytes_of(3);
    const std::vector<uint32_t> S2 = shorts_of(1), S3 = shorts_of(2);
    const uint32_t ngroups = (nverts + 3) / 4;
    size_t p2 = 0, p3 = 0, wp = 0;
    std::vector<Influence> out;
    std::optional<std::array<std::vector<uint32_t>, 4>> cols;
    std::pair<size_t, std::vector<Bytes>> prev{0, {}};
    auto idx = [](const auto &v, size_t i) {
        if (i >= v.size()) throw Stop("IndexError: index out of range");
        return v[i];
    };
    for (uint32_t gi = 0; gi < ngroups; gi++) {
        const uint32_t f = idx(S1, gi);
        if (f != 0 && f != 2 && f != 4 && f != 6 && f != 8 && f != 10) throw Stop(fmt("ValueError: skin group flags %u", f));
        const bool hasw = f == 6 || f == 8 || f == 10;
        if (f == 2 || f == 10) {   // per-lane joint rows from s3
            std::vector<std::array<uint32_t, 4>> Jr;
            while (true) {
                std::array<uint32_t, 4> row;
                for (int k = 0; k < 4; k++) row[k] = idx(S3, p3 + k);   // a short row: IndexError on row[1] / row[3]
                p3 += 4;
                Jr.push_back({row[1] / 48, row[0] / 48, row[3] / 48, row[2] / 48});
                if (row[0] % 48 == 0 && row[1] % 48 == 0 && row[2] % 48 == 0 && row[3] % 48 == 0) break;
            }
            std::array<std::vector<uint32_t>, 4> c;
            for (int v = 0; v < 4; v++)
                for (const auto &r : Jr) c[v].push_back(r[v]);
            cols = c;
        } else if (f == 4 || f == 6) {   // shared joint list from s2
            std::vector<uint32_t> L;
            while (true) {
                const uint32_t x = idx(S2, p2++);
                L.push_back(x / 48);
                if (x % 48 == 0) break;
            }
            cols = std::array<std::vector<uint32_t>, 4>{L, L, L, L};
        }
        size_t r;
        std::vector<Bytes> Wr;
        if (f == 0) {   // repeat the previous group (joints and weights)
            r = prev.first;
            Wr = prev.second;
        } else {
            if (hasw && !cols) throw Stop("TypeError: 'NoneType' object is not subscriptable");
            r = hasw ? (*cols)[0].size() : 0;
            for (size_t k = 0; k < r; k++) {
                const size_t s = std::min(wp + k * 4, S4.size()), e = std::min(wp + k * 4 + 4, S4.size());
                Wr.push_back(Bytes(S4.begin() + s, S4.begin() + std::max(s, e)));
            }
            wp += 4 * r;
        }
        prev = {r, Wr};
        for (uint32_t v = 0; v < 4; v++) {
            if (gi * 4 + v >= nverts) break;
            if (!cols) throw Stop("TypeError: 'NoneType' object is not subscriptable");
            if (r == 0) {
                out.push_back({{idx(pal, idx((*cols)[v], 0)), 255}});
            } else {
                std::vector<std::pair<uint32_t, uint32_t>> d;   // dict in insertion order
                for (size_t k = 0; k < r; k++) {
                    const uint32_t w = idx(Wr[k], v);
                    if (!w) continue;
                    const uint32_t j = idx(pal, idx((*cols)[v], k));
                    auto it = std::find_if(d.begin(), d.end(), [&](const auto &e) { return e.first == j; });
                    if (it == d.end()) d.push_back({j, w});
                    else it->second += w;
                }
                std::stable_sort(d.begin(), d.end(), [](const auto &a, const auto &c) { return a.second < c.second; });
                out.push_back(d);
            }
        }
    }
    if (p2 * 2 != sz[1] || p3 * 2 != sz[2] || wp != sz[3]) throw Stop("AssertionError: skin stream sizes");
    return out;
}

namespace {

std::vector<std::optional<std::string>> block_materials(const Pak &p, const Ref &proto) {
    const auto [pg, po] = proto;
    const uint32_t nmat = p.u16(p.pbase(pg) + po + 4), nblk = p.u16(p.pbase(pg) + po + 6);
    const Ref t = need(p, pg, po + 0x10);
    std::vector<std::optional<std::string>> out(nblk);
    for (uint32_t s = 0; s < nmat; s++) {
        const uint64_t e = t.second + 0x10 * s;
        const Ref sd = need(p, t.first, e);
        const uint32_t first = p.u16(p.pbase(t.first) + e + 4), cnt = p.u16(p.pbase(t.first) + e + 6);
        const Ref m = need(p, sd.first, sd.second);
        const Ref n = need(p, m.first, m.second);
        const std::string nm = p.pstr(n.first, n.second);
        for (uint32_t k = first; k < first + cnt; k++) {
            if (k >= out.size()) throw Stop("IndexError: list assignment index out of range");
            out[k] = nm;
        }
    }
    return out;
}

std::vector<Group> material_groups(const Pak &p, const Ref &proto) {
    const auto [pg, po] = proto;
    const uint32_t nmat = p.u16(p.pbase(pg) + po + 4);
    const Ref t = need(p, pg, po + 0x10);
    std::vector<Group> out;
    for (uint32_t s = 0; s < nmat; s++) {
        const uint64_t e = t.second + 0x10 * s;
        const Ref sd = need(p, t.first, e);
        const uint32_t first = p.u16(p.pbase(t.first) + e + 4), cnt = p.u16(p.pbase(t.first) + e + 6);
        const Ref m = need(p, sd.first, sd.second);
        const Ref n = need(p, m.first, m.second);
        out.push_back({p.pstr(n.first, n.second), first, cnt});
    }
    return out;
}

}  // namespace

std::vector<Proto> collection(const Pak &p) {
    const auto pms = p.items_of("PM_COLLECTION_2");
    if (pms.empty()) return {};   // water-only / empty background level
    const PakItem &pm = pms[0];
    const uint32_t n = p.pu32(pm.page, pm.data_off());
    std::vector<Proto> out;
    for (uint32_t k = 0; k < n; k++) {
        Proto pr;
        pr.entry = need(p, pm.page, pm.data_off() + 4 + 4 * k);
        pr.hdr = need(p, pr.entry.first, pr.entry.second);
        const Ref il = need(p, pr.entry.first, pr.entry.second + 4);
        const uint32_t ni = p.pu32(pr.entry.first, pr.entry.second + 8);
        for (uint32_t i = 0; i < ni; i++) pr.insts.push_back(res(p, il.first, il.second + 4 * i));
        const uint32_t nblk = p.u16(p.pbase(pr.hdr.first) + pr.hdr.second + 6);
        const Ref tb = need(p, pr.hdr.first, pr.hdr.second + 0x14);
        for (uint32_t i = 0; i < nblk; i++) {
            const ORef r = res(p, tb.first, tb.second + 4 * i);
            pr.blocks.push_back(need(r));   // a missing block stops at its first use in Python; earlier here
        }
        pr.mats = block_materials(p, pr.hdr);
        pr.groups = material_groups(p, pr.hdr);
        out.push_back(std::move(pr));
    }
    return out;
}

Instance instance(const Pak &p, const Ref &ia) {
    const auto [pg, o] = ia;
    const uint64_t b = p.pbase(pg);
    Instance r;
    r.addr = ia;
    r.x8 = p.pu32(pg, o + 8);
    for (int i = 0; i < 16; i++) r.mat[i] = double(bef32(p, b + o + 0x40 + 4 * i));
    r.blockdata = res(p, pg, o + 0x18);
    return r;
}

Mat gen_tangents(const Mat &pos, const Mat &nrm, const Mat *uv, const std::vector<std::array<uint32_t, 3>> &tris) {
    const size_t nv = pos.rows;
    Mat t(nv, 3), bt(nv, 3);
    if (uv && !tris.empty()) {
        const size_t nt = tris.size();
        std::vector<double> sd(nt * 3), td(nt * 3);
        for (size_t i = 0; i < nt; i++) {
            const auto &T = tris[i];
            for (int k = 0; k < 3; k++)
                if (T[k] >= nv || T[k] >= uv->rows) throw Stop("IndexError: triangle index out of range");
            double e1[3], e2[3], d1[2], d2[2];
            for (int c = 0; c < 3; c++) {
                e1[c] = pos.at(T[1], c) - pos.at(T[0], c);
                e2[c] = pos.at(T[2], c) - pos.at(T[0], c);
            }
            for (int c = 0; c < 2; c++) {
                d1[c] = uv->at(T[1], c) - uv->at(T[0], c);
                d2[c] = uv->at(T[2], c) - uv->at(T[0], c);
            }
            double r = d1[0] * d2[1] - d2[0] * d1[1];
            r = std::fabs(r) < 1e-12 ? 0.0 : 1.0 / r;
            for (int c = 0; c < 3; c++) {
                sd[i * 3 + c] = (e1[c] * d2[1] - e2[c] * d1[1]) * r;
                td[i * 3 + c] = (e2[c] * d1[0] - e1[c] * d2[0]) * r;
            }
        }
        for (int k = 0; k < 3; k++)   // np.add.at, corner by corner, in triangle order
            for (size_t i = 0; i < nt; i++)
                for (int c = 0; c < 3; c++) {
                    t.at(tris[i][k], c) += sd[i * 3 + c];
                    bt.at(tris[i][k], c) += td[i * 3 + c];
                }
    }
    Mat out(nv, 4);
    for (size_t i = 0; i < nv; i++) {
        double n[3], tv[3];
        const double nl = std::sqrt(nrm.at(i, 0) * nrm.at(i, 0) + nrm.at(i, 1) * nrm.at(i, 1) + nrm.at(i, 2) * nrm.at(i, 2));
        const double nd = std::max(nl, 1e-9);
        for (int c = 0; c < 3; c++) n[c] = nrm.at(i, c) / nd;
        const double dot = n[0] * t.at(i, 0) + n[1] * t.at(i, 1) + n[2] * t.at(i, 2);
        for (int c = 0; c < 3; c++) tv[c] = t.at(i, c) - n[c] * dot;
        double ln = std::sqrt(tv[0] * tv[0] + tv[1] * tv[1] + tv[2] * tv[2]);
        if (ln < 1e-9) {   // any vector perpendicular to the normal
            const double a[3] = {std::fabs(n[0]) < 0.9 ? 1.0 : 0.0, std::fabs(n[0]) < 0.9 ? 0.0 : 1.0, 0.0};
            double alt[3] = {n[1] * a[2] - n[2] * a[1], n[2] * a[0] - n[0] * a[2], n[0] * a[1] - n[1] * a[0]};
            const double al = std::sqrt(alt[0] * alt[0] + alt[1] * alt[1] + alt[2] * alt[2]);
            for (int c = 0; c < 3; c++) tv[c] = alt[c] / al;
            ln = 1.0;
        }
        for (int c = 0; c < 3; c++) tv[c] = tv[c] / ln;
        const double cr[3] = {n[1] * tv[2] - n[2] * tv[1], n[2] * tv[0] - n[0] * tv[2], n[0] * tv[1] - n[1] * tv[0]};
        const double s = cr[0] * bt.at(i, 0) + cr[1] * bt.at(i, 1) + cr[2] * bt.at(i, 2);
        for (int c = 0; c < 3; c++) out.at(i, c) = tv[c];
        out.at(i, 3) = s < 0 ? -1.0 : 1.0;
    }
    return out;
}

void read_materials(const Pak &p, std::vector<Material3> &out, std::vector<Texture3> &texs) {
    out.clear();
    texs.clear();
    const auto mts = p.items_of("MATERIAL_TABLE_1");
    if (mts.empty()) return;
    const uint32_t pg = mts[0].page;
    const uint64_t b = mts[0].data_off();
    const uint32_t ntex = p.pu32(pg, b + 4), nmat = p.pu32(pg, b + 0xc);
    const ORef th = res(p, pg, b + 0x18), nm = res(p, pg, b + 0x30), ma = res(p, pg, b + 0x20);
    for (uint32_t i = 0; i < ntex; i++) {
        const ORef r = res(p, need(nm).first, need(nm).second + 4 * i);
        Texture3 t;
        t.name = r ? p.pstr(r->first, r->second) : "";
        t.hash = p.pu32(need(th).first, need(th).second + 4 * i);
        texs.push_back(t);
    }
    // a texture-table entry: the texture index, or none
    auto tex_index = [&](const ORef &rr) -> std::optional<uint32_t> {
        if (!rr) return std::nullopt;
        const Ref t = need(th);   // th None: TypeError
        if (rr->first == t.first && t.second <= rr->second && rr->second < t.second + 4ull * ntex) return uint32_t((rr->second - t.second) / 4);
        return std::nullopt;
    };
    for (uint32_t i = 0; i < nmat; i++) {
        const Ref m = need(p, need(ma).first, need(ma).second + 4 * i);
        const Ref mn = need(p, m.first, m.second);
        Material3 mat;
        mat.name = p.pstr(mn.first, mn.second);
        std::vector<uint32_t> idx;
        try {
            const ORef r = res(p, m.first, m.second + 0x10);
            const ORef node = r ? res(p, r->first, r->second) : std::nullopt;
            const ORef bb = node ? res(p, node->first, node->second + 0x10) : std::nullopt;
            if (bb) {
                p.pu32(bb->first, bb->second);
                const uint32_t n = p.pu32(bb->first, bb->second + 4);
                for (uint32_t k = 0; k < std::min<uint32_t>(n, 32); k++)
                    if (auto j = tex_index(res(p, bb->first, bb->second + 0x50 + 0x10 * k))) idx.push_back(*j);
            }
        } catch (const std::exception &) {
        }
        // the pass/sampler tree under +0 also references textures the first node misses: bounded walk, in walk order
        std::set<Ref> seen;
        std::vector<uint32_t> extra;
        std::function<void(const Ref &, int)> walk = [&](const Ref &a, int depth) {
            if (depth > 4 || seen.count(a)) return;
            seen.insert(a);
            for (uint64_t k = 0; k < 0x80; k += 4) {
                ORef r;
                try {
                    r = res(p, a.first, a.second + k);
                } catch (const std::exception &) {
                    return;
                }
                if (!r) continue;
                const Ref t = need(th);
                if (r->first == t.first && t.second <= r->second && r->second < t.second + 4ull * ntex) {
                    const uint32_t j = uint32_t((r->second - t.second) / 4);
                    if (std::find(idx.begin(), idx.end(), j) == idx.end() && std::find(extra.begin(), extra.end(), j) == extra.end()) extra.push_back(j);
                } else
                    walk(*r, depth + 1);
            }
        };
        try {
            const ORef pl = res(p, m.first, m.second + 0x10);
            if (pl) {
                seen.insert(m);
                for (uint32_t k = 0; k < 3; k++)
                    if (const ORef r = res(p, pl->first, pl->second + 4 * k)) walk(*r, 2);
            }
        } catch (const std::exception &) {
        }
        // the exact set: the binding blocks of the material's 8 technique passes (textures at +0x50, stride 0x10)
        std::vector<uint32_t> exact;
        try {
            const ORef pl = res(p, m.first, m.second + 0x10);
            for (uint32_t k = 0; k < (pl ? 8u : 0u); k++) {
                const ORef r = res(p, pl->first, pl->second + 4 * k);
                const ORef bb = r ? res(p, r->first, r->second + 0x10) : std::nullopt;
                if (!bb) continue;
                const uint32_t n = std::min<uint32_t>(p.pu32(bb->first, bb->second + 4), 32);
                for (uint32_t kk = 0; kk < n; kk++)
                    if (auto j = tex_index(res(p, bb->first, bb->second + 0x50 + 0x10 * kk)))
                        if (std::find(exact.begin(), exact.end(), *j) == exact.end()) exact.push_back(*j);
            }
        } catch (const std::exception &) {
            exact.clear();
        }
        if (!exact.empty()) mat.textures = exact;
        else {
            mat.textures = idx;
            mat.textures.insert(mat.textures.end(), extra.begin(), extra.end());
        }
        out.push_back(std::move(mat));
    }
}

// ------------------------------------------------------------------------------------------------ ps3params
namespace {

bool is_param_table(const Pak &p, const Ref &a) {
    try {
        for (uint32_t k = 0; k < 2; k++) {
            const ORef r = res(p, a.first, a.second + 0x10 * k + 0xc);
            if (!r) return false;
            const std::string s = p.pstr(r->first, r->second);
            if (s.empty() || !py_printable(s) || s.size() < 3) return false;
        }
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

void node_params(const Pak &p, const Ref &node, Params &out) {
    const ORef tbl = res(p, node.first, node.second + 0x14), vals = res(p, node.first, node.second + 0x1c);
    if (!tbl || !vals || !is_param_table(p, *tbl)) return;
    const uint64_t vb = p.pbase(vals->first) + vals->second, tb = p.pbase(tbl->first) + tbl->second;
    for (uint32_t k = 0; k < 256; k++) {
        const ORef r = res(p, tbl->first, tbl->second + 0x10 * k + 0xc);
        if (!r) break;
        std::string name;
        try {
            name = p.pstr(r->first, r->second);
        } catch (const std::exception &) {
            break;
        }
        if (name.empty() || !py_printable(name)) break;
        if (tb + 0x10 * k + 12 > p.d.size()) throw Stop("struct.error: param entry past the end");
        const uint32_t size = be16(p, tb + 0x10 * k + 4), kind = p.d[tb + 0x10 * k + 7], off = be32(p.d, tb + 0x10 * k + 8);
        if (kind == 2 && size > 0 && size <= 64 && size % 4 == 0 && off < 0x1000 && !param(out, name)) {
            std::vector<double> v;
            for (uint32_t j = 0; j < size / 4; j++) v.push_back(double(bef32(p, vb + off + 4 * j)));
            out.push_back({name, v});
        }
    }
}

struct WalkNode {
    uint32_t k, e;
    Ref node;
};

std::vector<WalkNode> walk_passes(const Pak &p, const Ref &m) {
    std::vector<WalkNode> out;
    const ORef pl = res(p, m.first, m.second + 0x10);
    if (!pl) return out;
    for (uint32_t k = 0; k < 8; k++) {
        const ORef ps = res(p, pl->first, pl->second + 4 * k);
        if (!ps) continue;
        for (uint32_t e = 0; e < 32; e++) {
            if (p.pu32(ps->first, ps->second + 0x20 * e) != 0) break;
            const ORef node = res(p, ps->first, ps->second + 0x20 * e + 0x10);
            if (!node) break;
            out.push_back({k, e, *node});
        }
    }
    return out;
}

}  // namespace

const std::vector<double> *param(const Params &ps, const std::string &k) {
    for (const auto &e : ps)
        if (e.first == k) return &e.second;
    return nullptr;
}

std::map<std::string, Params> all_material_params(const Pak &p) {
    std::map<std::string, Params> res_;
    const auto mts = p.items_of("MATERIAL_TABLE_1");
    if (mts.empty()) return res_;
    const uint32_t pg = mts[0].page;
    const uint64_t b = mts[0].data_off();
    const uint32_t nmat = p.pu32(pg, b + 0xc);
    const ORef ma = res(p, pg, b + 0x20);
    std::vector<std::pair<std::string, Ref>> mats;
    for (uint32_t i = 0; i < nmat; i++) {
        const Ref m = need(p, need(ma).first, need(ma).second + 4 * i);
        const Ref n = need(p, m.first, m.second);
        mats.push_back({p.pstr(n.first, n.second), m});
    }
    // a pass run is not terminated where the material's entries end: a node reached at entry > 0 that is another
    // material's entry-0 (own) node is skipped
    std::map<std::string, std::vector<WalkNode>> W;
    std::map<Ref, std::set<std::string>> own;
    for (const auto &[n, m] : mats) {
        W[n] = walk_passes(p, m);
        for (const WalkNode &w : W[n])
            if (w.e == 0) own[w.node].insert(n);
    }
    for (const auto &[n, m] : mats) {
        Params out;
        for (const WalkNode &w : W[n]) {
            auto o = own.find(w.node);
            if (w.e > 0 && o != own.end() && !o->second.count(n)) continue;
            node_params(p, w.node, out);
        }
        res_[n] = out;
    }
    return res_;
}

// ------------------------------------------------------------------------------------------------ WATER_DATA
std::vector<std::pair<Ref, std::optional<std::string>>> water_objects(const Pak &p) {
    const auto its = p.items_of("WATER_DATA");
    if (its.empty()) return {};
    const PakItem &it = its[0];
    const uint32_t ver = p.pu32(it.page, it.data_off()), n = p.pu32(it.page, it.data_off() + 4);
    if (ver != 5) throw Stop(fmt("AssertionError: WATER_DATA version %u", ver));
    const ORef arr = res(p, it.page, it.data_off() + 8);
    std::map<uint32_t, std::string> mh;   // PS3 material entry hash (+0x14) -> material name
    if (const auto mts = p.items_of("MATERIAL_TABLE_1"); !mts.empty()) {
        const uint32_t pg = mts[0].page;
        const uint64_t b = mts[0].data_off();
        const uint32_t nmat = p.pu32(pg, b + 0xc);
        const ORef ma = res(p, pg, b + 0x20);
        for (uint32_t i = 0; i < nmat; i++) {
            const Ref m = need(p, need(ma).first, need(ma).second + 4 * i);
            const Ref nm = need(p, m.first, m.second);
            mh[p.pu32(m.first, m.second + 0x14)] = p.pstr(nm.first, nm.second);
        }
    }
    std::vector<std::pair<Ref, std::optional<std::string>>> out;
    for (uint32_t k = 0; k < n; k++) {
        const Ref o{need(arr).first, need(arr).second + 0xf0ull * k};
        auto f = mh.find(p.pu32(o.first, o.second + 4));
        out.push_back({o, f == mh.end() ? std::nullopt : std::optional<std::string>(f->second)});
    }
    return out;
}

std::vector<std::string> water_materials(const Pak &p) {
    std::vector<std::string> out;
    for (const auto &[o, m] : water_objects(p))
        if (m && !m->empty() && std::find(out.begin(), out.end(), *m) == out.end()) out.push_back(*m);
    return out;
}

}  // namespace u3::bg
