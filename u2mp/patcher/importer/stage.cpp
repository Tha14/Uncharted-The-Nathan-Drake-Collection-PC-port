// importer/stage.cpp: Uncharted 2 stage (background) paks, PS3 -> PS4, on the Uncharted 3 stage converter (dctools
// ps4stage with u3::g_u2: the U2 cook path, VIS_INFO_1 version 7, login order, no ATTACHED_FG_MAPPING / TAG_FLOAT). The
// items whose U2 layout differs from U3's are written here:
//
// LIGHT_TABLE_1  PS3 header 0x40 (big endian) -> PS4 header 0x68 (little endian):
//     PS3 +00 ptr flags (bytes, up to the end of the item)     PS4 +00 ptr
//     PS3 +04..+20 eight u32 (+10 record count, +18 local      PS4 +08..+24, then +28 / +2c zero
//                  light count)
//     PS3 +24..+3c seven pointers                               PS4 +30..+60 (8-byte slots, same order)
//         +28 records (0x3c bytes each), +30 local lights (0xf0 each), the others up to the next block
//   PS4 order: header, 0xaa padding to 0x70, the flags, then the blocks in slot order, each 16-aligned. Blocks are
//   byte-swapped as u32 words.
// COLLISION_DATA_HAVOK_BG version 3: U2 keeps its older layout on PS4 (no Havok packfile), widened to 64-bit pointers,
//   every block inline in the item (header 0x70):
//     PS3 +00 version 3 -> PS4 +00 (+04 pad)       +04 per-subpart table A -> +08    +08 subpart pointers -> +10
//     +0c subpart count -> +18                    +10 word -> +1c                    +14 MOPP -> +20
//     +18 welding u16[] -> +28                    +1c welding count -> +30           +20 four flag bytes -> +34
//     +30 u32 per subpart (zero) -> +50, placed right after the header; +2c and +3c (stale PS3 pointers) -> null
//   A entry PS3 12 bytes {ptr material index u8[nTri], ptr materials, u32 n} -> PS4 0x18; a material {u32 a, u32 b} ->
//   {u32 a, f32 1.0, u32 b}. A subpart PS3 0x40 -> PS4 0x90: triangle count +10 -> +20, vertices +14 -> +28, vertex
//   count +1c -> +30, indices +30 -> +38, vertex stride +18 -> +40 (u16), triangle offset +3c -> +44, index stride +34 ->
//   +48 (u16), striding type +38 -> +4a (+1: the newer Havok inserted an 8-bit index type), flip +39 -> +4b, extrusion
//   +20 -> +50, and an identity transform at +60. Vertices are floats, indices u16 or u32, both byte-swapped. MOPP:
//   {4 floats, ptr code, u32 size} -> 0x20 bytes, the code copied. PS4 order: header, the u32 array, A, the subpart
//   pointers, per subpart (material index, materials, vertices, indices, the subpart), MOPP, its code, welding; blocks
//   16-aligned (0xaa), vertices, indices, MOPP code and welding 128-aligned, the item data 128-aligned in its page.
// WATER_DATA version 3: {3, n, ptr objects} -> the same with an 8-byte pointer; an object 0x70 -> 0x88 bytes: words 0..18
//   swapped (word 1, the material, becomes the string id of the PS4 material key), 0xaa pad, vertices +4c -> +50, indices
//   +50 -> +58, +54 -> +60, +58 -> null, +5c..+6c -> +70..+80, +84 zero. Then per object its vertices (40 bytes: three
//   floats and seven words, each word with its own byte order) and indices (u16, swapped), 16-aligned.
#include "ndpak.hpp"
#include "ps3bg.hpp"
#include "ps4stage.hpp"
#include "u2conv.hpp"

