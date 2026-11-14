// importer/twin.cpp: Uncharted 2 multiplayer skins built from the PS4 Collection's own remastered campaign actor (the
// "twin").
//
// Most MP skins (actor27/mp-hero-*.pak) are, on PS3, the campaign character of the same name plus the MP setup: the
// campaign actor's mesh as LOD0 plus three lower LODs, per-LOD tags, an artgroup listing the four FG sections; the same
// skeleton. Retail PS4 mp-hero LOD0 is the PS4 campaign actor's mesh, its materials the campaign records, its textures
// the campaign descriptors.
//
// So a skin is built as: the PS3 MP skin converted with the PS4 twin as template (actor.cpp: LOD1-3, FG sections, tags,
// artgroup, MP-only materials); then every LOD0 submesh whose PS3 blocks equal a twin submesh (the same triangles and
// skinning, positions within 1 mm, UVs within 0.002; or the same surface in another vertex order) gets the twin's PS4
// buffers (vertex streams, index buffer, skinning) - the remastered mesh - and every material the twin has by name gets
// the twin's own record, plus the twin textures and GEOMETRY_1 texture entries those records bind. Submeshes and
// materials the twin lacks stay converted. The converted pak is edited as raw pages; what the twin provides is copied
// into new pages, and the login / VRAM table page is rebuilt.
#include "actor.hpp"

#include <cmath>
#include <cstring>

