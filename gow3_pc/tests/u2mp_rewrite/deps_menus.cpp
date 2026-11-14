// u2mp rewrite tests, group "menus" (menus.cpp, menuimages.cpp): stand-ins for what the sources call in other
// modules (session.cpp, online.cpp, voice.cpp, progression.cpp, dlc.cpp). Compiled once per namespace by run.sh;
// every call is recorded and answered from hx::mn::s (deps_menus.h).
#include "u2mp_internal.h"
#include "online.h"
#include "progression.h"
#include "session_api.h"
#include "voice.h"

#include "deps_menus.h"
#include "harness.h"

#include <cstring>

using hx::mn::s;

namespace {
const char *row_of(const std::vector<std::string> &v, int i) {
    return i >= 0 && size_t(i) < v.size() ? v[size_t(i)].c_str() : "(out of range)";
}
}  // namespace

namespace u2mp {

const uint8_t *package_by_id(uint32_t id) {
    hx::rec("package_by_id 0x%x", id);
    return s.package ? hx::mem + s.package : nullptr;
}

bool dlc_net_game_available(int32_t index) {
    bool off = false;
    for (int32_t i : s.dlc_off) off = off || i == index;
    hx::rec("dlc_net_game_available %d -> %d", index, off ? 0 : 1);
    return !off;
}

std::vector<std::string> progression_recent_players(size_t max) {
    hx::rec("progression_recent_players %zu -> %zu", max, s.recent.size());
    return s.recent;
}

namespace session {
std::string local_name() {
    hx::rec("session::local_name");
    return s.local_name;
}
int member_count() {
    hx::rec("session::member_count -> %d", s.members);
    return s.members;
}
bool member_at(int row, uint32_t &net_id, std::string &name) {
    const bool ok = row >= 0 && size_t(row) < s.member_ok.size() && s.member_ok[size_t(row)];
    hx::rec("session::member_at %d -> %d", row, ok ? 1 : 0);
    if (!ok) return false;
    net_id = s.member_id[size_t(row)];
    name = s.member_name[size_t(row)];
    return true;
}
bool is_host() {
    hx::rec("session::is_host -> %d", s.host ? 1 : 0);
    return s.host;
}
}  // namespace session

namespace online {
bool active() {
    hx::rec("online::active -> %d", s.online ? 1 : 0);
    return s.online;
}
int friend_count() {
    hx::rec("online::friend_count -> %d", s.friends);
    return s.friends;
}
bool friend_online(int i) {
    const bool on = i >= 0 && size_t(i) < s.friend_on.size() && s.friend_on[size_t(i)];
    hx::rec("online::friend_online %d -> %d", i, on ? 1 : 0);
    return on;
}
const char *friend_row(int i) {
    hx::rec("online::friend_row %d", i);
    return row_of(s.friend_rows, i);
}
int join_rows() {
    hx::rec("online::join_rows -> %d", s.join_rows);
    return s.join_rows;
}
const char *join_row(int row) {
    hx::rec("online::join_row %d", row);
    return row_of(s.join_row_texts, row);
}
const char *join_title() {
    hx::rec("online::join_title");
    return s.join_title.c_str();
}
}  // namespace online

namespace voice {
float volume() {
    uint32_t b;
    std::memcpy(&b, &s.volume, 4);
    hx::rec("voice::volume -> %08x", b);
    return s.volume;
}
void set_volume(float v) {
    uint32_t b;
    std::memcpy(&b, &v, 4);
    hx::rec("voice::set_volume %08x", b);
}
}  // namespace voice

}  // namespace u2mp
