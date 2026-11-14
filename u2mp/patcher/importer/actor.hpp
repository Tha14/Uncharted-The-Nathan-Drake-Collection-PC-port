// importer/actor.hpp: the Uncharted 2 actor converters (actor.cpp, actorparts.cpp, actors.cpp, twin.cpp, havok.cpp).
#pragma once
#include "ndpak.hpp"
#include "ps3bg.hpp"
#include "ps4actor.hpp"
#include "ps4writer.hpp"
#include "u2conv.hpp"
#include "u3fix.hpp"   // rd32le, be32, wr32le (dctools fixlib)

#include <functional>
#include <variant>

namespace u2imp {

using u3::Blob;
using u3::Pak;
using u3::PakItem;
using u3::PakWItem;
using u3::Ref;
using u3::ORef;
using BlobP = std::shared_ptr<Blob>;
using u3imp::fmt;   // (u3 has its own)
using ItemP = std::shared_ptr<PakWItem>;

// a U2 PS3 pak seen with the U3 item names: a FOREGROUND_SECTION, or an unnamed PM_COLLECTION_2 {1, ptr}, points at a
// 0x20-byte LOD record {ptr prototype, ...}; resolving those two slots gives the prototype itself
struct U2Src {
    explicit U2Src(const fs::path &path);
    Pak p;
    std::map<Ref, Ref> inner;
    ORef res(uint32_t page, uint64_t off) const;
    std::vector<PakItem> fg_items() const;         // FOREGROUND_SECTION(_2) items (PS3)
    std::vector<PakItem> fg_collections() const;   // the unnamed PM_COLLECTION_2 items of the FG prototypes
    std::set<std::string> types() const;
};

std::set<std::string> ps3_material_names(const U2Src &s);
// the material of each block of a PS3 prototype (a name without '/' gets the folder u2mp/unnamed)
std::vector<std::string> ps3_block_materials(const U2Src &s, const Ref &proto);
uint32_t skeleton_sid(const Pak &p);

// the PS3 actor converted on a PS4 template -> out_path (cbuffer capacities raised)
void convert_actor_pak(const fs::path &ps3_path, const fs::path &tmpl_path, const fs::path &out_path);
void fix_cbuffer_capacity(Bytes &pak);

// ------------------------------------------------------------------------------------------------ actorparts.cpp
struct ArtgroupU2 {
    std::string name;
    std::vector<std::optional<PakItem>> targets;
    uint32_t nb = 0;
    struct M {
        uint32_t sid;
        Bytes f;     // the float, PS4 byte order
        uint32_t k4;
        Bytes data;  // u32 words, PS4 byte order
    };
    std::vector<std::pair<uint32_t, std::vector<M>>> lists;
};
ArtgroupU2 read_artgroup(const Pak &p);
std::vector<std::optional<uint32_t>> keys_for(const std::vector<std::optional<PakItem>> &targets);
BlobP build_artgroup(const std::string &name, const std::vector<ItemP> &entries, const std::vector<std::optional<uint32_t>> &keys, uint32_t nb,
                     const std::vector<std::pair<uint32_t, std::vector<ArtgroupU2::M>>> &lists);
using Made = std::map<std::pair<std::string, std::string>, ItemP>;   // (type, PS3 name) -> PS4 item
std::vector<ItemP> artgroup_entries(const std::vector<std::optional<PakItem>> &targets, const Made &made);
Bytes level_pats(const Pak &src);
BlobP lookup_blob(const std::string &art, const ItemP &artgroup);
std::string proto_name(size_t pi, const std::string &art);
std::string submesh_name(const std::string &art, size_t pi, uint32_t first);
// retail login order: ARTGROUP_DATA / LEVEL_PATS_1 as the PS3 pak has them, then the fixed order, then the tags
std::vector<ItemP> login_order(const Pak &src, std::map<std::string, std::vector<ItemP>> by, const std::vector<ItemP> &tags);
struct FgInfoU2 {
    uint32_t skel, hier, njoints;
    Bytes names;
};
// a prop with its own JOINT_HIERARCHY uses it, else the template's (skins: the shared MP hero skeleton)
template <class F> auto skeleton_info(const U2Src &s, F tmpl) -> decltype(tmpl());
struct ExtraItems {
    Made made;
    std::vector<std::pair<std::string, ItemP>> order;   // (type, item) in creation order
};
ExtraItems extra_items(const U2Src &s, u3::actor::AutoPages &page, const std::vector<std::optional<PakItem>> &targets);
BlobP jh_blob(const Pak &p, const PakItem &it);
BlobP anim_blob(const Pak &p, const PakItem &it);
BlobP fg_coll_blob(const Pak &p, const PakItem &it);
BlobP havok_jointed_blob(const Pak &p, const PakItem &it);
std::optional<Bytes> jh_d_packfile(const Pak &p, const PakItem &it);
Bytes d_wrapper(const Bytes &pf4);
struct JhInfo {
    uint32_t hier, nj;
    Bytes names;   // the joint sids, little endian
};
JhInfo jh_info(const Pak &p, const PakItem &it);
// animation-only paks and loose ragdoll paks
void convert_anim_pak(const fs::path &src, const fs::path &out);
void convert_havok_jointed_pak(const fs::path &src, const fs::path &out);

// ------------------------------------------------------------------------------------------------ havok.cpp
// a Havok 6.1 packfile (PS3) -> 2011.3 (PS4), with the class tables made from the player's executables (work/u2hk)
void havok_configure(const Context &c);
Bytes havok_convert(const Bytes &pf3);

// ------------------------------------------------------------------------------------------------ twin.cpp
// a skin built from its PS4 campaign twin: notes, or why not (no twin build)
std::variant<std::vector<std::string>, std::string> twin_build(const fs::path &ps3_mp, const fs::path &ps3_twin, const fs::path &ps4_twin, const fs::path &out);
void rekey_artgroup(const fs::path &path);
extern const std::map<std::string, std::string> TWINS;

template <class F> auto skeleton_info(const U2Src &s, F tmpl) -> decltype(tmpl()) {
    auto t = tmpl();   // the template's info (the Python converter read it first in any case)
    const auto jh = s.p.items_of("JOINT_HIERARCHY");
    if (jh.empty()) return t;
    const JhInfo j = jh_info(s.p, jh[0]);
    const auto fg = s.fg_items();
    if (fg.empty()) throw u3::Stop("IndexError: no FG section");
    t.skel = s.p.pu32(fg[0].page, fg[0].data_off() + 8);
    t.hier = j.hier;
    t.names = j.names;
    t.njoints = j.nj;
    return t;
}

}  // namespace u2imp
