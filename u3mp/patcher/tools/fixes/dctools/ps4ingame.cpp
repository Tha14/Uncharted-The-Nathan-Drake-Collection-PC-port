// dctools/ps4ingame.cpp: PS3 -> PS4 converter of Uncharted 3 '-ingame12' level data packages (the latest converter's
// ps4ingame.py with navmesh.py and the FEATURE_DB_2 part of fgcoll.py, as convert_one.py runs it: other item types are
// left out). Items: TAG_INT, LEVEL_INFO_1, LEVEL_OFFSET_1, SPLINE_TABLE, SPAWNER_GROUP, REGION_TREE, NAV_MESH_1,
// FEATURE_DB_2. General rules PS3 (big endian, 4-byte pointers) -> PS4 (little endian, 8-byte pointers): pointers
// grow to 8 bytes; every separately pointed-to block is 16-aligned; 32-bit flag words are bit-reversed; spawner
// positions store w = 0; 'u32 value' slots in tag records get the 0x12345 filler. See ps4ingame.hpp for the details.
#include "ps4ingame.hpp"

#include "ndpak.hpp"
#include "ps4writer.hpp"
#include "u3fix.hpp"

#include <algorithm>
#include <cstring>
#include <functional>

namespace u3 {
namespace {

constexpr uint32_t FILLER = 0x12345, PADW = 0xaaaaaaaa;
constexpr uint32_t NO_ASYNC_TAG = 0x66c6d3fe, ANIM_DATA_TAG = 0x5d4729de;
const std::set<uint32_t> DEBRIS_TYPES = {0x8b7d6f1a, 0x775bef07, 0x035f6332, 0x69de5580};

using Addr = std::pair<uint32_t, uint64_t>;   // (page, offset)
using OAddr = std::optional<Addr>;
using BlobP = std::shared_ptr<Blob>;

uint32_t rev32(uint32_t x) {
    uint32_t r = 0;
    for (int i = 0; i < 32; i++) r |= ((x >> i) & 1u) << (31 - i);
    return r;
}
uint8_t rev8(uint8_t x) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) r |= uint8_t(((x >> i) & 1u) << (7 - i));
    return r;
}
// a PS3 float32 as Python's struct gives it back (via a double: a signalling NaN comes out quiet)
// a PS3 string as the converter writes it: decoded as latin-1, written as UTF-8
std::string utf8_of(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x80) o += char(c);
        else { o += char(0xc0 | c >> 6); o += char(0x80 | (c & 0x3f)); }
    }
    return o;
}
uint32_t f32bits(uint32_t bits) {
    if ((bits & 0x7f800000u) == 0x7f800000u && (bits & 0x7fffffu) && !(bits & 0x400000u)) bits |= 0x400000u;
    return bits;
}

class Conv {
public:
    Conv(const Pak &p, const std::set<std::string> &anim, const std::vector<ExtraSpawner> &extra) : p(p), anim(anim), extra(extra) {
        std::map<uint32_t, std::set<uint64_t>> pt;
        for (const auto &[k, tp] : p.ptrmap) pt[tp].insert(p.pptr(k.first, k.second));
        for (const PakItem &it : p.items) {
            auto &s = pt[it.page];
            s.insert(it.off);
            s.insert(it.data_off());
            s.insert(uint64_t(it.data_off()) + it.size);
        }
        for (auto &[k, v] : pt) cuts[k] = std::vector<uint64_t>(v.begin(), v.end());
    }
    const Pak &p;
    const std::set<std::string> &anim;
    const std::vector<ExtraSpawner> &extra;
    std::map<Addr, BlobP> memo;
    std::vector<BlobP> blocks;
    std::map<uint32_t, std::vector<uint64_t>> cuts;

    uint32_t u32(const Addr &a, uint64_t o = 0) const { return p.pu32(a.first, a.second + o); }
    uint16_t u16(const Addr &a, uint64_t o = 0) const { return p.u16(p.pbase(a.first) + a.second + o); }
    uint32_t f32(const Addr &a, uint64_t o = 0) const { return f32bits(u32(a, o)); }
    Bytes raw(const Addr &a, uint64_t o, uint64_t n) const {
        const uint64_t b = std::min<uint64_t>(p.pbase(a.first) + a.second + o, p.d.size()), e = std::min<uint64_t>(b + n, p.d.size());
        return Bytes(p.d.begin() + std::ptrdiff_t(b), p.d.begin() + std::ptrdiff_t(e));
    }
    OAddr ptr(const Addr &a, uint64_t o = 0) const {
        auto r = p.resolve(a.first, uint32_t(a.second + o));
        if (!r) return std::nullopt;
        return Addr{r->first, r->second};
    }
    const Addr &need(const OAddr &a, const char *what) const {
        if (!a) throw Stop(std::string("TypeError: no pointer (") + what + ")");
        return *a;
    }
    uint64_t extent(const Addr &a) const {   // bytes from a to the next pointer target / item boundary on its page
        const auto &c = cuts.at(a.first);
        auto it = std::upper_bound(c.begin(), c.end(), a.second);
        return (it != c.end() ? *it : p.d.size()) - a.second;
    }
    BlobP fresh(const OAddr &a, const char *name) {
        auto b = std::make_shared<Blob>(name);
        if (a) memo[*a] = b;
        blocks.push_back(b);
        return b;
    }
    static void ptr_or_null(Blob &b, const BlobP &t) {
        if (t) b.ptr(t);
        else b.u64(0);
    }
    BlobP string(const OAddr &a) {
        if (!a) return nullptr;
        if (auto it = memo.find(*a); it != memo.end()) return it->second;
        auto b = fresh(a, "str");
        b->cstr(utf8_of(p.pstr(a->first, a->second)));
        return b;
    }
    void swap_u32s(Blob &b, const Bytes &src, size_t n) {   // n big-endian u32 -> little endian
        for (size_t i = 0; i < n; i++) {
            const uint32_t v = 4 * i + 3 < src.size() ? uint32_t(src[4 * i]) << 24 | uint32_t(src[4 * i + 1]) << 16 | uint32_t(src[4 * i + 2]) << 8 | src[4 * i + 3]
                                                      : throw Stop("struct.error: short block");
            b.u32(v);
        }
    }
    void swap_f32s(Blob &b, const Bytes &src, size_t n) {
        for (size_t i = 0; i < n; i++) {
            if (4 * i + 3 >= src.size()) throw Stop("struct.error: short block");
            b.u32(f32bits(uint32_t(src[4 * i]) << 24 | uint32_t(src[4 * i + 1]) << 16 | uint32_t(src[4 * i + 2]) << 8 | src[4 * i + 3]));
        }
    }

