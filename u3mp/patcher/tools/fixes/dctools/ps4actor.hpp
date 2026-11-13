// dctools/ps4actor.hpp: PS3 -> PS4 Uncharted 3 foreground actor paks (characters, accessories, props); the actor
// toolset's ps4actor.py (and the post passes of its worker) and the actor-materials variant ps4actor_am.py in C++.
//
// The geometry comes from the PS3 pak (FOREGROUND_SECTION_2 prototypes, blocks merged per material run); materials,
// compiled effects (copied as one span at the template's page offset) and the joint-name table come from a PS4 template
// pak of the same skeleton. One material per template material in the template's order, each re-bound to the
// converted PS3 textures (by texture kind: -color, -normal, -spec-mask, ...; donor paks for textures the PS3 pak
// lacks; neutral flat textures). Props with their own skeleton get the PS3 JOINT_HIERARCHY, ANIMs and
// COLLISION_DATA_FOREGROUND converted (own_skel) or the template's grafted (graft).
//
// The older actor-materials rules (Options::am, the character paks of steps actor-materials / hat-colour /
// airport-igc2): one material per PS3 material, named '<path>:<name>,00000000-0000-0000-0000-<string id>,<shader>',
// on the template material am_choice() picked; textures from the PS3 material table (plus their -normal / -spec-mask
// siblings); the effects after the texture table (page phase only); a page limit of 0x1c0000 and no entry cap; the
// texture cap for 2D textures only; no stream filling, no vertex colour format, prototype cbuffer capacities 2 / 1.
#pragma once
#include "ndpak.hpp"
#include "ps4writer.hpp"

#include <set>

namespace u3::actor {

struct Options {   // the converter's PS4ACTOR_* settings
    bool sb_exact = false, sb_drop = false, fill_streams = true, spread_mats = false, no_tmpl_tex = false;
    int cb3_min = 4;
    std::string drop_mat;
    std::vector<fs::path> donors;
    std::optional<fs::path> skel3;
    // the actor-materials rules
    bool am = false;
    std::map<std::string, std::string> mat_choice;                       // PS3 material -> part of a template material name
    std::map<std::pair<uint32_t, size_t>, std::string> basis_targets;   // blocks (vertices, triangles) whose lost normals and
                                                                        // tangents are rebuilt; every one must be met
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

// the actor-materials choice of template materials for the PS3 materials without an exact match: by role (eyes, mouth,
// hair, head / arm skin, cloth), then the most texture kinds in common with the fewest missing; tint: a PS3 material
// with a tint mask gets a template material that applies the instance tint, one without does not; overlay: no
// template materials with OVERLAY / UV_3 features (they need a second UV set)
std::map<std::string, std::string> am_choice(const fs::path &ps3, const fs::path &tmpl, bool tint, bool overlay);

// EFFECT_TABLE and every item it reaches (EFFECTs, RAW_DATA) of a retail PS4 pak, as one RawSpan to copy (page-0 phase
// kept modulo 0x1000), and those items in page order
std::pair<std::shared_ptr<struct RawSpan>, std::vector<PakItem>> extract_effect_span(const Pak &ref);

}  // namespace u3::actor