namespace u2imp {

using namespace u3;

const std::map<std::string, std::string> TWINS = {
    {"mp-hero", "costume-hero-nd"}, {"mp-hero-year-1", "costume-hero-nd"}, {"mp-hero-frosty", "costume-hero-nd"},
    {"mp-hero-nd", "costume-hero-nd"}, {"mp-hero-nd-frosty", "costume-hero-nd"},
    {"mp-hero-chloe", "costume-hero-chloe"}, {"mp-hero-chloe-heist", "costume-hero-chloe"},
    {"mp-hero-chloe-variation", "costume-hero-chloe"}, {"mp-hero-chloe-winter", "costume-hero-chloe-winter"},
    {"mp-hero-dead-explorer", "costume-hero-dead-explorer"}, {"mp-hero-divesuit", "costume-hero-divesuit"},
    {"mp-hero-doughnut", "costume-hero-doughnut"}, {"mp-hero-eddie", "costume-hero-eddie"},
    {"mp-hero-elena", "costume-hero-elena"}, {"mp-hero-elena-reporter", "costume-hero-elena-reporter"},
    {"mp-hero-elena-winter", "costume-hero-elena-winter"}, {"mp-hero-flynn", "costume-hero-flynn"},
    {"mp-hero-flynn-frosty", "costume-hero-flynn-winter"}, {"mp-hero-flynn-heist", "costume-hero-flynn-heist"},
    {"mp-hero-flynn-winter", "costume-hero-flynn-winter"}, {"mp-hero-heist", "hero-heist"}, {"mp-hero-jeff", "costume-hero-jeff"},
    {"mp-hero-lazarevic", "costume-hero-lazarevic"}, {"mp-hero-lieutenant", "costume-hero-lieutenant"},
    {"mp-hero-merc", "costume-hero-merc"}, {"mp-hero-navarro", "costume-hero-navarro"}, {"mp-hero-parka", "hero-parka"},
    {"mp-hero-pirate", "costume-hero-pirate"}, {"mp-hero-pirate-nd", "costume-hero-pirate-nd"},
    {"mp-hero-red", "costume-hero-red"}, {"mp-hero-rescuer", "costume-hero-rescuer"}, {"mp-hero-roman", "costume-hero-roman"},
    {"mp-hero-schafer", "costume-hero-schafer"}, {"mp-hero-skeleton-1", "costume-hero-skeleton-1"},
    {"mp-hero-skeleton-1-glow", "costume-hero-skeleton-1"}, {"mp-hero-skeleton-1-platinum", "costume-hero-skeleton-1"},
    {"mp-hero-skeleton-ghengis", "costume-hero-skeleton-ghengis"}, {"mp-hero-skeleton-marco", "costume-hero-skeleton-marco"},
    {"mp-hero-sol-med-b-1a", "costume-hero-sol-med-b-1a"}, {"mp-hero-sol-med-b-1a-v2", "costume-hero-sol-med-b-1a-v2"},
    {"mp-hero-sol-med-b-1b", "costume-hero-sol-med-b-1b"}, {"mp-hero-sol-med-b-2a", "costume-hero-sol-med-b-2a"},
    {"mp-hero-sol-med-b-2b", "costume-hero-sol-med-b-2b"}, {"mp-hero-sol-med-b-3a", "costume-hero-sol-med-b-3a"},
    {"mp-hero-sullivan", "costume-hero-sullivan"}, {"mp-hero-sullivan-blue", "costume-hero-sullivan"},
    {"mp-hero-sullivan-red", "costume-hero-sullivan"}, {"mp-hero-sullivan-winter", "costume-hero-sullivan"},
    {"mp-hero-udf", "costume-hero-udf"},
};

namespace {

const size_t SUB = 0x88;   // a PS4 submesh record
const size_t MREC = 0x48;  // a PS4 material record
const size_t PAGE_LIMIT_T = 0x1c0000;

struct NoTwin : std::runtime_error {
    using std::runtime_error::runtime_error;
};

bool lod_name(const std::string &n) {   // ends in '-lod<digits>'
    const size_t p = n.rfind("-lod");
    if (p == std::string::npos || p + 4 >= n.size()) return false;
    for (size_t i = p + 4; i < n.size(); i++)
        if (!isdigit(uint8_t(n[i]))) return false;
    return true;
}

// ------------------------------------------------------------------------------------------------ PS3 LOD0 blocks
struct Block {
    std::string key, loose;
    Mat pos;
    std::optional<Mat> uv;
    std::string mat;
    size_t ntris = 0;
    bool same(const Block &o) const {
        if (key != o.key || uv.has_value() != o.uv.has_value()) return false;
        double mx = 0;
        for (size_t i = 0; i < pos.v.size() && i < o.pos.v.size(); i++) mx = std::max(mx, std::fabs(pos.v[i] - o.pos.v[i]));
        if (mx > 1e-3) return false;
        if (!uv) return true;
        mx = 0;
        for (size_t i = 0; i < uv->v.size() && i < o.uv->v.size(); i++) mx = std::max(mx, std::fabs(uv->v[i] - o.uv->v[i]));
        return mx <= 2e-3;
    }
};
std::string md5s(const std::string &s) { return md5_hex(Bytes(s.begin(), s.end())); }
// the exact key: md5 of repr((nverts, triangles, skin)) as Python writes it
std::string exact_key(const bg::FgBlock &b) {
    std::string r = "(" + std::to_string(b.nverts) + ", [";
    for (size_t i = 0; i < b.tris.size(); i++)
        r += (i ? ", (" : "(") + std::to_string(b.tris[i][0]) + ", " + std::to_string(b.tris[i][1]) + ", " + std::to_string(b.tris[i][2]) + ")";
    r += "], ";
    if (!b.has_skin) r += "None";
    else {
        r += "[";
        for (size_t i = 0; i < b.skin.size(); i++) {
            r += i ? ", [" : "[";
            for (size_t k = 0; k < b.skin[i].size(); k++) r += (k ? ", (" : "(") + std::to_string(b.skin[i][k].first) + ", " + std::to_string(b.skin[i][k].second) + ")";
            r += "]";
        }
        r += "]";
    }
    return md5s(r + ")");
}
// the same surface whatever the vertex / triangle order: the sorted triangles as (position, UV) corners rounded to 1e-4
std::string loose_digest(const bg::FgBlock &b) {
    const Mat &p = *b.get("pos");
    const Mat *uv = b.get("uv0");
    std::vector<std::vector<double>> rows(p.rows);
    for (size_t r = 0; r < p.rows; r++) {
        for (size_t c = 0; c < p.cols; c++) rows[r].push_back(std::rint(p.at(r, c) * 10000.0) / 10000.0);
        if (uv)
            for (size_t c = 0; c < uv->cols; c++) rows[r].push_back(std::rint(uv->at(r, c) * 10000.0) / 10000.0);
    }
    using Row = std::vector<double>;
    std::vector<std::array<Row, 3>> tris;
    for (const auto &t : b.tris) {
        std::array<Row, 3> x = {rows.at(t[0]), rows.at(t[1]), rows.at(t[2])};
        std::stable_sort(x.begin(), x.end());
        tris.push_back(x);
    }
    std::stable_sort(tris.begin(), tris.end());
    std::string s = "[";
    for (size_t i = 0; i < tris.size(); i++) {
        s += i ? ", (" : "(";
        for (size_t k = 0; k < 3; k++) {
            s += k ? ", (" : "(";
            for (size_t c = 0; c < tris[i][k].size(); c++) s += (c ? ", " : "") + json::real(tris[i][k][c]);
            s += tris[i][k].size() == 1 ? ",)" : ")";
        }
        s += ")";
    }
    return md5s(s + "]");
}
std::vector<Block> ps3_lod0_blocks(const U2Src &s) {
    const Pak &p = s.p;
    const auto fgs = s.fg_items();
    std::vector<PakItem> plain;
    for (const PakItem &i : fgs)
        if (!lod_name(i.name)) plain.push_back(i);
    if (fgs.empty()) throw std::runtime_error("IndexError: no FG section");
    const PakItem it = plain.empty() ? fgs[0] : plain[0];
    const Ref pr = bg::need(s.res(it.page, it.data_off()));
    const auto mats = ps3_block_materials(s, pr);
    const uint32_t nsub = p.u16(p.pbase(pr.first) + pr.second + 6);
    const Ref t = bg::need(p, pr.first, pr.second + 0x14);
    std::vector<Block> out;
    for (uint32_t k = 0; k < nsub; k++) {
        const Ref r = bg::need(s.res(t.first, t.second + 4ull * k));
        const bg::FgBlock b = bg::decode_fg_block(p, r.first, r.second);
        Block x;
        x.key = exact_key(b);
        x.pos = *b.get("pos");
        if (const Mat *u = b.get("uv0")) x.uv = *u;
        x.loose = loose_digest(b);
        x.mat = mats.at(k);
        x.ntris = b.tris.size();
        out.push_back(std::move(x));
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ PS4 side
struct G {
    struct S {
        uint32_t page;
        uint64_t off;
        uint32_t nv, ni;
        std::string mat;
        uint32_t first;
    };
    std::vector<std::pair<std::string, S>> subs;
    struct Pr {
        uint32_t page;
        uint64_t off;
        std::vector<std::string> subs;
    };
    std::vector<std::pair<std::string, Pr>> protos;
    struct M {
        uint32_t page;
        uint64_t off;
        std::string name;
    };
    std::vector<M> mats;
    std::vector<std::pair<std::string, std::string>> fg;   // FG name -> prototype name
    const S &sub(const std::string &n) const {
        for (const auto &[k, v] : subs)
            if (k == n) return v;
        throw std::runtime_error("KeyError: '" + n + "'");
    }
    const Pr &proto(const std::string &n) const {
        for (const auto &[k, v] : protos)
            if (k == n) return v;
        throw std::runtime_error("KeyError: '" + n + "'");
    }
    std::string lod0_proto() const {
        for (const auto &[n, p] : fg)
            if (!lod_name(n)) return p;
        throw std::runtime_error("IndexError: list index out of range");
    }
};
template <class V> void put_in(std::vector<std::pair<std::string, V>> &v, const std::string &k, V x) {
    for (auto &e : v)
        if (e.first == k) {
            e.second = std::move(x);
            return;
        }
    v.push_back({k, std::move(x)});
}
G geometry(const Pak &q) {
    const auto gs = q.items_of("GEOMETRY_1");
    if (gs.empty()) throw std::runtime_error("IndexError: no GEOMETRY_1");
    const uint32_t pg = gs[0].page;
    const uint64_t b = gs[0].data_off();
    G g;
    const uint32_t nsub = q.pu32(pg, b + 8), npro = q.pu32(pg, b + 0x18);
    const Ref sp = bg::need(q, pg, b + 0x10), lp = bg::need(q, pg, b + 0x20);
    for (uint32_t i = 0; i < nsub; i++) {
        const uint64_t o = sp.second + SUB * i;
        const Ref nr = bg::need(q, sp.first, o + 8), mr = bg::need(q, sp.first, o + 0x48);
        const std::string nm = q.pstr(nr.first, nr.second);
        const size_t k = nm.rfind("Submesh");
        size_t e = k + 7;
        while (k != std::string::npos && e < nm.size() && isdigit(uint8_t(nm[e]))) e++;
        if (k == std::string::npos || e != nm.size() || e == k + 7) throw std::runtime_error("AttributeError: submesh name " + nm);
        put_in(g.subs, nm, G::S{sp.first, o, q.pu32(sp.first, o + 0x20), q.pu32(sp.first, o + 0x24), q.pstr(mr.first, mr.second), uint32_t(std::stoul(nm.substr(k + 7)))});
    }
    for (uint32_t i = 0; i < npro; i++) {
        const Ref pp = bg::need(q, lp.first, lp.second + 8ull * i);
        const uint32_t n = q.pu32(pp.first, pp.second + 4);
        const Ref nr = bg::need(q, pp.first, pp.second + 0x10), snp = bg::need(q, pp.first, pp.second + 0x18);
        G::Pr pr{pp.first, pp.second, {}};
        for (uint32_t k = 0; k < n; k++) {
            const Ref r = bg::need(q, snp.first, snp.second + 0x10ull * k);
            pr.subs.push_back(q.pstr(r.first, r.second));
        }
        put_in(g.protos, q.pstr(nr.first, nr.second), pr);
    }
    const uint32_t nm_ = q.pu32(pg, b + 0x48);
    const Ref mp = bg::need(q, pg, b + 0x50);
    for (uint32_t i = 0; i < nm_; i++) {
        const uint64_t o = mp.second + 0x48ull * i;
        const Ref r = bg::need(q, mp.first, o);
        g.mats.push_back({mp.first, o, q.pstr(r.first, r.second)});
    }
    for (const PakItem &it : q.items_of("FOREGROUND_SECTION_2")) put_in(g.fg, it.name, q.cstr(q.pbase(it.page) + it.data_off() + 0x1c));
    return g;
}
std::string mat_key(const std::string &n) { return n.substr(0, n.find(',')); }

// block digests grouped as the PS4 submeshes (first block numbers) group them, by first block
std::vector<std::pair<uint32_t, std::vector<const Block *>>> groups(const std::vector<Block> &blocks, const std::vector<uint32_t> &firsts) {
    std::vector<uint32_t> fs = firsts;
    std::sort(fs.begin(), fs.end());
    std::vector<std::pair<uint32_t, std::vector<const Block *>>> out;
    for (size_t i = 0; i < fs.size(); i++) {
        const size_t end = i + 1 < fs.size() ? fs[i + 1] : blocks.size();
        std::vector<const Block *> v;
        for (size_t k = fs[i]; k < end && k < blocks.size(); k++) v.push_back(&blocks[k]);
        out.push_back({fs[i], v});
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ raw pak editing
using Fix = std::map<uint64_t, uint32_t>;   // offset in the page -> destination page
std::vector<Fix> fix_of(const Pak &q) {
    std::vector<Fix> f(q.pages.size());
    for (const auto &x : q.fixups) f.at(x.src)[x.off] = x.dst;
    return f;
}
std::vector<Bytes> pages_of(const Pak &q) {
    std::vector<Bytes> v;
    for (const auto &pg : q.pages) v.push_back(Bytes(q.d.begin() + std::ptrdiff_t(pg.file), q.d.begin() + std::ptrdiff_t(std::min<uint64_t>(uint64_t(pg.file) + pg.size, q.d.size()))));
    return v;
}
uint64_t r64(const Bytes &d, uint64_t o) { return rd64le(d, o); }

// deep copies of a PS4 pak's pointer targets into new blobs (one per source item, memoised)
struct Graph {
    const Pak &q;
    std::map<std::pair<uint32_t, uint64_t>, BlobP> memo;
    std::map<uint32_t, std::vector<std::pair<uint64_t, uint64_t>>> items;
    std::vector<Fix> fix;
    std::vector<std::vector<std::pair<uint64_t, uint32_t>>> fix_order;   // per page, in fixup-table order (first position kept)
    std::vector<BlobP> blobs;
    explicit Graph(const Pak &q_) : q(q_), fix(fix_of(q_)), fix_order(q_.pages.size()) {
        for (const auto &x : q.fixups) {
            auto &v = fix_order.at(x.src);
            auto it = std::find_if(v.begin(), v.end(), [&](const auto &e) { return e.first == x.off; });
            if (it == v.end()) v.push_back({x.off, x.dst});
            else it->second = x.dst;
        }
        for (const PakItem &it : q.items) items[it.page].push_back({it.data_off(), uint64_t(it.data_off()) + it.size});
        for (auto &[pg, v] : items) std::sort(v.begin(), v.end());
    }
    std::pair<BlobP, int64_t> ptr(uint32_t pg, uint64_t off) {
        const auto &lst = items[pg];
        const std::pair<uint64_t, uint64_t> *reg = nullptr;
        for (const auto &x : lst)
            if ((x.first <= off && off < x.second) || (x.first == off && off == x.second)) {
                reg = &x;
                break;
            }
        if (!reg) throw std::runtime_error(fmt("ValueError: pointer target p%u+%#llx is not inside an item", pg, (unsigned long long)off));
        const auto [lo, hi] = *reg;
        const auto key = std::make_pair(pg, lo);
        if (!memo.count(key)) {
            auto b = std::make_shared<Blob>("twin");
            b->raw(Bytes(q.d.begin() + std::ptrdiff_t(q.pbase(pg) + lo), q.d.begin() + std::ptrdiff_t(q.pbase(pg) + hi)));
            memo[key] = b;
            blobs.push_back(b);
            for (const auto &[o, dp] : fix.at(pg))
                if (lo <= o && o < hi) {
                    const auto [tb, toff] = ptr(dp, q.pptr(pg, o));
                    b->relocs.push_back({size_t(o - lo), tb, toff});
                }
        }
        return {memo[key], int64_t(off - lo)};
    }
};

// retail (and PS3) artgroups key FG sections as id('geo.' + name); the converter keys U2 FG sections (PS3 type
// FOREGROUND_SECTION) as id(name): re-key and re-hash the buckets (linear probing). Returns the keys changed.
size_t fix_artgroup(const Pak &L, std::vector<Bytes> &pages, std::vector<Fix> &fix) {
    std::map<uint32_t, uint32_t> remap;
    for (const PakItem &it : L.items_of("FOREGROUND_SECTION_2")) remap[u3::sid(bg::utf8_of(it.name))] = u3::sid(bg::utf8_of("geo." + it.name));
    size_t n = 0;
    for (const PakItem &it : L.items_of("ARTGROUP_DATA")) {
        const uint32_t pg = it.page;
        const uint64_t b = it.data_off();
        Bytes &d = pages.at(pg);
        bool any = false;
        for (uint64_t o = b; o + 15 < b + it.size; o += 8)   // range(b, b + size - 15, 8)
            if (rd32le(d, o + 4) == 0xaaaaaaaa && fix[pg].count(o + 8) && remap.count(rd32le(d, o))) any = true;
        if (!any || !fix[pg].count(b + 0x40) || fix[pg][b + 0x40] != pg) continue;
        const uint64_t xo = r64(d, b + 0x40);
        const uint32_t nb = rd32le(d, xo + 0x1c);
        const uint64_t tbl = r64(d, xo + 0x20);
        std::vector<std::pair<uint32_t, uint64_t>> ents;
        for (uint32_t k = 0; k < nb; k++) {
            const uint64_t o = tbl + 16ull * k;
            const uint32_t key = rd32le(d, o);
            if (fix[pg].count(o + 8)) {
                if (remap.count(key)) n++;
                ents.push_back({remap.count(key) ? remap[key] : key, r64(d, o + 8)});
                fix[pg].erase(o + 8);
            }
            wr32le(&d[o], 0);
            wr32le(&d[o + 4], 0xaaaaaaaa);
            for (int q = 0; q < 8; q++) d[o + 8 + uint64_t(q)] = 0;
        }
        std::vector<std::optional<std::pair<uint32_t, uint64_t>>> slots(nb);
        for (const auto &e : ents) {
            size_t bi = e.first % nb;
            while (slots[bi]) bi = (bi + 1) % nb;
            slots[bi] = e;
        }
        for (uint32_t k = 0; k < nb; k++)
            if (slots[k]) {
                const uint64_t o = tbl + 16ull * k;
                wr32le(&d[o], slots[k]->first);
                wr32le(&d[o + 4], 0xaaaaaaaa);
                for (int q = 0; q < 8; q++) d[o + 8 + uint64_t(q)] = uint8_t(slots[k]->second >> (8 * q));
                fix[pg][o + 8] = pg;
            }
    }
    return n;
}

std::shared_ptr<PtrTarget> unowned(PtrTarget &t) { return std::shared_ptr<PtrTarget>(std::shared_ptr<PtrTarget>(), &t); }
std::vector<std::pair<uint64_t, size_t>> fix_list(const Fix &f) {
    std::vector<std::pair<uint64_t, size_t>> v;
    for (const auto &[o, d] : f) v.push_back({o, d});
    return v;
}

std::string stem(const std::string &path) {   // without a trailing '.ndb' and '.error'
    std::string s = path;
    if (s.size() >= 6 && s.compare(s.size() - 6, 6, ".error") == 0) s.resize(s.size() - 6);
    if (s.size() >= 4 && s.compare(s.size() - 4, 4, ".ndb") == 0) s.resize(s.size() - 4);
    return s;
}

bool drop_entry(Bytes &page, uint64_t item_off) {   // the last page entry takes the dropped one's slot
    const uint16_t n = uint16_t(page.at(0x12) | page.at(0x13) << 8);
    for (uint16_t j = 0; j < n; j++)
        if (rd32le(page, 0x14 + 16ull * j + 8) == item_off) {
            std::copy(page.begin() + std::ptrdiff_t(0x14 + 16ull * (n - 1)), page.begin() + std::ptrdiff_t(0x14 + 16ull * n), page.begin() + std::ptrdiff_t(0x14 + 16ull * j));
            std::fill(page.begin() + std::ptrdiff_t(0x14 + 16ull * (n - 1)), page.begin() + std::ptrdiff_t(0x14 + 16ull * n), 0);
            page[0x12] = uint8_t(n - 1);
            page[0x13] = uint8_t((n - 1) >> 8);
            return true;
        }
    return false;
}

struct Redirect {
    uint32_t page;
    uint64_t off;
    BlobP blob;
    int64_t boff;
};
struct RawRef {   // a pointer of a new blob into one of L's raw pages (its reloc target is set once the pages exist)
    BlobP blob;
    size_t reloc;
    uint32_t page;
};

}  // namespace

// ------------------------------------------------------------------------------------------------ artgroup keys
void rekey_artgroup(const fs::path &path) {
    const Pak q(path);
    std::vector<Bytes> pages = pages_of(q);
    std::vector<Fix> fix = fix_of(q);
    if (!fix_artgroup(q, pages, fix)) return;
    PakWriter w;
    for (size_t i = 0; i < pages.size(); i++) w.raw_page(pages[i], fix_list(fix[i]));
    w.login_page = std::make_pair(size_t(q.h[2]), size_t(q.h[3]));
    w.vram_table_loc = std::make_pair(size_t(q.h[9]), size_t(q.h[10]));
    w.vram = Bytes(q.d.begin() + std::ptrdiff_t(q.vram_base), q.d.end());
    write_bytes(path, w.build());
}

std::string TwinStats::note() const {
    return fmt("twin %s: %zu/%zu LOD0 submeshes from PS4 (%.0f%% of triangles), %zu/%zu materials from PS4", twin.c_str(), swapped, submeshes, 100 * share,
               twin_materials, materials);
}
Value TwinStats::json() const {
    Value s;
    s.kind = Value::Float;
    s.f = share;
    auto n = [](size_t v) { return num((long long)v); };
    return obj({{"twin", str(twin)}, {"submeshes", n(submeshes)}, {"swapped", n(swapped)}, {"loose", n(loose)}, {"share", s}, {"materials", n(materials)},
                {"twin_materials", n(twin_materials)}, {"artgroup_keys_fixed", n(artgroup_keys_fixed)}, {"textures_added", n(textures_added)},
                {"geometry_textures_added", n(geometry_textures_added)}, {"size", n(size)}, {"cbuffer_capacity_raised", n(cbuffer_capacity_raised)}});
}

// ------------------------------------------------------------------------------------------------ the twin build
std::variant<TwinStats, std::string> twin_build(const fs::path &ps3_mp, const fs::path &ps3_twin, const fs::path &ps4_twin, const fs::path &out) {
    try {
        const U2Src src(ps3_mp), tw3(ps3_twin);
        const Pak C(ps4_twin);
        const std::string twin = S(ps4_twin.filename()).substr(0, S(ps4_twin.filename()).size() - 4);
        if (skeleton_sid(src.p) != skeleton_sid(C)) throw NoTwin(fmt("skeleton %08x != twin %08x", skeleton_sid(src.p), skeleton_sid(C)));
        const std::vector<Block> mp_blocks = ps3_lod0_blocks(src), tw_blocks = ps3_lod0_blocks(tw3);
        const G cg = geometry(C);
        const auto &c0 = cg.proto(cg.lod0_proto()).subs;
        std::map<uint32_t, std::string> c_first;
        for (const std::string &n : c0) c_first[cg.sub(n).first] = n;
        if (c_first.empty()) throw std::runtime_error("ValueError: max() arg is an empty sequence");
        if (c_first.rbegin()->first >= tw_blocks.size()) throw NoTwin("PS4 twin submeshes do not follow the PS3 twin blocks");
        std::vector<uint32_t> cf_keys;
        for (const auto &[f, n] : c_first) cf_keys.push_back(f);
        const auto tw_groups = groups(tw_blocks, cf_keys);
        std::set<uint32_t> bad;
        for (const auto &[f, g] : tw_groups) {
            size_t t = 0;
            for (const Block *x : g) t += x->ntris;
            if (cg.sub(c_first.at(f)).ni != 3 * t) bad.insert(f);
        }
        if (bad.size() > c_first.size() / 4) throw NoTwin(fmt("PS4 twin %s is not the PS3 twin mesh (%zu of %zu submeshes differ)", S(ps4_twin.filename()).c_str(), bad.size(), c_first.size()));
        std::map<std::vector<std::string>, uint32_t> by_loose;
        for (const auto &[f, g] : tw_groups)
            if (!bad.count(f)) {
                std::vector<std::string> k;
                for (const Block *x : g) k.push_back(x->loose);
                std::sort(k.begin(), k.end());
                by_loose.emplace(k, f);
            }

        // 1. the PS3 skin converted with the twin as template
        const fs::path lpath = S(out) + ".conv";
        convert_actor_pak(ps3_mp, ps4_twin, lpath);
        const Pak L(lpath);
        const G lg = geometry(L);
        const auto &l0 = lg.proto(lg.lod0_proto()).subs;
        std::map<uint32_t, std::string> l_first;
        for (const std::string &n : l0) l_first[lg.sub(n).first] = n;
        std::vector<uint32_t> lf_keys;
        for (const auto &[f, n] : l_first) lf_keys.push_back(f);
        const auto mp_groups = groups(mp_blocks, lf_keys);
        std::vector<std::pair<std::string, std::string>> swap;   // L submesh -> C submesh
        size_t loose = 0;
        for (const auto &[f, g] : mp_groups) {
            std::optional<uint32_t> cf;
            for (const auto &[tf, tg] : tw_groups) {
                if (bad.count(tf) || tg.size() != g.size()) continue;
                bool all = true;
                for (size_t i = 0; i < g.size() && all; i++) all = g[i]->same(*tg[i]);
                if (all) {
                    cf = tf;
                    break;
                }
            }
            if (!cf) {
                std::vector<std::string> k;
                for (const Block *x : g) k.push_back(x->loose);
                std::sort(k.begin(), k.end());
                if (auto it = by_loose.find(k); it != by_loose.end()) cf = it->second, loose++;
            }
            if (cf && std::none_of(swap.begin(), swap.end(), [&](const auto &e) { return e.second == c_first.at(*cf); }))
                put_in(swap, l_first.at(f), c_first.at(*cf));
        }
        size_t tris = 0, tris_swapped = 0;
        for (const Block &x : mp_blocks) tris += x.ntris;
        for (const auto &[f, g] : mp_groups)
            if (std::any_of(swap.begin(), swap.end(), [&](const auto &e) { return e.first == l_first.at(f); }))
                for (const Block *x : g) tris_swapped += x->ntris;
        const double share_raw = double(tris_swapped) / double(tris ? tris : 1);
        std::map<std::string, const G::M *> cmats;
        for (const G::M &m : cg.mats) cmats[mat_key(m.name)] = &m;
        size_t twin_materials = 0;
        for (const G::M &m : lg.mats) twin_materials += cmats.count(mat_key(m.name));
        if (swap.empty()) {
            std::error_code ec;
            fs::remove(lpath, ec);
            throw NoTwin("no LOD0 submesh of " + S(ps3_mp.filename()) + " is in " + twin);
        }

        // 2. raw edit of L: LOD0 buffers from C, twin material names, LOD0 prototype words
        std::vector<Bytes> pages = pages_of(L);
        std::vector<Fix> fix = fix_of(L);
        Graph g(C);
        std::vector<Redirect> redirect;
        auto put_ptr = [&](uint32_t pg, uint64_t off, const BlobP &blob, int64_t boff) {
            fix[pg].erase(off);
            redirect.push_back({pg, off, blob, boff});
        };
        for (const auto &[ln, cn] : swap) {
            const G::S &ls = lg.sub(ln), &cs = cg.sub(cn);
            const Bytes crec(C.d.begin() + std::ptrdiff_t(C.pbase(cs.page) + cs.off), C.d.begin() + std::ptrdiff_t(C.pbase(cs.page) + cs.off + SUB));
            std::map<uint64_t, uint32_t> cfix, lfix;
            for (const auto &[o, d] : g.fix.at(cs.page))
                if (cs.off <= o && o < cs.off + SUB) cfix[o - cs.off] = d;
            for (const auto &[o, d] : fix[ls.page])
                if (ls.off <= o && o < ls.off + SUB) lfix[o - ls.off] = d;
            Bytes &rec = pages[ls.page];
            for (uint64_t o = 0; o < SUB; o += 4) {
                if (o == 8 || o == 0xc || o == 0x48 || o == 0x4c) continue;   // name, material: L's
                bool covered = false;
                for (const auto &[x, _] : cfix) covered = covered || (x <= o && o < x + 8);
                for (const auto &[x, _] : lfix) covered = covered || (x <= o && o < x + 8);
                if (covered) continue;
                std::copy(crec.begin() + std::ptrdiff_t(o), crec.begin() + std::ptrdiff_t(o + 4), rec.begin() + std::ptrdiff_t(ls.off + o));
            }
            std::set<uint64_t> all;
            for (const auto &[o, _] : cfix) all.insert(o);
            for (const auto &[o, _] : lfix) all.insert(o);
            for (uint64_t o : all) {
                if (o == 8 || o == 0x48) continue;
                if (cfix.count(o)) {
                    const auto [tb, toff] = g.ptr(cfix[o], r64(crec, o));
                    put_ptr(ls.page, ls.off + o, tb, toff);
                } else {
                    fix[ls.page].erase(ls.off + o);
                    std::copy(crec.begin() + std::ptrdiff_t(o), crec.begin() + std::ptrdiff_t(o + 8), rec.begin() + std::ptrdiff_t(ls.off + o));
                }
            }
        }
        // the LOD0 prototype words +0x30 (the scope-3 cbuffer capacity, raised again after the build)
        const G::Pr &lpr = lg.proto(lg.lod0_proto()), &cpr = cg.proto(cg.lod0_proto());
        if (swap.size() == l0.size())
            std::copy(C.d.begin() + std::ptrdiff_t(C.pbase(cpr.page) + cpr.off + 0x30), C.d.begin() + std::ptrdiff_t(C.pbase(cpr.page) + cpr.off + 0x38),
                      pages[lpr.page].begin() + std::ptrdiff_t(lpr.off + 0x30));
        const size_t keys_fixed = fix_artgroup(L, pages, fix);
        // twin materials: the converter's record is replaced by the twin's own (0x48 bytes + everything it points at);
        // submeshes (+0x48) name it by that string
        std::map<std::string, std::vector<std::string>> cbinds;
        for (const actor::TMat &m : actor::tmpl_materials(C)) {
            std::vector<std::string> v;
            for (const auto &b : m.binds) v.push_back(b.full);
            cbinds[m.name] = v;
        }
        std::map<std::string, std::pair<BlobP, int64_t>> noff;
        std::set<std::string> bound;
        for (const G::M &m : lg.mats) {
            auto cmi = cmats.find(mat_key(m.name));
            if (cmi == cmats.end()) continue;
            const G::M &cm = *cmi->second;
            for (const std::string &x : cbinds.at(cm.name)) bound.insert(x);
            const Bytes crec(C.d.begin() + std::ptrdiff_t(C.pbase(cm.page) + cm.off), C.d.begin() + std::ptrdiff_t(C.pbase(cm.page) + cm.off + MREC));
            std::vector<std::pair<uint64_t, uint32_t>> cfix;   // in the twin's fixup-table order
            for (const auto &[o, d] : g.fix_order.at(cm.page))
                if (cm.off <= o && o < cm.off + MREC) cfix.push_back({o - cm.off, d});
            for (auto it = fix[m.page].begin(); it != fix[m.page].end();) {
                if (m.off <= it->first && it->first < m.off + MREC) it = fix[m.page].erase(it);
                else ++it;
            }
            std::copy(crec.begin(), crec.end(), pages[m.page].begin() + std::ptrdiff_t(m.off));
            for (const auto &[o, dp] : cfix) {
                const auto [tb, toff] = g.ptr(dp, r64(crec, o));
                put_ptr(m.page, m.off + o, tb, toff);
                if (o == 0) noff[m.name] = {tb, toff};
            }
        }
        for (const auto &[n, s_] : lg.subs)
            if (auto it = noff.find(s_.mat); it != noff.end()) put_ptr(s_.page, s_.off + 0x48, it->second.first, it->second.second);
        // twin textures those materials bind that the converter did not carry over, with their in-pak mips
        std::set<std::string> lstem;
        for (const PakTexture &t : L.textures()) lstem.insert(stem(t.path));
        std::vector<PakTexture> add_tex;
        for (const PakTexture &t : C.textures())
            if (bound.count(stem(t.path)) && !lstem.count(stem(t.path))) add_tex.push_back(t);
        // GEOMETRY_1 texture entries: every material binding's full name must be listed; the twin's are added
        const PakItem lgeo = L.items_of("GEOMETRY_1").at(0), cgeo = C.items_of("GEOMETRY_1").at(0);
        auto tex_entries = [](const Pak &q, const PakItem &it) {
            std::vector<std::tuple<uint32_t, uint64_t, std::string>> v;
            const uint32_t n = q.pu32(it.page, it.data_off() + 0x38);
            const ORef r = bg::res(q, it.page, it.data_off() + 0x40);
            if (r)
                for (uint32_t k = 0; k < n; k++) {
                    const Ref s = bg::need(q, r->first, r->second + 0x28ull * k);
                    v.push_back({r->first, r->second + 0x28ull * k, q.pstr(s.first, s.second)});
                }
            return v;
        };
        const auto lent = tex_entries(L, lgeo);
        std::set<uint32_t> have, want;
        for (const auto &e : lent) have.insert(u3::sid(bg::utf8_of(std::get<2>(e))));
        for (const std::string &f : bound) want.insert(u3::sid(bg::utf8_of(f)));
        std::vector<std::tuple<uint32_t, uint64_t, std::string>> cent;
        for (const auto &e : tex_entries(C, cgeo)) {
            const uint32_t h = u3::sid(bg::utf8_of(std::get<2>(e)));
            if (want.count(h) && !have.count(h)) cent.push_back(e);
        }
        std::vector<RawRef> raw_refs;
        if (!cent.empty()) {
            auto arr = std::make_shared<Blob>("geotextures");
            for (const auto &[p_, o_, n_] : lent) {
                raw_refs.push_back({arr, arr->relocs.size(), p_});
                arr->relocs.push_back({arr->size(), nullptr, int64_t(L.pptr(p_, o_))});
                arr->u64(0);
                arr->raw(Bytes(L.d.begin() + std::ptrdiff_t(L.pbase(p_) + o_ + 8), L.d.begin() + std::ptrdiff_t(L.pbase(p_) + o_ + 0x28)));
            }
            for (const auto &[p_, o_, n_] : cent) {
                const Ref r = bg::need(C, p_, o_);
                const auto [tb, toff] = g.ptr(r.first, r.second);
                arr->ptr(tb, toff);
                arr->raw(Bytes(C.d.begin() + std::ptrdiff_t(C.pbase(p_) + o_ + 8), C.d.begin() + std::ptrdiff_t(C.pbase(p_) + o_ + 0x28)));
            }
            g.blobs.push_back(arr);
            for (int k = 0; k < 8; k++) pages[lgeo.page][lgeo.data_off() + 0x38 + uint64_t(k)] = uint8_t(uint64_t(lent.size() + cent.size()) >> (8 * k));
            put_ptr(lgeo.page, lgeo.data_off() + 0x40, arr, 0);
        }

        // ---- write: L's data pages (raw, edited), new pages with the copied twin data, a rebuilt login / vram page
        const size_t last = pages.size() - 1;
        if (L.h[2] != last || L.h[9] != last) throw std::runtime_error("AssertionError: converter layout changed (login page)");
        for (size_t i = 0; i < last; i++)
            for (const auto &[o, d] : fix[i])
                if (d == last) throw std::runtime_error("AssertionError: pointer into the login page");
        Bytes vram(L.d.begin() + std::ptrdiff_t(L.vram_base), L.d.end());
        PakWriter w;
        std::vector<RawPage *> raws;
        for (size_t i = 0; i < last; i++) raws.push_back(&w.raw_page(pages[i], fix_list(fix[i])));
        for (const Redirect &r : redirect) raws.at(r.page)->redirect(r.off, r.blob, r.boff);
        for (const RawRef &r : raw_refs) r.blob->relocs.at(r.reloc).target = unowned(*raws.at(r.page));
        struct NewDesc {
            PakTexture t;
            BlobP db, name;
        };
        std::vector<NewDesc> new_desc;
        for (const PakTexture &t : add_tex) {
            const PakItem it = C.item_at(t.page, t.off);
            const uint64_t base = C.pbase(it.page) + it.data_off();
            auto db = std::make_shared<Blob>("vramdesc");
            db->raw(Bytes(C.d.begin() + std::ptrdiff_t(base), C.d.begin() + std::ptrdiff_t(base + it.size)));
            while (vram.size() % 0x100) vram.push_back(0);
            for (int k = 0; k < 8; k++) db->b[8 + size_t(k)] = uint8_t(uint64_t(vram.size()) >> (8 * k));
            const Bytes tx = C.vram_bytes(t.vramOffset, t.vramSize);
            vram.insert(vram.end(), tx.begin(), tx.end());
            db->relocs.push_back({0x38, db, int64_t(C.pptr(it.page, it.data_off() + 0x38) - it.data_off())});
            auto name = std::make_shared<Blob>("texname");
            const size_t k = t.path.find(cook_prefix());
            name->cstr(bg::utf8_of(k == std::string::npos ? t.path : t.path.substr(k + cook_prefix().size())));
            new_desc.push_back({t, db, name});
        }
        std::vector<std::pair<std::string, BlobP>> blobs;
        for (const BlobP &b : g.blobs) blobs.push_back({"RAW_DATA", b});
        for (const NewDesc &nd : new_desc) blobs.push_back({"VRAM_DESC", nd.db}), blobs.push_back({"RAW_DATA", nd.name});
        size_t size = PAGE_LIMIT_T;
        Page *pg = nullptr;
        for (const auto &[ty, b] : blobs) {
            if (size + b->size() + 0x40 > PAGE_LIMIT_T) {
                pg = &w.page();
                size = 0x200;
            }
            pg->item(ty, b);
            size += b->size() + 0x40;
        }
        const PakItem li = L.item_at(uint32_t(L.h[2]), uint32_t(L.h[3]));
        const uint64_t lb = L.pbase(uint32_t(last)) + li.data_off();
        std::vector<std::pair<uint32_t, uint32_t>> login;
        for (uint32_t k = 0; k < rd32le(L.d, lb); k++) login.push_back({rd32le(L.d, lb + 8 + 8ull * k), rd32le(L.d, lb + 12 + 8ull * k)});
        std::vector<ItemP> targets;
        for (const auto &[p_, o_] : login) {
            auto r = std::make_shared<PakWItem>();   // a login target inside a raw page
            r->page = raws.at(p_);
            r->off = o_;
            targets.push_back(r);
        }
        if (!new_desc.empty()) {
            // the TEXTURE_TABLE rebuilt with the added textures (sorted by hash, as the converter writes it)
            if (!pg || size > PAGE_LIMIT_T - 0x4000) pg = &w.page();
            const PakItem tt = L.items_of("TEXTURE_TABLE").at(0);
            const uint64_t tb = L.pbase(tt.page) + tt.data_off();
            const uint64_t n = rd64le(L.d, tb);
            const Ref hp = bg::need(L, tt.page, tt.data_off() + 8), np_ = bg::need(L, tt.page, tt.data_off() + 0x10);
            struct E {
                uint32_t hash;
                BlobP name;
                Ref name_ref;
                BlobP desc;
                Ref desc_ref;
            };
            std::vector<E> ents;
            for (uint64_t k = 0; k < n; k++)
                ents.push_back({rd32le(L.d, L.pbase(hp.first) + hp.second + 4 * k), nullptr, bg::need(L, np_.first, np_.second + 8 * k), nullptr,
                                bg::need(L, tt.page, tt.data_off() + 0x20 + 8 * k)});
            for (const NewDesc &nd : new_desc) ents.push_back({nd.t.hash, nd.name, {}, nd.db, {}});
            std::stable_sort(ents.begin(), ents.end(), [](const E &a, const E &b) { return a.hash < b.hash; });
            auto hb = std::make_shared<Blob>("tthash"), nb_ = std::make_shared<Blob>("ttnames"), tbb = std::make_shared<Blob>("texturetable");
            for (const E &e : ents) {
                hb->u32(e.hash);
                if (e.name) nb_->ptr(e.name);
                else nb_->ptr(unowned(*raws.at(e.name_ref.first)), int64_t(e.name_ref.second));
            }
            auto hi = pg->item("RAW_DATA", hb), ni = pg->item("RAW_DATA", nb_);
            tbb->u64(ents.size()).ptr(hi->data).ptr(ni->data).ptr(tbb, 0x20);
            for (const E &e : ents) {
                if (e.desc) tbb->ptr(e.desc);
                else tbb->ptr(unowned(*raws.at(e.desc_ref.first)), int64_t(e.desc_ref.second));
            }
            auto ttab = pg->item("TEXTURE_TABLE", tbb);
            drop_entry(raws.at(tt.page)->data, tt.off);   // the old table stays as unreferenced bytes, unlisted
            for (size_t i = 0; i < login.size(); i++)
                if (L.item_at(login[i].first, login[i].second).type == "TEXTURE_TABLE") targets[i] = ttab;
        }
        // the login / vram table page (as the converter lays it out: unlisted PAK_LOGIN_TABLE + VRAM_DESC_TABLE)
        Page &lp = w.page();
        auto lgb = std::make_shared<Blob>("login");
        lgb->u32(targets.size()).u32(0).raw(Bytes(8 * targets.size(), 0));
        w.login_item = lp.item("PAK_LOGIN_TABLE", lgb, "", false);
        w.login_targets = targets;
        const auto vt = L.vram_desc_table();
        auto vb = std::make_shared<Blob>("vramtable");
        vb->u32(vt.size() + new_desc.size()).u32(0);
        for (const auto &[p_, o_] : vt) {
            if (!p_) throw std::runtime_error("TypeError: a VRAM desc table entry without a page");
            vb->ptr(unowned(*raws.at(*p_)), int64_t(o_));
        }
        for (const NewDesc &nd : new_desc) vb->ptr(nd.db, -0x20);
        w.vram_table_item = lp.item("VRAM_DESC_TABLE", vb, "", false);
        w.vram = vram;
        Bytes data = w.build();
        for (const auto &p : w.pages)   // break the self-reference cycles
            for (auto &part : p->parts)
                if (auto *it = std::get_if<std::shared_ptr<PakWItem>>(&part)) (*it)->data->relocs.clear();
        for (const BlobP &b : g.blobs) b->relocs.clear();
        std::error_code ec;
        fs::remove(lpath, ec);
        {
            const Pak check(data, S(out));
            const auto errs = check.validate();
            if (!errs.empty()) {
                std::string s;
                for (size_t i = 0; i < errs.size() && i < 3; i++) s += (i ? "; " : "") + errs[i];
                throw std::runtime_error("RuntimeError: twin build: " + s);
            }
        }
        // +0x30 = the scope-3 cbuffer capacity: the twin's LOD0 value is retail's for the twin materials, but the skin
        // keeps converted materials too, and LOD1-3 keep the converter's value
        TwinStats st;
        st.twin = twin;
        st.submeshes = l0.size();
        st.swapped = swap.size();
        st.loose = loose;
        st.share = std::stod(fmt("%.3f", share_raw));   // round(share, 3)
        st.materials = lg.mats.size();
        st.twin_materials = twin_materials;
        st.artgroup_keys_fixed = keys_fixed;
        st.textures_added = add_tex.size();
        st.geometry_textures_added = cent.size();
        st.size = data.size();
        st.cbuffer_capacity_raised = fix_cbuffer_capacity(data);
        write_bytes(out, data);
        return st;
    } catch (const NoTwin &e) {
        return std::string(e.what());
    }
}


// ------------------------------------------------------------------------------------------------ validation
namespace {

Bytes slice_at(const Pak &q, const Ref &r, uint64_t n) {
    const uint64_t s = std::min<uint64_t>(q.pbase(r.first) + r.second, q.d.size()), e = std::min<uint64_t>(s + n, q.d.size());
    return Bytes(q.d.begin() + std::ptrdiff_t(s), q.d.begin() + std::ptrdiff_t(e));
}

// a submesh's vertex streams by slot (the last of a slot wins) and its index buffer
std::pair<std::map<int, Bytes>, Bytes> subdata(const Pak &q, const G::S &s) {
    std::map<int, Bytes> streams;
    const uint32_t ns = q.pu32(s.page, s.off + 0x28);
    const ORef st = bg::res(q, s.page, s.off + 0x30);
    for (uint32_t k = 0; k < ns; k++) {
        const uint64_t so = bg::need(st).second + 0x18ull * k;
        const uint32_t ln = q.pu32(st->first, so + 4);
        const Ref fm = bg::need(q, st->first, so + 8), dr = bg::need(q, st->first, so + 0x10);
        streams[q.d.at(q.pbase(fm.first) + fm.second)] = slice_at(q, dr, ln);
    }
    return {streams, slice_at(q, bg::need(q, s.page, s.off + 0x40), 2ull * s.ni)};
}

// float32 positions (rows of 3)
std::vector<std::array<float, 3>> rows3(const Bytes &b) {
    if (b.size() % 12) throw std::runtime_error(fmt("ValueError: cannot reshape array of size %zu into shape (3)", b.size() / 4));
    std::vector<std::array<float, 3>> v(b.size() / 12);
    if (!v.empty()) std::memcpy(v.data(), b.data(), b.size());
    return v;
}
float max_diff(const std::vector<std::array<float, 3>> &x, const std::vector<std::array<float, 3>> &y) {   // numpy broadcasting
    if (x.size() != y.size() && x.size() != 1 && y.size() != 1)
        throw std::runtime_error(fmt("ValueError: operands could not be broadcast together with shapes (%zu,3) (%zu,3)", x.size(), y.size()));
    const size_t n = (x.empty() || y.empty()) ? 0 : std::max(x.size(), y.size());
    float d = 0;
    for (size_t i = 0; i < n; i++)
        for (size_t c = 0; c < 3; c++) d = std::max(d, std::fabs(x[x.size() == 1 ? 0 : i][c] - y[y.size() == 1 ? 0 : i][c]));
    return d;
}
// the rows ordered by their coordinates rounded to 0.01 (z first, then y, then x; equal keys keep their order)
std::vector<std::array<float, 3>> sorted_rows(const std::vector<std::array<float, 3>> &x) {
    auto r2 = [](float v) {
        const float t = v * 100.0f;
        return std::nearbyint(t) / 100.0f;
    };
    std::vector<size_t> ix(x.size());
    for (size_t i = 0; i < ix.size(); i++) ix[i] = i;
    std::stable_sort(ix.begin(), ix.end(), [&](size_t a, size_t b) {
        for (int c = 2; c >= 0; c--) {
            const float ka = r2(x[a][size_t(c)]), kb = r2(x[b][size_t(c)]);
            if (ka != kb) return ka < kb;
        }
        return false;
    });
    std::vector<std::array<float, 3>> o;
    for (size_t i : ix) o.push_back(x[i]);
    return o;
}

std::string lod_key(const std::string &fg) {   // '-lod<n>' of an FG name, else 'lod0'
    const size_t k = fg.rfind("-lod");
    if (k != std::string::npos && k + 4 < fg.size() && std::all_of(fg.begin() + std::ptrdiff_t(k + 4), fg.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; }))
        return fg.substr(k);
    return "lod0";
}

// FG sections by name (sorted): (prototype, LOD distance)
std::map<std::string, std::pair<std::string, float>> fg_sections(const Pak &q) {
    std::map<std::string, std::pair<std::string, float>> o;
    for (const PakItem &it : q.items_of("FOREGROUND_SECTION_2")) {
        const uint64_t base = q.pbase(it.page) + it.data_off();
        float lod;
        const uint32_t w = q.u32(base + 0x18);
        std::memcpy(&lod, &w, 4);
        o[it.name] = {q.cstr(base + 0x1c), lod};
    }
    return o;
}

bool same_tmat(const actor::TMat &a, const actor::TMat &b) {
    if (a.name != b.name || a.shader != b.shader || a.hash != b.hash || a.feats != b.feats || a.x3 != b.x3 || a.tail != b.tail) return false;
    if (a.params.size() != b.params.size() || a.binds.size() != b.binds.size()) return false;
    for (size_t i = 0; i < a.params.size(); i++)
        if (a.params[i].name != b.params[i].name || a.params[i].cnt != b.params[i].cnt || a.params[i].vals != b.params[i].vals) return false;
    for (size_t i = 0; i < a.binds.size(); i++)
        if (a.binds[i].pname != b.binds[i].pname || a.binds[i].shrt != b.binds[i].shrt || a.binds[i].full != b.binds[i].full) return false;
    return true;
}

Value int_array(std::initializer_list<long long> v) {
    Value a;
    a.kind = Value::Array;
    for (long long x : v) a.items.push_back(num(x));
    return a;
}

// what of a built skin matches the retail PS4 pak of the same name
Value twin_compare(const fs::path &built, const fs::path &retail) {
    const Pak a(built), b(retail);
    const G ga = geometry(a), gb = geometry(b);
    const std::vector<std::string> pa = ga.proto(ga.lod0_proto()).subs, pb = gb.proto(gb.lod0_proto()).subs;
    std::vector<std::string> free = pb;
    long long same_ib = 0, same_pos = 0, near_pos = 0, same_uv = 0, same_mat = 0, same_n = 0, paired = 0;
    for (const std::string &n : pa) {
        const G::S &s = ga.sub(n);
        // the retail submesh of the same material and index count, else the same index count
        auto pick = [&](bool by_mat) -> std::optional<size_t> {
            for (size_t i = 0; i < free.size(); i++) {
                const G::S &x = gb.sub(free[i]);
                if (x.ni == s.ni && (!by_mat || mat_key(x.mat) == mat_key(s.mat))) return i;
            }
            return std::nullopt;
        };
        auto ti = pick(true);
        if (!ti) ti = pick(false);
        if (!ti) continue;
        const G::S &t = gb.sub(free[*ti]);
        free.erase(free.begin() + std::ptrdiff_t(*ti));
        paired++;
        same_mat += mat_key(s.mat) == mat_key(t.mat);
        same_n += s.nv == t.nv;
        const auto [sa, ia] = subdata(a, s);
        const auto [sb, ib] = subdata(b, t);
        auto slot = [](const std::map<int, Bytes> &m, int k) { return m.count(k) ? std::optional<Bytes>(m.at(k)) : std::nullopt; };
        same_ib += ia == ib;
        same_pos += slot(sa, 0) == slot(sb, 0);
        same_uv += slot(sa, 1) == slot(sb, 1);
        if (s.nv == t.nv && sa.count(0) && sb.count(0)) {
            const auto x = rows3(sa.at(0)), y = rows3(sb.at(0));
            float d = max_diff(x, y);
            if (d > 1e-3) d = max_diff(sorted_rows(x), sorted_rows(y));   // the same vertices in another order
            near_pos += d <= 1e-3;
        }
    }
    Value r = Value::object();
    r.set("lod0", obj({{"submeshes", str(fmt("%zu/%zu", pa.size(), pb.size()))},
                       {"paired", num(paired)},
                       {"material", num(same_mat)},
                       {"vertex_count", num(same_n)},
                       {"index", num(same_ib)},
                       {"positions", num(same_pos)},
                       {"positions_1mm", num(near_pos)},
                       {"uv", num(same_uv)}}));
    // per LOD: (submeshes, indices, LOD distance) of the built and the retail pak
    Value lods = Value::object();
    const auto fa = fg_sections(a), fb = fg_sections(b);
    auto add_lods = [&](const G &gg, const std::map<std::string, std::pair<std::string, float>> &fgs, const char *tag) {
        for (const auto &[fn, f] : fgs) {
            const G::Pr &pr = gg.proto(f.first);
            long long ni = 0;
            for (const std::string &s : pr.subs) ni += gg.sub(s).ni;
            Value v = int_array({(long long)pr.subs.size(), ni});
            Value lodv;
            lodv.kind = Value::Float;
            lodv.f = double(f.second);
            v.items.push_back(lodv);
            const std::string key = lod_key(fn);
            if (!lods.get(key)) lods.set(key, Value::object());
            lods.get(key)->set(tag, v);
        }
    };
    add_lods(ga, fa, "built");
    add_lods(gb, fb, "retail");
    r.set("lods", lods);
    std::set<std::string> ma, mb;
    for (const auto &m : ga.mats) ma.insert(m.name);
    for (const auto &m : gb.mats) mb.insert(m.name);
    long long same_name = 0;
    for (const std::string &m : ma) same_name += (long long)mb.count(m);
    std::map<std::string, actor::TMat> ra, rb;
    for (actor::TMat &m : actor::tmpl_materials(a)) ra[m.name] = m;
    for (actor::TMat &m : actor::tmpl_materials(b)) rb[m.name] = m;
    long long same_record = 0;
    for (const auto &[k, m] : ra)
        if (rb.count(k) && same_tmat(m, rb.at(k))) same_record++;
    r.set("materials", obj({{"built", num((long long)ma.size())}, {"retail", num((long long)mb.size())}, {"same_name", num(same_name)}, {"same_record", num(same_record)}}));
    auto texmap = [](const Pak &q) {
        std::map<std::string, std::array<uint32_t, 6>> m;
        for (const PakTexture &t : q.textures()) m[t.path] = {t.hash, t.format, t.width, t.height, t.mips, t.unk34};
        return m;
    };
    const auto ta = texmap(a), tb = texmap(b);
    long long same_tex = 0;
    for (const auto &[k, v] : ta)
        if (tb.count(k) && tb.at(k) == v) same_tex++;
    r.set("textures", obj({{"built", num((long long)ta.size())}, {"retail", num((long long)tb.size())}, {"same", num(same_tex)}}));
    auto tags = [](const Pak &q) {
        std::vector<std::tuple<std::string, std::string, Bytes>> o;
        for (const PakItem &it : q.items)
            if (starts(it.type, "TAG")) {
                const uint64_t base = q.pbase(it.page) + it.data_off();
                o.emplace_back(it.type, it.name, Bytes(q.d.begin() + std::ptrdiff_t(base), q.d.begin() + std::ptrdiff_t(std::min<uint64_t>(base + it.size, q.d.size()))));
            }
        std::sort(o.begin(), o.end());
        return o;
    };
    r.set("tags_equal", Value::boolean(tags(a) == tags(b)));
    auto fg_list = [](const std::map<std::string, std::pair<std::string, float>> &f) {
        std::vector<std::pair<std::string, std::string>> o;
        for (const auto &[k, v] : f) o.emplace_back(k, fmt("%.2f", double(v.second)));   // round(lod, 2)
        return o;
    };
    r.set("fg_equal", Value::boolean(fg_list(fa) == fg_list(fb)));
    // the artgroup's bucket keys: 16-byte {u32 key, u32 0xaaaaaaaa, ptr} entries (layout independent)
    auto artgroup_keys = [](const Pak &q) {
        const auto its = q.items_of("ARTGROUP_DATA");
        if (its.empty()) throw std::runtime_error("IndexError: list index out of range");
        const PakItem &it = its[0];
        const uint64_t base = q.pbase(it.page) + it.data_off();
        std::vector<uint32_t> keys;
        for (uint64_t o = 0; o + 15 < it.size; o += 8) {
            const uint32_t k = q.u32(base + o), pad = q.u32(base + o + 4);
            if (k && pad == 0xaaaaaaaa && q.is_ptr(it.page, uint32_t(it.data_off() + o + 8))) keys.push_back(k);
        }
        std::sort(keys.begin(), keys.end());
        return std::make_tuple(keys, q.pu32(it.page, it.data_off()), it.name);
    };
    r.set("artgroup_equal", Value::boolean(artgroup_keys(a) == artgroup_keys(b)));
    auto types = [](const Pak &q) {
        std::map<std::string, long long> c;
        for (const PakItem &i : q.items)
            if (i.type != "RAW_DATA") c[i.type]++;
        return c;
    };
    r.set("types_equal", Value::boolean(types(a) == types(b)));
    auto login = [](const Pak &q) {
        std::vector<std::string> o;
        for (const auto &[pg, off] : q.login) o.push_back(q.item_at(pg, off).type);
        return o;
    };
    r.set("login_types", Value::boolean(login(a) == login(b)));
    return r;
}

}  // namespace

// the retail PS4 mp-hero skins that have a PS4 twin, built from their PS3 files and compared with retail
Value twin_validate(const PS3 &ps3, const PS4 &ps4, const fs::path &work) {
    std::error_code ec;
    fs::create_directories(work, ec);
    Value res = Value::object();
    for (const auto &[name, twin] : TWINS) {
        const fs::path ret = ps4.actor_dir / P(name + ".pak"), c4 = ps4.actor_dir / P(twin + ".pak");
        if (!fs::is_regular_file(ret, ec) || !fs::is_regular_file(c4, ec)) continue;
        std::vector<fs::path> paths;
        for (const std::string &k : {name, twin}) {
            const auto c = ps3.candidates("actor27/" + k + ".pak");
            const fs::path p = work / P("ps3-" + k + ".pak");
            if (!c.empty() && !fs::exists(p, ec)) write_bytes(p, c[0]->read());
            paths.push_back(p);
        }
        const fs::path out = work / P(name + ".pak");
        Value r;
        const auto built = twin_build(paths[0], paths[1], c4, out);
        if (const auto *why = std::get_if<std::string>(&built))
            r = obj({{"no_twin", str(*why)}});
        else {
            const Value notes = std::get<TwinStats>(built).json();
            Value chk;
            chk.kind = Value::Array;
            for (const std::string &e : actor_check(out)) chk.items.push_back(str(e));
            r = obj({{"notes", notes}, {"cmp", twin_compare(out, ret)}, {"ps4check", chk}});
        }
        log(name + " " + dumps(r));
        res.set(name, r);
    }
    write_text(work / "twin-validation.json", dump_indent1(res));
    return res;
}

}  // namespace u2imp