    // ---- SPLINE_TABLE
    void spline_table(const PakItem &it, Blob &root) {
        const Addr a{it.page, it.data_off()};
        const uint32_t n = u32(a);
        root.u32(n).u32(PADW).u32(PADW).u32(PADW);
        std::vector<std::pair<Addr, std::vector<uint32_t>>> recs;
        for (uint32_t k = 0; k < n; k++) {
            const Addr r{a.first, a.second + 4 + 0x30ull * k};
            std::vector<uint32_t> W;
            for (int i = 0; i < 12; i++) W.push_back(u32(r, 4 * i));
            recs.push_back({r, W});
        }
        for (const auto &[r, W] : recs) {
            const uint32_t nA = W[4], nP = W[6], nS = W[8];
            root.u32(W[0]).u32(PADW);
            const OAddr par = ptr(r, 4);
            ptr_or_null(root, par ? spawner(*par) : nullptr);
            root.u32(W[2]).u32(W[3]).u32(nA).u32(PADW);
            const OAddr A = ptr(r, 0x14);
            ptr_or_null(root, A ? region_tags(*A, nA) : nullptr);
            const OAddr Pp = ptr(r, 0x1c);
            BlobP pb;
            if (Pp) {
                pb = fresh(Pp, "splPts");
                swap_f32s(*pb, raw(*Pp, 0, 16ull * nP), 4ull * nP);
            }
            root.u32(nP).u32(PADW);
            ptr_or_null(root, pb);
            const OAddr S = ptr(r, 0x24), L = ptr(r, 0x28);
            uint32_t nseg = nP;
            if (S && nP) {
                const Bytes last = raw(*S, 4ull * nS * (nP - 1), 4ull * nS);
                if (std::all_of(last.begin(), last.end(), [](uint8_t c) { return c == 0; })) nseg = nP - 1;
            }
            BlobP sb, lb;
            if (S) {
                sb = fresh(S, "splSamp");
                swap_u32s(*sb, raw(*S, 0, 4ull * nS * nseg), size_t(nS) * nseg);
            }
            root.u32(nS).u32(PADW);
            ptr_or_null(root, sb);
            if (L) {
                lb = fresh(L, "splLen");
                swap_u32s(*lb, raw(*L, 0, 4ull * nseg), nseg);
            }
            ptr_or_null(root, lb);
            const OAddr X = ptr(r, 0x2c);
            BlobP xb;
            if (X) {
                xb = fresh(X, "splX");
                const uint32_t nv = u32(*X), nt = u32(*X, 4);
                xb->u32(nv).u32(nt);
                const OAddr T = ptr(*X, 8);
                BlobP tb;
                if (T) {
                    tb = fresh(T, "splXtags");
                    std::vector<std::pair<size_t, OAddr>> datas;
                    for (uint32_t j = 0; j < nt; j++) {
                        const Addr q{T->first, T->second + 12ull * j};
                        const uint32_t ty = u32(q, 4);
                        tb->u32(u32(q)).u32(ty);
                        datas.push_back({tb->size(), ptr(q, 8)});
                        tb->u64(0);
                    }
                    for (uint32_t j = 0; j < datas.size(); j++) {
                        const auto &[pos, D] = datas[j];
                        if (!D) continue;
                        BlobP db;
                        if (u32(Addr{T->first, T->second + 12ull * j}, 4) == 3) {   // per-vertex string pointers, widened
                            db = fresh(D, "splXstr");
                            for (uint32_t k = 0; k < nv; k++) ptr_or_null(*db, string(ptr(*D, 4ull * k)));
                        } else {
                            db = fresh(D, "splXdata");
                            swap_u32s(*db, raw(*D, 0, 4ull * nv), nv);
                        }
                        tb->relocs.push_back({pos, db, 0});
                    }
                }
                ptr_or_null(*xb, tb);
            }
            ptr_or_null(root, xb);
        }
    }

    // ---- SPAWNER_GROUP
    void spawner_group(const PakItem &it, const BlobP &root) {
        const Addr a{it.page, it.data_off()};
        memo[a] = root;
        if (ptr(a, 0) || u32(a, 0) != 0) throw Stop("AssertionError: spawner group word 0");
        const OAddr arr = ptr(a, 4), srt = ptr(a, 12);
        const uint32_t n = u32(a, 8), n2 = u32(a, 16);
        std::vector<BlobP> sp;
        for (uint32_t k = 0; k < n; k++) sp.push_back(spawner(need(ptr(need(arr, "spawners"), 4ull * k), "spawner")));
        root->u64(0);
        auto ab = fresh(arr, "sp[]");
        if (extra.empty()) {
            for (const auto &b : sp) ab->ptr(b);
            root->ptr(ab).u32(n).u32(PADW);
            auto sb = fresh(srt, "sorted");
            for (uint32_t k = 0; k < n2; k++) sb->u32(u32(need(srt, "sorted"), 8ull * k)).u32(PADW).ptr(spawner(need(ptr(*srt, 8ull * k + 4), "sorted spawner")));
            root->ptr(sb).u32(n2).u32(PADW);
            return;
        }
        // with added spawners: the converted sorted table first, then each addition's blocks (names, tags, spawner),
        // the spawner array with the additions at the end, the sorted table re-sorted by string id
        std::vector<std::pair<uint32_t, BlobP>> srtd;
        for (uint32_t k = 0; k < n2; k++) srtd.push_back({u32(need(srt, "sorted"), 8ull * k), spawner(need(ptr(*srt, 8ull * k + 4), "sorted spawner"))});
        for (const ExtraSpawner &x : extra) {
            // tags: 11 integer tags 0 and the string-id tag 0xe07cb817 = 0x39bad0f3, in key order
            static const uint32_t ZERO[] = {0x560619f3, 0x56da20e1, 0x65c281d4, 0x66c6d3fe, 0x78812e7e, 0x871aab05,
                                            0x97587862, 0xa88f1ca6, 0xbaa50d5b, 0xc867d993, 0xc87fbb93};
            std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> ents;   // key, type, value
            for (uint32_t k : ZERO) ents.push_back({k, 1, 0});
            ents.push_back({0xe07cb817, 13, 0x39bad0f3});
            std::sort(ents.begin(), ents.end());
            auto nm = fresh(std::nullopt, "str");
            nm->cstr(x.name);
            auto at = fresh(std::nullopt, "str");
            at->cstr(x.art);
            auto tt = fresh(std::nullopt, "tagtab");
            for (const auto &[k, ty, val] : ents) {
                auto vb = fresh(std::nullopt, "tagval");
                auto db = fresh(std::nullopt, "tagdata");
                db->u32(val);
                vb->u16(ty).u16(4).u32(PADW).ptr(db);
                tt->u32(k).u32(PADW).ptr(vb);
            }
            auto tg = fresh(std::nullopt, "tags");
            tg->ptr(tt).u32(uint32_t(ents.size())).u32(PADW);
            auto b = fresh(std::nullopt, "spawner");
            for (double c : x.center) {   // position (w 0), rotation quaternion (0, 0, 0, 1)
                const float f = float(c);
                uint32_t v;
                std::memcpy(&v, &f, 4);
                b->u32(v);
            }
            b->u32(0).u32(0).u32(0).u32(0).u32(0x3f800000);
            const uint32_t id = sid(x.name);
            b->ptr(nm).u32(id).u32(PADW).u64(0).u64(0);
            b->u32(0x2db62a42).u32(PADW).ptr(at).u32(0).u32(PADW).ptr(root).u64(0).ptr(tg).u64(0).u64(0).u64(0);
            sp.push_back(b);
            srtd.push_back({id, b});
        }
        for (const auto &b : sp) ab->ptr(b);
        root->ptr(ab).u32(uint32_t(sp.size())).u32(PADW);
        std::stable_sort(srtd.begin(), srtd.end(), [](const auto &l, const auto &r) { return l.first < r.first; });
        auto sb = fresh(srt, "sorted");
        for (const auto &[k, b] : srtd) sb->u32(k).u32(PADW).ptr(b);
        root->ptr(sb).u32(uint32_t(srtd.size())).u32(PADW);
    }

