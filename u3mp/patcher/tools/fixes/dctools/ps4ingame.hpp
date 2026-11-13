// dctools/ps4ingame.hpp: the PS3 -> PS4 converter of '-ingame12' level data packages (see ps4ingame.cpp).
#pragma once
#include "dc.hpp"

namespace u3 {

// the actor names whose spawners get the animated-object tag rules: retail PS4 actor33 paks with an ANIM item, plus
// the names in conv_json (our converted animated actors)
std::set<std::string> anim_actor_names(const fs::path &retail_actor33, const fs::path &conv_json);
// the PS4 package converted from a PS3 '-ingame12' pak
Bytes ps4ingame_convert(const fs::path &src, const std::set<std::string> &anim_arts);

}  // namespace u3
