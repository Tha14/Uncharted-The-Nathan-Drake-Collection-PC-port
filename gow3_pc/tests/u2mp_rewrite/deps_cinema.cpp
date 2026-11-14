// u2mp rewrite tests, group cinema: what cinema.cpp needs from other modules (frontend.cpp show_page,
// remaining.cpp cinema_pick_friend). Compiled once per namespace; every call is recorded.
#include "harness.h"
#include "u2mp_internal.h"

namespace u2mp {

void show_page(uint32_t page) { hx::rec("show_page 0x%08x", page); }
void cinema_pick_friend() { hx::rec("cinema_pick_friend"); }

}  // namespace u2mp
