// dctools/ps4actor.hpp: PS3 -> PS4 Uncharted 3 foreground actor paks (characters, accessories, props); the actor
// toolset's ps4actor.py (and the post passes of its worker) in C++.
//
// The geometry comes from the PS3 pak (FOREGROUND_SECTION_2 prototypes, blocks merged per material run); materials,
// compiled effects (copied as one span at the template's page offset) and the joint-name table come from a PS4 template
// pak of the same skeleton. One material per template material in the template's order, each re-bound to the
// converted PS3 textures (by texture kind: -color, -normal, -spec-mask, ...; donor paks for textures the PS3 pak
// lacks; neutral flat textures). Props with their own skeleton get the PS3 JOINT_HIERARCHY, ANIMs and
// COLLISION_DATA_FOREGROUND converted (own_skel) or the template's grafted (graft).
#pragma once
#include "ndpak.hpp"
#include "ps4writer.hpp"

namespace u3::actor {

struct Options {   // the converter's PS4ACTOR_* settings
    bool sb_exact = false, sb_drop = false, fill_streams = true, spread_mats = false, no_tmpl_tex = false;
    int cb3_min = 4;
    std::string drop_mat;
    std::vector<fs::path> donors;
    std::optional<fs::path> skel3;
};
struct Kw {   // convert()'s keyword arguments
    std::optional<std::string> rename;
    int stream_flags = 3, max_tex = 0;
    bool lod0_only = false, graft = false, mat_by_file = false, match_streams = false, own_skel = false;
    std::optional<fs::path> skel_tmpl;
};
Bytes convert(const fs::path &ps3, const fs::path &tmpl, const Kw &kw, const Options &o);

// the worker's post passes: cubefix (PS3 cube maps on retail-copied descs), cb3fix (prototype scope-3 cbuffer
// capacity), flatfix (the generated flat-color texture sRGB), cubetype (retail cube descs type 2)
Bytes cb3fix(const Bytes &d, uint32_t cap_min = 8);
Bytes vdesc_fix(const Bytes &d, const std::string &what);

// a retail PS4 actor pak under another name (MP twins whose PS3 data equals a PS4 campaign actor): every page is kept,
// renamed strings are appended to the page that held the original and the pointers to them redirected. Renamed: names
// without '/' or ',' in which OLD stands as a whole token (followed by the end, '.' or '-lod'), except inside sound bank,
// texture and effect tables; the PACKAGE_ITEM_LOOKUP id and the ARTGROUP_DATA buckets (re-hashed) follow.
Bytes rename(const fs::path &ps4, const std::string &old_name, const std::string &new_name);

// EFFECT_TABLE and every item it reaches (EFFECTs, RAW_DATA) of a retail PS4 pak, as one RawSpan to copy (page-0 phase
// kept modulo 0x1000), and those items in page order
std::pair<std::shared_ptr<struct RawSpan>, std::vector<PakItem>> extract_effect_span(const Pak &ref);

}  // namespace u3::actor