    BlobP spawner(const Addr &a) {
        if (auto it = memo.find(a); it != memo.end()) return it->second;
        auto b = fresh(a, "spawner");
        b->u32(f32(a, 0)).u32(f32(a, 4)).u32(f32(a, 8)).u32(0);
        for (int i = 0; i < 4; i++) b->u32(f32(a, 16 + 4 * i));
        ptr_or_null(*b, string(ptr(a, 32)));
        b->u32(u32(a, 36)).u32(PADW);
        for (int o : {40, 44}) {
            if (ptr(a, o) || u32(a, o) != 0) throw Stop(fmt("AssertionError: ('spawner field', '%s', %d)", pyhex(a.second).c_str(), o));
            b->u64(0);
        }
        const OAddr art = ptr(a, 52);
        const std::optional<std::string> artname = art ? std::optional<std::string>(p.pstr(art->first, art->second)) : std::nullopt;
        b->u32(u32(a, 48)).u32(PADW);
        ptr_or_null(*b, string(art));
        b->u32(rev32(u32(a, 56))).u32(PADW);
        const OAddr g = ptr(a, 60);
        if (g) b->ptr(memo.at(*g));
        else b->u64(0);
        const OAddr par = ptr(a, 64);
        ptr_or_null(*b, par ? spawner(*par) : nullptr);
        const bool animated = artname && anim.count(*artname);
        std::optional<std::pair<uint32_t, uint32_t>> add;
        if (DEBRIS_TYPES.count(u32(a, 48)) && animated) add = std::make_pair(ANIM_DATA_TAG, 1u);
        std::optional<uint32_t> zero;
        if (animated) {
            const auto v = int_tag(ptr(a, 68), NO_ASYNC_TAG);
            if (v && *v) zero = NO_ASYNC_TAG;
        }
        ptr_or_null(*b, spawner_tags(ptr(a, 68), add, zero));
        for (int o : {72, 76, 80}) {
            if (ptr(a, o) || u32(a, o) != 0) throw Stop(fmt("AssertionError: ('spawner field', '%s', %d)", pyhex(a.second).c_str(), o));
            b->u64(0);
        }
        return b;
    }

    std::optional<uint32_t> int_tag(const OAddr &a, uint32_t key) const {
        if (!a) return std::nullopt;
        const OAddr tab = ptr(*a, 0);
        const uint32_t n = u32(*a, 4);
        for (uint32_t k = 0; k < n; k++) {
            if (u32(need(tab, "tag table"), 8ull * k) != key) continue;
            const OAddr v = ptr(*tab, 8ull * k + 4);
            if (!v || u16(*v, 0) != 1) return std::nullopt;
            const OAddr d = ptr(*v, 4);
            return d ? std::optional<uint32_t>(u32(*d, 0)) : std::nullopt;
        }
        return std::nullopt;
    }

    struct TagEnt {
        uint32_t key;
        OAddr value;
        std::optional<uint32_t> int_value;   // a tag the converter writes itself: int value
    };
    BlobP spawner_tags(const OAddr &a, std::optional<std::pair<uint32_t, uint32_t>> add, std::optional<uint32_t> zero) {
        if (!add && !zero) {
            if (!a) return nullptr;
            if (auto it = memo.find(*a); it != memo.end()) return it->second;
        }
        std::vector<TagEnt> ents;
        OAddr tab;
        if (a) {
            tab = ptr(*a, 0);
            const uint32_t n = u32(*a, 4), z = u32(*a, 8);
            if (z != 0) throw Stop("AssertionError: tag table word 8");
            for (uint32_t k = 0; k < n; k++) ents.push_back({u32(need(tab, "tag table"), 8ull * k), ptr(*tab, 8ull * k + 4), std::nullopt});
        }
        if (add && std::any_of(ents.begin(), ents.end(), [&](const TagEnt &e) { return e.key == add->first; })) add.reset();
        const bool fresh_tags = add || zero;
        auto b = fresh(fresh_tags ? std::nullopt : a, "tags");
        if (add) {   // binary-searched by key
            ents.push_back({add->first, std::nullopt, add->second});
            std::stable_sort(ents.begin(), ents.end(), [](const TagEnt &x, const TagEnt &y) { return x.key < y.key; });
        }
        if (zero)
            for (TagEnt &e : ents)
                if (e.key == *zero) e.int_value = 0u;
        if (ents.empty()) {
            // the PS4 MP spawn filter reads data[0] without a count check: one terminator entry above every key
            auto tb = fresh(std::nullopt, "tagterm");
            tb->u32(0xffffffff).u32(PADW).u64(0);
            b->ptr(tb);
        } else {
            auto tb = fresh((a && !fresh_tags) ? tab : std::nullopt, "tagtab");
            for (const TagEnt &e : ents) {
                if (e.int_value) {
                    auto vb = fresh(std::nullopt, "tagval");
                    auto db = fresh(std::nullopt, "tagdata");
                    db->u32(*e.int_value);
                    vb->u16(1).u16(4).u32(PADW).ptr(db);
                    tb->u32(e.key).u32(PADW).ptr(vb);
                } else {
                    tb->u32(e.key).u32(PADW).ptr(tag_value(need(e.value, "tag value")));
                }
            }
            b->ptr(tb);
        }
        b->u32(uint32_t(ents.size())).u32(PADW);
        return b;
    }

    BlobP tag_value(const Addr &a) {
        if (auto it = memo.find(a); it != memo.end()) return it->second;
        auto b = fresh(a, "tagval");
        const uint16_t ty = u16(a, 0), sz = u16(a, 2);
        const OAddr d = ptr(a, 4);
        b->u16(ty).u16(sz).u32(PADW);
        if (!d) {
            b->u64(0);
            return b;
        }
        auto db = fresh(d, "tagdata");
        if (ty == 1 || ty == 4 || ty == 13) {
            if (sz != 4) throw Stop("AssertionError: tag size");
            db->u32(u32(*d));
        } else if (ty == 8) {
            if (sz != 16) throw Stop("AssertionError: tag size");
            for (int i = 0; i < 4; i++) db->u32(f32(*d, 4 * i));
        } else if (ty == 5) {
            ptr_or_null(*db, string(ptr(*d, 0)));
        } else {
            throw Stop(fmt("ValueError: spawner tag type %u", ty));
        }
        b->ptr(db);
        return b;
    }

