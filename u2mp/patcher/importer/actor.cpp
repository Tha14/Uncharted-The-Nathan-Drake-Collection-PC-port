// importer/actor.cpp: Uncharted 2 PS3 foreground actors (skins, props) -> PS4.
//
// The geometry comes from the PS3 pak (decoded by dctools ps3bg: prototypes, blocks merged per material run);
// materials, compiled effects (one span copied from the template) and the joint-name table come from a retail PS4
// template pak of the same skeleton. Each PS3 material becomes the template material of the same name, else a
// shadow-blocker one for shadow blockers, else the template's first main material, re-bound to the converted PS3
// textures (by texture kind: -color, -normal, -spec-mask ...; the remastered PS4 texture when the material exists on
// PS4; flat textures for a missing kind).
//
// What Uncharted 2 differs in (found on the PS3/PS4 pairs mp-hero-elena, mp-hero-merc, ...):
//   * item names: U2 PS3 FOREGROUND_SECTION (0x10: ptr LOD record, 0, skeleton sid, f32 lod) and unnamed PM_COLLECTION_2
//     items {1, ptr LOD record}, one per prototype ('mesh.collision.*' ones are collision); a LOD record (0x20 bytes)
//     points at the prototype. PS4 uses FOREGROUND_SECTION_2 / GEOMETRY_1 as U3.
//   * retail U2 PS4 skins carry the second UV set (slot 12, half2) and the vertex colour (slot 4, half4) where the PS3
//     blocks have them; submesh flags are 0 (U3 0x10000 on all but the last LOD); names Prototype0_mp_hero_elena,
//     mp_hero_elena_Prototype0_Submesh0.
//   * ARTGROUP_DATA, LEVEL_PATS_1 (the PS3 value kept), PACKAGE_ITEM_LOOKUP and the login order in the U2 layouts
//     (actorparts.cpp); props carry their own skeleton, animations, collision and sound banks.
//   * a prototype's instance-scope cbuffer capacity is raised to the number of distinct scope-3 cbuffers of the pak
//     (the converter's fixed 2 overflowed MeshInstance's list: the Gompa first-frame crash, u2mp/docs/PORT-RENDER.md).
#include "actor.hpp"

#include <variant>

#include "ps3bg.hpp"
#include "ps4actor.hpp"
#include "texcodec.hpp"

#include <cmath>
#include <cstring>
#include <regex>

