// u2mp rewrite tests, group "frontend": the scenario state the stand-ins of deps_frontend.cpp answer from.
// One copy (namespace hx::fe), shared by the old and the new build of the stand-ins.
#pragma once
#include <cstdint>
#include <vector>

namespace hx::fe {

struct State {
    // session.cpp
    bool host = true;
    int members = 1;
    uint32_t clock = 0;                  // net_ticks(): set by the test between calls, still within one call
    bool create_clears_phase = false;    // create_match() writes netInfo+0x34 = 0
    const int *install_ref = nullptr;    // session::install: the counter it must receive
    // online.cpp
    bool select_join = false;
    bool mm_online = false;
    int mm_step = 0;
    // menus.cpp / dlc.cpp
    bool playlist = false;
    std::vector<int32_t> types;
    std::vector<int32_t> dlc_off;        // net game indexes dlc_net_game_available() refuses
    // record the pure queries too (is_host, member_count, mm_online)
    bool rec_queries = true;
};
extern State s;

}  // namespace hx::fe