    // ---- REGION_TREE
    void region_tree(const PakItem &it, const BlobP &root) {
        const Addr a{it.page, it.data_off()};
        memo[a] = root;
        const OAddr grid_a = ptr(a, 0);
        std::vector<Addr> regs;
        for (uint64_t k = 4; ptr(a, k); k += 4) regs.push_back(*ptr(a, k));
        const BlobP gb = grid_a ? grid(*grid_a) : nullptr;
        std::vector<BlobP> rbs;
        for (const Addr &r : regs) rbs.push_back(region(r));
        ptr_or_null(*root, gb);
        for (const auto &r : rbs) root->ptr(r);
        root->u64(0);
    }
    BlobP grid(const Addr &a) {
        auto b = fresh(a, "grid");
        for (int i = 0; i < 8; i++) b->u32(f32(a, 4 * i));
        const uint32_t nx = u32(a, 32), ny = u32(a, 36), nz = u32(a, 40);
        b->u32(nx).u32(ny).u32(nz).u32(PADW);
        for (uint64_t k = 0; k < uint64_t(nx) * ny * nz + 1; k++) ptr_or_null(*b, cell(ptr(a, 44 + 4 * k)));
        return b;
    }
    BlobP cell(const OAddr &a) {
        if (!a) return nullptr;
        if (auto it = memo.find(*a); it != memo.end()) return it->second;
        auto b = fresh(a, "cell");
        const OAddr g = ptr(*a, 0);
        const uint32_t n = u32(*a, 4);
        if (n == 0) b->u64(0);
        else {
            auto gb = fresh(g, "groups");
            for (uint32_t k = 0; k < n; k++) gb->u32(u32(need(g, "groups"), 8ull * k)).u32(PADW).ptr(reglist(need(ptr(*g, 8ull * k + 4), "region list")));
            b->ptr(gb);
        }
        b->u32(n).u32(PADW);
        return b;
    }
    BlobP reglist(const Addr &a) {
        if (auto it = memo.find(a); it != memo.end()) return it->second;
        auto b = fresh(a, "reglist");
        const OAddr r = ptr(a, 0);
        const uint32_t n = u32(a, 4);
        if (n == 0) b->u64(0);
        else {
            auto rb = fresh(r, "regptrs");
            for (uint32_t k = 0; k < n; k++) rb->ptr(region(need(ptr(need(r, "regions"), 4ull * k), "region")));
            b->ptr(rb);
        }
        b->u32(n).u32(PADW);
        return b;
    }
    BlobP region(const Addr &a) {
        if (auto it = memo.find(a); it != memo.end()) return it->second;
        auto b = fresh(a, "region");
        ptr_or_null(*b, string(ptr(a, 0)));
        b->u32(u32(a, 4)).u16(u16(a, 8)).u16(u16(a, 10));
        ptr_or_null(*b, region_a(ptr(a, 12)));
        const OAddr sp = ptr(a, 16);
        ptr_or_null(*b, sp ? spawner(*sp) : nullptr);
        if (ptr(a, 20) || u32(a, 20) != 0) throw Stop("AssertionError: ('region C', '" + pyhex(a.second) + "')");
        b->u64(0);
        ptr_or_null(*b, shape(ptr(a, 24)));
        const uint16_t ntags = u16(a, 28);
        b->u16(ntags).u16(u16(a, 30)).u32(PADW);
        const OAddr sc = ptr(a, 32);
        ptr_or_null(*b, sc ? script(*sc) : nullptr);
        const OAddr t = ptr(a, 36);
        ptr_or_null(*b, t ? region_tags(*t, ntags) : nullptr);
        return b;
    }
    BlobP region_a(const OAddr &a) {
        if (!a) return nullptr;
        if (auto it = memo.find(*a); it != memo.end()) return it->second;
        auto b = fresh(a, "regA");
        b->u32(u32(*a, 0)).u32(u32(*a, 4)).u32(u32(*a, 8)).u32(PADW);
        for (int o : {12, 16}) {
            if (ptr(*a, o) || u32(*a, o) != 0) throw Stop("AssertionError: ('regionA ptr', '" + pyhex(a->second) + "')");
            b->u64(0);
        }
        return b;
    }
    BlobP shape(const OAddr &a) {
        if (!a) return nullptr;
        if (auto it = memo.find(*a); it != memo.end()) return it->second;
        auto b = fresh(a, "shape");
        const OAddr nxt = ptr(*a, 0);   // the shapes of a region form a linked list
        ptr_or_null(*b, nxt ? shape(nxt) : nullptr);
        if (ptr(*a, 4) || u32(*a, 4) != 0) throw Stop("AssertionError: ('shape ptr', '" + pyhex(a->second) + "')");
        b->u64(0);
        b->u32(u32(*a, 8)).u16(u16(*a, 12)).u16(u16(*a, 14)).u32(PADW).u32(PADW);
        const int64_t n = int64_t(extent(*a)) - 16;
        if (n % 4 != 0) throw Stop("AssertionError: shape size");
        if (n > 0) swap_u32s(*b, raw(*a, 16, uint64_t(n)), size_t(n / 4));
        return b;
    }
    BlobP region_tags(const Addr &a, uint32_t n) {
        if (auto it = memo.find(a); it != memo.end()) return it->second;
        auto b = fresh(a, "rtags");
        for (uint32_t k = 0; k < n; k++) {
            const uint64_t o = 12ull * k;
            const uint32_t key = u32(a, o), ty = u32(a, o + 4);
            b->u32(key).u32(ty);
            if (ty == 3) ptr_or_null(*b, string(ptr(a, o + 8)));
            else {
                if (p.is_ptr(a.first, uint32_t(a.second + o + 8))) throw Stop(fmt("AssertionError: ('region tag ptr', %u)", ty));
                b->u32(u32(a, o + 8)).u32(FILLER);
            }
        }
        return b;
    }
    // Uncharted 2 region scripts: two {u32 sid, ptr lambda} entries (U3: one entry). A lambda is {ptr body, u32 sid}
    // (PS4 {ptr body, pad}); a body {ptr code, ptr consts, u32 hash, u32 tag} (PS4 {ptr code, ptr consts, u32 hash, pad
    // x3}). The code is copied as it is, except that a last word equal to the body's tag becomes padding on PS4; the
    // constants widen from 4 to 8 bytes as in U3.
    BlobP script_u2(const Addr &a) {
        if (auto it = memo.find(a); it != memo.end()) return it->second;
        auto b = fresh(a, "script");
        for (uint64_t k = 0; k < 2; k++) {
            b->u32(u32(a, 8 * k)).u32(PADW);
            const OAddr x = ptr(a, 8 * k + 4);
            ptr_or_null(*b, x ? lambda_u2(*x) : nullptr);
        }
        return b;
    }
    BlobP lambda_u2(const Addr &x) {
        if (auto it = memo.find(x); it != memo.end()) return it->second;
        auto lb = fresh(x, "lambda");
        const OAddr body = ptr(x, 0);
        ptr_or_null(*lb, body ? body_u2(*body) : nullptr);
        lb->u32(PADW).u32(PADW);
        return lb;
    }
    BlobP body_u2(const Addr &y) {
        if (auto it = memo.find(y); it != memo.end()) return it->second;
        auto bb = fresh(y, "lambda-body");
        const OAddr code = ptr(y, 0), consts = ptr(y, 4);
        const uint32_t hash = u32(y, 8), tag = u32(y, 12);
        BlobP cb, kb;
        if (code) {
            // the code runs up to the constants when they follow it on the same page, else to the next boundary
            const bool to_consts = consts && consts->first == code->first && consts->second > code->second;
            Bytes cd = raw(*code, 0, to_consts ? consts->second - code->second : extent(*code));
            if (cd.size() >= 4) {
                const size_t e = cd.size();
                if ((uint32_t(cd[e - 4]) << 24 | uint32_t(cd[e - 3]) << 16 | uint32_t(cd[e - 2]) << 8 | cd[e - 1]) == tag)
                    std::fill(cd.end() - 4, cd.end(), uint8_t(0xaa));
            }
            cb = fresh(code, "code");
            cb->raw(cd);
        }
        if (consts) {
            kb = fresh(consts, "consts");
            for (uint64_t c = 0; c < extent(*consts) / 4; c++) {
                if (p.is_ptr(consts->first, uint32_t(consts->second + 4 * c))) ptr_or_null(*kb, string(ptr(*consts, 4 * c)));
                else kb->u32(u32(*consts, 4 * c)).u32(0);
            }
        }
        ptr_or_null(*bb, cb);
        ptr_or_null(*bb, kb);
        bb->u32(hash).u32(PADW).u32(PADW).u32(PADW);
        return bb;
    }
    BlobP script(const Addr &a) {
        if (g_u2) return script_u2(a);
        if (auto it = memo.find(a); it != memo.end()) return it->second;
        auto b = fresh(a, "script");
        const Addr x = need(ptr(a, 4), "lambda");
        b->u32(u32(a, 0)).u32(PADW);
        auto xb = fresh(x, "lambda");
        const OAddr code = ptr(x, 0), consts = ptr(x, 4);
        if (!code || *code != Addr{x.first, x.second + 20}) throw Stop("AssertionError: lambda layout");
        const Addr k = need(consts, "lambda consts");
        const uint64_t ncode = k.second - code->second;
        auto cb = fresh(code, "code");
        // bytecode is little endian on both; PS4 registers are 64-bit: LoadStaticI32Imm (0x3f) becomes LoadStaticU32Imm (0x44)
        Bytes cd = raw(*code, 0, ncode);
        for (size_t i = 0; i + 3 < cd.size(); i += 4)
            if (cd[i] == 0x3f) cd[i] = 0x44;
        cb->raw(cd);
        const uint64_t nconst = extent(k) / 4;
        auto kb = fresh(consts, "consts");
        for (uint64_t c = 0; c < nconst; c++) {
            if (p.is_ptr(k.first, uint32_t(k.second + 4 * c))) ptr_or_null(*kb, string(ptr(k, 4 * c)));
            else kb->u32(u32(k, 4 * c)).u32(0);
        }
        xb->ptr(cb).ptr(kb).u32(u32(x, 8)).u32(u32(x, 12)).u32(u32(x, 16)).u32(PADW);
        b->ptr(xb);
        return b;
    }
};

