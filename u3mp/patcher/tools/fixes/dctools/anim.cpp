// dctools/anim.cpp: see anim.hpp.
#include "anim.hpp"

#include "ps3bg.hpp"
#include "u3fix.hpp"

#include <algorithm>
#include <set>

namespace u3::anim {
namespace {

const uint32_t PAD32 = 0xaaaaaaaa;
const size_t PAGE_LIMIT = 0x1b0000;
const uint32_t PS3_MAGIC = 0x43440107, PS4_MAGIC = 0x41430104;

uint32_t quiet(uint32_t v) { return ((v & 0x7f800000u) == 0x7f800000u && (v & 0x7fffffu)) ? v | 0x400000u : v; }

// reads of a byte buffer in one endianness (struct.error past the end)
struct Rd {
    const Bytes &c;
    bool be;
    uint64_t get(uint64_t o, int n) const {
        if (o + uint64_t(n) > c.size()) throw Stop("struct.error: unpack_from requires a buffer of more bytes");
        uint64_t v = 0;
        for (int k = 0; k < n; k++) v |= uint64_t(c[o + uint64_t(be ? n - 1 - k : k)]) << (8 * k);
        return v;
    }
    uint32_t u8(uint64_t o) const { return uint32_t(get(o, 1)); }
    uint32_t u16(uint64_t o) const { return uint32_t(get(o, 2)); }
    uint32_t u32(uint64_t o) const { return uint32_t(get(o, 4)); }
    uint64_t u64(uint64_t o) const { return get(o, 8); }
};

void put16(Bytes &o, uint32_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); }
void put32(Bytes &o, uint32_t v) { for (int k = 0; k < 4; k++) o.push_back(uint8_t(v >> (8 * k))); }
void set16(Bytes &o, size_t at, uint32_t v) { o.at(at) = uint8_t(v); o.at(at + 1) = uint8_t(v >> 8); }
void set32(Bytes &o, size_t at, uint32_t v) { for (int k = 0; k < 4; k++) o.at(at + k) = uint8_t(v >> (8 * k)); }
void set64(Bytes &o, size_t at, uint64_t v) { for (int k = 0; k < 8; k++) o.at(at + k) = uint8_t(v >> (8 * k)); }

Bytes slice(const Bytes &c, int64_t a, int64_t e) {   // c[a:e] (Python slice of non-negative bounds)
    a = std::clamp<int64_t>(a, 0, int64_t(c.size()));
    e = std::clamp<int64_t>(e, 0, int64_t(c.size()));
    return e > a ? Bytes(c.begin() + a, c.begin() + e) : Bytes();
}

Bytes swapk(const Bytes &b, int k) {   // reverse every k-byte element; a short tail stays
    if (k == 1) return b;
    Bytes o(b);
    const size_t n = b.size() - b.size() % size_t(k);
    for (size_t i = 0; i < n; i += size_t(k))
        for (int j = 0; j < k; j++) o[i + size_t(j)] = b[i + size_t(k - 1 - j)];
    return o;
}

uint64_t a16(uint64_t x) { return (x + 15) & ~uint64_t(15); }

struct Group {
    uint32_t cmd, bsz, b6, b7, fdo, stride, ue;
};
struct Header {
    uint32_t w[14];   // magic hash size (ngroups u0e) off10 grpoff off18 fps (flags nframes) f24 f28 f2c: see clip_header
    uint32_t ngroups, grpoff, off10;
    std::vector<Group> groups;
};
const char *const HFMT = "IIIHHIIIfHHfff";   // the 0x30-byte clip header

Header clip_header(const Bytes &c, bool ps3) {
    const Rd r{c, ps3};
    Header h;
    uint64_t o = 0;
    for (int i = 0; i < 14; i++) {
        const int n = HFMT[i] == 'H' ? 2 : 4;
        h.w[i] = uint32_t(r.get(o, n));
        o += uint64_t(n);
    }
    h.ngroups = h.w[3];
    h.off10 = h.w[5];
    h.grpoff = h.w[6];
    for (uint32_t i = 0; i < h.ngroups; i++) {
        const uint64_t g = h.grpoff + 16ull * i;
        h.groups.push_back({r.u32(g), r.u16(g + 4), r.u8(g + 6), r.u8(g + 7), r.u32(g + 8), r.u16(g + 12), r.u16(g + 14)});
    }
    return h;
}
// the header fields of one clip in the other byte order (floats as Python packs them back)
Bytes header_le(const Bytes &c3, const uint32_t *override_vals) {
    const Rd r{c3, true};
    Bytes out;
    uint64_t o = 0;
    for (int i = 0; i < 14; i++) {
        const int n = HFMT[i] == 'H' ? 2 : 4;
        uint32_t v = override_vals ? override_vals[i] : uint32_t(r.get(o, n));
        if (HFMT[i] == 'f') v = quiet(v);
        if (n == 2) put16(out, v);
        else put32(out, v);
        o += uint64_t(n);
    }
    return out;
}

int nmasks(const Group &g) { return (g.b6 ? 1 : 0) + (g.b7 ? 1 : 0); }

struct Cmd {
    uint64_t off;
    uint32_t op;
    std::vector<uint32_t> a;
};

std::vector<Cmd> parse_cmds(const Bytes &c, bool ps3, uint64_t off, std::optional<uint64_t> end) {
    const Rd r{c, ps3};
    std::vector<Cmd> out;
    while (true) {
        if (end && off + 2 > *end) break;
        const uint32_t w = r.u16(off);
        if ((w & 0xff) == 0) break;
        const uint32_t n = w >> 8;
        Cmd cm{off, w & 0xff, {}};
        for (uint32_t k = 0; k < n; k++) cm.a.push_back(r.u16(off + 2 + 2ull * k));
        out.push_back(std::move(cm));
        off += 2 + 2ull * n;
    }
    return out;
}

struct GroupCmds {
    Group g;
    uint64_t b0;
    std::vector<Cmd> l;
    uint64_t end;
};

std::vector<GroupCmds> clip_groups(const Bytes &c, bool ps3, Header &h) {
    h = clip_header(c, ps3);
    std::vector<GroupCmds> out;
    for (const Group &g : h.groups) {
        const uint64_t b0 = g.cmd + (ps3 ? 16ull * nmasks(g) : 0), end = uint64_t(g.cmd) + g.bsz;
        out.push_back({g, b0, b0 < end ? parse_cmds(c, ps3, b0, c.size()) : std::vector<Cmd>{}, end});
    }
    return out;
}

// the PS4 byte length of the const-data region an opcode references (arg 2)
uint64_t need(uint32_t op, const std::vector<uint32_t> &a) {
    const uint64_t n = a.at(0), m = n & 0x7ff;
    switch (op) {
    case 0x0b: return n >> 15 ? m : 1;                              // u8 bit widths (per item if bit 15)
    case 0x0c: return n >> 15 ? 4 * (n & 0x1fff) : 1;               // u8 bit widths, 4 components
    case 0x0f: return 2 * ((m + 7) & ~uint64_t(7));                 // u16 destination index table (lerp vec)
    case 0x10: return 2 * ((m + 15) & ~uint64_t(15));               // (slerp quat)
    case 0x11: return 2 * (((n & 0x1fff) + 3) & ~uint64_t(3));      // (float lerp)
    case 0x15: return 14 * n;                                       // u16 idx[n] + f32 vec3[n]
    case 0x17: return 18 * n;                                       // u16 idx[n] + f32 quat[n]
    case 0x16: case 0x18: return 8 * n;                             // u16 idx[n] + half3 / packed quat48
    case 0x19: return 6 * n;                                        // u16 idx[n] + f32[n]
    case 0x1b: return 16 * m + (n >> 15 ? 4 * m : 4);               // 2x u64 range + u32 format
    case 0x1c: return n >> 15 ? 4 * m : 4;                          // u32 format words
    case 0x1d: return 8 * m;                                        // s16 x4
    case 0x1e: return 16 * m;                                       // s16 x8
    default: throw Stop(u3::fmt("KeyError: 'unsupported clip opcode %#x'", op));
    }
}

Bytes cat(Bytes a, const Bytes &b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

Bytes xform(uint32_t op, const std::vector<uint32_t> &a, const Bytes &b) {
    const uint64_t n = a.at(0);
    auto head = [&](uint64_t k) { return slice(b, 0, int64_t(k)); };
    auto tail = [&](uint64_t k) { return slice(b, int64_t(k), int64_t(b.size())); };
    switch (op) {
    case 0x0b: case 0x0c: case 0x1c: return b;
    case 0x0f: case 0x10: case 0x11: case 0x16: case 0x1d: case 0x1e: return swapk(b, 2);
    case 0x18: return cat(swapk(head(2 * n), 2), tail(2 * n));
    case 0x19: case 0x15: case 0x17: return cat(swapk(head(2 * n), 2), swapk(tail(2 * n), 4));
    case 0x1b: {
        const uint64_t m = n & 0x7ff;
        return cat(swapk(head(16 * m), 8), tail(16 * m));
    }
    default: throw Stop(u3::fmt("KeyError: %u", op));
    }
}

Bytes convert_block(const Bytes &c3, uint64_t b3, const std::vector<Cmd> &l3, uint64_t ext) {
    if (l3.empty()) return Bytes{0, 0};
    const uint64_t cend = l3.back().off + 2 + 2 * l3.back().a.size() + 2;
    std::map<uint32_t, std::vector<const Cmd *>> refs;
    for (const Cmd &c : l3) {
        if (c.op == 0x1a) continue;   // arg 2 unused on PS4 (zeroed by the retail converter)
        if (c.a.size() < 3) throw Stop(u3::fmt("KeyError: 'opcode %#x with %zu args'", c.op, c.a.size()));
        refs[c.a[2]].push_back(&c);
    }
    std::vector<uint32_t> keys;
    for (const auto &[k, v] : refs) keys.push_back(k);
    const uint64_t first = keys.empty() ? a16(cend - b3) : keys[0];
    std::map<uint32_t, uint64_t> newoff;
    Bytes out(first, 0);
    for (size_t i = 0; i < keys.size(); i++) {
        const uint32_t k = keys[i];
        const bool last = i + 1 == keys.size();
        const int64_t L3 = (last ? int64_t(ext) - int64_t(b3) : int64_t(keys[i + 1])) - int64_t(k);
        const Cmd *best = nullptr;
        uint64_t bn = 0;
        for (const Cmd *c : refs[k]) {
            const uint64_t nd = need(c->op, c->a);
            if (!best || nd > bn) {
                best = c;
                bn = nd;
            }
        }
        const uint64_t nd = bn;
        const uint64_t L4 = last ? nd : a16(uint64_t(std::max<int64_t>(L3, int64_t(nd))));
        Bytes src = slice(c3, int64_t(b3 + k), std::min<int64_t>(int64_t(b3 + k + L4), int64_t(ext)));
        if (src.size() < L4) src.resize(L4, 0xff);
        newoff[k] = out.size();
        const Bytes x = xform(best->op, best->a, src);
        out.insert(out.end(), x.begin(), x.end());
    }
    Bytes cmd;
    for (const Cmd &c : l3) {
        std::vector<uint32_t> a = c.a;
        if (a.size() < 2) throw Stop("IndexError: list index out of range");
        static const std::set<uint32_t> OK = {0x0b, 0x0c, 0x0f, 0x10, 0x11, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e};
        if (a[1] == 0x000c) a[1] = 0x0003;
        else if ((a[1] & 0xf) || !OK.count(c.op)) throw Stop(u3::fmt("KeyError: 'unexpected context location %#x for op %#x'", a[1], c.op));
        if (a.size() < 3) throw Stop("IndexError: list assignment index out of range");
        if (c.op == 0x1a) a[2] = 0;
        else {
            auto it = newoff.find(a[2]);
            if (it == newoff.end()) throw Stop("KeyError: const-data offset");
            if (it->second > 0xffff) throw Stop("struct.error: 'H' format requires 0 <= number <= 65535");
            a[2] = uint32_t(it->second);
        }
        put16(cmd, c.op | uint32_t(a.size() << 8));
        for (uint32_t x : a) put16(cmd, x);
    }
    put16(cmd, 0);
    if (cmd.size() > first) throw Stop("AssertionError: command list longer than its block");
    std::copy(cmd.begin(), cmd.end(), out.begin());
    // out[len(cmd):first] = swapped PS3 bytes (a short slice shrinks the buffer, as a bytearray does)
    const Bytes mid = swapk(slice(c3, int64_t(b3 + cmd.size()), int64_t(b3 + first)), 2);
    Bytes res(out.begin(), out.begin() + std::ptrdiff_t(cmd.size()));
    res.insert(res.end(), mid.begin(), mid.end());
    res.insert(res.end(), out.begin() + std::ptrdiff_t(first), out.end());
    return res;
}

}  // namespace

bool is_streamed(const Bytes &c3) { return Rd{c3, true}.u32(0x14) == 0; }

Bytes convert_clip(const Bytes &c3) {
    const Rd r{c3, true};
    if (r.u32(0) != PS3_MAGIC) throw Stop("ValueError: not a PS3 clip");
    if (is_streamed(c3)) {
        // header-only clip of a streamed animation (the frames live in the -chunk-N items): retail keeps the PS3
        // magic and only swaps the header fields
        if (r.u32(8) != 0x30) throw Stop("ValueError: streamed clip with a body");
        return header_le(c3, nullptr);
    }
    Header h3;
    const std::vector<GroupCmds> G3 = clip_groups(c3, true, h3);
    const auto &gs = h3.groups;
    const size_t ng = gs.size();
    if (gs.empty()) throw Stop("ValueError: min() arg is an empty sequence");
    uint64_t fd0 = gs[0].fdo;
    for (const Group &g : gs) fd0 = std::min<uint64_t>(fd0, g.fdo);
    std::vector<Bytes> blocks;
    for (size_t gi = 0; gi < ng; gi++) {
        const uint64_t ext = gi + 1 < ng ? gs[gi + 1].cmd : fd0;
        blocks.push_back(convert_block(c3, G3[gi].b0, G3[gi].l, ext));
    }
    uint64_t nm = 0;
    for (const Group &g : gs) nm += uint64_t(nmasks(g));
    Bytes out(0x30 + 16 * ng + 16 * nm, 0);
    std::vector<uint64_t> cmdoffs;
    for (const Bytes &b : blocks) {
        const uint64_t off = a16(out.size());
        out.resize(off, 0);
        cmdoffs.push_back(off);
        out.insert(out.end(), b.begin(), b.end());
    }
    const uint64_t fd4 = a16(out.size());
    out.resize(fd4, 0);
    if (h3.off10 && h3.off10 < fd0) throw Stop("ValueError: keyframe bitmask table before frame data - unsupported");
    const Bytes tail = slice(c3, int64_t(fd0), int64_t(c3.size()));
    out.insert(out.end(), tail.begin(), tail.end());
    out.resize(out.size() + 16 * ng, 0);   // the retail converter pads 16 bytes per group
    uint32_t hw[14];
    std::copy(h3.w, h3.w + 14, hw);
    hw[0] = PS4_MAGIC;
    hw[2] = uint32_t(out.size());
    if (hw[5]) hw[5] = uint32_t(hw[5] + (fd4 - fd0));
    const Bytes hdr = header_le(c3, hw);
    std::copy(hdr.begin(), hdr.end(), out.begin());
    uint64_t mo = 0x30 + 16 * ng;
    for (size_t gi = 0; gi < ng; gi++) {
        const Group &g = gs[gi];
        const uint64_t fdo = (g.fdo < c3.size() || fd0 < c3.size()) ? g.fdo - fd0 + fd4 : out.size();
        const size_t o = 0x30 + gi * 16;
        set32(out, o, uint32_t(cmdoffs[gi]));
        if (blocks[gi].size() > 0xffff) throw Stop("struct.error: block size");
        set16(out, o + 4, uint32_t(blocks[gi].size()));
        out[o + 6] = uint8_t(g.b6);
        out[o + 7] = uint8_t(g.b7);
        set32(out, o + 8, uint32_t(fdo));
        set16(out, o + 12, g.stride);
        set16(out, o + 14, g.ue);
        for (int k = 0; k < nmasks(g); k++) {
            set64(out, mo, r.u64(g.cmd + 16ull * k));
            set64(out, mo + 8, r.u64(g.cmd + 16ull * k + 8));
            mo += 16;
        }
    }
    return out;
}

namespace {

std::vector<uint64_t> item_targets(const Pak &p, const PakItem &it) {
    const uint64_t lo = it.data_off(), hi = uint64_t(it.data_off()) + it.size;
    std::set<uint64_t> t;
    for (const auto &[k, tp] : p.ptrmap) {
        const auto [pg, o] = k;
        if (pg == it.page && lo <= o && o < hi && tp == it.page) {
            const uint64_t v = p.pptr(pg, o);
            if (lo <= v && v <= hi) t.insert(v);
        }
    }
    return std::vector<uint64_t>(t.begin(), t.end());
}

const PakItem *containing_item(const Pak &p, uint32_t pg, uint64_t off) {
    for (const PakItem &x : p.items)
        if (x.page == pg && x.data_off() <= off && off < uint64_t(x.data_off()) + x.size) return &x;
    return nullptr;
}

}  // namespace

Anim read_ps3_anim(const Pak &p, const PakItem &it) {
    const uint32_t pg = it.page;
    const uint64_t b = it.data_off(), base = p.pbase(pg) + b;
    uint32_t w[7];
    for (int k = 0; k < 7; k++) w[k] = be32(p.d, base + 4ull * k);
    Anim a;
    a.name = it.name;
    a.w0 = w[0];
    a.w1 = w[1];
    a.w4 = w[4];
    a.skel = w[5];
    if (w[0] != 6 || w[4] != 0) throw Stop("ValueError: " + it.name + ": unexpected ANIM header");
    if (const ORef r = bg::res(p, pg, b + 8)) {   // clip
        const uint64_t cb = p.pbase(r->first) + r->second;
        const uint32_t size = be32(p.d, cb + 8);
        a.clip = slice(p.d, int64_t(cb), int64_t(cb + size));
    }
    if (const ORef r = bg::res(p, pg, b + 12)) {   // float channels (ANIM_RAWCHANNELS item)
        const auto [cpg, co] = *r;
        const PakItem *ci = containing_item(p, cpg, co);
        if (!ci) throw Stop("AttributeError: 'NoneType' object has no attribute 'data_off'");
        std::vector<uint64_t> T = item_targets(p, *ci);
        T.push_back(uint64_t(ci->data_off()) + ci->size);
        const uint32_t n = p.pu32(cpg, co);
        std::vector<Anim::Chan> chans;
        if (n) {
            const uint64_t sp = bg::need(p, cpg, co + 4).second, pp = bg::need(p, cpg, co + 8).second;
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t s = p.pu32(cpg, sp + 4ull * i);
                const uint64_t hh = bg::need(p, cpg, pp + 4ull * i).second;
                const uint32_t nfr = bg::be16(p, p.pbase(cpg) + hh), typ = bg::be16(p, p.pbase(cpg) + hh + 2);
                const ORef dr = bg::res(p, cpg, hh + 4);
                if (!dr) {
                    chans.push_back({s, nfr, typ, std::nullopt});
                    continue;
                }
                const uint64_t d = dr->second;
                auto nx = std::find_if(T.begin(), T.end(), [&](uint64_t t) { return t > d; });
                if (nx == T.end()) throw Stop("IndexError: list index out of range");
                chans.push_back({s, nfr, typ, slice(p.d, int64_t(p.pbase(cpg) + d), int64_t(p.pbase(cpg) + *nx))});
            }
        }
        a.chans = chans;
    }
    if (const ORef r = bg::res(p, pg, b + 24)) {   // events
        const auto [epg, eo] = *r;
        Anim::Events ev;
        ev.tsid = p.pu32(epg, eo);
        ev.cnt = p.pu32(epg, eo + 4);
        if (const ORef er = bg::res(p, epg, eo + 8))
            for (uint32_t i = 0; i < ev.cnt; i++) {
                const uint64_t e = er->second + 16ull * i, eb = p.pbase(epg) + e;
                Anim::Event x{be32(p.d, eb), be32(p.d, eb + 4), be32(p.d, eb + 8), std::nullopt};
                if (const ORef kvr = bg::res(p, epg, e + 12)) {
                    std::vector<std::pair<uint32_t, uint32_t>> kv;
                    for (uint32_t j = 0; j < x.cnt; j++) {
                        const uint64_t ko = kvr->second + 8ull * j;
                        if (p.is_ptr(epg, uint32_t(ko)) || p.is_ptr(epg, uint32_t(ko + 4))) throw Stop("ValueError: " + it.name + ": pointer inside event key/value list");
                        kv.push_back({be32(p.d, p.pbase(epg) + ko), be32(p.d, p.pbase(epg) + ko + 4)});
                    }
                    x.kv = kv;
                }
                ev.ents.push_back(std::move(x));
            }
        a.events = ev;
    }
    return a;
}