namespace u2imp {

using namespace u3;
using u3::actor::TMat;

namespace {

const uint32_t PAD32 = 0xaaaaaaaa;
const std::string PS3_COOK_MARK = "texture1/";

uint32_t sidl(const std::string &latin1) { return u3::sid(bg::utf8_of(latin1)); }
std::string u8s(const std::string &latin1) { return bg::utf8_of(latin1); }
float f32be(const Bytes &d, uint64_t o) {
    const uint32_t v = uint32_t(d.at(o)) << 24 | uint32_t(d.at(o + 1)) << 16 | uint32_t(d.at(o + 2)) << 8 | d.at(o + 3);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// ------------------------------------------------------------------------------------------------ PS3 side
// the material name of each block of a prototype: proto+4 u16 material count, +6 u16 block count, +0x10 the material
// table {ptr material desc, u16 first block, u16 block count, ...}; desc -> material -> name. U2: a name without '/'
// gets the folder 'u2mp/unnamed' (some blockers have none)
std::vector<std::string> block_materials(const U2Src &s, const Ref &proto) {
    const Pak &p = s.p;
    const auto [pg, po] = proto;
    const uint32_t nmat = p.u16(p.pbase(pg) + po + 4), nblk = p.u16(p.pbase(pg) + po + 6);
    const Ref t = bg::need(p, pg, po + 0x10);
    std::vector<std::optional<std::string>> out(nblk);
    for (uint32_t m = 0; m < nmat; m++) {
        const uint64_t e = t.second + 0x10ull * m;
        const Ref sd = bg::need(p, t.first, e);
        const uint32_t first = bg::be16(p, p.pbase(t.first) + e + 4), cnt = bg::be16(p, p.pbase(t.first) + e + 6);
        const Ref mr = bg::need(p, sd.first, sd.second);
        const Ref nr = bg::need(p, mr.first, mr.second);
        const std::string nm = p.pstr(nr.first, nr.second);
        for (uint32_t k = first; k < first + cnt; k++) {
            if (k >= out.size()) throw Stop("IndexError: list assignment index out of range");
            out[k] = nm;
        }
    }
    std::vector<std::string> names;
    for (const auto &n : out) {
        if (!n) throw Stop("AssertionError: a block without material");
        names.push_back(n->find('/') != std::string::npos ? *n : "u2mp/unnamed/" + (n->empty() ? std::string("none") : *n));
    }
    return names;
}

struct Tag {
    std::string type, name;
    Bytes data;
};
std::vector<Tag> read_tags(const Pak &p) {
    std::vector<Tag> out;
    for (const PakItem &it : p.items)
        if (it.type == "TAG_FLOAT" || it.type == "TAG_INT" || it.type == "TAG_VEC4") {
            const uint64_t b = p.pbase(it.page) + it.data_off();
            out.push_back({it.type, it.name, Bytes(p.d.begin() + std::ptrdiff_t(b), p.d.begin() + std::ptrdiff_t(std::min<uint64_t>(b + it.size, p.d.size())))});
        }
    return out;
}

// the artgroup's targets as the U3 reader sees them (the entries resolved on the item's page) and its bucket count
struct Artgroup3 {
    std::string name;
    std::vector<std::optional<PakItem>> targets;
};
Artgroup3 ps3_artgroup(const U2Src &s) {
    const Pak &p = s.p;
    const auto ags = p.items_of("ARTGROUP_DATA");
    if (ags.empty()) throw Stop("IndexError: no ARTGROUP_DATA");
    const PakItem &it = ags[0];
    const uint32_t pg = it.page;
    const uint64_t b = it.data_off();
    const uint32_t n = p.pu32(pg, b);
    const uint64_t ents = bg::need(p, pg, b + 8).second;
    Artgroup3 a;
    a.name = it.name;
    for (uint32_t k = 0; k < n; k++) {
        const ORef r = s.res(pg, ents + 8ull * k);
        a.targets.push_back(r ? std::optional<PakItem>(p.item_at(r->first, uint32_t(r->second))) : std::nullopt);
    }
    return a;
}

// ------------------------------------------------------------------------------------------------ PS4 template
struct FgInfo {
    uint32_t skel, hier, njoints;
    Bytes names;
};
FgInfo tmpl_fg_info(const Pak &q) {
    const auto fs_ = q.items_of("FOREGROUND_SECTION_2");
    if (fs_.empty()) throw Stop("IndexError: no FOREGROUND_SECTION_2 in the template");
    const PakItem &it = fs_[0];
    const uint64_t b = q.pbase(it.page) + it.data_off();
    FgInfo f;
    f.skel = rd32le(q.d, b + 8);
    f.hier = rd32le(q.d, b + 12);
    const Ref jr = bg::need(q, it.page, it.data_off() + 0x10);
    const PakItem jitem = q.item_at(jr.first, uint32_t(jr.second - 0x20));
    const uint64_t jb = q.pbase(jr.first) + jr.second;
    f.names = Bytes(q.d.begin() + std::ptrdiff_t(jb), q.d.begin() + std::ptrdiff_t(std::min<uint64_t>(jb + jitem.size, q.d.size())));
    const auto gs = q.items_of("GEOMETRY_1");
    if (gs.empty()) throw Stop("IndexError: no GEOMETRY_1 in the template");
    const uint64_t sm = bg::need(q, gs[0].page, gs[0].data_off() + 0x10).second;
    f.njoints = q.pu32(gs[0].page, sm + 0x58);
    return f;
}

// ------------------------------------------------------------------------------------------------ helpers
const std::vector<std::pair<std::string, std::vector<std::string>>> TEX_KINDS = {
    {"-detail-normal", {"g_tNdFetchDetail3Map", "g_tNdFetchDetail1Map", "g_tNdFetchDetail2Map"}},
    {"-spec-mask", {"g_tNdFetchMask4Map"}},
    {"-tint-mask", {"g_tNdFetchColorTintMask1Map"}},
    {"-normal", {"g_tNdFetchNormal1Map", "g_tNdFetchNormal2Map"}},
    {"-color", {"g_tNdFetchColor1Map", "g_tNdFetchColor2Map"}},
};
std::optional<std::string> tex_kind(const std::string &shrt) {
    std::string base = shrt.substr(0, shrt.find(".tga"));
    base = base.substr(0, base.find(".dds"));
    for (const auto &[suf, _] : TEX_KINDS)
        if ((base.size() >= suf.size() && base.compare(base.size() - suf.size(), suf.size(), suf) == 0) || base.find(suf + "-") != std::string::npos) return suf;
    return std::nullopt;
}
std::set<std::string> split_set(const std::string &s, const std::string &seps) {
    std::set<std::string> out;
    size_t a = 0;
    for (size_t i = 0; i <= s.size(); i++)
        if (i == s.size() || seps.find(s[i]) != std::string::npos) {
            out.insert(s.substr(a, i - a));
            a = i + 1;
        }
    return out;
}
std::string dir_of(const std::string &n) { return n.substr(0, n.rfind('/')); }
std::string last_of(const std::string &n) { return n.substr(n.rfind('/') + 1); }
std::string key_of(const std::string &mn) { return dir_of(mn) + ":" + last_of(mn); }
std::string before_comma(const std::string &s) { return s.substr(0, s.find(',')); }

struct TexE {
    uint32_t hash;
    std::string shrt, full;
    std::optional<std::string> kind;
    std::shared_ptr<PakWItem> desc, name;
};
using TexP = std::shared_ptr<TexE>;
struct Ps3Tex {   // a PS3 texture whose VRAM_DESC is made the first time a material binds it
    std::string shrt, full;
    uint32_t hash;
    std::optional<std::string> kind;
    uint32_t fmt4, w, h, mips;
    Bytes tiled;
    TexP real;
};

// the GEOMETRY_1 texture entries and bindings name a texture by its full path without '.ndb'
std::string bindfull(const std::string &full) { return full.size() >= 4 && full.compare(full.size() - 4, 4, ".ndb") == 0 ? full.substr(0, full.size() - 4) : full; }

struct SpanTarget {
    std::shared_ptr<RawSpan> span;
    size_t pos;
};

}  // namespace

std::vector<std::string> ps3_block_materials(const U2Src &s, const Ref &proto) { return block_materials(s, proto); }

// ------------------------------------------------------------------------------------------------ U2 source view
U2Src::U2Src(const fs::path &path) : p(path) {
    if (!p.ps3) return;
    for (const PakItem &i : p.items) {
        std::optional<Ref> slot;
        if (i.type == "FOREGROUND_SECTION") slot = Ref{i.page, i.data_off()};
        else if (i.type == "PM_COLLECTION_2" && i.name.empty() && p.pu32(i.page, i.data_off()) == 1) slot = Ref{i.page, i.data_off() + 4};
        if (!slot) continue;
        const ORef rec = bg::res(p, slot->first, slot->second);
        const ORef in = rec ? bg::res(p, rec->first, rec->second) : std::nullopt;
        if (in) inner[*slot] = *in;
    }
}
ORef U2Src::res(uint32_t page, uint64_t off) const {
    if (auto it = inner.find(Ref{page, off}); it != inner.end()) return it->second;
    return bg::res(p, page, off);
}
std::vector<PakItem> U2Src::fg_items() const {
    if (!p.ps3) return p.items_of("FOREGROUND_SECTION_2");
    std::vector<PakItem> v;
    for (const PakItem &i : p.items)
        if (i.type == "FOREGROUND_SECTION" || i.type == "FOREGROUND_SECTION_2") v.push_back(i);
    return v;
}
std::vector<PakItem> U2Src::fg_collections() const {
    std::set<Ref> fg;
    for (const PakItem &i : fg_items())
        if (const ORef r = res(i.page, i.data_off())) fg.insert(*r);
    std::vector<PakItem> out;
    for (const PakItem &i : p.items)
        if (i.type == "PM_COLLECTION_2" && i.name.empty() && p.pu32(i.page, i.data_off()) == 1)
            if (const ORef r = res(i.page, i.data_off() + 4); r && fg.count(*r)) out.push_back(i);
    return out;
}
std::set<std::string> U2Src::types() const {
    std::set<std::string> t;
    for (const PakItem &i : p.items) t.insert(i.type);
    return t;
}

std::set<std::string> ps3_material_names(const U2Src &s) {
    std::set<std::string> names;
    for (const PakItem &i : s.fg_collections()) {
        const ORef pr = s.res(i.page, i.data_off() + 4);
        if (!pr) continue;
        try {
            for (const std::string &n : block_materials(s, *pr)) names.insert(n);
        } catch (const std::exception &) {
        }
    }
    return names;
}

uint32_t skeleton_sid(const Pak &p) {
    if (p.ps3) {
        for (const PakItem &i : p.items)
            if (i.type == "FOREGROUND_SECTION" || i.type == "FOREGROUND_SECTION_2") return p.pu32(i.page, i.data_off() + 8);
        throw Stop("IndexError: no FOREGROUND_SECTION");
    }
    const auto fs_ = p.items_of("FOREGROUND_SECTION_2");
    if (fs_.empty()) throw Stop("IndexError: no FOREGROUND_SECTION_2");
    return rd32le(p.d, p.pbase(fs_[0].page) + fs_[0].data_off() + 8);
}

// ------------------------------------------------------------------------------------------------ conversion
void convert_actor_pak(const fs::path &ps3_path, const fs::path &tmpl_path, const fs::path &out_path) {
    const U2Src s(ps3_path);
    const Pak &src = s.p;
    const Pak T(tmpl_path);
    if (!src.ps3 || T.ps3) throw Stop("AssertionError: a PS3 actor and a PS4 template");
    const Artgroup3 ag3 = ps3_artgroup(s);
    const std::string art = ag3.name;
    const std::vector<TMat> tmats = actor::tmpl_materials(T);
    const FgInfo tinfo = tmpl_fg_info(T);
    const FgInfo fi = skeleton_info(s, [&] { return tinfo; });
    std::vector<const TMat *> sb_tmpl, main_tmpl;
    for (const TMat &m : tmats) (m.name.find("shadowblocker") != std::string::npos ? sb_tmpl : main_tmpl).push_back(&m);

    // ---------------- geometry
    struct ProtoInfo {
        Ref proto;
        std::vector<Ref> blocks;
    };
    std::vector<std::pair<std::string, ProtoInfo>> protos;   // FG name -> prototype (a later name replaces, first position)
    for (const PakItem &it : s.fg_items()) {
        const Ref pr = bg::need(s.res(it.page, it.data_off()));
        const uint32_t nsub = src.u16(src.pbase(pr.first) + pr.second + 6);
        const Ref t = bg::need(src, pr.first, pr.second + 0x14);
        ProtoInfo x{pr, {}};
        for (uint32_t k = 0; k < nsub; k++)
            if (const ORef r = bg::res(src, t.first, t.second + 4ull * k)) x.blocks.push_back(*r);
        auto e = std::find_if(protos.begin(), protos.end(), [&](const auto &y) { return y.first == it.name; });
        if (e == protos.end()) protos.push_back({it.name, x});
        else e->second = x;
    }
    const std::vector<PakItem> fg_items = s.fg_items();
    std::vector<Ref> proto_list;   // the prototypes in collection order
    for (const PakItem &pm : s.fg_collections()) proto_list.push_back(bg::need(s.res(pm.page, pm.data_off() + 4)));
    std::map<Ref, size_t> pindex;
    for (size_t i = 0; i < proto_list.size(); i++) pindex[proto_list[i]] = i;
    struct Sub {
        std::string mat;
        uint32_t first;
        std::vector<bg::FgBlock> blocks;
    };
    std::vector<std::vector<Sub>> proto_out;
    std::vector<std::string> used_mats;
    for (const Ref &pr : proto_list) {
        const ProtoInfo *info = nullptr;
        for (const auto &[n, v] : protos)
            if (v.proto == pr) {
                info = &v;
                break;
            }
        if (!info) throw Stop("IndexError: list index out of range");
        const std::vector<std::string> mnames = block_materials(s, pr);
        std::vector<bg::FgBlock> blocks;
        for (const Ref &b : info->blocks) blocks.push_back(bg::decode_fg_block(src, b.first, b.second));
        std::vector<Sub> subs;
        for (size_t bi = 0; bi < blocks.size() && bi < mnames.size(); bi++) {   // zip
            bg::FgBlock &blk = blocks[bi];
            if (!subs.empty() && subs.back().mat == mnames[bi]) subs.back().blocks.push_back(std::move(blk));
            else subs.push_back({mnames[bi], uint32_t(bi), {}}), subs.back().blocks.push_back(std::move(blk));
        }
        for (const Sub &x : subs)
            if (std::find(used_mats.begin(), used_mats.end(), x.mat) == used_mats.end()) used_mats.push_back(x.mat);
        proto_out.push_back(std::move(subs));
    }

    // ---------------- textures
    PakWriter w;
    actor::AutoPages p0(w, std::nullopt, 0x1c0000);
    Bytes vram;
    std::vector<TexP> tex_entries;
    auto add_texture = [&](const std::string &shrt, uint32_t hash, uint32_t fmt4, uint32_t w_, uint32_t h_, uint32_t mips, const Bytes &tiled,
                           std::optional<uint32_t> flags, std::optional<uint32_t> unk20) {
        while (vram.size() % 0x100) vram.push_back(0);
        const uint64_t voff = vram.size();
        vram.insert(vram.end(), tiled.begin(), tiled.end());
        auto nameb = std::make_shared<Blob>("texname");
        nameb->cstr(u8s(shrt));
        auto name_item = p0.item("RAW_DATA", nameb);
        const std::string full4 = cook_prefix() + shrt;
        auto db = std::make_shared<Blob>("vramdesc");
        db->u64(0).u64(voff).u32(tiled.size()).u32(hash);
        db->u32(1).u32(fmt4);
        db->u32(unk20 ? *unk20 : (fmt4 == 0x48 || fmt4 == 0x4b || fmt4 == 0x4e ? 0x010f0000 : 0x01000000));
        db->u32(mips).u32(w_).u32(h_).u32(0).u32(flags ? *flags : 3);
        db->ptr(db, 0x40);
        db->cstr(u8s(full4));
        auto desc_item = p0.item("VRAM_DESC", db);
        auto e = std::make_shared<TexE>(TexE{hash, shrt, full4, tex_kind(shrt), desc_item, name_item});
        tex_entries.push_back(e);
        return e;
    };
    std::vector<std::shared_ptr<Ps3Tex>> ps3tex;
    for (const PakTexture &t : src.textures()) {
        const size_t k3 = t.path.find(PS3_COOK_MARK);
        const std::string shrt = k3 == std::string::npos ? t.path : t.path.substr(k3 + PS3_COOK_MARK.size());
        const auto f3 = tc::ps3_format(t);
        if (!f3 || !f3->compressed) continue;
        const Bytes texels = src.vram_bytes(t.vramOffset, t.vramSize);
        const Bytes tiled = tc::ps3_to_ps4_texels(texels, t.width, t.height, t.mips, f3->bpb);
        const auto k = tex_kind(shrt);
        const bool color = k && *k == "-color";
        uint32_t fmt4;
        if (f3->kind == "DXT1") fmt4 = color ? 0x48 : 0x47;
        else if (f3->kind == "DXT5") fmt4 = color ? 0x4e : 0x4d;
        else if (f3->kind == "DXT3") fmt4 = color ? 0x4b : 0x4a;
        else throw Unsupported("NotImplementedError: " + f3->kind);
        ps3tex.push_back(std::make_shared<Ps3Tex>(Ps3Tex{shrt, cook_prefix() + shrt, t.hash, k, fmt4, t.width, t.height, t.mips, tiled, nullptr}));
    }
    // a binding target: a texture entry (made) or a PS3 texture (made on first use)
    struct Ref_ {
        TexP made;
        std::shared_ptr<Ps3Tex> lazy;
        explicit operator bool() const { return made || lazy; }
    };
    auto use = [&](const Ref_ &e) -> TexP {
        if (e.made) return e.made;
        if (!e.lazy) return nullptr;
        if (!e.lazy->real) e.lazy->real = add_texture(e.lazy->shrt, e.lazy->hash, e.lazy->fmt4, e.lazy->w, e.lazy->h, e.lazy->mips, e.lazy->tiled, std::nullopt, std::nullopt);
        return e.lazy->real;
    };
    std::map<std::string, TexP> flat_cache;
    auto flat_texture = [&](const std::string &kind) {
        if (auto it = flat_cache.find(kind); it != flat_cache.end()) return it->second;
        static const std::map<std::string, std::array<uint32_t, 3>> RGB = {
            {"-spec-mask", {40, 40, 40}}, {"-normal", {128, 128, 255}}, {"-color", {128, 128, 128}}, {"-tint-mask", {0, 0, 0}}, {"-detail-normal", {128, 128, 255}}};
        const auto [lin, mips] = actor::bc1_flat(64, 64, RGB.at(kind).data());
        const Bytes tiled = tc::ps3_to_ps4_texels(lin, 64, 64, mips, 8);
        const std::string shrt = fmt("art/generated/u3mp/flat%s.tga/%016llX.ndb", kind.c_str(), (unsigned long long)sidl(kind));
        TexP e = add_texture(shrt, sidl(shrt), 0x47, 64, 64, mips, tiled, std::nullopt, std::nullopt);
        flat_cache[kind] = e;
        return e;
    };
    std::map<std::string, PakTexture> ttex;
    for (const PakTexture &t : T.textures()) ttex[t.path] = t;
    std::map<std::string, TexP> tcache;
    std::set<std::string> tnone;
    auto tmpl_texture = [&](std::string tfull) -> TexP {
        if (auto it = tcache.find(tfull); it != tcache.end()) return it->second;
        if (tnone.count(tfull)) return nullptr;
        auto it = ttex.find(tfull);
        if (it == ttex.end()) it = ttex.find(tfull + ".ndb");
        if (it == ttex.end()) {
            tnone.insert(tfull);
            return nullptr;
        }
        const PakTexture &t = it->second;
        tfull = t.path;
        const size_t k = tfull.find(cook_prefix());
        const std::string shrt = k == std::string::npos ? tfull : tfull.substr(k + cook_prefix().size());
        TexP e = add_texture(shrt, t.hash, t.format, t.width, t.height, t.mips, T.vram_bytes(t.vramOffset, t.vramSize), t.unk34, t.unk20);
        e->full = tfull;
        tcache[tfull] = e;
        return e;
    };
    auto ttab = p0.item("TEXTURE_TABLE");

    // ---------------- materials (template copies, re-bound)
    auto pick_template = [&](const std::string &mname) -> const TMat & {
        const std::string key = key_of(mname);
        for (const TMat &m : tmats)
            if (before_comma(m.name) == key) return m;
        if (mname.find("shadowblocker") != std::string::npos && !sb_tmpl.empty()) return *sb_tmpl[0];
        if (main_tmpl.empty()) throw Stop("IndexError: the template has no main material");
        return *main_tmpl[0];
    };
    auto textures_for = [&](const std::string &mname) {
        const std::string stem = last_of(dir_of(mname));   // .../<file>/<material>
        std::vector<std::shared_ptr<Ps3Tex>> own;
        for (const auto &e : ps3tex)
            if (e->shrt.find(stem) != std::string::npos) own.push_back(e);
        if (own.empty()) {
            std::set<std::string> toks = split_set(stem, "-/");
            for (const char *x : {"mp", "a", "b", "c", "00", "01"}) toks.erase(x);
            std::vector<std::pair<size_t, std::shared_ptr<Ps3Tex>>> sc;
            for (const auto &e : ps3tex) {
                const std::set<std::string> t2 = split_set(e->shrt, "-/.");
                size_t n = 0;
                for (const std::string &t : toks) n += t2.count(t);
                sc.push_back({n, e});
            }
            std::stable_sort(sc.begin(), sc.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            for (size_t i = 0; i < sc.size() && i < 4; i++) own.push_back(sc[i].second);
        }
        return own;
    };
    struct Bind {
        std::string pname;
        TexP e;
        std::string tshort, tfull;
    };
    struct MatRec {
        std::string ps3, name;
        const TMat *tm;
        std::vector<Bind> binds;
    };
    std::vector<MatRec> mat_recs;
    for (const std::string &mn : used_mats) {
        const TMat &tm = pick_template(mn);
        const auto own = textures_for(mn);
        const bool exact = before_comma(tm.name) == key_of(mn);
        std::map<std::string, std::shared_ptr<Ps3Tex>> by_path;
        for (const auto &x : ps3tex) by_path[dir_of(x->shrt)] = x;
        MatRec rec{mn, "", &tm, {}};
        for (const auto &b : tm.binds) {
            std::vector<std::string> want;
            for (const auto &[suf, ps] : TEX_KINDS)
                if (std::find(ps.begin(), ps.end(), b.pname) != ps.end()) want.push_back(suf);
            Ref_ e;
            if (exact) {
                if (TexP t = tmpl_texture(b.full)) e.made = t;
                else if (auto it = by_path.find(b.shrt); it != by_path.end()) e.lazy = it->second;
            }
            if (e) {
            } else if (auto it = by_path.find(b.shrt); it != by_path.end())
                e.lazy = it->second;
            else if (!want.empty()) {
                std::shared_ptr<Ps3Tex> c;
                for (const auto &x : own)
                    if (x->kind && *x->kind == want[0]) {
                        c = x;
                        break;
                    }
                if (c) e.lazy = c;
                else if (TexP t = tmpl_texture(b.full)) e.made = t;
                else e.made = flat_texture(want[0]);
            } else
                e.made = tmpl_texture(b.full);   // environment maps, cornea, hair noise ...: the template's texture
            rec.binds.push_back({b.pname, use(e), b.shrt, b.full});
        }
        rec.name = dir_of(mn) + ":" + last_of(mn) + fmt(",00000000-0000-0000-0000-%012llx,", (unsigned long long)sidl(mn)) + tm.shader;
        mat_recs.push_back(std::move(rec));
    }
    {   // the texture table (every texture, by hash)
        std::vector<TexP> order = tex_entries;
        std::stable_sort(order.begin(), order.end(), [](const TexP &a, const TexP &b) { return a->hash < b->hash; });
        auto namesb = std::make_shared<Blob>("ttnames");
        for (const TexP &e : order) namesb->ptr(e->name->data);
        auto names_item = p0.item("RAW_DATA", namesb);
        auto hashb = std::make_shared<Blob>("tthash");
        for (const TexP &e : order) hashb->u32(e->hash);
        auto hash_item = p0.item("RAW_DATA", hashb);
        auto tb = ttab->data;
        tb->u64(order.size()).ptr(hash_item->data).ptr(names_item->data);
        tb->ptr(tb, 0x20);
        for (const TexP &e : order) tb->ptr(e->desc->data);
    }

    // ---------------- effects (verbatim from the template)
    auto [span, span_items] = actor::extract_effect_span(T);
    p0.span(span);

    // ---------------- geometry buffers
    auto strs = std::make_shared<Blob>("geostrings");
    std::map<std::string, size_t> spool;
    auto S = [&](const std::string &x) -> int64_t {
        auto it = spool.find(x);
        if (it == spool.end()) it = spool.emplace(x, strs->cstr(u8s(x))).first;
        return int64_t(it->second);
    };
    auto raw = [&](const Bytes &data, const std::string &name = "") {
        auto b = std::make_shared<Blob>("raw");
        b->raw(data);
        return p0.item("RAW_DATA", b, name)->data;
    };
    auto fmtb = std::make_shared<Blob>("vtxfmt");
    // slots 0 position, 1 uv, 2 normal, 3 tangent; U2 adds 12 (second uv) and 4 (vertex colour)
    const std::vector<std::pair<int, std::array<uint8_t, 4>>> FMT = {
        {0, {0, 0, 0x0c, 0x06}}, {1, {1, 0, 0x04, 0x22}}, {2, {2, 0, 0x08, 0x0a}}, {3, {3, 0, 0x08, 0x0a}}, {12, {12, 0, 0x04, 0x22}}, {4, {4, 0, 0x08, 0x0a}}};
    std::map<int, size_t> fmt_off;
    for (const auto &[slot, f] : FMT) {
        fmt_off[slot] = fmtb->size();
        fmtb->raw(Bytes(f.begin(), f.end()));
    }
    p0.item("RAW_DATA", fmtb);
    auto halfs = [](const std::vector<double> &v) {
        Bytes o;
        for (double x : v) {
            const uint16_t h = actor::f16b(x);
            o.push_back(uint8_t(h));
            o.push_back(uint8_t(h >> 8));
        }
        return o;
    };
    // the rows of an attribute over a submesh's blocks, with `extra` columns of 1.0 appended (concatenate + ones)
    auto rows = [](const std::vector<bg::FgBlock> &bl, const char *k, size_t pad_to) {
        std::vector<double> out;
        for (const auto &b : bl) {
            const Mat &m = *b.get(k);
            for (size_t r = 0; r < m.rows; r++) {
                for (size_t c = 0; c < m.cols; c++) out.push_back(m.at(r, c));
                for (size_t c = m.cols; c < pad_to; c++) out.push_back(1.0);
            }
        }
        return out;
    };
    auto all_have = [](const std::vector<bg::FgBlock> &bl, const char *k) {
        for (const auto &b : bl)
            if (!b.get(k)) return false;
        return true;
    };
    struct SubRec {
        std::string name, mat;
        uint32_t nv, ni, ns, flags;
        std::shared_ptr<PakWItem> st, skin;
        std::shared_ptr<Blob> ib;
        bool sb;
    };
    std::vector<SubRec> sub_records;
    struct ProtoRec {
        std::string name;
        std::vector<std::string> subnames;
        size_t off = 0;
    };
    std::vector<ProtoRec> proto_records;
    for (size_t pi = 0; pi < proto_out.size(); pi++) {
        ProtoRec pr{proto_name(pi, art), {}, 0};
        for (const Sub &x : proto_out[pi]) {
            uint32_t nv = 0;
            for (const auto &b : x.blocks) nv += b.nverts;
            Bytes pos;
            for (const auto &b : x.blocks) {
                const Mat &m = *b.get("pos");
                for (double v : m.v) {
                    const float f = float(v);
                    uint8_t q[4];
                    std::memcpy(q, &f, 4);
                    pos.insert(pos.end(), q, q + 4);
                }
            }
            Bytes idx;
            uint32_t base = 0;
            for (const auto &b : x.blocks) {
                for (const auto &t : b.tris)
                    for (uint32_t v : t) {
                        const uint16_t u = uint16_t(v + base);
                        idx.push_back(uint8_t(u));
                        idx.push_back(uint8_t(u >> 8));
                    }
                base += b.nverts;
            }
            std::vector<std::tuple<int, int, Bytes>> streams = {{0, 12, pos}};
            if (all_have(x.blocks, "uv0")) streams.push_back({1, 4, halfs(rows(x.blocks, "uv0", 0))});
            if (all_have(x.blocks, "uv1")) streams.push_back({12, 4, halfs(rows(x.blocks, "uv1", 0))});
            if (all_have(x.blocks, "normal")) {
                std::vector<double> n;
                for (const auto &b : x.blocks) {
                    const Mat &m = *b.get("normal");
                    for (size_t r = 0; r < m.rows; r++) {
                        for (size_t c = 0; c < m.cols; c++) n.push_back(m.at(r, c));
                        n.push_back(1.0);
                    }
                }
                streams.push_back({2, 8, halfs(n)});
            }
            if (all_have(x.blocks, "tangent")) {
                const size_t cols = x.blocks[0].get("tangent")->cols;
                streams.push_back({3, 8, halfs(rows(x.blocks, "tangent", cols == 3 ? 4 : cols))});
            }
            if (all_have(x.blocks, "color0")) {
                std::vector<double> c = rows(x.blocks, "color0", 0);
                const size_t cols = x.blocks[0].get("color0")->cols;
                double mn = c.empty() ? 0 : c[0];
                for (double v : c) mn = std::min(mn, v);
                if (mn < 0)
                    for (double &v : c) v = (v + 1.0) / 2.0;
                if (cols == 3) {
                    std::vector<double> c4;
                    for (size_t i = 0; i < c.size(); i += 3) c4.insert(c4.end(), {c[i], c[i + 1], c[i + 2], 1.0});
                    c = c4;
                }
                streams.push_back({4, 8, halfs(c)});
            }
            std::shared_ptr<PakWItem> sk_item;
            std::shared_ptr<Blob> ib;
            bool skinned = true;
            for (const auto &b : x.blocks) skinned = skinned && b.has_skin;
            struct SkinRaw {
                uint32_t total;
                std::shared_ptr<Blob> counts, offs, joints, wts;
            };
            std::optional<SkinRaw> skin;
            if (skinned) {
                Bytes counts, offs, joints, wts;
                uint64_t total = 0;
                for (const auto &b : x.blocks)
                    for (const auto &inf : b.skin) {
                        const uint16_t c = uint16_t(inf.size());
                        counts.push_back(uint8_t(c)), counts.push_back(uint8_t(c >> 8));
                        offs.push_back(uint8_t(total)), offs.push_back(uint8_t(total >> 8));
                        total += c;
                        for (const auto &[j, wv] : inf) {
                            joints.push_back(uint8_t(j)), joints.push_back(uint8_t(j >> 8));
                            const float f = float(double(wv) / 255.0);
                            uint8_t q[4];
                            std::memcpy(q, &f, 4);
                            wts.insert(wts.end(), q, q + 4);
                        }
                    }
                skin = SkinRaw{uint32_t(total), raw(counts), raw(offs), raw(joints), raw(wts)};
            }
            ib = raw(idx);
            auto st = std::make_shared<Blob>("streams");
            for (const auto &[slot, stride, data] : streams) {
                auto db = raw(data);
                st->u32((uint32_t(stride) << 16) | 1).u32(data.size()).ptr(fmtb, int64_t(fmt_off.at(slot))).ptr(db);
            }
            auto st_item = p0.item("RAW_DATA", st);
            if (skin) {
                auto skb = std::make_shared<Blob>("skin");
                skb->u32(0).u32(skin->total).ptr(skin->counts).ptr(skin->offs).ptr(skin->joints).ptr(skin->wts);
                sk_item = p0.item("RAW_DATA", skb);
            }
            const MatRec *mrec = nullptr;
            for (const MatRec &m : mat_recs)
                if (m.ps3 == x.mat) {
                    mrec = &m;
                    break;
                }
            const std::string sname = submesh_name(art, pi, x.first);
            pr.subnames.push_back(sname);
            const bool sb = x.mat.find("shadowblocker") != std::string::npos;
            sub_records.push_back({sname, mrec->name, nv, uint32_t(idx.size() / 2), uint32_t(streams.size()), sb ? 0x01000100u : 0u, st_item, sk_item, ib, sb});
        }
        proto_records.push_back(std::move(pr));
    }
    auto subs_blob = std::make_shared<Blob>("submeshes");
    for (const SubRec &r : sub_records) {
        Blob &b = *subs_blob;
        b.u64(0).ptr(strs, S(r.name)).u32(1).u32(r.flags).u32(r.sb ? 1 : 0).u32(0);
        b.u32(r.nv).u32(r.ni).u32(r.ns).u32(0).ptr(r.st->data).u64(0).ptr(r.ib);
        b.ptr(strs, S(r.mat)).i32(-1).u32(0).u32(fi.njoints).u32(0);
        b.ptr(r.skin ? r.skin->data : nullptr);
        b.raw(Bytes(0x20, 0));
    }
    p0.item("RAW_DATA", subs_blob);
    auto protob = std::make_shared<Blob>("prototypes"), plist = std::make_shared<Blob>("protolist"), flagsb = std::make_shared<Blob>("protoflags"),
         snl = std::make_shared<Blob>("subnames");
    for (ProtoRec &pr : proto_records) {
        const size_t foff = flagsb->size();
        flagsb->raw(Bytes{1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0});
        const size_t soff = snl->size();
        for (const std::string &n : pr.subnames) snl->ptr(strs, S(n)).i32(-1).u32(0);
        pr.off = protob->size();
        protob->u32(3).u32(pr.subnames.size()).u32(0).u32(0x5791a421);
        protob->ptr(strs, S(pr.name)).ptr(snl, int64_t(soff)).ptr(flagsb, int64_t(foff)).u64(0);
        protob->u32(2).u32(1);
        protob->raw(Bytes(0x78 - 0x38, 0));
    }
    for (const ProtoRec &pr : proto_records) plist->ptr(protob, int64_t(pr.off));
    for (const auto &b : {protob, flagsb, snl, plist}) p0.item("RAW_DATA", b);
    // the textures the materials bind (GEOMETRY_1 texture entries)
    auto texb = std::make_shared<Blob>("geotextures");
    std::vector<TexP> gtex;
    for (const MatRec &m : mat_recs)
        for (const Bind &b : m.binds)
            if (b.e && std::find(gtex.begin(), gtex.end(), b.e) == gtex.end()) gtex.push_back(b.e);
    for (const TexP &e : gtex) texb->ptr(strs, S(bindfull(e->full))).u32(e->hash).u32(0x200).u32(0x101).i32(-1).u32(0).u32(4).u64(0);
    auto featb = std::make_shared<Blob>("features"), parb = std::make_shared<Blob>("params"), valb = std::make_shared<Blob>("paramvalues"),
         bindb = std::make_shared<Blob>("bindings"), matb = std::make_shared<Blob>("materials");
    for (const MatRec &m : mat_recs) {
        const TMat &tm = *m.tm;
        const size_t foff = featb->size();
        for (const std::string &f : tm.feats) featb->ptr(strs, S(f));
        const size_t poff = parb->size();
        for (const auto &pa : tm.params) {
            const size_t voff = valb->size();
            valb->raw(pa.vals);
            parb->ptr(strs, S(pa.name)).ptr(valb, int64_t(voff)).u32(pa.cnt).u32(0);
        }
        const size_t boff = bindb->size();
        for (const Bind &b : m.binds) {
            std::string shrt, full;
            if (b.e) {
                shrt = b.e->shrt.size() >= 4 && b.e->shrt.compare(b.e->shrt.size() - 4, 4, ".ndb") == 0 ? dir_of(b.e->shrt) : b.e->shrt;
                full = bindfull(b.e->full);
            } else
                shrt = b.tshort, full = b.tfull;
            bindb->ptr(strs, S(b.pname)).ptr(strs, S(shrt)).ptr(strs, S(full)).i32(-1).u32(0).u64(0);
        }
        matb->ptr(strs, S(m.name)).ptr(strs, S(tm.shader));
        matb->ptr(tm.feats.empty() ? nullptr : featb, int64_t(foff)).ptr(strs, S(tm.hash));
        matb->ptr(tm.params.empty() ? nullptr : parb, int64_t(poff)).ptr(m.binds.empty() ? nullptr : bindb, int64_t(boff));
        matb->u32(tm.feats.size()).u32(tm.params.size()).u32(m.binds.size()).u32(tm.x3);
        matb->u32(tm.tail).u32(0);
    }
    for (const auto &b : {featb, parb, valb, bindb, matb, texb})
        if (b->size()) p0.item("RAW_DATA", b);
    p0.item("RAW_DATA", strs);
    auto gb = std::make_shared<Blob>("geometry");
    gb->u32(0x10).u32(1);
    gb->u64(sub_records.size()).ptr(subs_blob);
    gb->u64(proto_records.size()).ptr(plist);
    gb->u64(0).ptr(nullptr);
    gb->u64(gtex.size()).ptr(gtex.empty() ? nullptr : texb);
    gb->u64(mat_recs.size()).ptr(matb);
    gb->u64(0).u64(0);
    auto geo = p0.item("GEOMETRY_1", gb);

    // ---------------- FOREGROUND_SECTION_2 (+ joint names)
    std::vector<std::shared_ptr<PakWItem>> fg_out;
    for (const PakItem &it : fg_items) {
        const Ref pr = bg::need(s.res(it.page, it.data_off()));
        const float lod = f32be(src.d, src.pbase(it.page) + it.data_off() + 0xc);
        const size_t pi = pindex.at(pr);
        auto jn = raw(fi.names, "debugMeshJointNameLoc");
        auto fb = std::make_shared<Blob>("fg");
        fb->ptr(fb, 0x1c).u32(fi.skel).u32(fi.hier).ptr(jn);
        uint8_t q[4];
        std::memcpy(q, &lod, 4);
        fb->raw(Bytes(q, q + 4));
        fb->cstr(u8s(proto_records[pi].name));
        fg_out.push_back(p0.item("FOREGROUND_SECTION_2", fb, u8s(it.name)));
    }
    // ---------------- tags
    std::vector<std::shared_ptr<PakWItem>> tag_items;
    for (const Tag &t : read_tags(src)) {
        auto tb = std::make_shared<Blob>("tag");
        Bytes d = t.data;
        const size_t n = t.type == "TAG_VEC4" ? 4 : 1;
        if (d.size() < 4 * n) throw Stop("struct.error: tag size");
        Bytes o;
        for (size_t k = 0; k < n; k++)
            for (int j = 3; j >= 0; j--) o.push_back(d[4 * k + size_t(j)]);
        tb->raw(o);
        tag_items.push_back(p0.item(t.type, tb, u8s(t.name)));
    }
    // ---------------- U2: the items a prop carries, the U2 artgroup
    const ExtraItems ex = extra_items(s, p0, ag3.targets);
    const ArtgroupU2 ag = read_artgroup(src);
    auto artgroup = p0.item("ARTGROUP_DATA", build_artgroup(ag.name, artgroup_entries(ag.targets, ex.made), keys_for(ag.targets), ag.nb, ag.lists), u8s(art));
    auto lp = std::make_shared<Blob>("levelpats");
    lp->raw(level_pats(src));
    auto levelpats = p0.item("LEVEL_PATS_1", lp);
    auto pkglookup = p0.item("PACKAGE_ITEM_LOOKUP", lookup_blob(art, artgroup));
    // ---------------- page 1 + login
    Page &p1 = w.page();
    auto span_item = std::make_shared<PakWItem>();
    span_item->type = "EFFECT_TABLE";
    span_item->span_ref = span;
    for (const PakItem &i : span_items)
        if (i.type == "EFFECT_TABLE") {
            span_item->span_pos = i.off - span_items[0].off;
            break;
        }
    std::map<std::string, std::vector<std::shared_ptr<PakWItem>>> by = {{"ARTGROUP_DATA", {artgroup}}, {"LEVEL_PATS_1", {levelpats}}, {"EFFECT_TABLE", {span_item}},
                                                                        {"TEXTURE_TABLE", {ttab}},       {"GEOMETRY_1", {geo}},          {"FOREGROUND_SECTION_2", fg_out},
                                                                        {"PACKAGE_ITEM_LOOKUP", {pkglookup}}};
    for (const auto &[t, it] : ex.order) by[t].push_back(it);
    std::vector<std::shared_ptr<PakWItem>> descs;
    for (const TexP &e : tex_entries) descs.push_back(e->desc);
    make_login_and_vram_page(w, p1, login_order(src, by, tag_items), descs);
    w.vram = vram;
    Bytes data = w.build();
    for (const auto &pg : w.pages)   // break the self-reference cycles
        for (auto &part : pg->parts)
            if (auto *it = std::get_if<std::shared_ptr<PakWItem>>(&part)) (*it)->data->relocs.clear();
    fix_cbuffer_capacity(data);
    write_bytes(out_path, data);
}

// ------------------------------------------------------------------------------------------------ cbuffer capacity
// A GEOMETRY_1 prototype states at +0x30 how many instance-scope (scope 3) constant buffers its materials' programs
// declare; MeshInstance::Init allocates that many slots per instance and appends one per distinct cbuffer unchecked.
// Every prototype states at least the number of distinct scope-3 cbuffer records of the pak ({ptr members, u32 n, u32
// size, u32 name, u32 scope sid 0x6bbcf890, u32 id, u32 1, ...}).
namespace {
const uint32_t CB_SCOPE_INSTANCE = 0x6bbcf890;
size_t scope3_cbuffers(const Bytes &d) {
    std::set<std::pair<uint32_t, uint32_t>> out;
    for (size_t i = 0; i + 4 <= d.size(); i++) {
        if (rd32le(d, i) != CB_SCOPE_INSTANCE) continue;
        if (i >= 4 && i % 4 == 0 && i + 12 <= d.size()) {
            const uint32_t name = rd32le(d, i - 4), cid = rd32le(d, i + 4), one = rd32le(d, i + 8);
            if (one == 1 && name && cid) out.insert({name, cid});
        }
    }
    return out.size();
}
}  // namespace
void fix_cbuffer_capacity(Bytes &d) {
    const Pak q(d, "converted");
    const size_t need = scope3_cbuffers(q.d);
    const auto gs = q.items_of("GEOMETRY_1");
    if (gs.empty()) return;
    const ORef pl = bg::res(q, gs[0].page, gs[0].data_off() + 0x20);
    const uint64_t n = rd64le(q.d, q.pbase(gs[0].page) + gs[0].data_off() + 0x18);
    for (uint64_t i = 0; i < (pl ? n : 0); i++) {
        const Ref r = bg::need(q, pl->first, pl->second + 8 * i);
        const uint64_t off = q.pbase(r.first) + r.second + 0x30;
        if (rd32le(q.d, off) < need) wr32le(&d[off], uint32_t(need));
    }
}

}  // namespace u2imp
