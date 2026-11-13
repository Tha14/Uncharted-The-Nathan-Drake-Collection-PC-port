// dctools/visinfo.cpp: see visinfo.hpp.
#include "visinfo.hpp"

#include "ps3bg.hpp"
#include "u3fix.hpp"

#include <set>

namespace u3 {
namespace {

const uint32_t AA = 0xaaaaaaaa;

Bytes sw4(const Bytes &x) {   // byte-swap every u32 (a short tail is reversed too)
    Bytes o;
    o.reserve(x.size());
    for (size_t i = 0; i < x.size(); i += 4) {
        const size_t e = std::min(i + 4, x.size());
        for (size_t k = e; k > i; k--) o.push_back(x[k - 1]);
    }
    return o;
}

// bytes of a stream up to and including its end code
size_t huff_len(const Bytes &stream, const std::vector<std::pair<uint32_t, uint32_t>> &dic, size_t &nout) {
    if (dic.empty()) throw Stop("IndexError: empty vis dictionary");
    const auto root = dic.back();
    uint32_t l = root.first, r = root.second;
    nout = 0;
    for (size_t i = 0; i < stream.size(); i++)
        for (uint32_t m = 0x80; m; m >>= 1) {
            const uint32_t s = (stream[i] & m) ? r : l;
            if (s < 0x100) {
                nout++;
                l = root.first;
                r = root.second;
            } else if (s == 0x100)
                return i + 1;
            else {
                if (s - 0x101 >= dic.size()) throw Stop("IndexError: vis dictionary symbol");
                l = dic[s - 0x101].first;
                r = dic[s - 0x101].second;
            }
        }
    throw Stop(u3::fmt("ValueError: vis stream without end code (%zu bytes, %zu decoded)", stream.size(), nout));
}

struct VNode {
    uint32_t a;
    int32_t b;
    uint32_t s;
    ORef bp;
};
struct VRegion {
    Bytes aabb, planes;
    uint32_t npl = 0, nfa = 0, x = 0;
    std::vector<std::pair<uint32_t, std::optional<Bytes>>> faces;
    std::vector<VNode> nodes;
    std::map<Ref, Bytes> streams;
};
struct Vis {
    std::vector<uint32_t> W;
    uint32_t nwords = 0;
    std::vector<std::pair<std::optional<Bytes>, std::vector<uint32_t>>> bufs;
    std::vector<std::pair<uint32_t, uint32_t>> dict;
    std::vector<VRegion> regions;
    std::vector<std::pair<uint32_t, std::optional<Bytes>>> G;
    std::vector<uint32_t> H;
};

Vis read(const Pak &p, const PakItem &it) {
    const uint32_t pg = it.page;
    const uint64_t o = it.data_off();
    auto f = [&](uint32_t pg_, uint64_t a, uint64_t n) {   // p.d[base + a : base + a + n] (a short slice at the end)
        const uint64_t s = std::min<uint64_t>(p.pbase(pg_) + a, p.d.size()), e = std::min<uint64_t>(p.pbase(pg_) + a + n, p.d.size());
        return Bytes(p.d.begin() + std::ptrdiff_t(s), p.d.begin() + std::ptrdiff_t(std::max(s, e)));
    };
    Vis m;
    for (uint32_t k = 0; k < 0x2a; k++) m.W.push_back(p.pu32(pg, o + 4 * k));
    if (m.W[0] != 9) throw Stop(u3::fmt("NotImplementedError: VIS_INFO_1 version %u", m.W[0]));
    const uint32_t nreg = m.W[1], ndict = m.W[0x90 / 4], ng = m.W[0x98 / 4], nh = m.W[0xa4 / 4];
    m.nwords = m.W[0x8c / 4];
    for (int k = 0; k < 4; k++) {
        const uint64_t base = 0x10 + 0x20 * k;
        const ORef r = bg::res(p, pg, o + base);
        std::vector<uint32_t> words(m.W.begin() + std::ptrdiff_t(base / 4 + 1), m.W.begin() + std::ptrdiff_t(base / 4 + 8));
        m.bufs.push_back({r ? std::optional<Bytes>(f(r->first, r->second, 4ull * m.nwords)) : std::nullopt, words});
    }
    {
        const Ref r = bg::need(p, pg, o + 0x94);
        const Bytes dic_raw = f(r.first, r.second, 4ull * ndict);
        for (uint32_t i = 0; i < ndict; i++) m.dict.push_back({be32(dic_raw, 4ull * i) >> 16, be32(dic_raw, 4ull * i) & 0xffff});
    }
    const ORef rr = bg::res(p, pg, o + 8);
    for (uint32_t i = 0; i < nreg; i++) {
        const uint32_t rp = bg::need(rr).first;
        const uint64_t ro = rr->second + 0x30ull * i;
        uint32_t RW[12];
        for (int k = 0; k < 12; k++) RW[k] = p.pu32(rp, ro + 4 * k);
        VRegion reg;
        reg.npl = RW[0];
        reg.nfa = RW[9];
        reg.x = RW[11];
        reg.aabb = f(rp, ro + 0xc, 24);
        const ORef q = bg::res(p, rp, ro + 4);
        if (q) reg.planes = f(q->first, q->second, 16ull * reg.npl);
        if (reg.npl && !q) throw Stop("ValueError: planes");
        const ORef qf = bg::res(p, rp, ro + 0x28);
        for (uint32_t j = 0; j < reg.nfa; j++) {
            const uint32_t fp = bg::need(qf).first;
            const uint64_t fo = qf->second + 8ull * j;
            const uint32_t nv = p.pu32(fp, fo);
            const ORef v = bg::res(p, fp, fo + 4);
            reg.faces.push_back({nv, v ? std::optional<Bytes>(f(v->first, v->second, 12ull * nv)) : std::nullopt});
        }
        // nodes: walk the tree from the root
        if (const ORef qn = bg::res(p, rp, ro + 8)) {
            const auto [np_, no] = *qn;
            std::map<uint32_t, VNode> nodes;
            std::vector<int64_t> todo = {0};
            while (!todo.empty()) {
                const int64_t n = todo.back();
                todo.pop_back();
                if (nodes.count(uint32_t(n))) continue;
                const uint64_t a0 = p.pbase(np_) + no + 16ull * uint64_t(n);
                VNode nd{be32(p.d, a0), int32_t(be32(p.d, a0 + 4)), be32(p.d, a0 + 8), bg::res(p, np_, no + 16ull * uint64_t(n) + 12)};
                nodes[uint32_t(n)] = nd;
                if (!nd.bp) {
                    const int64_t left = int64_t(int32_t(nd.a)) >> 2;
                    for (int64_t c : {left, int64_t(nd.b)})
                        if (c >= 0) todo.push_back(c);
                }
            }
            const uint32_t nn = nodes.rbegin()->first + 1;
            if (nodes.size() != nn) throw Stop("ValueError: vis node indices not contiguous");
            for (uint32_t k = 0; k < nn; k++) reg.nodes.push_back(nodes[k]);
        }
        for (const VNode &nd : reg.nodes)
            if (nd.bp && !reg.streams.count(*nd.bp)) {
                const Bytes raw = f(nd.bp->first, nd.bp->second, 0x10000);
                size_t nout;
                const size_t n = huff_len(raw, m.dict, nout);
                if (nout > 4ull * m.nwords) throw Stop(u3::fmt("ValueError: vis leaf decodes to %zu bytes, expected %u", nout, 4 * m.nwords));
                reg.streams[*nd.bp] = Bytes(raw.begin(), raw.begin() + std::ptrdiff_t(n));
            }
        m.regions.push_back(std::move(reg));
    }
    const ORef qg = bg::res(p, pg, o + 0x9c);
    for (uint32_t j = 0; j < ng; j++) {
        const uint32_t gp = bg::need(qg).first;
        const uint64_t go = qg->second + 8ull * j;
        const uint32_t v = p.pu32(gp, go);
        const ORef b = bg::res(p, gp, go + 4);
        m.G.push_back({v, b ? std::optional<Bytes>(f(b->first, b->second, 4ull * m.nwords)) : std::nullopt});
    }
    const ORef qh = bg::res(p, pg, o + 0xa0);
    for (uint32_t j = 0; j < nh; j++) m.H.push_back(p.pu32(bg::need(qh).first, qh->second + 8ull * j));
    return m;
}

std::vector<std::shared_ptr<Blob>> build_parts(const Vis &m, size_t limit) {
    std::vector<std::shared_ptr<Blob>> objs;   // in placement order
    auto obj = [&](const char *name) {
        auto o = std::make_shared<Blob>(name);
        objs.push_back(o);
        return o;
    };
    auto hd = obj("vis_hdr");
    auto regb = obj("vis_regions");
    auto hb = obj("vis_H");
    for (uint32_t sid_ : m.H) hb->u32(sid_).u32(AA).u64(0);
    struct Parts {
        std::shared_ptr<Blob> planes, nodes, faces;
    };
    std::vector<Parts> reg_parts;
    for (const VRegion &reg : m.regions) {
        Parts parts;
        if (!reg.planes.empty()) {
            parts.planes = obj("vis_planes");
            parts.planes->raw(sw4(reg.planes));
        }
        if (!reg.nodes.empty()) {
            auto nb = obj("vis_nodes");
            parts.nodes = nb;
            std::map<Ref, std::shared_ptr<Blob>> sb;
            for (const VNode &nd : reg.nodes) {
                nb->u32(nd.a).i32(nd.b).u32(nd.s).u32(AA);
                if (!nd.bp) nb->u64(0);
                else {
                    auto &s = sb[*nd.bp];
                    if (!s) {
                        s = std::make_shared<Blob>("vis_leaf");
                        s->raw(reg.streams.at(*nd.bp));
                    }
                    nb->ptr(s);
                }
            }
            std::set<Ref> seen;   // streams in node order
            for (const VNode &nd : reg.nodes)
                if (nd.bp && seen.insert(*nd.bp).second) objs.push_back(sb[*nd.bp]);
        }
        if (!reg.faces.empty()) {
            auto fb = obj("vis_faces");
            parts.faces = fb;
            std::vector<std::shared_ptr<Blob>> vbs;
            for (const auto &[nv, v] : reg.faces) {
                fb->u32(nv).u32(AA);
                if (!v) fb->u64(0);
                else {
                    auto vb = std::make_shared<Blob>("vis_verts");
                    vb->raw(sw4(*v));
                    vbs.push_back(vb);
                    fb->ptr(vb);
                }
            }
            objs.insert(objs.end(), vbs.begin(), vbs.end());
        }
        reg_parts.push_back(parts);
    }
    for (size_t i = 0; i < m.regions.size(); i++) {
        const VRegion &reg = m.regions[i];
        const Parts &parts = reg_parts[i];
        regb->ptr(parts.planes).ptr(parts.nodes).ptr(parts.faces);
        regb->raw(sw4(reg.aabb));
        regb->u32(reg.npl).u32(reg.nfa).u32(reg.x).u32(AA);
    }
    std::shared_ptr<Blob> bufs[4];
    for (int k = 0; k < 4; k++)
        if (m.bufs[k].first) {
            bufs[k] = obj("vis_buf");
            bufs[k]->raw(sw4(*m.bufs[k].first));
        }
    auto db = obj("vis_dict");
    for (const auto &[l, r] : m.dict) db->u16(l).u16(r);
    std::shared_ptr<Blob> gb;
    if (!m.G.empty()) {
        gb = obj("vis_G");
        std::vector<std::shared_ptr<Blob>> gbits;
        for (const auto &[v, bits] : m.G) {
            gb->u32(v).u32(AA);
            if (!bits) gb->u64(0);
            else {
                auto x = std::make_shared<Blob>("vis_Gbits");
                x->raw(*bits);
                gbits.push_back(x);
                gb->ptr(x);
            }
        }
        objs.insert(objs.end(), gbits.begin(), gbits.end());
    }
    hd->u32(9).u32(m.regions.size()).ptr(regb);
    hd->u32(m.W[3]).u32(0);
    for (int k = 0; k < 4; k++) {
        hd->ptr(bufs[k]);
        for (uint32_t w : m.bufs[k].second) hd->u32(w);
        if (k < 3) hd->u32(0);
    }
    hd->u32(m.dict.size()).ptr(db);
    hd->u32(m.G.size()).u32(AA).ptr(gb).ptr(hb);
    hd->u32(m.H.size()).u32(AA);
    if (hd->size() != 0xe0) throw Stop("AssertionError: vis header");
    std::vector<std::shared_ptr<Blob>> out = {std::make_shared<Blob>("vis_info")};
    for (const auto &o : objs) {
        auto cur = out.back();
        if (limit && cur->size() && cur->size() + 15 + o->size() > limit) {
            out.push_back(std::make_shared<Blob>("vis_part"));
            cur = out.back();
        }
        cur->embed(o, cur->size() ? 16 : 1, 0xaa);
    }
    return out;
}

}  // namespace

std::vector<std::shared_ptr<Blob>> vis_blobs(const Pak &p, size_t limit) {
    const auto its = p.items_of("VIS_INFO_1");
    if (its.empty()) return {};
    const Vis m = read(p, its[0]);
    auto one = build_parts(m, 0);
    if (one[0]->size() <= limit) return one;
    return build_parts(m, limit);
}

}  // namespace u3