std::shared_ptr<Blob> build_ps4_anim(const Anim &a) {
    auto B = std::make_shared<Blob>("anim:" + a.name);
    std::optional<Bytes> clip4;
    if (a.clip) clip4 = convert_clip(*a.clip);
    const size_t hdr = B->size();
    B->u32(a.w0).u32(a.w1).u64(0).u64(0).u64(0).u32(a.skel).u32(PAD32).u64(0);
    std::vector<std::pair<size_t, size_t>> fix;   // (field, target) resolved after layout
    if (a.chans) {
        B->pad(16, 0xaa);
        const size_t ct = B->size();
        fix.push_back({hdr + 0x10, ct});
        const size_t n = a.chans->size();
        B->u32(uint32_t(n)).u32(PAD32).u64(0).u64(0).u32(PAD32).u32(PAD32);
        if (n) {
            B->pad(16, 0xaa);
            fix.push_back({ct + 8, B->size()});
            for (const auto &c : *a.chans) B->u32(c.sid);
            B->pad(16, 0xaa);
            const size_t pp = B->size();
            fix.push_back({ct + 0x10, pp});
            B->raw(Bytes(8 * n, 0));
            for (size_t i = 0; i < n; i++) {
                const auto &c = (*a.chans)[i];
                B->pad(16, 0xaa);
                fix.push_back({pp + 8 * i, B->size()});
                const size_t ch = B->size();
                B->u16(c.nfr).u16(c.typ).u32(PAD32).u64(0);
                if (c.data) {
                    B->pad(16, 0xaa);
                    fix.push_back({ch + 8, B->size()});
                    B->raw(swapk(*c.data, 2));
                }
            }
        }
    }
    if (a.events) {
        B->pad(16, 0xaa);
        const size_t et = B->size();
        fix.push_back({hdr + 0x28, et});
        B->u32(a.events->tsid).u32(a.events->cnt).u64(0);
        if (!a.events->ents.empty()) {
            B->pad(16, 0xaa);
            const size_t ep = B->size();
            fix.push_back({et + 8, ep});
            for (const auto &e : a.events->ents) B->u32(e.sid).u32(quiet(e.fbits)).u32(e.cnt).u32(PAD32).u64(0);
            for (size_t i = 0; i < a.events->ents.size(); i++) {
                const auto &e = a.events->ents[i];
                if (!e.kv || e.kv->empty()) continue;   // retail writes null for an empty key/value list
                B->pad(16, 0xaa);
                fix.push_back({ep + 24 * i + 16, B->size()});
                for (const auto &[k, v] : *e.kv) B->u32(k).u32(v);
            }
        }
    }
    if (clip4) {
        B->pad(16, 0xaa);
        fix.push_back({hdr + 8, B->size()});
        B->raw(*clip4);
    }
    for (const auto &[pos, tgt] : fix) B->relocs.push_back({pos, B, int64_t(tgt)});
    return B;
}

