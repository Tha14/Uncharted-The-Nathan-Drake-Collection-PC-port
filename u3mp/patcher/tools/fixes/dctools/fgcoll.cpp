// dctools/fgcoll.cpp: see fgcoll.hpp.
#include "fgcoll.hpp"

#include "collision.hpp"
#include "hkconvert.hpp"
#include "ps3bg.hpp"
#include "u3fix.hpp"

#include <cstring>
#include <functional>

namespace u3::fgcoll {
namespace {

const uint32_t AA = 0xaaaaaaaa;

uint32_t rev32(uint32_t x) {
    uint32_t r = 0;
    for (int k = 0; k < 32; k++) r |= ((x >> k) & 1) << (31 - k);
    return r;
}

// PS3 big-endian 32-bit words -> little endian; the word indices in pairs hold two u16
Bytes swap_words(const Bytes &b, const std::vector<int> &pairs = {}) {
    Bytes out;
    for (size_t i = 0; i < b.size(); i += 4) {
        const size_t e = std::min(i + 4, b.size());
        const bool pair = std::find(pairs.begin(), pairs.end(), int(i / 4)) != pairs.end();
        if (pair) {   // w[1::-1] + w[3:1:-1]
            for (size_t k = std::min(i + 1, e - 1) + 1; k > i; k--) out.push_back(b[k - 1]);
            for (size_t k = e; k > i + 2; k--) out.push_back(b[k - 1]);
        } else
            for (size_t k = e; k > i; k--) out.push_back(b[k - 1]);
    }
    return out;
}

const std::pair<uint32_t, std::vector<int>> ELEM[3] = {{0x40, {14, 15}}, {0x38, {12}}, {0x28, {9}}};

struct R {
    const Pak &p;
    uint32_t u32(const Ref &a, uint64_t o = 0) const { return p.pu32(a.first, a.second + o); }
    Bytes raw(const Ref &a, uint64_t o, uint64_t n) const {
        const uint64_t b = p.pbase(a.first) + a.second + o;
        const uint64_t s = std::min<uint64_t>(b, p.d.size()), e = std::min<uint64_t>(b + n, p.d.size());
        return Bytes(p.d.begin() + std::ptrdiff_t(s), p.d.begin() + std::ptrdiff_t(std::max(s, e)));
    }
    ORef ptr(const Ref &a, uint64_t o = 0) const { return bg::res(p, a.first, a.second + o); }
};
uint32_t be16b(const Bytes &b) {
    if (b.size() < 2) throw Stop("struct.error: unpack requires a buffer of 2 bytes");
    return uint32_t(b[0] << 8 | b[1]);
}

struct Node {
    Bytes sphere;
    uint32_t cnt, tag, u;
    std::vector<Node> kids;
};
struct Tr3 {
    std::vector<std::pair<uint32_t, Bytes>> arrs;
    uint32_t u;
    std::vector<std::optional<Node>> trees;
};
struct Rec5 {
    std::optional<Tr3> tr;
    Bytes pair, tail;
    uint32_t bits;
};
struct Hk {
    uint32_t size, type, u14, u18, ival = 0;
    Bytes f, pf;
    std::optional<FgMesh> mesh;
    std::optional<Tr3> tr3;
    std::optional<std::vector<Rec5>> tr5;
};
struct Part {
    Bytes xf;
    uint32_t joint;
    std::optional<Hk> hk;
};
struct Con {
    uint32_t size, type, u8;
    Bytes pair, pf;
};
struct Fg {
    uint32_t ver, flags;
    std::vector<Part> parts;
    std::vector<Hk> extras;
    std::vector<Con> cons;
};

Node read_node(const R &r, const Ref &a) {
    Node n;
    n.sphere = r.raw(a, 0, 16);
    n.cnt = be16b(r.raw(a, 0x14, 2));
    n.tag = be16b(r.raw(a, 0x16, 2));
    n.u = r.u32(a, 0x18);
    if (n.cnt) {
        const Ref ka = bg::need(r.ptr(a, 0x10));
        for (uint32_t i = 0; i < n.cnt; i++) n.kids.push_back(read_node(r, bg::need(r.ptr(ka, 4ull * i))));
    }
    return n;
}

Tr3 read_tr3(const R &r, const Ref &a) {
    Tr3 t;
    for (int k = 0; k < 3; k++) {
        const uint32_t cnt = r.u32(a, 8ull * k + 4);
        Bytes data;
        if (cnt) data = r.raw(bg::need(r.ptr(a, 8ull * k)), 0, uint64_t(ELEM[k].first) * cnt);
        t.arrs.push_back({cnt, data});
    }
    t.u = r.u32(a, 0x18);
    for (uint64_t o : {0x1c, 0x20, 0x24}) {
        const ORef x = r.ptr(a, o);
        t.trees.push_back(x ? std::optional<Node>(read_node(r, *x)) : std::nullopt);
    }
    return t;
}

// the breakable trailer: nrec records (nrec = hk +0x24) {trailer3*, u16 pair, u32 bits, 12 bytes}
std::vector<Rec5> read_tr5(const R &r, const Ref &a, uint32_t nrec) {
    std::vector<Rec5> recs;
    for (uint32_t i = 0; i < nrec; i++) {
        const uint64_t o = 0x18ull * i;
        Rec5 rc;
        if (const ORef x = r.ptr(a, o)) rc.tr = read_tr3(r, *x);
        rc.pair = r.raw(a, o + 4, 4);
        rc.bits = r.u32(a, o + 8);
        rc.tail = r.raw(a, o + 0xc, 0xc);
        recs.push_back(std::move(rc));
    }
    return recs;
}

Hk read_hk(const R &r, const Ref &a) {
    Hk h;
    h.size = r.u32(a, 0);
    h.type = r.u32(a, 4);
    h.u14 = r.u32(a, 0x14);
    h.u18 = r.u32(a, 0x18);
    h.f = r.raw(a, 0x1c, 8);
    for (uint64_t o : {0x8, 0xc, 0x28, 0x2c})
        if (r.u32(a, o)) throw Stop(u3::fmt("NotImplementedError: hk block: non-zero word at +%#llx", (unsigned long long)o));
    if (const ORef m = r.ptr(a, 0x24)) h.mesh = fg_mesh(r.p, m->first, m->second);   // the PS3 ND mesh -> (packfile, mats)
    else h.ival = r.u32(a, 0x24);
    h.pf = r.raw(a, 0x30, h.size);
    if (const ORef t = r.ptr(a, 0x10)) {
        if (h.type == 5) h.tr5 = read_tr5(r, *t, h.ival);
        else h.tr3 = read_tr3(r, *t);
    }
    return h;
}

Fg read_fg(const Pak &p, const PakItem &it) {
    const R r{p};
    const Ref a{it.page, it.data_off()};
    Fg m;
    m.ver = r.u32(a, 0);
    m.flags = r.u32(a, 0x14);
    const uint32_t n = r.u32(a, 4), ne = r.u32(a, 0xc), nc = r.u32(a, 0x18);
    if (n) {
        const Ref pa = bg::need(r.ptr(a, 8));
        for (uint32_t i = 0; i < n; i++) {
            const Ref pp = bg::need(r.ptr(pa, 4ull * i));
            const ORef hk = r.ptr(pp, 0x28);
            if (r.u32(pp, 0x24) || r.u32(pp, 0x2c)) throw Stop(u3::fmt("NotImplementedError: part %u: non-zero +24/+2c", i));
            Part pt;
            pt.xf = r.raw(pp, 0, 0x20);
            pt.joint = r.u32(pp, 0x20);
            if (hk) pt.hk = read_hk(r, *hk);
            m.parts.push_back(std::move(pt));
        }
    }
    if (ne) {
        const Ref ea = bg::need(r.ptr(a, 0x10));
        for (uint32_t i = 0; i < ne; i++) m.extras.push_back(read_hk(r, bg::need(r.ptr(ea, 4ull * i))));
    }
    if (nc) {
        const Ref ca = bg::need(r.ptr(a, 0x1c));
        for (uint32_t i = 0; i < nc; i++) {
            const Ref c = bg::need(r.ptr(ca, 4ull * i));
            Con cn;
            cn.size = r.u32(c, 0);
            cn.type = r.u32(c, 4);
            cn.u8 = r.u32(c, 8);
            cn.pair = r.raw(c, 0xc, 4);
            cn.pf = r.raw(c, 0x10, cn.size);
            m.cons.push_back(std::move(cn));
        }
    }
    return m;
}

// one Blob for the whole item; pointers are self relocations
struct W {
    std::shared_ptr<Blob> b = std::make_shared<Blob>("fgcoll");
    size_t pos() const { return b->size(); }
    void pad(size_t a = 16, uint8_t fill = 0xaa) { b->pad(a, fill); }
    void ptr_at(size_t at, size_t target) { b->relocs.push_back({at, b, int64_t(target)}); }
    void put32(size_t at, uint32_t v) {
        if (at + 4 > b->b.size()) throw Stop("struct.error: pack_into past the end");
        for (int k = 0; k < 4; k++) b->b[at + size_t(k)] = uint8_t(v >> (8 * k));
    }
    void put16(size_t at, uint32_t v) {
        if (at + 2 > b->b.size()) throw Stop("struct.error: pack_into past the end");
        b->b[at] = uint8_t(v);
        b->b[at + 1] = uint8_t(v >> 8);
    }
    void setb(size_t at, const Bytes &x, size_t n) {   // b[at:at+n] = x (same length: the PS3 data is complete)
        if (x.size() != n) throw Stop("ValueError: a short PS3 field");
        std::copy(x.begin(), x.end(), b->b.begin() + std::ptrdiff_t(at));
    }
    size_t reserve(size_t n, uint8_t fill = 0) {
        const size_t o = pos();
        b->raw(Bytes(n, fill));
        return o;
    }
};

size_t w_node(W &w, const Node &n) {
    const size_t o = w.reserve(0x20);
    w.setb(o, swap_words(n.sphere), 16);
    w.put16(o + 0x18, n.cnt);
    w.put16(o + 0x1a, n.tag);
    w.put32(o + 0x1c, n.u);
    if (n.cnt) {
        const size_t ka = w.reserve(8ull * n.cnt);
        w.pad();
        w.ptr_at(o + 0x10, ka);
        for (size_t i = 0; i < n.kids.size(); i++) w.ptr_at(ka + 8 * i, w_node(w, n.kids[i]));
    }
    return o;
}

size_t w_tr3(W &w, const Tr3 &t) {
    w.pad();
    const size_t o = w.reserve(0x50);
    for (int k = 0; k < 3; k++) w.put32(o + 0x10 * size_t(k) + 0xc, AA);
    w.put32(o + 0x30, t.u);
    for (int k = 0; k < 3; k++) {
        const auto &[cnt, data] = t.arrs[size_t(k)];
        w.put32(o + 0x10 * size_t(k) + 8, cnt);
        if (cnt) {
            const size_t a = w.pos();
            const uint32_t sz = ELEM[k].first;
            for (uint32_t i = 0; i < cnt; i++) {
                const size_t s = std::min<size_t>(size_t(sz) * i, data.size()), e = std::min<size_t>(size_t(sz) * (i + 1), data.size());
                w.b->raw(swap_words(Bytes(data.begin() + std::ptrdiff_t(s), data.begin() + std::ptrdiff_t(e)), ELEM[k].second));
            }
            w.pad();
            w.ptr_at(o + 0x10 * size_t(k), a);
        }
    }
    for (size_t k = 0; k < t.trees.size(); k++)
        if (t.trees[k]) w.ptr_at(o + 0x38 + 8 * k, w_node(w, *t.trees[k]));
    return o;
}

size_t w_tr5(W &w, const std::vector<Rec5> &recs) {
    const size_t o = w.reserve(0x28 * recs.size());
    for (size_t i = 0; i < recs.size(); i++) {
        const Rec5 &rc = recs[i];
        const size_t ro = o + 0x28 * i;
        w.setb(ro + 8, swap_words(rc.pair, {0}), 4);
        w.put32(ro + 0xc, rev32(rc.bits));
        for (uint8_t c : rc.tail)
            if (c) throw Stop("NotImplementedError: breakable trailer record tail");
    }
    for (size_t i = 0; i < recs.size(); i++)
        if (recs[i].tr) w.ptr_at(o + 0x28 * i, w_tr3(w, *recs[i].tr));
    return o;
}

uint32_t fbits(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return ((v & 0x7f800000u) == 0x7f800000u && (v & 0x7fffffu)) ? v | 0x400000u : v;
}

size_t w_mesh(W &w, const FgMesh &mesh) {
    w.pad();
    const size_t pfo = w.pos();
    w.b->raw(mesh.pf);
    w.pad();
    const size_t mo = w.pos();
    for (const auto &[fr, fl] : mesh.mats) w.b->u32(1).u32(fr != 0 ? fbits(fr) : fbits(1.0f)).u32(fl);
    w.pad();
    const size_t so = w.reserve(0x20);
    w.put32(so + 0x10, uint32_t(mesh.pf.size()));
    w.put32(so + 0x14, uint32_t(mesh.mats.size()));
    w.put32(so + 0x18, AA);
    w.put32(so + 0x1c, AA);
    w.ptr_at(so, mo);
    w.ptr_at(so + 8, pfo);
    return so;
}

size_t w_hk(W &w, const Hk &h) {
    std::optional<size_t> so;
    if (h.mesh) so = w_mesh(w, *h.mesh);
    w.pad();
    const Bytes pf = hk::convert_packfile(h.pf);
    const bool brk = h.type == 5;
    const size_t o = w.reserve(0x40);
    if (h.type == 0) throw Stop("struct.error: argument out of range");
    w.put32(o, uint32_t(pf.size()));
    w.put32(o + 4, h.type - 1);
    w.put32(o + 0x20, h.u14);
    w.put32(o + 0x24, brk ? AA : 0);
    w.put32(o + 0x28, h.u18);
    w.setb(o + 0x30, swap_words(h.f), 8);
    if (so) w.ptr_at(o + 0x38, *so);
    else w.put32(o + 0x38, h.ival);
    w.b->raw(pf);
    w.pad();
    if (h.tr5) w.ptr_at(o + 0x18, w_tr5(w, *h.tr5));
    else if (h.tr3) w.ptr_at(o + 0x18, w_tr3(w, *h.tr3));
    return o;
}

}  // namespace

std::shared_ptr<Blob> fg_blob(const Pak &p, const PakItem &it) {
    const Fg m = read_fg(p, it);
    W w;
    const size_t o = w.reserve(0x30);
    w.put32(o, m.ver);
    w.put32(o + 4, uint32_t(m.parts.size()));
    w.put32(o + 0x10, uint32_t(m.extras.size()));
    w.put32(o + 0x14, AA);
    w.put32(o + 0x20, m.flags);
    w.put32(o + 0x24, uint32_t(m.cons.size()));
    if (!m.parts.empty()) {
        const size_t pa = w.reserve(8 * m.parts.size());
        w.pad();
        w.ptr_at(o + 8, pa);
        for (size_t i = 0; i < m.parts.size(); i++) {
            const Part &pt = m.parts[i];
            const size_t po = w.reserve(0x40);
            w.ptr_at(pa + 8 * i, po);
            w.setb(po, swap_words(pt.xf), 0x20);
            w.put32(po + 0x20, pt.joint);
            w.put32(po + 0x24, AA);
            if (pt.hk) w.ptr_at(po + 0x30, w_hk(w, *pt.hk));
        }
    }
    if (!m.extras.empty()) {
        const size_t ea = w.reserve(8 * m.extras.size());
        w.pad();
        w.ptr_at(o + 0x18, ea);
        for (size_t i = 0; i < m.extras.size(); i++) w.ptr_at(ea + 8 * i, w_hk(w, m.extras[i]));
    }
    if (!m.cons.empty()) {
        const size_t ca = w.reserve(8 * m.cons.size());
        w.pad();
        w.ptr_at(o + 0x28, ca);
        for (size_t i = 0; i < m.cons.size(); i++) {
            const Con &c = m.cons[i];
            const Bytes pf = hk::convert_packfile(c.pf);
            const size_t co = w.reserve(0x20);
            w.ptr_at(ca + 8 * i, co);
            if (c.type == 0) throw Stop("struct.error: argument out of range");
            w.put32(co, uint32_t(pf.size()));
            w.put32(co + 4, c.type - 1);
            w.put32(co + 8, c.u8);
            w.setb(co + 0x10, swap_words(c.pair, {0}), 4);
            w.b->raw(pf);
            w.pad();
        }
    }
    w.pad(8, 0);
    return w.b;
}

}  // namespace u3::fgcoll