// ---- NAV_MESH_1 (navmesh.py): PS3 'NavM' v3 -> PS4
uint32_t pair16(uint32_t w) { return ((w & 0xffff) << 16) | (w >> 16); }
uint32_t bswap32(uint32_t w) { return (w >> 24) | ((w >> 8) & 0xff00) | ((w << 8) & 0xff0000) | (w << 24); }
Bytes sw(const Bytes &x, size_t n) {   // reverse every n-byte group
    Bytes o;
    for (size_t i = 0; i < x.size(); i += n) {
        const size_t e = std::min(i + n, x.size());
        for (size_t j = e; j > i; j--) o.push_back(x[j - 1]);
    }
    return o;
}

BlobP nav_blob(const Pak &p, const PakItem &it) {
    const uint32_t pg = it.page;
    const uint64_t o = it.data_off();
    const uint32_t hs = p.pu32(pg, o + 8);
    if ((hs - 0xd0) % 16 != 0 || hs < 0xd0 || hs > 0x400) throw Stop("AssertionError: nav header size " + pyhex(hs));
    std::vector<uint32_t> W;
    for (uint32_t k = 0; k < std::max<uint32_t>(0x38, hs / 4); k++) W.push_back(p.pu32(pg, o + 4ull * k));
    if (W[0] != 0x4e61764d) throw Stop("AssertionError: not a NavM");
    const uint64_t end = o + it.size;
    std::map<int, std::optional<uint64_t>> T;
    for (int k : {3, 8, 9, 31, 44}) {
        auto r = p.resolve(pg, uint32_t(o + 4ull * k));
        T[k] = r ? std::optional<uint64_t>(r->second) : std::nullopt;
    }
    for (uint32_t k = 0; k < W.size(); k++)
        if (!T.count(int(k)) && p.ptrmap.count({pg, uint32_t(o + 4ull * k)}) && k < hs / 4) throw Stop(fmt("NotImplementedError: nav header word %u is a pointer", k));
    const uint32_t nverts = W[11], npolys = W[10] >> 16;
    auto B = [&](uint64_t a, uint64_t n) {
        const uint64_t b = std::min<uint64_t>(p.pbase(pg) + a, p.d.size()), e = std::min<uint64_t>(b + n, p.d.size());
        return Bytes(p.d.begin() + std::ptrdiff_t(b), p.d.begin() + std::ptrdiff_t(e));
    };
    const Bytes verts = T[9] ? B(*T[9], 16ull * nverts) : Bytes{}, polys = T[8] ? B(*T[8], 0x50ull * npolys) : Bytes{};
    const std::optional<Bytes> bytes = T[31] ? std::optional<Bytes>(B(*T[31], W[35])) : std::nullopt;
    const std::optional<std::string> name = T[3] ? std::optional<std::string>(utf8_of(p.pstr(pg, *T[3]))) : std::nullopt;
    const std::optional<Bytes> data = T[44] ? std::optional<Bytes>(B(*T[44], end - *T[44])) : std::nullopt;
    if (T[44] && T[3] && !(*T[3] < *T[44])) throw Stop("NotImplementedError: unexpected nav data order");

    const uint32_t nrec = (hs - 0xd0) / 16;   // extra 16-byte records after the 0xd0 header
    auto b = std::make_shared<Blob>("navmesh");
    std::vector<uint32_t> H(0x3c + 4 * nrec, 0);
    constexpr uint32_t FILL = 0x12341234;
    H[2] = pair16(W[39]); H[3] = W[35];
    for (int i = 0; i < 3; i++) H[4 + i] = W[28 + i];
    H[7] = FILL;
    for (int i = 0; i < 3; i++) { H[8 + i] = W[32 + i]; H[11 + i] = W[36 + i]; }
    H[14] = H[15] = FILL;
    for (int i = 0; i < 8; i++) H[16 + i] = W[20 + i];
    for (int i = 0; i < 4; i++) { H[24 + i] = W[12 + i]; H[28 + i] = W[16 + i]; H[32 + i] = W[40 + i]; }
    H[46] = W[0]; H[47] = W[1]; H[48] = W[2]; H[49] = W[11]; H[50] = W[4];
    for (int i = 0; i < 3; i++) H[51 + i] = W[5 + i];
    H[54] = 0;
    H[55] = pair16(W[10]);
    for (int i = 0; i < 4; i++) H[56 + i] = W[45 + i];
    for (int k : {49, 50, 51})
        if (W[k]) throw Stop(fmt("NotImplementedError: nav header word %d = %s", k, pyhex(W[k]).c_str()));
    for (uint32_t r = 0; r < nrec; r++) {
        for (int i = 0; i < 3; i++) H[60 + 4 * r + i] = W.at(52 + 4 * r + i);
        H[63 + 4 * r] = bswap32(W.at(55 + 4 * r));
    }
    for (uint32_t v : H) b->u32(v);
    std::vector<std::pair<size_t, size_t>> rel;
    const size_t vo = b->size();
    b->raw(sw(verts, 4));
    if (data) {
        const size_t d0 = b->size();
        b->raw(*data);
        b->pad(16, 0xaa);
        rel.push_back({44 * 4, d0});
    }
    if (name) {
        const size_t no = b->size();
        b->cstr(*name);
        b->pad(16, 0xaa);
        rel.push_back({36 * 4, no});
    }
    const size_t po = b->size();
    for (size_t i = 0; i + 0x50 <= polys.size(); i += 0x50) {
        const uint8_t *q = &polys[i];
        uint32_t hdr[4];
        for (int k = 0; k < 4; k++) hdr[k] = uint32_t(q[4 * k]) << 24 | uint32_t(q[4 * k + 1]) << 16 | uint32_t(q[4 * k + 2]) << 8 | q[4 * k + 3];
        if (hdr[2] || hdr[3]) throw Stop("NotImplementedError: nav poly header words 2/3 non-zero");
        b->b.resize(b->b.size() + 32, 0);
        for (int k = 0; k < 4; k++) {
            const uint8_t *v = q + 16 + 16 * k;
            b->raw(sw(Bytes(v, v + 12), 4));
            if (k == 0) {   // byte 2 is a flag bitfield: bit order reversed on PS4
                Bytes w(v + 12, v + 16);
                w[2] = rev8(w[2]);
                b->raw(w);
            } else if (k == 1) b->raw(Bytes(v + 12, v + 16));
            else if (k == 2) b->u32(hdr[1]);
            else b->u32(pair16(hdr[0]));
        }
    }
    if (!polys.empty()) rel.push_back({38 * 4, po});
    if (!verts.empty()) rel.push_back({40 * 4, vo});
    if (bytes) {
        const size_t bo = b->size();
        b->raw(*bytes);
        rel.push_back({0, bo});
    }
    for (const auto &[pos, t] : rel) b->relocs.push_back({pos, b, int64_t(t)});
    return b;
}

