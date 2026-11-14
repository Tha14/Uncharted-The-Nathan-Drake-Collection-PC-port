// dctools/ps4ingame.hpp: the PS3 -> PS4 converter of '-ingame12' level data packages (see ps4ingame.cpp).
#pragma once
#include "dc.hpp"

namespace u3 {

// an Uncharted 2 NAV_MESH_1 layout the converter does not know (the item counts as unsupported)
struct NavMeshUnsupported : Stop {
    using Stop::Stop;
};

// the actor names whose spawners get the animated-object tag rules: retail PS4 actor33 paks with an ANIM item, plus
// the names in conv_json (our converted animated actors)
std::set<std::string> anim_actor_names(const fs::path &retail_actor33, const fs::path &conv_json);
// a spawner added to the SPAWNER_GROUP (wall-emblem): an NdSimpleObject of actor ART at CENTER, with the tag table
// the Airstrip wall-emblem spawners use
struct ExtraSpawner {
    std::string name, art;
    double center[3];
};
// the PS4 package converted from a PS3 '-ingame12' pak (plus the extra spawners, after the converted ones)
Bytes ps4ingame_convert(const fs::path &src, const std::set<std::string> &anim_arts, const std::vector<ExtraSpawner> &extra = {});
// one Uncharted 2 NAV_MESH_1 item in the PS4 layout (throws NavMeshUnsupported)
std::shared_ptr<class Blob> nav_mesh_u2(const class Pak &p, const struct PakItem &it);
// drop_nav: NAV_MESH_1 items left out (the U2 import's --drop-navmesh fallback)
Bytes ps4ingame_convert(const class Pak &p, const std::set<std::string> &anim_arts, const std::vector<ExtraSpawner> &extra = {}, bool drop_nav = false);

}  // namespace u3