namespace u2imp {
namespace {

using u3::Blob;
using u3::Pak;
using BlobP = std::shared_ptr<Blob>;
const uint32_t AA = 0xaaaaaaaa;

uint32_t be(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
Bytes swap32(const uint8_t *p, size_t n) {   // n bytes as big-endian u32 -> little endian (whole words only)
    Bytes o;
    for (size_t i = 0; i + 4 <= n; i += 4)
        for (int k = 3; k >= 0; k--) o.push_back(p[i + size_t(k)]);
    return o;
}
Bytes swap16(const uint8_t *p, size_t n) {
    Bytes o;
    for (size_t i = 0; i + 2 <= n; i += 2) o.push_back(p[i + 1]), o.push_back(p[i]);
    return o;
}
void self_ptr(Blob &b, const BlobP &self, size_t target) {
    b.relocs.push_back({b.size(), self, int64_t(target)});
    b.u64(0);
}
void put32(Bytes &b, size_t at, uint32_t v) {
    for (int k = 0; k < 4; k++) b[at + size_t(k)] = uint8_t(v >> (8 * k));
}

// a reader of one PS3 struct (page, offset)
struct R {
    const Pak &p;
    uint32_t pg;
    uint64_t off;
    uint32_t u32(uint64_t o) const { return p.pu32(pg, off + o); }
    std::optional<std::pair<uint32_t, uint64_t>> ptr(uint64_t o) const { return p.resolve(pg, uint32_t(off + o)); }
    Bytes raw(uint64_t o, uint64_t n) const {
        const uint64_t b = std::min<uint64_t>(p.pbase(pg) + off + o, p.d.size()), e = std::min<uint64_t>(b + n, p.d.size());
        return Bytes(p.d.begin() + std::ptrdiff_t(b), p.d.begin() + std::ptrdiff_t(e));
    }
};
R at(const Pak &p, const std::pair<uint32_t, uint64_t> &r) { return R{p, r.first, r.second}; }

// ------------------------------------------------------------------------------------------------ LIGHT_TABLE_1
BlobP light_table(const Pak &p) {
    const auto its = p.items_of("LIGHT_TABLE_1");
    if (its.empty()) return nullptr;
    const auto &it = its[0];
    const uint32_t pg = it.page;
    const uint64_t b = it.data_off(), base = p.pbase(pg) + b;
    auto u = [&](uint64_t o) { return p.pu32(pg, b + o); };
    std::map<uint64_t, uint64_t> ptrs;   // header offset -> block offset in the item
    for (uint64_t o = 0; o < 0x40; o += 4) {
        if (o > 0 && o < 0x24) continue;
        if (const auto r = p.resolve(pg, uint32_t(b + o))) {
            if (r->first != pg) throw std::runtime_error("ValueError: LIGHT_TABLE_1 block on another page");
            ptrs[o] = r->second - b;
        }
    }
    std::set<uint64_t> ends = {it.size};
    for (const auto &[o, t] : ptrs) ends.insert(t);
    auto block = [&](uint64_t o) {
        const uint64_t t = ptrs.at(o);
        const auto nx = ends.upper_bound(t);
        if (nx == ends.end()) throw std::runtime_error("IndexError: list index out of range");
        const uint64_t n = *nx - t;
        return Bytes(p.d.begin() + std::ptrdiff_t(base + t), p.d.begin() + std::ptrdiff_t(std::min<uint64_t>(base + t + n, p.d.size())));
    };
    std::map<uint64_t, Bytes> parts;   // PS4 header slot -> block
    for (const auto &[o3, t] : ptrs) {
        if (!o3) continue;
        Bytes d = block(o3);
        const uint64_t o4 = 0x30 + (o3 - 0x24) * 2;
        if (o3 == 0x28) d.resize(std::min<size_t>(d.size(), 0x3cull * u(0x10)));
        else if (o3 == 0x30) d.resize(std::min<size_t>(d.size(), 0xf0ull * u(0x18)));
        parts[o4] = swap32(d.data(), d.size());
    }
    const Bytes flags = ptrs.count(0) ? block(0) : Bytes();
    auto lb = std::make_shared<Blob>("lighttable");
    if (ptrs.count(0)) self_ptr(*lb, lb, 0x70);
    else lb->u64(0);
    for (uint64_t o = 4; o < 0x24; o += 4) lb->u32(u(o));
    lb->u32(0).u32(0);
    size_t off = 0x70 + ((flags.size() + 15) & ~size_t(15));
    std::map<uint64_t, size_t> pos;
    for (const auto &[o4, d] : parts) {
        pos[o4] = off;
        off = (off + d.size() + 15) & ~size_t(15);
    }
    for (uint64_t o4 = 0x30; o4 < 0x68; o4 += 8) {
        if (pos.count(o4)) self_ptr(*lb, lb, pos[o4]);
        else lb->u64(0);
    }
    if (lb->size() != 0x68) throw std::runtime_error("AssertionError: light table header");
    while (lb->size() < 0x70) lb->u8(0xaa);
    lb->raw(flags);
    while (lb->size() % 16) lb->u8(0xaa);
    for (const auto &[o4, d] : parts) {
        while (lb->size() < pos[o4]) lb->u8(0xaa);
        lb->raw(d);
    }
    return lb;
}

// ------------------------------------------------------------------------------------------------ collision
struct Sub {
    uint32_t ntri, nv, vstride, istride, tri_off, nmat;
    uint8_t stype, flip;
    Bytes extr;
    std::optional<Bytes> verts, idx, mi;
    std::optional<std::vector<std::pair<uint32_t, uint32_t>>> mats;
};
struct Coll {
    uint32_t w10, nweld;
    Bytes flags, zarr;
    std::vector<Sub> subs;
    bool has_mopp = false;
    Bytes mopp_off;
    uint32_t mopp_size = 0;
    std::optional<Bytes> mopp_code, weld;
};

Coll read_coll(const Pak &p, const u3::PakItem &it) {
    const R h{p, it.page, it.data_off()};
    const uint32_t ver = h.u32(0);
    if (ver != 3) throw Unsupported(fmt("COLLISION_DATA_HAVOK_BG version %u (U2 is 3)", ver));
    for (uint64_t o : {0x24, 0x28, 0x34, 0x38})
        if (h.u32(o) && !h.ptr(o)) throw Unsupported(fmt("collision header +%#llx = %#x", (unsigned long long)o, h.u32(o)));
    const uint32_t n = h.u32(0xc);
    Coll c;
    c.w10 = h.u32(0x10);
    c.flags = h.raw(0x20, 4);
    c.nweld = h.u32(0x1c);
    const auto z = h.ptr(0x30);
    if (z) {
        const Bytes zr = at(p, *z).raw(0, 4ull * n);
        c.zarr = swap32(zr.data(), zr.size());
    } else
        c.zarr.assign(4ull * n, 0);
    const auto A = h.ptr(4), S = h.ptr(8);
    if (n && (!A || !S)) throw Unsupported("collision without subpart tables");
    for (uint32_t k = 0; k < n; k++) {
        const R a{p, A->first, A->second + 12ull * k};
        const auto sp = R{p, S->first, S->second + 4ull * k}.ptr(0);
        if (!sp) throw std::runtime_error("TypeError: collision subpart pointer");
        const R s = at(p, *sp);
        const Bytes b0 = s.raw(0, 0x10);
        if (std::any_of(b0.begin(), b0.end(), [](uint8_t x) { return x != 0; })) {
            std::string hx;
            for (uint8_t x : b0) hx += fmt("%02x", x);
            throw Unsupported("subpart base fields set (" + hx + ")");
        }
        Sub su;
        su.ntri = s.u32(0x10);
        su.nv = s.u32(0x1c);
        su.vstride = s.u32(0x18);
        su.istride = s.u32(0x34);
        su.stype = s.raw(0x38, 1).at(0);
        su.flip = s.raw(0x39, 1).at(0);
        const Bytes x3a = s.raw(0x3a, 2);
        if (x3a != Bytes{0, 0} || (su.stype != 1 && su.stype != 2) || su.vstride < 12)
            throw Unsupported(fmt("subpart striding %u/%u type %u", su.vstride, su.istride, su.stype));
        const uint32_t isz = su.stype == 1 ? 2 : 4;
        const auto vr = s.ptr(0x14), ir = s.ptr(0x30);
        if (vr) {
            const Bytes v = at(p, *vr).raw(0, uint64_t(su.vstride) * su.nv);
            su.verts = swap32(v.data(), v.size());
        }
        if (ir) {
            const Bytes raw = su.ntri ? at(p, *ir).raw(0, uint64_t(su.istride) * (su.ntri - 1) + 3ull * isz) : Bytes();
            su.idx = isz == 2 ? swap16(raw.data(), raw.size()) : swap32(raw.data(), raw.size());
        }
        su.nmat = a.u32(8);
        const auto xr = a.ptr(0), yr = a.ptr(4);
        if (xr) su.mi = at(p, *xr).raw(0, su.ntri);
        if (yr) {
            const R m = at(p, *yr);
            su.mats.emplace();
            for (uint32_t j = 0; j < su.nmat; j++) su.mats->push_back({m.u32(8ull * j), m.u32(8ull * j + 4)});
        }
        su.tri_off = s.u32(0x3c);
        const Bytes e = s.raw(0x20, 0x10);
        su.extr = swap32(e.data(), e.size());
        c.subs.push_back(std::move(su));
    }
    if (const auto mr = h.ptr(0x14)) {
        const R m = at(p, *mr);
        c.has_mopp = true;
        c.mopp_size = m.u32(0x14);
        const Bytes o = m.raw(0, 0x10);
        c.mopp_off = swap32(o.data(), o.size());
        if (const auto cr = m.ptr(0x10)) {
            Bytes code = at(p, *cr).raw(0, (uint64_t(c.mopp_size) + 3) / 4 * 4);
            if (code.size() > c.mopp_size) code.resize(c.mopp_size);
            c.mopp_code = code;
        }
    }
    const auto wr = h.ptr(0x18);
    if (wr && c.nweld) {
        const Bytes w = at(p, *wr).raw(0, 2ull * c.nweld);
        c.weld = swap16(w.data(), w.size());
    }
    return c;
}

BlobP coll_blob(const Coll &c) {
    auto B = std::make_shared<Blob>("u2coll");
    auto pad = [&](size_t a = 16) { B->pad(a, 0xaa); };
    auto ptr_at = [&](size_t pos, size_t off) { B->relocs.push_back({pos, B, int64_t(off)}); };
    const size_t n = c.subs.size();
    B->u32(3).u32(AA);
    B->raw(Bytes(0x68, 0));
    put32(B->b, 0x18, uint32_t(n));
    put32(B->b, 0x1c, c.w10);
    put32(B->b, 0x30, c.nweld);
    std::copy(c.flags.begin(), c.flags.end(), B->b.begin() + 0x34);
    ptr_at(0x50, B->size());
    B->raw(c.zarr);
    pad();
    const size_t a_pos = B->size();
    if (n) ptr_at(0x08, a_pos);
    B->raw(Bytes(0x18 * n, 0));
    for (size_t k = 0; k < n; k++) {
        put32(B->b, a_pos + 0x18 * k + 0x10, c.subs[k].nmat);
        put32(B->b, a_pos + 0x18 * k + 0x14, AA);
    }
    pad();
    const size_t s_pos = B->size();
    if (n) ptr_at(0x10, s_pos);
    B->raw(Bytes(8 * n, 0));
    pad();
    for (size_t k = 0; k < n; k++) {
        const Sub &s = c.subs[k];
        const size_t ap = a_pos + 0x18 * k;
        if (s.mi) {
            ptr_at(ap, B->size());
            B->raw(*s.mi);
            pad();
        }
        if (s.mats) {
            ptr_at(ap + 8, B->size());
            for (const auto &[a, b] : *s.mats) B->u32(a).u32(0x3f800000).u32(b);
            pad();
        }
        std::optional<size_t> vp, ip;
        if (s.verts) {
            pad(0x80);
            vp = B->size();
            B->raw(*s.verts);
        }
        if (s.idx) {
            pad(0x80);
            ip = B->size();
            B->raw(*s.idx);
        }
        pad();
        ptr_at(s_pos + 8 * k, B->size());
        const size_t o = B->size();
        B->raw(Bytes(0x90, 0));
        put32(B->b, o + 0x20, s.ntri);
        put32(B->b, o + 0x24, AA);
        put32(B->b, o + 0x30, s.nv);
        put32(B->b, o + 0x34, AA);
        if (vp) ptr_at(o + 0x28, *vp);
        if (ip) ptr_at(o + 0x38, *ip);
        B->b[o + 0x40] = uint8_t(s.vstride);
        B->b[o + 0x41] = uint8_t(s.vstride >> 8);
        put32(B->b, o + 0x44, s.tri_off);
        B->b[o + 0x48] = uint8_t(s.istride);
        B->b[o + 0x49] = uint8_t(s.istride >> 8);
        B->b[o + 0x4a] = uint8_t(s.stype + 1);
        B->b[o + 0x4b] = s.flip;
        put32(B->b, o + 0x4c, AA);
        std::copy(s.extr.begin(), s.extr.end(), B->b.begin() + std::ptrdiff_t(o + 0x50));
        put32(B->b, o + 0x7c, 0x3f800000);   // translation 0 (+60), rotation (0, 0, 0, 1) (+70)
        for (size_t i = 0; i < 4; i++) put32(B->b, o + 0x80 + 4 * i, 0x3f800000);   // scale 1 (+80)
    }
    if (c.has_mopp) {
        pad();
        ptr_at(0x20, B->size());
        const size_t mo = B->size();
        B->raw(c.mopp_off).u64(0).u32(c.mopp_size).u32(AA);
        if (c.mopp_code) {
            pad(0x80);
            ptr_at(mo + 0x10, B->size());
            B->raw(*c.mopp_code);
        }
    }
    if (c.weld) {
        pad(0x80);
        ptr_at(0x28, B->size());
        B->raw(*c.weld);
    }
    return B;
}

std::vector<BlobP> collision(const Pak &p) {
    std::vector<BlobP> v;
    for (const auto &it : p.items_of("COLLISION_DATA_HAVOK_BG")) v.push_back(coll_blob(read_coll(p, it)));
    return v;
}

// ------------------------------------------------------------------------------------------------ water
// the byte order of each vertex word: PS4 byte i is PS3 byte ORDER[word][i]
const char *VORDER[10] = {"3210", "3210", "3210", "1032", "1032", "1032", "1023", "0132", "1032", "1032"};

BlobP water(const Pak &p, const std::function<std::string(const std::string &)> &matkey) {
    const auto obs = u3::bg::water_objects(p);
    if (obs.empty()) return nullptr;
    auto B = std::make_shared<Blob>("u2water");
    B->u32(3).u32(uint32_t(obs.size()));
    self_ptr(*B, B, 0x10);
    B->pad(16, 0xaa);
    struct Obj {
        Bytes out;
        std::optional<Bytes> verts, idx;
    };
    std::vector<Obj> body;
    for (const auto &[ref, mname] : obs) {
        const uint32_t pg = ref.first;
        const uint64_t off = ref.second, base = p.pbase(pg) + off;
        if (base + 0x70 > p.d.size()) throw std::runtime_error("struct.error: water object");
        const uint8_t *q = p.d.data() + base;
        auto u = [&](uint64_t o) { return be(q + o); };
        Obj ob;
        ob.out = swap32(q, 19 * 4);
        if (mname) put32(ob.out, 4, u3::sid(matkey(*mname)));
        for (uint32_t v : {AA, 0u, 0u, 0u, 0u}) ob.out.insert(ob.out.end(), {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)});
        for (uint32_t v : {u(0x54), AA, 0u, 0u, u(0x5c), u(0x60), u(0x64), u(0x68), u(0x6c), 0u})
            ob.out.insert(ob.out.end(), {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)});
        if (ob.out.size() != 0x88) throw std::runtime_error("AssertionError: water object size");
        const uint32_t nv = u(0x44), nt = u(0x48);
        const auto rv = p.resolve(pg, uint32_t(off + 0x4c)), ri = p.resolve(pg, uint32_t(off + 0x50));
        if (rv) {
            ob.verts.emplace();
            if (nv) {
                const uint64_t vb = p.pbase(rv->first) + rv->second;
                for (uint32_t k = 0; k < nv; k++)
                    for (int j = 0; j < 10; j++)
                        for (int i = 0; i < 4; i++) {
                            const uint64_t a = vb + 40ull * k + 4ull * size_t(j) + uint64_t(VORDER[j][i] - '0');
                            if (a >= p.d.size()) throw std::runtime_error("IndexError: water vertices");
                            ob.verts->push_back(p.d[a]);
                        }
            }
        }
        if (ri) {
            ob.idx.emplace();
            if (nt) {
                const uint64_t ib = p.pbase(ri->first) + ri->second;
                if (ib + 6ull * nt > p.d.size()) throw std::runtime_error("struct.error: water indices");
                *ob.idx = swap16(p.d.data() + ib, 6ull * nt);
            }
        }
        body.push_back(std::move(ob));
    }
    const size_t objstart = B->size();
    for (const Obj &ob : body) B->raw(ob.out);
    for (size_t k = 0; k < body.size(); k++) {
        const size_t o = objstart + 0x88 * k;
        if (body[k].verts) {
            B->pad(16, 0xaa);
            B->relocs.push_back({o + 0x50, B, int64_t(B->size())});
            B->raw(*body[k].verts);
        }
        if (body[k].idx) {
            B->pad(16, 0xaa);
            B->relocs.push_back({o + 0x58, B, int64_t(B->size())});
            B->raw(*body[k].idx);
        }
    }
    return B;
}

}  // namespace