// ---- FEATURE_DB_2 (fgcoll.py trailer3)
Bytes swap_words(const Bytes &b, const std::set<size_t> &u16pairs = {}) {
    Bytes out;
    for (size_t i = 0; i < b.size(); i += 4) {
        const size_t e = std::min(i + 4, b.size());
        Bytes w(b.begin() + std::ptrdiff_t(i), b.begin() + std::ptrdiff_t(e));
        if (u16pairs.count(i / 4)) {   // w[1::-1] + w[3:1:-1]
            for (int j = std::min<int>(1, int(w.size()) - 1); j >= 0; j--) out.push_back(w[size_t(j)]);
            for (int j = int(w.size()) - 1; j >= 2; j--) out.push_back(w[size_t(j)]);
        } else {
            for (size_t j = w.size(); j > 0; j--) out.push_back(w[j - 1]);
        }
    }
    return out;
}
struct Node {
    Bytes sphere;
    uint16_t cnt = 0, tag = 0;
    uint32_t u = 0;
    std::vector<Node> kids;
};
struct Tr3 {
    std::vector<std::pair<uint32_t, Bytes>> arrs;
    uint32_t u = 0;
    std::vector<std::optional<Node>> trees;
};
const std::pair<size_t, std::set<size_t>> ELEM[3] = {{0x40, {14, 15}}, {0x38, {12}}, {0x28, {9}}};

Node read_node(const Conv &r, const Addr &a) {
    Node n;
    n.sphere = r.raw(a, 0, 16);
    n.cnt = r.u16(a, 0x14);
    n.tag = r.u16(a, 0x16);
    n.u = r.u32(a, 0x18);
    if (n.cnt) {
        const Addr ka = r.need(r.ptr(a, 0x10), "node children");
        for (uint32_t i = 0; i < n.cnt; i++) n.kids.push_back(read_node(r, r.need(r.ptr(ka, 4ull * i), "node child")));
    }
    return n;
}
Tr3 read_tr3(const Conv &r, const Addr &a) {
    Tr3 t;
    t.u = r.u32(a, 0x18);
    for (int k = 0; k < 3; k++) {
        const uint32_t cnt = r.u32(a, 8ull * k + 4);
        Bytes data;
        if (cnt) data = r.raw(r.need(r.ptr(a, 8ull * k), "trailer array"), 0, ELEM[k].first * cnt);
        t.arrs.push_back({cnt, data});
    }
    for (uint64_t o : {0x1cull, 0x20ull, 0x24ull}) {
        const OAddr x = r.ptr(a, o);
        t.trees.push_back(x ? std::optional<Node>(read_node(r, *x)) : std::nullopt);
    }
    return t;
}
struct FW {   // one Blob for the whole item; pointers are self relocations
    BlobP b = std::make_shared<Blob>("fgcoll");
    size_t pos() const { return b->size(); }
    void pad(size_t a = 16, uint8_t fill = 0xaa) { b->pad(a, fill); }
    void ptr_at(size_t at, size_t target) { b->relocs.push_back({at, b, int64_t(target)}); }
    void put32(size_t at, uint32_t v) { wr32le(&b->b[at], v); }
    size_t reserve(size_t n, uint8_t fill = 0) {
        const size_t o = pos();
        b->b.resize(b->b.size() + n, fill);
        return o;
    }
};
size_t w_node(FW &w, const Node &n) {
    const size_t o = w.reserve(0x20);
    const Bytes s = swap_words(n.sphere);
    std::copy(s.begin(), s.end(), w.b->b.begin() + std::ptrdiff_t(o));
    w.b->b[o + 0x18] = uint8_t(n.cnt); w.b->b[o + 0x19] = uint8_t(n.cnt >> 8);
    w.b->b[o + 0x1a] = uint8_t(n.tag); w.b->b[o + 0x1b] = uint8_t(n.tag >> 8);
    w.put32(o + 0x1c, n.u);
    if (n.cnt) {
        const size_t ka = w.reserve(8ull * n.cnt);
        w.pad();
        w.ptr_at(o + 0x10, ka);
        for (size_t i = 0; i < n.kids.size(); i++) w.ptr_at(ka + 8 * i, w_node(w, n.kids[i]));
    }
    return o;
}
size_t w_tr3(FW &w, const Tr3 &t) {
    w.pad();
    const size_t o = w.reserve(0x50);
    for (int k = 0; k < 3; k++) w.put32(o + 0x10 * k + 0xc, PADW);
    w.put32(o + 0x30, t.u);
    for (int k = 0; k < 3; k++) {
        const auto &[cnt, data] = t.arrs[size_t(k)];
        w.put32(o + 0x10 * k + 8, cnt);
        if (!cnt) continue;
        const size_t a = w.pos();
        const auto &[sz, pairs] = ELEM[k];
        for (size_t i = 0; i < cnt; i++) {
            const size_t b0 = std::min(sz * i, data.size()), e0 = std::min(sz * (i + 1), data.size());
            w.b->raw(swap_words(Bytes(data.begin() + std::ptrdiff_t(b0), data.begin() + std::ptrdiff_t(e0)), pairs));
        }
        w.pad();
        w.ptr_at(o + 0x10 * k, a);
    }
    for (size_t k = 0; k < t.trees.size(); k++)
        if (t.trees[k]) w.ptr_at(o + 0x38 + 8 * k, w_node(w, *t.trees[k]));
    return o;
}

// ---- Uncharted 2 FEATURE_DB_2: two element arrays and two trees (U3: three and three)
//   PS3 0x1c {ptr A, nA, ptr B, nB, u32 u, ptr T0, ptr T1}
//   PS4 0x40 {ptr A, nA, pad, ptr B, nB, pad, u32 u, 0, ptr T0, ptr T1, pad pad}
//   A elements 0x40 bytes (words 14, 15 are u16 pairs, as U3), B elements 0x34 (U3 0x38; word 12 a u16 pair); the tree
//   nodes as U3.
const std::pair<size_t, std::set<size_t>> ELEM_U2[2] = {{0x40, {14, 15}}, {0x34, {12}}};
Tr3 read_fdb_u2(const Conv &r, const Addr &a) {
    Tr3 t;
    t.u = r.u32(a, 0x10);
    for (int k = 0; k < 2; k++) {
        const uint32_t cnt = r.u32(a, 8ull * k + 4);
        Bytes data;
        if (cnt) data = r.raw(r.need(r.ptr(a, 8ull * k), "feature array"), 0, ELEM_U2[k].first * cnt);
        t.arrs.push_back({cnt, data});
    }
    for (uint64_t o : {0x14ull, 0x18ull}) {
        const OAddr x = r.ptr(a, o);
        t.trees.push_back(x ? std::optional<Node>(read_node(r, *x)) : std::nullopt);
    }
    return t;
}
size_t w_fdb_u2(FW &w, const Tr3 &t) {
    w.pad();
    const size_t o = w.reserve(0x40);
    for (int k = 0; k < 2; k++) w.put32(o + 0x10 * k + 0xc, PADW);
    w.put32(o + 0x20, t.u);
    w.put32(o + 0x38, PADW);
    w.put32(o + 0x3c, PADW);
    for (int k = 0; k < 2; k++) {
        const auto &[cnt, data] = t.arrs[size_t(k)];
        w.put32(o + 0x10 * k + 8, cnt);
        if (!cnt) continue;
        const size_t a = w.pos();
        const auto &[sz, pairs] = ELEM_U2[k];
        for (size_t i = 0; i < cnt; i++) {
            const size_t b0 = std::min(sz * i, data.size()), e0 = std::min(sz * (i + 1), data.size());
            w.b->raw(swap_words(Bytes(data.begin() + std::ptrdiff_t(b0), data.begin() + std::ptrdiff_t(e0)), pairs));
        }
        w.pad();
        w.ptr_at(o + 0x10 * k, a);
    }
    for (size_t k = 0; k < t.trees.size(); k++)
        if (t.trees[k]) w.ptr_at(o + 0x28 + 8 * k, w_node(w, *t.trees[k]));
    return o;
}