namespace {

// a PS3 igc-stream-*.stm file: blocks {16-byte tag, a one-page PS3 pak whose login lists one ANIM '<anim>-chunk-N'}
std::vector<std::pair<std::string, Anim>> read_stm(const fs::path &path) {
    const Bytes d = read_file(path);
    std::vector<std::pair<std::string, Anim>> out;
    size_t o = 0;
    while (o < d.size()) {
        if (d.size() - o < 0x40) {
            for (size_t k = o; k < d.size(); k++)
                if (d[k]) throw Stop(u3::fmt("ValueError: %s: trailing data at %#zx", utf8(path).c_str(), o));
            break;
        }
        const Pak p0(Bytes(d.begin() + std::ptrdiff_t(o + 16), d.end()), utf8(path));
        const uint64_t size = uint64_t(p0.pages.back().file) + p0.pages.back().size;
        if (p0.login.size() != 1 || p0.item_at(p0.login[0].first, p0.login[0].second).type != "ANIM")
            throw Stop(u3::fmt("ValueError: %s: block at %#zx is not a single ANIM", utf8(path).c_str(), o));
        const Pak p(Bytes(d.begin() + std::ptrdiff_t(o + 16), d.begin() + std::ptrdiff_t(std::min<uint64_t>(o + 16 + size, d.size()))), utf8(path));
        const PakItem li = p.item_at(p.login[0].first, p.login[0].second);
        out.push_back({li.name, read_ps3_anim(p, li)});
        o += 16 + size;
    }
    return out;
}

struct SB {
    std::string name;
    uint32_t fps, bufsize, size;
    std::vector<std::string> names;
    std::vector<uint32_t> nch, nstr, counts;
    std::vector<std::vector<uint32_t>> sids, slots;
    std::vector<std::vector<uint64_t>> tables_at, table_len;
};

// a PS3 STREAMING_BUFFER (stream set): list counts, sids and slots, fps, chunks per file, buffer size, chunk-size
// tables, file names, streams per file, a runtime with three buffer records (the two buffers end the item)
SB read_ps3_sb(const Pak &p, const PakItem &it) {
    const uint32_t pg = it.page;
    const uint64_t b = it.data_off();
    auto R = [&](uint64_t o) { return bg::need(p, pg, o).second; };
    auto Ro = [&](uint64_t o) { return bg::res(p, pg, o); };
    auto U = [&](uint64_t o) { return p.pu32(pg, o); };
    SB sb;
    sb.name = it.name;
    sb.size = it.size;
    const uint64_t rt = R(b + 0x24);
    const uint32_t nl = U(rt);
    const uint64_t names_at = R(b + 0x1c), first_str = R(names_at);
    const uint64_t nf = (first_str - names_at) / 4;
    for (uint64_t i = 0; i < nf; i++) sb.names.push_back(p.pstr(pg, R(names_at + 4 * i)));
    const uint64_t A = R(b);
    for (uint32_t i = 0; i < nl; i++) sb.counts.push_back(U(A + 4ull * i));
    for (uint32_t i = 0; i < nl; i++) {
        std::vector<uint32_t> s, sl;
        for (uint32_t j = 0; j < sb.counts[i]; j++) {
            s.push_back(U(R(R(b + 4) + 4ull * i) + 4ull * j));
            sl.push_back(U(R(R(b + 8) + 4ull * i) + 4ull * j));
        }
        sb.sids.push_back(s);
        sb.slots.push_back(sl);
    }
    for (uint64_t f = 0; f < nf; f++) sb.nch.push_back(U(R(b + 0x10) + 4 * f));
    for (uint64_t f = 0; f < nf; f++) sb.nstr.push_back(U(R(b + 0x20) + 4 * f));
    const uint64_t D = R(b + 0x18);
    for (uint32_t i = 0; i < nl; i++) {
        std::vector<uint64_t> t;
        for (uint32_t j = 0; j < sb.counts[i]; j++) t.push_back(R(R(D + 4ull * i) + 4ull * j));
        sb.tables_at.push_back(t);
    }
    const uint64_t recs = R(rt + 0x20);
    ORef bufs[3];
    for (int k = 0; k < 3; k++) bufs[k] = Ro(recs + 0x14ull * k + 0xc);
    if (bufs[0] || !bufs[1] || !bufs[2]) throw Stop("ValueError: " + it.name + ": unexpected stream buffer records");
    sb.fps = U(b + 0xc);
    sb.bufsize = U(b + 0x14);
    if (bufs[2]->second + sb.bufsize != b + it.size || bufs[1]->second + sb.bufsize != bufs[2]->second)
        throw Stop("ValueError: " + it.name + ": stream buffers do not end the item");
    uint64_t s1 = 0, s2 = 0;
    for (uint32_t c : sb.counts) s1 += c;
    for (uint32_t c : sb.nstr) s2 += c;
    if (s1 != s2) throw Stop("ValueError: " + it.name + ": list/file stream counts differ");
    return sb;
}

// one stm file's blocks in rows: [chunk 1..nch] -> [anim names in file order]
std::vector<std::vector<std::string>> stm_streams(const std::vector<std::pair<std::string, Anim>> &stm, uint32_t nch) {
    const size_t n = nch ? stm.size() / nch : 0;
    if (n * nch != stm.size()) throw Stop(u3::fmt("ValueError: stm block count %zu is not a multiple of %u chunks", stm.size(), nch));
    std::vector<std::vector<std::string>> rows;
    for (uint32_t k = 0; k < nch; k++) {
        std::vector<std::string> row;
        for (size_t i = k * n; i < (k + 1) * n; i++) {
            const std::string suf = "-chunk-" + std::to_string(k + 1);
            const std::string &nm = stm[i].first;
            if (nm.size() < suf.size() || nm.compare(nm.size() - suf.size(), suf.size(), suf) != 0)
                throw Stop(u3::fmt("ValueError: stm block %s out of order (chunk %u)", nm.c_str(), k + 1));
            row.push_back(nm);
        }
        rows.push_back(row);
    }
    return rows;
}

std::shared_ptr<Blob> build_ps4_sb(const Pak &p, const PakItem &it, const SB &sb, const std::vector<std::vector<std::vector<std::string>>> &file_rows,
                                   const std::map<std::string, std::shared_ptr<PakWItem>> &chunk_items) {
    const uint32_t pg = it.page;
    auto B = std::make_shared<Blob>("sb:" + sb.name);
    const size_t nl = sb.counts.size(), nf = sb.names.size();
    std::vector<std::pair<size_t, size_t>> fix;
    auto al = [&] { B->pad(16, 0xaa); };
    auto u32s = [&](const std::vector<uint32_t> &vals) {
        al();
        const size_t o = B->size();
        for (uint32_t v : vals) B->u32(v);
        return o;
    };
    auto ptrs = [&](size_t n) {
        al();
        const size_t o = B->size();
        B->raw(Bytes(8 * n, 0));
        return o;
    };
    const size_t hdr = B->size();
    B->raw(Bytes(0x18, 0)).u32(sb.fps).u32(PAD32).u64(0).u32(sb.bufsize).u32(PAD32);
    B->raw(Bytes(0x20, 0));
    fix.push_back({hdr + 0, u32s(sb.counts)});
    const size_t pb = ptrs(nl);
    fix.push_back({hdr + 8, pb});
    const size_t pc = ptrs(nl);
    fix.push_back({hdr + 0x10, pc});
    for (size_t i = 0; i < nl; i++) {
        fix.push_back({pb + 8 * i, u32s(sb.sids[i])});
        fix.push_back({pc + 8 * i, u32s(sb.slots[i])});
    }
    fix.push_back({hdr + 0x20, u32s(sb.nch)});
    fix.push_back({hdr + 0x40, u32s(sb.nstr)});
    const size_t pn = ptrs(nf);
    fix.push_back({hdr + 0x38, pn});
    for (size_t f = 0; f < nf; f++) {
        al();
        fix.push_back({pn + 8 * f, B->size()});
        B->cstr(bg::utf8_of(sb.names[f]));
    }
    const size_t pd = ptrs(nl);
    fix.push_back({hdr + 0x30, pd});
    for (size_t i = 0; i < nl; i++) {
        const size_t pl = ptrs(sb.counts[i]);
        fix.push_back({pd + 8 * i, pl});
        for (size_t j = 0; j < sb.tables_at[i].size(); j++) {
            std::vector<uint32_t> v;
            for (uint64_t k = 0; k < sb.table_len[i][j]; k++) v.push_back(p.pu32(pg, sb.tables_at[i][j] + 4 * k));
            fix.push_back({pl + 8 * j, u32s(v)});
        }
    }
    al();
    const size_t rt = B->size();
    fix.push_back({hdr + 0x48, rt});
    B->u32(uint32_t(nl)).u32(PAD32).u64(0).u32(0).u32(PAD32).u64(0).u32(0).u32(PAD32).u64(0).u64(0);
    B->u32(0).u32(PAD32).u64(0).u64(0).u64(0).u32(0).u32(PAD32);
    fix.push_back({rt + 8, u32s(std::vector<uint32_t>(nl, 0))});
    fix.push_back({rt + 0x18, u32s(std::vector<uint32_t>(nl, 0))});
    al();
    const size_t recs = B->size();
    fix.push_back({rt + 0x40, recs});
    for (uint32_t k = 0; k < 3; k++) B->u32(k).u32(0).u32(0).u32(PAD32).u64(0).u64(0);
    al();
    const size_t ct = B->size();
    fix.push_back({rt + 0x48, ct});
    B->u64(0);
    const size_t pf = ptrs(nf);
    fix.push_back({ct, pf});
    for (size_t f = 0; f < nf; f++) {
        const auto &rows = file_rows[f];
        const size_t pr = ptrs(rows.size() + 1);
        fix.push_back({pf + 8 * f, pr});
        fix.push_back({pr, ptrs(sb.nstr[f])});   // chunk 0 is resident: a null row
        for (size_t k = 0; k < rows.size(); k++) {
            const size_t o = ptrs(rows[k].size());
            fix.push_back({pr + 8 * (k + 1), o});
            for (size_t j = 0; j < rows[k].size(); j++) B->relocs.push_back({o + 8 * j, chunk_items.at(rows[k][j])->data, -0x20});
        }
    }
    fix.push_back({recs + 0x18, ptrs(nl)});
    for (int k = 1; k <= 2; k++) {
        al();
        fix.push_back({recs + 0x20 * size_t(k) + 0x10, B->size()});
        B->raw(Bytes(sb.bufsize, 0));
    }
    for (const auto &[pos, tgt] : fix) B->relocs.push_back({pos, B, int64_t(tgt)});
    return B;
}

struct Pager {
    PakWriter &w;
    Page *cur;
    size_t size = 0x200;
    explicit Pager(PakWriter &w_) : w(w_), cur(&w_.page()) {}
    std::shared_ptr<PakWItem> item(const std::string &type, const std::shared_ptr<Blob> &data, const std::string &name = "") {
        const size_t est = a16(data->size() + 0x40 + type.size() + name.size()) + 16;
        if (size + est > PAGE_LIMIT && size > 0x200) {
            cur = &w.page();
            size = 0x200;
        }
        size += est;
        return cur->item(type, data, name);
    }
};

fs::path find_stm(const std::string &name, const std::vector<fs::path> &dirs) {
    for (const fs::path &d : dirs)
        if (fs::exists(d / upath(name + ".stm"))) return d / upath(name + ".stm");
    throw Stop("FileNotFoundError: stream " + name + ".stm not found");
}

}  // namespace

