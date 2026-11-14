// u2mp rewrite tests, group "frontend": stand-ins for what frontend*.cpp call in other modules (session.cpp,
// online.cpp, menus.cpp, dlc.cpp). Compiled for both namespaces; they record the state-changing calls and answer
// from hx::fe::s (deps_frontend.h).
#include <string>
#include <vector>

#include "deps_frontend.h"
#include "frontend3.h"
#include "harness.h"
#include "online.h"
#include "session_api.h"
#include "u2mp_internal.h"

namespace u2mp {

bool playlist_game_types(std::vector<int32_t> &types) {
    hx::rec("playlist_game_types -> %d", hx::fe::s.playlist);
    if (hx::fe::s.playlist) types = hx::fe::s.types;
    return hx::fe::s.playlist;
}

bool dlc_net_game_available(int32_t index) {
    bool ok = true;
    for (int32_t i : hx::fe::s.dlc_off)
        if (i == index) ok = false;
    hx::rec("dlc_net_game_available %d -> %d", index, ok);
    return ok;
}

namespace session {

void install(int &installed) {
    hx::rec("session::install same counter %d (value %d)", &installed == hx::fe::s.install_ref, installed);
    installed += 5;
}
bool is_host() {
    if (hx::fe::s.rec_queries) hx::rec("is_host -> %d", hx::fe::s.host);
    return hx::fe::s.host;
}
int member_count() {
    if (hx::fe::s.rec_queries) hx::rec("member_count -> %d", hx::fe::s.members);
    return hx::fe::s.members;
}
uint32_t net_ticks() { return hx::fe::s.clock; }
void send_message(int slot, const std::vector<uint8_t> &body, int except) {
    std::string b;
    char x[4];
    for (uint8_t c : body) {
        std::snprintf(x, sizeof x, "%02x", c);
        b += x;
    }
    hx::rec("send slot %d [%s] except %d", slot, b.c_str(), except);
}
void create_match() {
    hx::rec("create_match");
    if (hx::fe::s.create_clears_phase) hx::put<int32_t>(0x104f1c8 + 0x34, 0);
}

}  // namespace session

namespace online {

bool select_join(int row) {
    hx::rec("online::select_join %d -> %d", row, hx::fe::s.select_join);
    return hx::fe::s.select_join;
}
bool mm_online() {
    if (hx::fe::s.rec_queries) hx::rec("mm_online -> %d", hx::fe::s.mm_online);
    return hx::fe::s.mm_online;
}
void mm_begin(uint32_t playlist) { hx::rec("mm_begin 0x%x", playlist); }
int mm_step() {
    hx::rec("mm_step -> %d", hx::fe::s.mm_step);
    return hx::fe::s.mm_step;
}
void mm_reset() { hx::rec("mm_reset"); }
void mm_research() { hx::rec("mm_research"); }
void mm_leave() { hx::rec("mm_leave"); }

}  // namespace online

}  // namespace u2mp