// ---- Uncharted 2 NAV_MESH_1: no 'NavM' header (U3 has one). Offsets are item relative.
//   PS3 header 0x80 (u32 words)                PS4 header 0xa0
//     w0-3 origin                                +00 w0-3
//     w4 verts*  w5 nVerts  w6                   +10 verts*  +18 w5  +1c w6
//     w7 name*                                   +20 name*
//     w13 polys*                                 +28 polys*
//     w8-11 extent                               +30 w8-11
//     w12 {u16 nPolys, u8 nPortals, u8}          +40 the same (the u16 swapped, the bytes as they are)
//     w15 info*  w16 portals* (or 0)             +50 info*  +58 portals*
//     w18 sid                                    +94 w18
//     w24-27 rotation                            +70 w24-27
//   every other header word is 0. Poly 0x50 -> 0x60: {u16 a, u16 b, f32, 0, 0, 4 x {f32 x, y, z, u8[4]}} ->
//   {u16 a, u16 b, f32, v0 bytes, v1 bytes, 16 zero bytes, 4 x {f32 x, y, z, 0}} (v2 / v3 bytes must be 0; v0 byte 2
//   holds flag bits whose order PS4 reverses, as U3). Portal 0x10 -> 0x18: {u32 sid, 0, 0, u8[4]} -> {u32 sid, 0, u64 0,
//   u8[4], 0}. Info 0x50 -> 0x54: 12 floats, 6 u16, u32 nBytes, bytes*, 3 zero words; the bytes are copied. Vertices:
//   4 floats each. PS4 order: header, vertices, name, polys, info, bytes, portals.
BlobP nav_blob_u2(const Pak &p, const PakItem &it) {
    const uint32_t pg = it.page;
    const uint64_t o = it.data_off(), end = o + it.size;
    if (it.size < 0x80) throw NavMeshUnsupported(fmt("NAV_MESH_1 of %s bytes", pyhex(it.size).c_str()));
    uint32_t W[32];
    for (int k = 0; k < 32; k++) W[k] = p.pu32(pg, o + 4ull * k);
    if (W[0] == 0x4e61764d) throw NavMeshUnsupported("NAV_MESH_1 with a NavM header (U3 layout) in a U2 pak");
    static const std::set<int> PTRS = {4, 7, 13, 15, 16}, WORDS = {0, 1, 2, 3, 5, 6, 8, 9, 10, 11, 12, 18, 24, 25, 26, 27};
    std::map<int, uint64_t> T;
    for (int k = 0; k < 32; k++) {
        const auto r = p.resolve(pg, uint32_t(o + 4ull * k));
        if (r) {
            if (!PTRS.count(k) || r->first != pg) throw NavMeshUnsupported(fmt("nav header word %d is a pointer", k));
            T[k] = r->second;
        } else if (W[k] && !WORDS.count(k))
            throw NavMeshUnsupported(fmt("nav header word %d = %s", k, pyhex(W[k]).c_str()));
    }
    for (int k : PTRS)
        if (!T.count(k) && W[k]) throw NavMeshUnsupported(fmt("nav header word %d = %s (not a pointer)", k, pyhex(W[k]).c_str()));
    const uint64_t base = p.pbase(pg);
    auto B = [&](uint64_t a, uint64_t n) {
        if (a < o || a + n > end) throw NavMeshUnsupported("nav data outside the item");
        return Bytes(p.d.begin() + std::ptrdiff_t(base + a), p.d.begin() + std::ptrdiff_t(base + a + n));
    };
    const uint32_t npolys = W[12] >> 16, nportals = (W[12] >> 8) & 0xff;
    std::optional<Bytes> verts, polys, portals, info, bytes;
    std::optional<std::string> name;
    if (T.count(4)) verts = B(T[4], 16ull * W[5]);
    if (T.count(7)) name = p.pstr(pg, T[7]);
    if (T.count(13)) polys = B(T[13], 0x50ull * npolys);
    if (T.count(16) != (nportals != 0)) throw NavMeshUnsupported(fmt("nav portal count %u / pointer mismatch", nportals));
    if (T.count(16)) portals = B(T[16], 0x10ull * nportals);
    if (T.count(15)) {
        info = B(T[15], 0x50);
        const auto r = p.resolve(pg, uint32_t(T[15] + 0x40));
        const uint32_t nb = be32(*info, 0x3c);
        for (size_t i = 0x44; i < 0x50; i++)
            if ((*info)[i]) throw NavMeshUnsupported("nav info tail words non-zero");
        for (uint64_t k = 0; k < 0x40; k += 4)
            if (p.resolve(pg, uint32_t(T[15] + k))) throw NavMeshUnsupported(fmt("nav info word %d is a pointer", int(k / 4)));
        if (r) {
            if (r->first != pg) throw NavMeshUnsupported("nav bytes on another page");
            bytes = B(r->second, nb);
        } else if (be32(*info, 0x40) != 0)
            throw NavMeshUnsupported("nav info word 16 not a pointer");
    }
    auto b = std::make_shared<Blob>("navmesh");
    Bytes H(0xa0, 0);
    auto put = [&](size_t off, int k, int n = 1) {
        for (int i = 0; i < n; i++) wr32le(&H[off + 4 * size_t(i)], W[k + i]);
    };
    put(0x00, 0, 4);
    put(0x18, 5, 2);
    put(0x30, 8, 4);
    H[0x40] = uint8_t(W[12] >> 16);   // the u16 count, little endian
    H[0x41] = uint8_t(W[12] >> 24);
    H[0x42] = uint8_t(W[12] >> 8);    // the two bytes as they are
    H[0x43] = uint8_t(W[12]);
    put(0x70, 24, 4);
    put(0x94, 18);
    b->raw(H);
    std::vector<std::pair<size_t, size_t>> rel;
    if (verts) {
        rel.push_back({0x10, b->size()});
        b->raw(sw(*verts, 4));
    }
    if (name) {
        rel.push_back({0x20, b->size()});
        b->cstr(utf8_of(*name));
        b->pad(16, 0xaa);
    }
    auto part = [](const Bytes &q, size_t a, size_t n) { return Bytes(q.begin() + std::ptrdiff_t(a), q.begin() + std::ptrdiff_t(a + n)); };
    if (polys) {
        rel.push_back({0x28, b->size()});
        for (uint32_t i = 0; i < polys->size() / 0x50; i++) {
            const Bytes q = part(*polys, 0x50ull * i, 0x50);
            auto zero = [&](size_t a, size_t n) {
                for (size_t j = a; j < a + n; j++)
                    if (q[j]) return false;
                return true;
            };
            if (!zero(8, 8) || !zero(0x3c, 4) || !zero(0x4c, 4)) throw NavMeshUnsupported(fmt("nav poly %u: unknown non-zero words", i));
            Bytes w0 = part(q, 0x1c, 4);
            uint8_t f = 0;   // the flag bits in reverse order
            for (int bit = 0; bit < 8; bit++)
                if (w0[2] & (1 << bit)) f |= uint8_t(0x80 >> bit);
            w0[2] = f;
            b->raw(sw(part(q, 0, 4), 2)).raw(sw(part(q, 4, 4), 4)).raw(w0).raw(part(q, 0x2c, 4)).raw(Bytes(16, 0));
            for (size_t k = 0; k < 4; k++) b->raw(sw(part(q, 0x10 + 0x10 * k, 12), 4)).raw(Bytes(4, 0));
        }
    }
    if (info) {
        rel.push_back({0x50, b->size()});
        const size_t io = b->size();
        b->raw(sw(part(*info, 0, 0x30), 4)).raw(sw(part(*info, 0x30, 0xc), 2)).raw(sw(part(*info, 0x3c, 4), 4));
        b->u64(0);
        b->raw(sw(part(*info, 0x44, 0xc), 4));
        b->pad(16, 0xaa);
        if (bytes) {
            b->relocs.push_back({io + 0x40, b, int64_t(b->size())});
            b->raw(*bytes);
        }
    }
    if (portals) {
        b->pad(16, 0xaa);
        rel.push_back({0x58, b->size()});
        for (uint32_t i = 0; i < portals->size() / 0x10; i++) {
            const Bytes q = part(*portals, 0x10ull * i, 0x10);
            for (size_t j = 4; j < 12; j++)
                if (q[j]) throw NavMeshUnsupported(fmt("nav portal %u: unknown non-zero words", i));
            b->raw(sw(part(q, 0, 4), 4)).raw(Bytes(12, 0)).raw(part(q, 12, 4)).raw(Bytes(4, 0));
        }
    }
    for (const auto &[pos, t] : rel) b->relocs.push_back({pos, b, int64_t(t)});
    return b;
}

}  // namespace

