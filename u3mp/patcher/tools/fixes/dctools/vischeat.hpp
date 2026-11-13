// dctools/vischeat.hpp: the PS3 -> PS4 converter of "-killvis" level packages (see vischeat.cpp).
#pragma once
#include "dc.hpp"

namespace u3 {
Bytes vischeat_convert(const fs::path &src);   // the PS4 package of a PS3 killvis pak
}  // namespace u3
