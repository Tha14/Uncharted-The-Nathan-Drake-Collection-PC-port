// u2mp rewrite tests, group "remaining" (remaining.cpp, clantag.cpp): stand-ins for what the sources call in other
// modules (frontend.cpp, session.cpp, texts.cpp, online.cpp, voice.cpp, progression.cpp, the runtime's text input).
// Compiled once per namespace by run.sh; every call is recorded and answered from hx::fk::st (deps_remaining.h).
// The C text-input functions get the OLD_FLAGS renames (..._old) for the old build.
#include "u2mp_internal.h"
#include "online.h"
#include "progression.h"
#include "session_api.h"
#include "texts.h"
#include "voice.h"

#include "deps_remaining.h"
#include "harness.h"

using hx::fk::st;

namespace {
const char *show(const char *s) { return s ? s : "(NULL)"; }
const hx::fk::Friend *fr(int i) { return i >= 0 && size_t(i) < st.friends.size() ? &st.friends[size_t(i)] : nullptr; }
}  // namespace

namespace u2mp {

// frontend.cpp / session.cpp
void show_page(uint32_t page) { hx::rec("show_page 0x%x", page); }
const uint8_t *package_by_id(uint32_t id) {
    hx::rec("package_by_id 0x%x", id);
    auto it = st.packages.find(id);
    return it == st.packages.end() || !it->second ? nullptr : hx::mem + it->second;
}

// texts.cpp
U2MP_GUEST const char *text_find(void *db, uint32_t id) {
    hx::rec("text_find db=%s id=0x%x", hx::where(db).c_str(), id);
    return "PCTEXT";
}
void text_find_original(TextFindFn original) {
    hx::rec("text_find_original %s", hx::where(reinterpret_cast<void *>(original)).c_str());
    st.tramp = reinterpret_cast<void *>(original);
}
const char *own_text(uint32_t id) {
    hx::rec("own_text 0x%x", id);
    if (id == kTextYou) return "(you)";
    if (id == kTextMuted) return "muted";
    if (id == kTextUnmuted) return "unmuted";
    return "own?";
}
void text_set_player(const char *text) { hx::rec("text_set_player [%s]", show(text)); }
void text_set_slot(int slot, const char *text) { hx::rec("text_set_slot %d [%s]", slot, show(text)); }

// session.cpp
namespace session {
bool member_at(int row, uint32_t &net_id, std::string &name) {
    hx::rec("session::member_at %d", row);
    for (const auto &m : st.members)
        if (m.row == row) {
            net_id = m.id;
            name = m.name;
            return true;
        }
    return false;
}
bool member_name(uint32_t net_id, std::string &name) {
    hx::rec("session::member_name %u", net_id);
    for (const auto &m : st.members)
        if (m.id == net_id) {
            name = m.name;
            return true;
        }
    return false;
}
uint32_t local_id() {
    hx::rec("session::local_id");
    return st.local_id;
}
bool is_host() {
    hx::rec("session::is_host");
    return st.is_host;
}
bool kick(uint32_t net_id) {
    hx::rec("session::kick %u", net_id);
    return st.kick_result;
}
std::string local_name() {
    hx::rec("session::local_name");
    return st.local_name;
}
}  // namespace session

// online.cpp
namespace online {
bool active() {
    hx::rec("online::active");
    return st.active;
}
bool linked() {
    hx::rec("online::linked");
    return st.linked;
}
bool promote(uint32_t net_id) {
    hx::rec("online::promote %u", net_id);
    return st.promote_result;
}
bool open_join_list() {
    hx::rec("online::open_join_list");
    return st.open_join_result;
}
int friend_count() {
    hx::rec("online::friend_count");
    return int(st.friends.size());
}
const char *friend_row(int i) {
    hx::rec("online::friend_row %d", i);
    return fr(i) ? fr(i)->row : nullptr;
}
std::string friend_name(int i) {
    hx::rec("online::friend_name %d", i);
    return fr(i) ? fr(i)->name : std::string();
}
bool friend_online(int i) {
    hx::rec("online::friend_online %d", i);
    return fr(i) && fr(i)->online;
}
bool friend_joinable(int i) {
    hx::rec("online::friend_joinable %d", i);
    return fr(i) && fr(i)->joinable;
}
bool join_friend(int i) {
    hx::rec("online::join_friend %d", i);
    return true;
}
bool invite_friend(int i) {
    hx::rec("online::invite_friend %d", i);
    return true;
}
int invite_count() {
    hx::rec("online::invite_count");
    return st.invite_count;
}
int friend_kind(int i) {
    hx::rec("online::friend_kind %d", i);
    return fr(i) ? fr(i)->kind : -1;
}
bool friend_add(const std::string &npid) {
    hx::rec("online::friend_add [%s]", npid.c_str());
    return st.friend_add_result;
}
bool accept_request(int i) {
    hx::rec("online::accept_request %d", i);
    return true;
}
bool remove_friend(int i) {
    hx::rec("online::remove_friend %d", i);
    return true;
}
std::string profile_text(int i) {
    hx::rec("online::profile_text %d", i);
    return fr(i) ? fr(i)->profile : std::string("no profile");
}
bool send_cinema(const std::string &npid) {
    hx::rec("online::send_cinema [%s]", npid.c_str());
    return true;
}
int cinema_received() {
    hx::rec("online::cinema_received");
    return st.cinema_received;
}
}  // namespace online

// voice.cpp
namespace voice {
bool enabled() {
    hx::rec("voice::enabled");
    return st.voice_enabled;
}
bool toggle_mute(uint32_t net_id) {
    hx::rec("voice::toggle_mute %u", net_id);
    return st.toggle_result;
}
}  // namespace voice

// progression.cpp
void progression_publish() { hx::rec("progression_publish"); }
std::vector<std::string> progression_recent_players(size_t max) {
    hx::rec("progression_recent_players %zu", max);
    std::vector<std::string> v = st.recent;
    if (v.size() > max) v.resize(max);
    return v;
}
std::string progression_player_summary(const std::string &name) {
    hx::rec("progression_player_summary [%s]", name.c_str());
    return st.summary;
}

}  // namespace u2mp

// gpu/shim/gow3gpu.cpp: the runtime's text input (renamed *_old for the old build by OLD_FLAGS)
extern "C" int gow3gpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8) {
    hx::rec("text_input_begin [%s] [%s]", show(initial_utf8), show(prompt_utf8));
    return st.begin_result;
}
extern "C" int gow3gpu_text_input_poll(char *out_utf8, uint64_t size) {
    bool zero = true;
    for (uint64_t i = 0; i < size; i++)
        if (out_utf8[i]) zero = false;
    int r = 0;
    if (!st.poll_results.empty()) {
        r = st.poll_results.front();
        st.poll_results.erase(st.poll_results.begin());
    }
    hx::rec("text_input_poll size=%llu zeroed=%d -> %d", (unsigned long long)size, zero ? 1 : 0, r);
    if (r == 1 && size) {
        size_t n = st.poll_text.size() < size - 1 ? st.poll_text.size() : size_t(size - 1);
        std::memcpy(out_utf8, st.poll_text.data(), n);
        out_utf8[n] = 0;
    }
    return r;
}