Bytes convert_pak(const fs::path &src, const std::vector<fs::path> &stm_dirs) {
    const Pak p(src);
    if (!p.ps3) throw Stop("ValueError: source must be a PS3 pak");
    static const std::set<std::string> SKIP = {"ANIM_RAWCHANNELS", "MATERIAL_TABLE_1", "VRAM_DESC_TABLE", "PAK_LOGIN_TABLE", "ANIM",
                                               "ARTGROUP_DATA", "LEVEL_PATS_1", "PACKAGE_ITEM_LOOKUP", "STREAMING_BUFFER"};
    std::set<std::string> other;
    for (const PakItem &i : p.items)
        if (!SKIP.count(i.type)) other.insert(i.type);
    if (!other.empty()) throw Stop("ValueError: unsupported item types in " + utf8(src) + ": " + *other.begin());
    PakWriter w;
    Pager pg(w);
    std::map<std::pair<uint32_t, uint32_t>, std::shared_ptr<PakWItem>> newitem;   // PS3 item header -> PS4 item
    std::vector<PakItem> login3;
    for (const auto &[a, b] : p.login) login3.push_back(p.item_at(a, b));
    std::vector<PakItem> anims;
    std::set<std::pair<uint32_t, uint32_t>> in_login;
    for (const PakItem &x : login3) {
        in_login.insert({x.page, x.off});
        if (x.type == "ANIM") anims.push_back(x);
    }
    for (const PakItem &x : p.items_of("ANIM"))
        if (!in_login.count({x.page, x.off})) anims.push_back(x);
    for (const PakItem &it : anims) newitem[{it.page, it.off}] = pg.item("ANIM", build_ps4_anim(read_ps3_anim(p, it)), bg::utf8_of(it.name));
    // streamed animations: the chunks 1..N from the .stm files become ANIM items
    std::shared_ptr<PakWItem> sbitem;
    const auto sbs = p.items_of("STREAMING_BUFFER");
    if (sbs.size() > 1) throw Stop("ValueError: more than one STREAMING_BUFFER");
    if (!sbs.empty()) {
        const PakItem &sb3 = sbs[0];
        SB sb = read_ps3_sb(p, sb3);
        std::vector<std::vector<std::vector<std::string>>> file_rows;
        std::map<std::string, std::shared_ptr<PakWItem>> chunk_items;
        std::map<uint32_t, size_t> file_of;
        for (size_t f = 0; f < sb.names.size(); f++) {
            const auto stm = read_stm(find_stm(sb.names[f], stm_dirs));
            const auto rows = stm_streams(stm, sb.nch[f]);
            if (!rows.empty() && rows[0].size() != sb.nstr[f])
                throw Stop(u3::fmt("ValueError: %s: %zu streams in stm, %u in STREAMING_BUFFER", sb.names[f].c_str(), rows[0].size(), sb.nstr[f]));
            for (const auto &[cn, a] : stm) chunk_items[cn] = pg.item("ANIM", build_ps4_anim(a), bg::utf8_of(cn));
            if (!rows.empty())
                for (const std::string &x : rows[0]) file_of[sid(bg::utf8_of(x.substr(0, x.size() - 8)))] = f;
            file_rows.push_back(rows);
        }
        for (const auto &l : sb.sids) {
            std::vector<uint64_t> v;
            for (uint32_t s : l) {
                auto fi = file_of.find(s);
                if (fi == file_of.end()) throw Stop("KeyError: stream sid");
                v.push_back(sb.nch[fi->second]);
            }
            sb.table_len.push_back(v);
        }
        sbitem = pg.item("STREAMING_BUFFER", build_ps4_sb(p, sb3, sb, file_rows, chunk_items), bg::utf8_of(sb3.name));
    }
    // artgroup
    const auto ags = p.items_of("ARTGROUP_DATA");
    if (ags.empty()) throw Stop("IndexError: no ARTGROUP_DATA");
    const PakItem &ag3 = ags[0];
    const uint32_t gp = ag3.page;
    const uint64_t gb = ag3.data_off();
    const uint32_t n = p.pu32(gp, gb);
    if (p.pu32(gp, gb + 4) != 1) throw Stop("ValueError: artgroup +4 != 1");
    const uint64_t ents = bg::need(p, gp, gb + 8).second;
    std::vector<std::shared_ptr<PakWItem>> targets;
    for (uint32_t k = 0; k < n; k++) {
        const ORef r = bg::res(p, gp, ents + 8ull * k);
        if (!r) {
            targets.push_back(nullptr);
            continue;
        }
        auto it = newitem.find({r->first, uint32_t(r->second)});
        if (it == newitem.end()) throw Stop("KeyError: artgroup target");
        targets.push_back(it->second);
    }
    const uint64_t h2 = bg::need(p, gp, gb + 0xc).second, x = bg::need(p, gp, h2 + 0xc).second;
    const uint32_t nb = p.pu32(gp, x + 0x10);
    const uint64_t tbl = bg::need(p, gp, x + 0x14).second;
    std::vector<std::pair<uint32_t, std::optional<uint64_t>>> buckets;
    for (uint32_t i = 0; i < nb; i++) {
        const uint32_t key = p.pu32(gp, tbl + 8ull * i);
        const ORef r = bg::res(p, gp, tbl + 8ull * i + 4);
        buckets.push_back({key, r ? std::optional<uint64_t>((r->second - ents) / 8) : std::nullopt});
    }
    auto ab = std::make_shared<Blob>("artgroup");
    ab->u32(n).u32(1).ptr(ab, 0x50).ptr(ab, 0x30).u64(0).u64(0).u32(PAD32).u32(PAD32);
    const size_t tbl_off = 0x50 + 16ull * n, x_off = tbl_off + 16ull * nb;
    ab->u64(0).u32(1).u32(0x12345).ptr(ab, int64_t(x_off)).u32(PAD32).u32(PAD32);
    for (const auto &t : targets) ab->ptr(t ? t->data : nullptr, -0x20).u64(0);
    for (const auto &[key, ei] : buckets) {
        ab->u32(key).u32(PAD32);
        if (ei) ab->ptr(ab, int64_t(0x50 + 16 * *ei));
        else ab->u64(0);
    }
    ab->u32(0).u32(PAD32).u64(0).u64(0).u32(0).u32(nb).ptr(ab, int64_t(tbl_off));
    const std::string art = ag3.name;
    auto artgroup = pg.item("ARTGROUP_DATA", ab, bg::utf8_of(art));
    auto lp = std::make_shared<Blob>("levelpats");
    lp->u64(0);
    auto levelpats = pg.item("LEVEL_PATS_1", lp);
    auto pl = std::make_shared<Blob>("pkglookup");
    const std::string art8 = bg::utf8_of(art);
    pl->ptr(pl, 0x20).u32(0).u32(1).ptr(pl, int64_t(0x20 + ((art8.size() + 1 + 15) / 16) * 16)).u32(PAD32).u32(PAD32);
    pl->cstr(art8);
    pl->pad(16, 0xaa);
    pl->u32(sid(art8)).u32(PAD32).ptr(artgroup->data, -0x20);
    auto lookup = pg.item("PACKAGE_ITEM_LOOKUP", pl);
    // the login page
    Page &lpage = w.page();
    std::vector<std::shared_ptr<PakWItem>> order;
    for (const PakItem &x2 : login3) {
        if (x2.type == "ARTGROUP_DATA") order.push_back(artgroup);
        else if (x2.type == "LEVEL_PATS_1") order.push_back(levelpats);
        else if (x2.type == "ANIM") order.push_back(newitem.at({x2.page, x2.off}));
        else if (x2.type == "PACKAGE_ITEM_LOOKUP") order.push_back(lookup);
        else if (x2.type == "STREAMING_BUFFER") order.push_back(sbitem);
    }
    std::vector<std::shared_ptr<PakWItem>> extras = {artgroup, levelpats};
    for (const PakItem &i : anims) extras.push_back(newitem.at({i.page, i.off}));
    extras.push_back(lookup);
    if (sbitem) extras.push_back(sbitem);
    for (const auto &e : extras)
        if (std::find(order.begin(), order.end(), e) == order.end()) order.push_back(e);
    make_login_and_vram_page(w, lpage, order, {});
    Bytes data = w.build();
    for (const auto &pgp : w.pages)   // break the self-reference cycles
        for (auto &part : pgp->parts)
            if (auto *it = std::get_if<std::shared_ptr<PakWItem>>(&part)) (*it)->data->relocs.clear();
    return data;
}

}  // namespace u3::anim
