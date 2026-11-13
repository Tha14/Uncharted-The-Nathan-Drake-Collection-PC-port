// dctools/ps4ingame.hpp: the PS3 -> PS4 converter of '-ingame12' level data packages (see ps4ingame.cpp).
#pragma once
#include "dc.hpp"

namespace u3 {

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
Bytes ps4ingame_convert(const class Pak &p, const std::set<std::string> &anim_arts, const std::vector<ExtraSpawner> &extra = {});

}  // namespace u3