// the retail PS4 material survey the planner reads, made once per PS4 game (work/res; a few minutes)
fs::path stage_res(const Context &c) {
    const fs::path d = c.work / "res";
    const fs::path stamp = d / "ok.json";
    size_t n = 0;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(c.ps4->pak_dir, ec)) (void)e, n++;
    const std::string want = dumps(obj({{"dir", str(S(fs::absolute(c.ps4->pak_dir)))}, {"n", num((long long)n)}}));
    if (fs::is_regular_file(stamp, ec) && read_text(stamp) == want && fs::is_regular_file(d / "ps4tex.tsv", ec)) return d;
    fs::create_directories(d, ec);
    log("level survey: reading the retail PS4 level paks (materials, textures) ...");
    u3::stage::make_res(d, c.ps4->pak_dir);
    write_text(stamp, want);
    return d;
}

std::vector<std::string> convert_stage(const Context &c, const Pak &p, const fs::path &src, const fs::path &out) {
    (void)p;
    u3::stage::Options o;
    o.res = stage_res(c);
    o.pak_dir = c.ps4->pak_dir;
    o.plan_dir = c.work / "levels";
    o.light_table = light_table;
    o.collision = collision;
    o.water = water;
    const Bytes data = u3::stage::stage_convert(src, o);
    const fs::path tmp = S(out) + ".tmp";
    write_bytes(tmp, data);
    std::error_code ec;
    if (const auto errs = stage_check(tmp); !errs.empty()) {
        fs::remove(tmp, ec);
        std::string s;
        for (size_t i = 0; i < errs.size() && i < 4; i++) s += (i ? "; " : "") + errs[i];
        throw std::runtime_error("RuntimeError: stagecheck: " + s);
    }
    fs::rename(tmp, out, ec);
    if (ec) throw std::runtime_error("cannot write " + S(out) + ": " + ec.message());
    return {};
}

}  // namespace u2imp
