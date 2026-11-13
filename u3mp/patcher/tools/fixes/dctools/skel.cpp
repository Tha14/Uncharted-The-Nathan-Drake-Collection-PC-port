// dctools/skel.cpp: see skel.hpp.
#include "skel.hpp"

#include "ps3bg.hpp"
#include "u3fix.hpp"

namespace u3::skel {
namespace {

const uint32_t AA = 0xaaaaaaaa;

Bytes slice(const Bytes &c, uint64_t a, uint64_t e) {
    a = std::min<uint64_t>(a, c.size());
    e = std::min<uint64_t>(std::max(a, e), c.size());
    return Bytes(c.begin() + std::ptrdiff_t(a), c.begin() + std::ptrdiff_t(e));
}
Bytes sw(const Bytes &b, size_t k) {   // reverse every k-byte word (a short tail too)
    Bytes o;
    for (size_t i = 0; i < b.size(); i += k) {
        const size_t e = std::min(i + k, b.size());
        for (size_t j = e; j > i; j--) o.push_back(b[j - 1]);
    }
    return o;
}
void set16(Bytes &o, size_t at, uint32_t v) { o.at(at) = uint8_t(v); o.at(at + 1) = uint8_t(v >> 8); }
void set32(Bytes &o, size_t at, uint32_t v) { for (int k = 0; k < 4; k++) o.at(at + k) = uint8_t(v >> (8 * k)); }
uint64_t al(uint64_t n, uint64_t a = 16) { return (n + a - 1) / a * a; }

Bytes D_TEMPLATE() {
    Bytes d = {3, 0, 0, 0, 0xaa, 0xaa, 0xaa, 0xaa};
    d.resize(d.size() + 0x4c, 0);
    d.insert(d.end(), 4, 0xaa);
    d.resize(d.size() + 0x14, 0);
    d.insert(d.end(), 4, 0xaa);
    d.resize(d.size() + 0x34, 0);
    d.insert(d.end(), 12, 0xaa);
    if (d.size() != 0xb0) throw Stop("AssertionError: D template");
    return d;
}

// a PS3 'JH' skeleton blob -> the PS4 'AH' blob
Bytes convert_blob(const Bytes &x) {
    uint32_t h[24];
    for (int i = 0; i < 24; i++) h[i] = be32(x, 4ull * i);
    if (h[0] != 0x4a48010a) throw Stop(u3::fmt("AssertionError: not a PS3 JH blob: %08x", h[0]));
    const uint32_t nj = h[3] >> 16;
    const uint32_t g0 = x.at(0x14), g1 = x.at(0x15), groups = g0;
    const uint32_t pose_o = h[11], inv_o = h[17], grp_o = h[18], cmd_o = h[19], sid_o = h[20], par_o = h[10];
    if ((h[4] >> 16) != nj || x.at(0x16) != 1 || x.at(0x17) != 1) throw Stop("AssertionError: JH header");
    const Bytes pose = sw(slice(x, pose_o, pose_o + 0x30ull * nj), 4), inv = sw(slice(x, inv_o, inv_o + 0x30ull * nj), 4);
    const Bytes grp = slice(x, grp_o, grp_o + 8ull * groups);
    const uint32_t csz = be32(x, cmd_o), coff = be32(x, cmd_o + 4);
    Bytes cmd = sw(slice(x, coff, uint64_t(coff) + csz), 2);
    if (cmd.size() < 6) throw Stop("struct.error: command list");
    if ((cmd[4] | cmd[5] << 8) != 0x000c) throw Stop("AssertionError: command list context location");
    set16(cmd, 4, 0x0003);   // the evaluation context: PS3 base 0xc -> PS4 base 3
    const Bytes sids = sw(slice(x, sid_o, sid_o + 4ull * nj), 4);
    if (par_o > x.size() || x.size() - par_o != 0x10ull * nj) throw Stop("AssertionError: parent table size");
    const Bytes pars = sw(slice(x, par_o, x.size()), 4);
    const uint64_t o_pose = 0x50, o_inv = o_pose + pose.size(), o_a = o_inv + inv.size(), o_b = o_a + 0x20, o_c = o_b + al(8ull * groups),
                   o_cmd = o_c + 0x10, o_d = o_cmd + al(cmd.size());
    const uint64_t d_sids = 0x20, d_par = d_sids + al(4ull * nj), d_size = d_par + pars.size(), total = o_d + d_size + 0x20;
    Bytes out(total, 0);
    set32(out, 0, 0x41480106);
    set32(out, 4, h[1]);
    set32(out, 8, uint32_t(total));
    set16(out, 12, nj);
    set16(out, 14, nj);
    set32(out, 16, 0xff010000);
    set16(out, 20, 0);
    set16(out, 22, groups);
    set32(out, 24, groups);
    set32(out, 28, 1);
    set32(out, 0x28, uint32_t(o_pose));
    set32(out, 0x30, uint32_t(o_inv));
    set32(out, 0x34, uint32_t(o_a));
    set32(out, 0x38, uint32_t(o_b));
    set32(out, 0x3c, 0);
    set32(out, 0x40, uint32_t(o_c));
    set32(out, 0x4c, uint32_t(o_d));
    std::copy(pose.begin(), pose.end(), out.begin() + std::ptrdiff_t(o_pose));
    std::copy(inv.begin(), inv.end(), out.begin() + std::ptrdiff_t(o_inv));
    set32(out, o_a, 0);
    set32(out, o_a + 4, 0);
    set16(out, o_a + 8, 0);
    set16(out, o_a + 10, nj);
    out[o_a + 12] = uint8_t(g0);
    out[o_a + 13] = uint8_t(g1);
    out[o_a + 14] = 0;
    out[o_a + 15] = 1;
    set32(out, o_a + 16, nj);
    for (uint32_t g = 0; g < groups; g++) {
        if (8ull * g + 8 > grp.size()) throw Stop("ValueError: group table short");
        for (int k = 0; k < 4; k++) out[o_b + 8 * g + k] = grp[8 * g + k];
        for (int k = 0; k < 4; k++) out[o_b + 8 * g + 4 + k] = grp[8 * g + 7 - k];
    }
    set16(out, o_c, 0);
    set16(out, o_c + 2, csz);
    set32(out, o_c + 4, uint32_t(o_cmd));
    std::copy(cmd.begin(), cmd.end(), out.begin() + std::ptrdiff_t(o_cmd));
    set16(out, o_d, 0);
    set16(out, o_d + 2, uint32_t(d_size));
    set32(out, o_d + 4, uint32_t(d_sids));
    set32(out, o_d + 0x14, uint32_t(d_par));
    std::copy(sids.begin(), sids.end(), out.begin() + std::ptrdiff_t(o_d + d_sids));
    std::copy(pars.begin(), pars.end(), out.begin() + std::ptrdiff_t(o_d + d_par));
    return out;
}

}  // namespace

JH read_jh(const Pak &p, const PakItem &it) {
    const uint32_t pg = it.page;
    const uint64_t o = it.data_off();
    JH m;
    m.ver = p.pu32(pg, o);
    m.nj = p.pu32(pg, o + 4);
    const Ref ba = bg::need(p, pg, o + 8);
    const uint64_t bb = p.pbase(ba.first) + ba.second;
    m.blob = slice(p.d, bb, bb + p.u32(bb + 8));
    const Ref na = bg::need(p, pg, o + 0x10);
    for (uint32_t i = 0; i < m.nj; i++) {
        const uint32_t s = p.pu32(na.first, na.second + 8ull * i);
        const Ref r = bg::need(p, na.first, na.second + 8ull * i + 4);
        m.names.push_back({s, p.pstr(r.first, r.second)});
    }
    if (p.pu32(pg, o + 0x14)) throw Stop("NotImplementedError: JOINT_HIERARCHY +0x14 non-zero");
    const ORef d = bg::res(p, pg, o + 0x18);
    if (d) {
        const Bytes dd = slice(p.d, p.pbase(d->first) + d->second, p.pbase(d->first) + d->second + 0x5c);
        bool zeros = true;
        for (size_t k = 4; k < dd.size(); k++) zeros = zeros && !dd[k];
        if (be32(dd, 0) != 3 || !zeros) throw Stop("NotImplementedError: JOINT_HIERARCHY D block differs from {3, 0...}");
    }
    m.d = d.has_value();
    const ORef B = bg::res(p, pg, o + 0xc);
    m.b = B.has_value();
    if (B)
        if (const ORef sa = bg::res(p, B->first, B->second + 0x2c)) {
            // S {u16* per group (inline), float**, u8**}: k = the group count of the skeleton blob
            const uint32_t k = m.blob.at(0x14);
            const Ref p0 = bg::need(p, sa->first, sa->second), p1 = bg::need(p, sa->first, sa->second + 4), p2 = bg::need(p, sa->first, sa->second + 8);
            m.s_cnt = slice(p.d, p.pbase(p0.first) + p0.second, p.pbase(p0.first) + p0.second + 2ull * k);
            const uint32_t h18 = be32(m.blob, 18 * 4);
            const Bytes grp = slice(m.blob, h18, h18 + 8ull * k);
            for (uint32_t g = 0; g < k; g++) {
                const uint32_t nf = uint32_t(m.s_cnt.at(2 * g)) + m.s_cnt.at(2 * g + 1);
                const uint32_t nb = (uint32_t(grp.at(8 * g)) + 1) & ~1u;   // joints of the group, rounded to even
                const Ref fa = bg::need(p, p1.first, p1.second + 4ull * g), bya = bg::need(p, p2.first, p2.second + 4ull * g);
                m.s_groups.push_back({slice(p.d, p.pbase(fa.first) + fa.second, p.pbase(fa.first) + fa.second + 4ull * nf),
                                      slice(p.d, p.pbase(bya.first) + bya.second, p.pbase(bya.first) + bya.second + nb)});
            }
            m.has_s = true;
        }
    m.sid = p.pu32(pg, o + 0x1c);
    return m;
}

std::shared_ptr<Blob> build_jh(const JH &m) {
    auto b = std::make_shared<Blob>("jointhier");
    b->u32(m.ver).u32(m.nj);
    std::vector<std::pair<size_t, size_t>> rel;   // (pos, target offset)
    auto ptr_here = [&](size_t pos) { rel.push_back({pos, b->size()}); };
    b->u64(0);   // +08 blob
    b->u64(0);   // +10 B
    b->u64(0);   // +18 names
    b->u64(0);   // +20
    b->u64(0);   // +28 D
    b->u32(m.sid).u32(AA).u32(AA).u32(AA);
    ptr_here(0x18);
    const size_t ent = b->size();
    for (const auto &[s, n] : m.names) b->u32(s).u32(AA).u64(0);
    for (size_t i = 0; i < m.names.size(); i++) {
        ptr_here(ent + 16 * i + 8);
        b->cstr(bg::utf8_of(m.names[i].second));
        b->pad(16, 0xaa);
    }
    if (m.d) {
        ptr_here(0x28);
        b->raw(D_TEMPLATE());
    }
    if (m.b) {
        ptr_here(0x10);
        const size_t bo = b->size();
        b->u64(0).u32(0).u32(AA).u64(0).u64(0).u32(0).u32(AA).u64(0).u32(0).u32(AA).u64(0).u64(0).u64(0).u64(0).u64(0);
        if (b->size() - bo != 0x60) throw Stop("AssertionError: B block");
        if (m.has_s) {
            const size_t k = m.s_groups.size();
            rel.push_back({bo + 0x50, b->size()});
            const size_t so = b->size();
            b->u64(0).u64(0).u64(0).u32(AA).u32(AA);
            rel.push_back({so, b->size()});
            b->raw(sw(m.s_cnt, 2));
            b->pad(16, 0xaa);
            rel.push_back({so + 8, b->size()});
            const size_t p1 = b->size();
            b->raw(Bytes(8 * k, 0));
            b->pad(16, 0xaa);
            rel.push_back({so + 0x10, b->size()});
            const size_t p2 = b->size();
            b->raw(Bytes(8 * k, 0));
            b->pad(16, 0xaa);
            for (size_t g = 0; g < k; g++) {
                rel.push_back({p1 + 8 * g, b->size()});
                b->raw(sw(m.s_groups[g].first, 4));
                b->pad(16, 0xaa);
                rel.push_back({p2 + 8 * g, b->size()});
                b->raw(m.s_groups[g].second);
                b->pad(16, 0xaa);
            }
        }
    }
    b->pad(16, 0xaa);
    ptr_here(0x08);
    b->raw(convert_blob(m.blob));
    for (const auto &[pos, t] : rel) b->relocs.push_back({pos, b, int64_t(t)});
    return b;
}

}  // namespace u3::skel
