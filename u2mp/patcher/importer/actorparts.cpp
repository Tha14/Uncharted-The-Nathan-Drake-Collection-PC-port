// importer/actorparts.cpp: the pieces of a Uncharted 2 actor pak around its geometry, PS3 -> PS4.
//
// ARTGROUP_DATA (the 1124 PS3/PS4 actor27 pairs: 1109 rebuilt byte for byte, the rest are character bases):
//   PS3 {u32 n, u32 1, ptr entries {ptr item, 0}[n], ptr H, ptr B, ...}; H {0, 1, 0x12345, ptr X};
//       X {f32 -1, f32 1, 0, 0, u32 nb, 0, 0, buckets {u32 key, ptr entry}[nb] inline}
//       B {ptr name, u32 cnt, ptr L}; L {u32 sid, u32 m, ptr M}[cnt]; M {u32 sid, f32, u32 k, ptr u32[2 (k - 1)]}[m]
//   PS4 {u32 n, u32 1, ptr entries (null when n = 0), ptr H, ptr B, u64 0, u32 0, pad} (0x30);
//       H {u64 0, u32 1, u32 0x12345, ptr X, pad pad}; entries {ptr item header, u64 0}[n];
//       B {ptr name, u32 cnt, pad, ptr L, pad pad}, the name (16-aligned), L {u32 sid, u32 m, ptr M}[cnt], per L entry
//       M {u32 sid, f32, u32 k - 1, pad, ptr data}[m] then each data (u32 words), X {0, pad, 0, 0, 0, nb}, buckets
//       {u32 key, pad, ptr entry}[nb] inline.
//   Bucket keys: id('geo.' + name) for FG sections, id(name) for the other entries, inserted in entry order at key % nb
//   with linear probing.
// SOUND_BANK_TABLE: PS3 {u32 n, ptr name[n]} -> PS4 {u32 n, pad, ptr name[n]} + the names.
// LEVEL_PATS_1: the PS3 u64 as little endian (retail keeps the PS3 value; U3 writes 0).
// PACKAGE_ITEM_LOOKUP: {ptr name, u32 0, u32 n, {u32 id, pad, ptr item header}[n] inline, name} (U3: a pointer to the
//   entries after the 16-aligned name).
// Login order (retail props): ARTGROUP_DATA / LEVEL_PATS_1 (the PS3 order), SOUND_BANK_TABLE, EFFECT_TABLE,
//   TEXTURE_TABLE, GEOMETRY_1, JOINT_HIERARCHY, ANIM*, FOREGROUND_SECTION_2*, COLLISION_DATA_FOREGROUND*,
//   COLLISION_HAVOK_JOINTED*, PACKAGE_ITEM_LOOKUP, TAG_*.
// ANIM: PS3 (0x1c) {u32 6, u32 0, ptr clip, ptr chans, u32 0, u32 skeleton, ptr events}; PS4 (0x30) {u32 6, u32 0, ptr
//   clip, ptr chans, u64 0, u32 skeleton, pad, ptr events}. The float channels PS3 keeps in ANIM_RAWCHANNELS are inside
//   the PS4 ANIM: chans {u32 n, f32 rate, ptr entry[n]}; entry PS3 {u32 sid, ptr D} -> PS4 {u32 sid, pad, ptr D}; D =
//   two keys (0x58 bytes of floats), ptr samples, u16 stride, u16, u16, u16 count (PS4 pointer 8 bytes; null when there
//   are no samples); samples count x stride bytes of floats. PS4 order: header, chans, per channel {entry, D, samples},
//   events, clip, every piece 16-aligned (0xaa). The clip is the U3 clip converter's (dctools anim).
// COLLISION_DATA_FOREGROUND (the 660 items of the pairs; U2 has none of the U3 extras): item {u32 0x1001, u32 n, parts**}
//   (PS3 0xc, the part pointers inline; PS4 0x10); part {transform f32[8], u32 joint (PS4 pad after), u64 0 (PS4 only),
//   hk*, 0} (0x30 -> 0x40); hk {u32 packfile size, u32 1, trailer* (PS3 +0x10, PS4 +0x18), packfile (PS3 +0x20, PS4
//   +0x30)}, PS4 +0x2c = the packfile's first word; trailer 3 x {ptr elements, u32 count} (elements 0x40 / 0x34 / 0x28
//   bytes of 32-bit words, u16 pairs at words 14-15 / 12 / 9) -> PS4 0x38. PS4 order: header, part pointers, per part
//   {part, hk + packfile, trailer, element arrays}, 16-aligned (0xaa).
// JOINT_HIERARCHY D block of props with a ragdoll / skeleton mapper: PS3 {3, zeros, +0x4c ptr W}, PS4 +0x90; W = {u32
//   packfile size, u32 1, zeros, packfile} (PS3 header 0x10; PS4 0x20 with the packfile's 8 magic bytes at +0x18).
// COLLISION_HAVOK_JOINTED: PS3 {u32 0x2001, 0, 0, 0, W} -> PS4 {u32 version, 0, 0, 0} + the PS4 W (the 3 retail pairs).
// The Havok packfiles go through havok.cpp (Havok 6.1 -> 2011.3).
#include "actor.hpp"

#include "anim.hpp"
#include "skel.hpp"

#include <cstring>