std::set<std::string> anim_actor_names(const fs::path &retail_actor33, const fs::path &conv_json) {
    std::set<std::string> s;
    if (fs::is_directory(retail_actor33))
        for (const auto &e : fs::directory_iterator(retail_actor33)) {
            const std::string f = utf8(e.path().filename());
            if (f.size() < 4 || f.compare(f.size() - 4, 4, ".pak") != 0) continue;
            try {
                if (!Pak(e.path()).items_of("ANIM").empty()) s.insert(f.substr(0, f.size() - 4));
            } catch (const std::exception &) {
            }
        }
    if (fs::exists(conv_json))
        for (const json::Value &v : read_json(conv_json).items) s.insert(v.s);
    return s;
}

Bytes ps4ingame_convert(const fs::path &src, const std::set<std::string> &anim_arts, const std::vector<ExtraSpawner> &extra) {
    return ps4ingame_convert(Pak(src), anim_arts, extra);
}

Bytes ps4ingame_convert(const Pak &p, const std::set<std::string> &anim_arts, const std::vector<ExtraSpawner> &extra, bool drop_nav) {
    if (!p.ps3) throw Stop("AssertionError: not a PS3 pak");
    Conv c(p, anim_arts, extra);
    PakWriter w;
    Page &page = w.page();
    const std::vector<std::string> order = {"TAG_INT", "SPLINE_TABLE", "FEATURE_DB_2", "NAV_MESH_1", "LEVEL_INFO_1", "LEVEL_OFFSET_1", "SPAWNER_GROUP", "REGION_TREE"};
    std::vector<std::string> conv_order = order;   // spawners and regions first: splines may point at a spawner
    std::stable_sort(conv_order.begin(), conv_order.end(), [](const std::string &a, const std::string &b) {
        auto rank = [](const std::string &t) { return t == "SPAWNER_GROUP" ? 0 : t == "REGION_TREE" ? 1 : 2; };
        return rank(a) < rank(b);
    });
    struct Made {
        PakItem it;
        BlobP root;
        std::vector<BlobP> blocks;
    };
    std::map<std::string, Made> items;
    std::vector<std::tuple<std::string, PakItem, BlobP>> other;
    for (const std::string &t : conv_order)
        for (const PakItem &it : drop_nav && t == "NAV_MESH_1" ? std::vector<PakItem>{} : p.items_of(t)) {
            auto root = std::make_shared<Blob>(t);
            c.blocks.clear();
            const Addr a{it.page, it.data_off()};
            if (t == "TAG_INT") root->u32(c.u32(a));
            else if (t == "LEVEL_INFO_1") {
                if (it.size != 4) throw Stop("AssertionError: LEVEL_INFO_1 size " + pyhex(it.size));
                root->u32(c.u32(a));
            } else if (t == "LEVEL_OFFSET_1") {   // the level placement offset, 3 floats (the PS4 login moves the whole level by it)
                if (it.size != 12) throw Stop("AssertionError: LEVEL_OFFSET_1 size " + pyhex(it.size));
                for (int k = 0; k < 3; k++) {
                    Bytes f = c.raw(a, 4ull * k, 4);
                    std::reverse(f.begin(), f.end());
                    root->raw(f);
                }
            } else if (t == "SPLINE_TABLE") {
                if (it.size == 4 && c.u32(a) == 0) root->u32(0);
                else c.spline_table(it, *root);
            } else if (t == "SPAWNER_GROUP") c.spawner_group(it, root);
            else if (t == "REGION_TREE") c.region_tree(it, root);
            else if (t == "NAV_MESH_1") {
                other.push_back({t, it, g_u2 ? nav_blob_u2(p, it) : nav_blob(p, it)});
                continue;
            } else if (t == "FEATURE_DB_2") {
                FW fw;
                if (g_u2) w_fdb_u2(fw, read_fdb_u2(c, a));
                else w_tr3(fw, read_tr3(c, a));
                fw.pad(8, 0);
                other.push_back({t, it, fw.b});
                continue;
            }
            items[t] = {it, root, c.blocks};
        }
    // blocks shared between items (spawners referenced by regions) are embedded once, in the item that made them
    std::set<const Blob *> done;
    std::vector<std::shared_ptr<PakWItem>> made;
    for (const auto &[t, it, blob] : other) made.push_back(page.item(t, blob, utf8_of(it.name)));
    for (const std::string &t : order) {
        auto f = items.find(t);
        if (f == items.end()) continue;
        Made &m = f->second;
        for (const auto &b : m.blocks) {
            if (done.count(b.get())) continue;
            done.insert(b.get());
            m.root->embed(b, 16, 0xaa);
        }
        m.root->pad(4, 0xaa);
        made.push_back(page.item(t, m.root, utf8_of(m.it.name)));
    }
    // other item types are left out (U3, as its recipe converter did); the Uncharted 2 import stops on them instead
    if (g_u2) {
        std::vector<std::string> others;
        for (const PakItem &it : p.items)
            if (std::find(order.begin(), order.end(), it.type) == order.end() && it.type != "PAK_LOGIN_TABLE" && it.type != "VRAM_DESC_TABLE")
                others.push_back("'" + it.type + "'");
        if (!others.empty()) {
            std::string s;
            for (size_t i = 0; i < others.size(); i++) s += (i ? ", " : "") + others[i];
            throw Stop("AssertionError: [" + s + "]");
        }
    }
    Page &p1 = w.page();
    std::vector<std::shared_ptr<PakWItem>> login = made;
    std::stable_sort(login.begin(), login.end(), [](const auto &a, const auto &b) { return a->type < b->type; });
    make_login_and_vram_page(w, p1, login, {});
    return w.build();
}

}  // namespace u3