namespace u2imp {

using namespace u3;

namespace {

const uint32_t PAD32 = 0xaaaaaaaa;
const Bytes PF_MAGIC = {0x57, 0xe0, 0xe0, 0x57, 0x10, 0xc0, 0xc0, 0x10};
uint32_t sidl(const std::string &latin1) { return u3::sid(bg::utf8_of(latin1)); }
std::string u8s(const std::string &latin1) { return bg::utf8_of(latin1); }
bool fg_type(const std::string &t) { return t == "FOREGROUND_SECTION" || t == "FOREGROUND_SECTION_2"; }
Bytes slice(const Bytes &d, uint64_t a, uint64_t n) {
    a = std::min<uint64_t>(a, d.size());
    return Bytes(d.begin() + std::ptrdiff_t(a), d.begin() + std::ptrdiff_t(std::min<uint64_t>(a + n, d.size())));
}
Bytes sw4(const Bytes &b) {   // u32 words byte-swapped, a partial word at the end kept
    Bytes o;
    const size_t n = b.size() - b.size() % 4;
    for (size_t i = 0; i < n; i += 4)
        for (int k = 3; k >= 0; k--) o.push_back(b[i + size_t(k)]);
    o.insert(o.end(), b.begin() + std::ptrdiff_t(n), b.end());
    return o;
}
// u32 words byte-swapped, except the given words that are two u16 (each half swapped)
Bytes swap_words(const Bytes &b, const std::set<size_t> &pairs = {}) {
    Bytes out;
    for (size_t i = 0; i < b.size(); i += 4) {
        const size_t e = std::min(i + 4, b.size());
        Bytes w(b.begin() + std::ptrdiff_t(i), b.begin() + std::ptrdiff_t(e));
        if (pairs.count(i / 4)) {   // w[1::-1] + w[3:1:-1]
            for (int j = std::min<int>(1, int(w.size()) - 1); j >= 0; j--) out.push_back(w[size_t(j)]);
            for (int j = int(w.size()) - 1; j >= 2; j--) out.push_back(w[size_t(j)]);
        } else
            for (size_t j = w.size(); j > 0; j--) out.push_back(w[j - 1]);
    }
    return out;
}
void put32(Blob &b, size_t at, uint32_t v) { wr32le(&b.b.at(at), v); }
void self_ptr(Blob &b, const BlobP &self, size_t target) {
    b.relocs.push_back({b.size(), self, int64_t(target)});
    b.u64(0);
}
void reloc(Blob &b, const BlobP &self, size_t pos, size_t target) { b.relocs.push_back({pos, self, int64_t(target)}); }
std::string hexs(const Bytes &b) {
    std::string s;
    for (uint8_t x : b) s += fmt("%02x", x);
    return s;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ ARTGROUP_DATA
ArtgroupU2 read_artgroup(const Pak &p) {
    const auto ags = p.items_of("ARTGROUP_DATA");
    if (ags.empty()) throw Stop("IndexError: no ARTGROUP_DATA");
    const PakItem &it = ags[0];
    const uint32_t pg = it.page;
    const uint64_t b = it.data_off();
    ArtgroupU2 a;
    a.name = it.name;
    const uint32_t n = p.pu32(pg, b);
    if (p.pu32(pg, b + 4) != 1) throw Unsupported("ARTGROUP_DATA +4 != 1");
    const ORef ents = bg::res(p, pg, b + 8);
    for (uint32_t k = 0; k < n; k++) {
        const ORef r = bg::res(p, bg::need(ents).first, bg::need(ents).second + 8ull * k);
        a.targets.push_back(r ? std::optional<PakItem>(p.item_at(r->first, uint32_t(r->second))) : std::nullopt);
    }
    const Ref h2 = bg::need(p, pg, b + 0xc);
    const Ref x = bg::need(p, h2.first, h2.second + 0xc);
    a.nb = p.pu32(x.first, x.second + 0x10);
    if (const ORef bb = bg::res(p, pg, b + 0x10)) {
        const Ref lr = bg::need(p, bb->first, bb->second + 8);
        const uint32_t cnt = p.pu32(bb->first, bb->second + 4);
        for (uint32_t i = 0; i < cnt; i++) {
            const uint32_t s = p.pu32(lr.first, lr.second + 12ull * i), m = p.pu32(lr.first, lr.second + 12ull * i + 4);
            const ORef mr = bg::res(p, lr.first, lr.second + 12ull * i + 8);
            std::vector<ArtgroupU2::M> ms;
            for (uint32_t j = 0; j < m; j++) {
                const uint64_t o = p.pbase(bg::need(mr).first) + mr->second + 16ull * j;
                const uint32_t s2 = be32(p.d, o), k = be32(p.d, o + 8);
                const uint32_t k4 = k > 1 ? k - 1 : 0;
                const ORef dr = bg::res(p, mr->first, mr->second + 16ull * j + 12);
                Bytes d;
                if (dr && k4) d = slice(p.d, p.pbase(dr->first) + dr->second, 8ull * k4);
                Bytes f = slice(p.d, o + 4, 4);
                std::reverse(f.begin(), f.end());
                ms.push_back({s2, f, k4, sw4(Bytes(d.begin(), d.begin() + std::ptrdiff_t(d.size() - d.size() % 4)))});
                // (Python joined d[q:q+4][::-1] over every 4 bytes: a partial last word is reversed too)
                if (d.size() % 4) {
                    Bytes t(d.end() - std::ptrdiff_t(d.size() % 4), d.end());
                    std::reverse(t.begin(), t.end());
                    ms.back().data.insert(ms.back().data.end(), t.begin(), t.end());
                }
            }
            a.lists.push_back({s, ms});
        }
    }
    return a;
}

std::vector<std::optional<uint32_t>> keys_for(const std::vector<std::optional<PakItem>> &targets) {
    std::vector<std::optional<uint32_t>> k;
    for (const auto &t : targets)
        k.push_back(t ? std::optional<uint32_t>(sidl(fg_type(t->type) ? "geo." + t->name : t->name)) : std::nullopt);
    return k;
}

BlobP build_artgroup(const std::string &name, const std::vector<ItemP> &entries, const std::vector<std::optional<uint32_t>> &keys, uint32_t nb,
                     const std::vector<std::pair<uint32_t, std::vector<ArtgroupU2::M>>> &lists) {
    const size_t n = entries.size();
    std::vector<std::pair<uint32_t, std::optional<size_t>>> bk(nb, {0, std::nullopt});
    std::vector<bool> used(nb, false);
    for (size_t k = 0; k < keys.size(); k++) {
        if (!keys[k]) continue;
        if (!nb) throw Stop("ZeroDivisionError: integer modulo by zero");
        size_t i = *keys[k] % nb;
        while (used[i]) i = (i + 1) % nb;
        used[i] = true;
        bk[i] = {*keys[k], k};
    }
    auto ab = std::make_shared<Blob>("artgroup");
    std::vector<std::pair<size_t, size_t>> fix;
    const size_t ents = 0x50, nameblk = 0x50 + 16 * n;
    ab->u32(n).u32(1).u64(0);
    self_ptr(*ab, ab, 0x30);
    self_ptr(*ab, ab, nameblk);
    ab->u64(0).u32(0).u32(PAD32);
    if (n) fix.push_back({8, ents});
    ab->u64(0).u32(1).u32(0x12345).u64(0).u32(PAD32).u32(PAD32);
    for (const ItemP &e : entries) {
        if (e) ab->ptr(e->data, -0x20).u64(0);
        else ab->u64(0).u64(0);
    }
    ab->u64(0).u32(lists.size()).u32(PAD32).u64(0).u32(PAD32).u32(PAD32);
    fix.push_back({nameblk, ab->size()});
    ab->cstr(u8s(name));
    ab->pad(16, 0xaa);
    if (!lists.empty()) {
        fix.push_back({nameblk + 0x10, ab->size()});
        const size_t lo = ab->size();
        for (const auto &[s, ms] : lists) ab->u32(s).u32(ms.size()).u64(0);
        for (size_t i = 0; i < lists.size(); i++) {
            ab->pad(16, 0xaa);
            fix.push_back({lo + 16 * i + 8, ab->size()});
            const size_t mo = ab->size();
            for (const auto &m : lists[i].second) ab->u32(m.sid).raw(m.f).u32(m.k4).u32(PAD32).u64(0);
            for (size_t j = 0; j < lists[i].second.size(); j++) {
                const auto &m = lists[i].second[j];
                if (!m.k4) continue;
                ab->pad(16, 0xaa);
                fix.push_back({mo + 24 * j + 16, ab->size()});
                ab->raw(m.data);
            }
        }
        ab->pad(16, 0xaa);
    }
    const size_t x = ab->size();
    fix.push_back({0x40, x});
    ab->u32(0).u32(PAD32).u64(0).u64(0).u32(0).u32(nb);
    for (const auto &[key, ei] : bk) {
        ab->u32(key).u32(PAD32);
        if (!ei) ab->u64(0);
        else self_ptr(*ab, ab, ents + 16 * *ei);
    }
    for (const auto &[pos, t] : fix) reloc(*ab, ab, pos, t);
    return ab;
}

std::vector<ItemP> artgroup_entries(const std::vector<std::optional<PakItem>> &targets, const Made &made) {
    std::vector<ItemP> v;
    for (const auto &t : targets) {
        if (!t) {
            v.push_back(nullptr);
            continue;
        }
        const auto it = made.find({t->type, t->name});
        v.push_back(it == made.end() ? nullptr : it->second);
    }
    return v;
}

Bytes level_pats(const Pak &src) {
    const auto it = src.items_of("LEVEL_PATS_1");
    if (it.empty()) return Bytes(8, 0);
    Bytes d = slice(src.d, src.pbase(it[0].page) + it[0].data_off(), 8);
    std::reverse(d.begin(), d.end());
    return d;
}

BlobP lookup_blob(const std::string &art, const ItemP &artgroup) {
    auto pl = std::make_shared<Blob>("pkglookup");
    self_ptr(*pl, pl, 0x20);
    pl->u32(0).u32(1);
    pl->u32(sidl(art)).u32(PAD32).ptr(artgroup->data, -0x20);
    pl->cstr(u8s(art));
    return pl;
}

std::string proto_name(size_t pi, const std::string &art) {
    std::string a = art;
    std::replace(a.begin(), a.end(), '-', '_');
    return "Prototype" + std::to_string(pi) + "_" + a;
}
std::string submesh_name(const std::string &art, size_t pi, uint32_t first) {
    std::string a = art;
    std::replace(a.begin(), a.end(), '-', '_');
    return a + "_Prototype" + std::to_string(pi) + "_Submesh" + std::to_string(first);
}

std::vector<ItemP> login_order(const Pak &src, std::map<std::string, std::vector<ItemP>> by, const std::vector<ItemP> &tags) {
    std::vector<std::string> first;
    for (const auto &[pg, off] : src.login) {
        const std::string t = src.item_at(pg, off).type;
        if ((t == "ARTGROUP_DATA" || t == "LEVEL_PATS_1") && std::find(first.begin(), first.end(), t) == first.end()) first.push_back(t);
    }
    for (const char *t : {"ARTGROUP_DATA", "LEVEL_PATS_1"})
        if (std::find(first.begin(), first.end(), t) == first.end()) first.push_back(t);
    for (const char *t : {"SOUND_BANK_TABLE", "EFFECT_TABLE", "TEXTURE_TABLE", "GEOMETRY_1", "JOINT_HIERARCHY", "ANIM", "FOREGROUND_SECTION_2",
                          "COLLISION_DATA_FOREGROUND", "COLLISION_HAVOK_JOINTED", "PACKAGE_ITEM_LOOKUP"})
        first.push_back(t);
    std::vector<ItemP> out;
    for (const std::string &t : first)
        if (auto it = by.find(t); it != by.end()) out.insert(out.end(), it->second.begin(), it->second.end());
    out.insert(out.end(), tags.begin(), tags.end());
    return out;
}

// ------------------------------------------------------------------------------------------------ skeleton
std::optional<Bytes> jh_d_packfile(const Pak &p, const PakItem &it) {
    const ORef d = bg::res(p, it.page, it.data_off() + 0x18);
    if (!d) return std::nullopt;
    const uint64_t base = p.pbase(d->first) + d->second;
    if (be32(p.d, base) != 3) throw Unsupported(fmt("JOINT_HIERARCHY D block version %u", be32(p.d, base)));
    for (uint64_t o = 4; o < 0x5c; o += 4)
        if (o != 0x4c && be32(p.d, base + o)) throw Unsupported(fmt("JOINT_HIERARCHY D block: non-zero word at +%#llx", (unsigned long long)o));
    const ORef wr = bg::res(p, d->first, d->second + 0x4c);
    if (!wr) return std::nullopt;
    const uint64_t wb = p.pbase(wr->first) + wr->second;
    const uint32_t size = be32(p.d, wb), one = be32(p.d, wb + 4), z0 = be32(p.d, wb + 8), z1 = be32(p.d, wb + 12);
    if (one != 1 || z0 || z1 || slice(p.d, wb + 0x10, 8) != PF_MAGIC) throw Unsupported("JOINT_HIERARCHY D block packfile wrapper " + hexs(slice(p.d, wb, 0x18)));
    return slice(p.d, wb + 0x10, size);
}
Bytes d_wrapper(const Bytes &pf4) {
    Bytes o(8, 0);
    wr32le(&o[0], uint32_t(pf4.size()));
    wr32le(&o[4], 1);
    o.insert(o.end(), 16, 0);
    o.insert(o.end(), pf4.begin(), pf4.begin() + std::ptrdiff_t(std::min<size_t>(8, pf4.size())));
    o.insert(o.end(), pf4.begin(), pf4.end());
    return o;
}
namespace {
skel::JH read_jh_u2(const Pak &p, const PakItem &it) {
    skel::d_packfile = jh_d_packfile;
    return skel::read_jh(p, it);
}
}  // namespace
JhInfo jh_info(const Pak &p, const PakItem &it) {
    const skel::JH m = read_jh_u2(p, it);
    JhInfo j;
    j.nj = m.nj;
    j.hier = be32(m.blob, 4);
    for (size_t i = 0; i < m.nj && i < m.names.size(); i++) {
        uint8_t q[4];
        wr32le(q, m.names[i].first);
        j.names.insert(j.names.end(), q, q + 4);
    }
    if (m.names.size() < m.nj) throw Stop("struct.error: pack expected more joint ids");
    return j;
}
BlobP jh_blob(const Pak &p, const PakItem &it) {
    skel::JH m = read_jh_u2(p, it);
    if (m.pf) m.pf4 = d_wrapper(havok_convert(*m.pf));
    return skel::build_jh(m);
}

// ------------------------------------------------------------------------------------------------ ANIM
namespace {
struct AnimU2 {
    std::string name;
    uint32_t w0, w1, skel;
    std::optional<Bytes> clip;
    struct Chan {
        uint32_t sid;
        Bytes keys;
        uint16_t u16[4];
        Bytes smp;
    };
    std::optional<std::pair<Bytes, std::vector<Chan>>> chans;
    struct Ev {
        uint32_t s;
        float f;
        uint32_t c;
        std::optional<std::vector<std::pair<uint32_t, uint32_t>>> kv;
    };
    std::optional<std::tuple<uint32_t, uint32_t, std::vector<Ev>>> events;
};
AnimU2 read_anim(const Pak &p, const PakItem &it) {
    const uint32_t pg = it.page;
    const uint64_t b = it.data_off(), base = p.pbase(pg) + b;
    uint32_t w[7];
    for (int k = 0; k < 7; k++) w[k] = be32(p.d, base + 4ull * size_t(k));
    if (w[0] != 6 || w[4] != 0) throw std::runtime_error("ValueError: " + it.name + ": unexpected ANIM header");
    AnimU2 a{it.name, w[0], w[1], w[5], std::nullopt, std::nullopt, std::nullopt};
    if (const ORef r = bg::res(p, pg, b + 8)) {
        const uint64_t cb = p.pbase(r->first) + r->second;
        a.clip = slice(p.d, cb, be32(p.d, cb + 8));
    }
    if (const ORef r = bg::res(p, pg, b + 12)) {
        const uint32_t cp = r->first;
        const uint64_t co = r->second;
        const uint32_t n = p.pu32(cp, co);
        const Bytes rate = slice(p.d, p.pbase(cp) + co + 4, 4);
        std::vector<AnimU2::Chan> chans;
        for (uint32_t i = 0; i < n; i++) {
            const Ref e = bg::need(p, cp, co + 8 + 4ull * i);
            AnimU2::Chan c{};
            c.sid = p.pu32(e.first, e.second);
            const Ref d = bg::need(p, e.first, e.second + 4);
            const uint64_t db = p.pbase(d.first) + d.second;
            c.keys = slice(p.d, db, 0x58);
            for (int k = 0; k < 4; k++) c.u16[k] = uint16_t(p.d.at(db + 0x5c + 2 * size_t(k)) << 8 | p.d.at(db + 0x5d + 2 * size_t(k)));
            const uint64_t nb = uint64_t(c.u16[0]) * c.u16[3];
            if (nb) {
                const Ref sr = bg::need(p, d.first, d.second + 0x58);
                c.smp = slice(p.d, p.pbase(sr.first) + sr.second, nb);
            }
            chans.push_back(std::move(c));
        }
        a.chans = std::make_pair(rate, chans);
    }
    if (bg::res(p, pg, b + 24)) {   // the U3 event list layout
        const Ref er0 = bg::need(p, pg, b + 24);
        const uint32_t tsid = p.pu32(er0.first, er0.second), cnt = p.pu32(er0.first, er0.second + 4);
        std::vector<AnimU2::Ev> ents;
        const ORef er = bg::res(p, er0.first, er0.second + 8);
        for (uint32_t i = 0; i < (er ? cnt : 0); i++) {
            const uint64_t e = er->second + 16ull * i;
            const uint64_t eb = p.pbase(er0.first) + e;
            AnimU2::Ev ev;
            ev.s = be32(p.d, eb);
            const uint32_t fb = be32(p.d, eb + 4);
            std::memcpy(&ev.f, &fb, 4);
            ev.c = be32(p.d, eb + 8);
            if (const ORef kvr = bg::res(p, er0.first, e + 12)) {
                ev.kv.emplace();
                for (uint32_t j = 0; j < ev.c; j++)
                    ev.kv->push_back({be32(p.d, p.pbase(er0.first) + kvr->second + 8ull * j), be32(p.d, p.pbase(er0.first) + kvr->second + 8ull * j + 4)});
            }
            ents.push_back(std::move(ev));
        }
        a.events = std::make_tuple(tsid, cnt, ents);
    }
    return a;
}
BlobP build_anim(const AnimU2 &a) {
    auto B = std::make_shared<Blob>("anim:" + a.name);
    const std::optional<Bytes> clip4 = a.clip ? std::optional<Bytes>(anim::convert_clip(*a.clip)) : std::nullopt;
    const size_t hdr = B->size();
    B->u32(a.w0).u32(a.w1).u64(0).u64(0).u64(0).u32(a.skel).u32(PAD32).u64(0);
    std::vector<std::pair<size_t, size_t>> fix;
    if (a.chans) {
        const auto &[rate, chans] = *a.chans;
        B->pad(16, 0xaa);
        const size_t ct = B->size();
        fix.push_back({hdr + 0x10, ct});
        B->u32(chans.size()).raw(sw4(rate));
        const size_t tab = B->size();
        B->raw(Bytes(8 * chans.size(), 0));
        for (size_t i = 0; i < chans.size(); i++) {
            const auto &c = chans[i];
            B->pad(16, 0xaa);
            fix.push_back({tab + 8 * i, B->size()});
            const size_t ent = B->size();
            B->u32(c.sid).u32(PAD32).u64(0);
            B->pad(16, 0xaa);
            fix.push_back({ent + 8, B->size()});
            const size_t d = B->size();
            B->raw(sw4(c.keys)).u64(0);
            for (uint16_t v : c.u16) B->u16(v);
            if (!c.smp.empty()) {
                B->pad(16, 0xaa);
                fix.push_back({d + 0x58, B->size()});
                B->raw(sw4(c.smp));
            }
        }
    }
    if (a.events) {
        const auto &[tsid, cnt, ents] = *a.events;
        B->pad(16, 0xaa);
        const size_t et = B->size();
        fix.push_back({hdr + 0x28, et});
        B->u32(tsid).u32(cnt).u64(0);
        if (!ents.empty()) {
            B->pad(16, 0xaa);
            const size_t ep = B->size();
            fix.push_back({et + 8, ep});
            for (const auto &e : ents) {
                uint32_t fb;
                std::memcpy(&fb, &e.f, 4);
                B->u32(e.s).u32(fb).u32(e.c).u32(PAD32).u64(0);
            }
            for (size_t i = 0; i < ents.size(); i++) {
                if (!ents[i].kv || ents[i].kv->empty()) continue;
                B->pad(16, 0xaa);
                fix.push_back({ep + 24 * i + 16, B->size()});
                for (const auto &[k, v] : *ents[i].kv) B->u32(k).u32(v);
            }
        }
    }
    if (clip4) {
        B->pad(16, 0xaa);
        fix.push_back({hdr + 8, B->size()});
        B->raw(*clip4);
    }
    for (const auto &[pos, t] : fix) reloc(*B, B, pos, t);
    return B;
}
}  // namespace
BlobP anim_blob(const Pak &p, const PakItem &it) { return build_anim(read_anim(p, it)); }

// ------------------------------------------------------------------------------------------------ foreground collision
namespace {
const std::pair<size_t, std::set<size_t>> ELEM[3] = {{0x40, {14, 15}}, {0x34, {12}}, {0x28, {9}}};
struct FgPart {
    Bytes xf;
    uint32_t joint;
    struct Hk {
        uint32_t size, type;
        Bytes pf;
        std::optional<std::vector<std::pair<uint32_t, Bytes>>> tr;
    };
    std::optional<Hk> hk;
};
struct R {
    const Pak &p;
    uint32_t u32(const Ref &a, uint64_t o = 0) const { return p.pu32(a.first, a.second + o); }
    Bytes raw(const Ref &a, uint64_t o, uint64_t n) const { return slice(p.d, p.pbase(a.first) + a.second + o, n); }
    ORef ptr(const Ref &a, uint64_t o = 0) const { return bg::res(p, a.first, a.second + o); }
};
}  // namespace
BlobP fg_coll_blob(const Pak &p, const PakItem &it) {
    const R r{p};
    const Ref a{it.page, it.data_off()};
    const uint32_t ver = r.u32(a, 0), n = r.u32(a, 4);
    const ORef pa = n ? r.ptr(a, 8) : std::nullopt;
    std::vector<FgPart> parts;
    for (uint32_t i = 0; i < n; i++) {
        const Ref pp = bg::need(r.ptr(bg::need(pa), 4ull * i));
        if (r.u32(pp, 0x24) || r.u32(pp, 0x2c)) throw Unsupported(fmt("FG collision part %u: non-zero +24/+2c", i));
        FgPart pt{r.raw(pp, 0, 0x20), r.u32(pp, 0x20), std::nullopt};
        if (const ORef hk = r.ptr(pp, 0x28)) {
            for (uint64_t o : {8, 0xc, 0x14, 0x18, 0x1c})
                if (r.u32(*hk, o)) throw Unsupported(fmt("FG collision hk block: non-zero word at +%#llx", (unsigned long long)o));
            const uint32_t size = r.u32(*hk, 0), typ = r.u32(*hk, 4);
            if (typ != 1) throw Unsupported(fmt("FG collision hk block type %u", typ));
            FgPart::Hk h{size, typ, r.raw(*hk, 0x20, size), std::nullopt};
            if (const ORef t = r.ptr(*hk, 0x10)) {
                h.tr.emplace();
                for (uint64_t k = 0; k < 3; k++) {
                    const uint32_t cnt = r.u32(*t, 8 * k + 4);
                    h.tr->push_back({cnt, cnt ? r.raw(bg::need(r.ptr(*t, 8 * k)), 0, ELEM[k].first * cnt) : Bytes()});
                }
            }
            pt.hk = std::move(h);
        }
        parts.push_back(std::move(pt));
    }
    auto b = std::make_shared<Blob>("fgcoll");
    auto reserve = [&](size_t len) {
        const size_t o = b->size();
        b->raw(Bytes(len, 0));
        return o;
    };
    auto pad = [&] { b->pad(16, 0xaa); };
    const size_t o = reserve(0x10);
    put32(*b, o, ver);
    put32(*b, o + 4, uint32_t(parts.size()));
    if (!parts.empty()) {
        const size_t pa4 = reserve(8 * parts.size());
        pad();
        reloc(*b, b, o + 8, pa4);
        for (size_t i = 0; i < parts.size(); i++) {
            const FgPart &pt = parts[i];
            const size_t po = reserve(0x40);
            reloc(*b, b, pa4 + 8 * i, po);
            const Bytes xf = swap_words(pt.xf);
            std::copy(xf.begin(), xf.end(), b->b.begin() + std::ptrdiff_t(po));
            put32(*b, po + 0x20, pt.joint);
            put32(*b, po + 0x24, PAD32);
            if (!pt.hk) continue;
            const Bytes pf = havok_convert(pt.hk->pf);
            const size_t ho = reserve(0x30);
            reloc(*b, b, po + 0x30, ho);
            put32(*b, ho, uint32_t(pf.size()));
            put32(*b, ho + 4, pt.hk->type);
            for (size_t k = 0; k < 4 && k < pf.size(); k++) b->b[ho + 0x2c + k] = pf[k];
            b->raw(pf);
            pad();
            if (pt.hk->tr) {
                const size_t to = reserve(0x38);
                reloc(*b, b, ho + 0x18, to);
                for (size_t k = 0; k < pt.hk->tr->size(); k++) {
                    const auto &[cnt, data] = (*pt.hk->tr)[k];
                    put32(*b, to + 0x10 * k + 8, cnt);
                    put32(*b, to + 0x10 * k + 12, k < 2 ? PAD32 : 0);
                    if (!cnt) continue;
                    pad();
                    const size_t eo = b->size();
                    const auto &[sz, pairs] = ELEM[k];
                    for (size_t j = 0; j < data.size(); j += sz) b->raw(swap_words(Bytes(data.begin() + std::ptrdiff_t(j), data.begin() + std::ptrdiff_t(std::min(j + sz, data.size()))), pairs));
                    reloc(*b, b, to + 0x10 * k, eo);
                }
            }
            if (i + 1 < parts.size()) pad();
        }
    }
    return b;
}

BlobP havok_jointed_blob(const Pak &p, const PakItem &it) {
    const uint64_t b = p.pbase(it.page) + it.data_off();
    uint32_t w[8];
    for (int k = 0; k < 8; k++) w[k] = be32(p.d, b + 4ull * size_t(k));
    if (w[1] || w[2] || w[3] || w[5] != 1 || w[6] || w[7] || slice(p.d, b + 0x20, 8) != PF_MAGIC)
        throw Unsupported("COLLISION_HAVOK_JOINTED header " + hexs(slice(p.d, b, 0x28)));
    auto blob = std::make_shared<Blob>("havokjointed");
    Bytes h(16, 0);
    wr32le(&h[0], w[0]);
    blob->raw(h).raw(d_wrapper(havok_convert(slice(p.d, b + 0x20, w[4]))));
    return blob;
}

// ------------------------------------------------------------------------------------------------ the items a prop carries
ExtraItems extra_items(const U2Src &s, actor::AutoPages &page, const std::vector<std::optional<PakItem>> &targets) {
    const Pak &src = s.p;
    ExtraItems ex;
    const auto sbt = src.items_of("SOUND_BANK_TABLE");
    if (!sbt.empty()) {
        const PakItem &it = sbt[0];
        const uint32_t n = src.pu32(it.page, it.data_off());
        auto b = std::make_shared<Blob>("soundbanks");
        b->u32(n).u32(PAD32);
        const size_t tab = b->size();
        b->raw(Bytes(8ull * n, 0));
        for (uint32_t k = 0; k < n; k++) {
            const Ref r = bg::need(src, it.page, it.data_off() + 4 + 4ull * k);
            reloc(*b, b, tab + 8ull * k, b->size());
            b->cstr(u8s(src.pstr(r.first, r.second)));
        }
        ex.order.push_back({"SOUND_BANK_TABLE", page.item("SOUND_BANK_TABLE", b)});
    }
    for (const auto &t : targets) {
        if (!t) continue;
        BlobP blob;
        if (t->type == "JOINT_HIERARCHY") blob = jh_blob(src, *t);
        else if (t->type == "ANIM") {
            const AnimU2 a = read_anim(src, *t);
            if (a.clip && anim::is_streamed(*a.clip)) throw Unsupported("streamed clip " + t->name);
            blob = build_anim(a);
        } else if (t->type == "COLLISION_HAVOK_JOINTED")
            blob = havok_jointed_blob(src, *t);
        else if (t->type == "COLLISION_DATA_FOREGROUND")
            blob = fg_coll_blob(src, *t);
        else
            continue;
        auto it = page.item(t->type, blob, u8s(t->name));
        ex.made[{t->type, t->name}] = it;
        ex.order.push_back({t->type, it});
    }
    for (const PakItem &t : src.items_of("COLLISION_HAVOK_JOINTED"))   // loose ragdolls: logged in, not in the artgroup
        if (!ex.made.count({t.type, t.name})) {
            auto it = page.item(t.type, havok_jointed_blob(src, t), u8s(t.name));
            ex.made[{t.type, t.name}] = it;
            ex.order.push_back({t.type, it});
        }
    return ex;
}

// ------------------------------------------------------------------------------------------------ whole paks
namespace {
void finish_pak(PakWriter &w, const fs::path &out) {
    Bytes data = w.build();
    for (const auto &pg : w.pages)
        for (auto &part : pg->parts)
            if (auto *it = std::get_if<std::shared_ptr<PakWItem>>(&part)) (*it)->data->relocs.clear();
    write_bytes(out, data);
}
}  // namespace

// a pak that holds only animations (artgroup + ANIM / ANIM_RAWCHANNELS) -> PS4 (anim-*, taps-*)
void convert_anim_pak(const fs::path &src_path, const fs::path &out) {
    const Pak p(src_path);
    if (!p.ps3) throw std::runtime_error("ValueError: source must be a PS3 pak");
    std::set<std::string> other;
    for (const PakItem &i : p.items)
        if (!std::set<std::string>{"ANIM_RAWCHANNELS", "MATERIAL_TABLE_1", "VRAM_DESC_TABLE", "PAK_LOGIN_TABLE", "ANIM", "ARTGROUP_DATA", "LEVEL_PATS_1",
                                   "PACKAGE_ITEM_LOOKUP", "STREAMING_BUFFER"}
                 .count(i.type))
            other.insert(i.type);
    if (!other.empty()) {
        std::string s;
        for (const std::string &t : other) s += (s.empty() ? "" : ", ") + t;
        throw std::runtime_error("ValueError: unsupported item types in " + S(src_path) + ": " + s);
    }
    PakWriter w;
    struct Pager {
        PakWriter &w;
        Page *cur;
        size_t size = 0x200;
        ItemP item(const std::string &type, const BlobP &data, const std::string &name = "") {
            const size_t est = ((data->size() + 0x40 + type.size() + name.size() + 15) & ~size_t(15)) + 16;
            if (size + est > 0x1b0000 && size > 0x200) {
                cur = &w.page();
                size = 0x200;
            }
            size += est;
            return cur->item(type, data, name);
        }
    } pg{w, &w.page()};
    std::map<std::pair<uint32_t, uint32_t>, ItemP> newitem;
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
    for (const PakItem &it : anims) newitem[{it.page, it.off}] = pg.item("ANIM", build_anim(read_anim(p, it)), u8s(it.name));
    if (!p.items_of("STREAMING_BUFFER").empty()) {   // (the stream chunks would come from .stm files, which this import has none of)
        throw std::runtime_error("FileNotFoundError: stream .stm not found in (no --stm dir)");
    }
    const ArtgroupU2 ag = read_artgroup(p);
    std::vector<ItemP> targets;
    for (const auto &t : ag.targets) {
        if (!t) {
            targets.push_back(nullptr);
            continue;
        }
        auto it = newitem.find({t->page, t->off});
        targets.push_back(it == newitem.end() ? nullptr : it->second);
    }
    auto artgroup = pg.item("ARTGROUP_DATA", build_artgroup(ag.name, targets, keys_for(ag.targets), ag.nb, ag.lists), u8s(ag.name));
    auto lp = std::make_shared<Blob>("levelpats");
    lp->raw(level_pats(p));
    auto levelpats = pg.item("LEVEL_PATS_1", lp);
    auto lookup = pg.item("PACKAGE_ITEM_LOOKUP", lookup_blob(ag.name, artgroup));
    Page &lpage = w.page();
    std::vector<ItemP> order;
    auto add = [&](const ItemP &x) {
        if (x && std::find(order.begin(), order.end(), x) == order.end()) order.push_back(x);
    };
    for (const PakItem &x : login3) {
        if (x.type == "ARTGROUP_DATA") order.push_back(artgroup);
        else if (x.type == "LEVEL_PATS_1") order.push_back(levelpats);
        else if (x.type == "ANIM") order.push_back(newitem.at({x.page, x.off}));
        else if (x.type == "PACKAGE_ITEM_LOOKUP") order.push_back(lookup);
    }
    add(artgroup);
    add(levelpats);
    for (const PakItem &i : anims) add(newitem.at({i.page, i.off}));
    add(lookup);
    make_login_and_vram_page(w, lpage, order, {});
    finish_pak(w, out);
}

// a pak that holds only COLLISION_HAVOK_JOINTED items + artgroup (loose ragdolls: hair, necklaces)
void convert_havok_jointed_pak(const fs::path &src_path, const fs::path &out) {
    const Pak p(src_path);
    PakWriter w;
    Page &pg = w.page();
    auto lookup = pg.item("PACKAGE_ITEM_LOOKUP", std::make_shared<Blob>("placeholder"));   // retail: the lookup first in the page
    Made made;
    std::vector<ItemP> hj;
    for (const PakItem &it : p.items_of("COLLISION_HAVOK_JOINTED")) {
        auto x = pg.item("COLLISION_HAVOK_JOINTED", havok_jointed_blob(p, it), u8s(it.name));
        if (!made.count({"COLLISION_HAVOK_JOINTED", it.name})) hj.push_back(x);
        else std::replace(hj.begin(), hj.end(), made[{"COLLISION_HAVOK_JOINTED", it.name}], x);
        made[{"COLLISION_HAVOK_JOINTED", it.name}] = x;
    }
    auto lp = std::make_shared<Blob>("levelpats");
    lp->raw(level_pats(p));
    auto levelpats = pg.item("LEVEL_PATS_1", lp);
    const ArtgroupU2 ag = read_artgroup(p);
    auto artgroup = pg.item("ARTGROUP_DATA", build_artgroup(ag.name, artgroup_entries(ag.targets, made), keys_for(ag.targets), ag.nb, ag.lists), u8s(ag.name));
    lookup->data = lookup_blob(ag.name, artgroup);
    std::vector<std::pair<std::string, std::vector<ItemP>>> by = {
        {"ARTGROUP_DATA", {artgroup}}, {"LEVEL_PATS_1", {levelpats}}, {"PACKAGE_ITEM_LOOKUP", {lookup}}, {"COLLISION_HAVOK_JOINTED", hj}};
    std::vector<ItemP> order;
    for (const auto &[a, b] : p.login) {
        const std::string t = p.item_at(a, b).type;
        auto it = std::find_if(by.begin(), by.end(), [&](const auto &e) { return e.first == t; });
        if (it == by.end()) continue;
        order.insert(order.end(), it->second.begin(), it->second.end());
        by.erase(it);
    }
    for (const auto &[t, v] : by) order.insert(order.end(), v.begin(), v.end());
    make_login_and_vram_page(w, w.page(), order, {});
    finish_pak(w, out);
}

}  // namespace u2imp
