// u2mp rewrite tests, group "session" (session.cpp): stand-ins for what the source calls in other modules
// (net::Transport / net::Address of net.cpp, gamenet, voice, online, matchmaking, frontend6, netheap, netinit,
// netpatch, menuimages, menus, dlc, unlocks, progression, remaining's show_notice, frontend's show_page, the
// runtime's file translation). Compiled once per namespace by run.sh; every call is recorded and answered from
// hx::ss::st (deps_session.h). runtime_file_translate gets the OLD_FLAGS rename (..._old) for the old build.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "frontend3.h"
#include "frontend6.h"
#include "gamenet.h"
#include "net.h"
#include "online.h"
#include "progression.h"
#include "session_api.h"
#include "u2mp_internal.h"
#include "unlocks.h"
#include "voice.h"

#include "deps_session.h"
#include "harness.h"

using hx::ss::st;

namespace u2mp {

namespace session {
void install(int &installed);   // declared in frontend.cpp
}

namespace {
struct FakeLink : net::Link {
    void send(const net::Address &, const uint8_t *, size_t) override {}
    int receive(net::Address &, uint8_t *, size_t) override { return 0; }
} fake_link;

const char *lk(const net::Link *l) { return !l ? "null" : l == &fake_link ? "link" : "other"; }
std::string at(const net::Address &a) {
    char b[48];
    std::snprintf(b, sizeof b, "%08x:%u", a.ip, a.port);
    return b;
}
}  // namespace

// ---- net.cpp -------------------------------------------------------------------------------------------
namespace net {
std::string Address::text() const {
    char b[32];
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&ip);
    std::snprintf(b, sizeof b, "%u.%u.%u.%u:%u", p[0], p[1], p[2], p[3], port);
    return b;
}
bool Address::parse(const std::string &text, uint16_t default_port, Address &out) {
    hx::rec("net::Address::parse [%s] %u", text.c_str(), default_port);
    unsigned a, b, c, d, port = default_port;
    if (!st.parse_ok || std::sscanf(text.c_str(), "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) < 4) return false;
    out.ip = a | b << 8 | c << 16 | d << 24;
    out.port = uint16_t(port);
    return true;
}
Transport::~Transport() {}
bool Transport::host(uint16_t port) {
    hx::rec("transport.host %u", port);
    hosting_ = true;
    local_slot_ = 0;
    return st.host_ok;
}
bool Transport::join(const Address &host_address, uint16_t local_port) {
    hx::rec("transport.join %s local %u", at(host_address).c_str(), local_port);
    hosting_ = false;
    local_slot_ = st.local_slot;
    return st.join_ok;
}
bool Transport::host_link(Link *link) {
    hx::rec("transport.host_link %s", lk(link));
    hosting_ = true;
    local_slot_ = 0;
    return st.host_link_ok;
}
bool Transport::join_link(Link *link, const Address &host_address, uint8_t want_slot) {
    hx::rec("transport.join_link %s %s want %u", lk(link), at(host_address).c_str(), want_slot);
    hosting_ = false;
    local_slot_ = st.local_slot;
    return st.join_link_ok;
}
void Transport::close() { hx::rec("transport.close"); }
void Transport::pump(double now) {
    hx::rec("transport.pump %.4f", now);
    local_slot_ = st.local_slot;
    std::vector<hx::ss::Incoming> in;
    in.swap(st.incoming);
    for (const auto &e : in) {
        if (e.peer) {
            hx::rec("  deliver peer %u %s", e.slot, e.joined ? "joined" : "left");
            if (on_peer) on_peer(e.slot, e.joined);
        } else {
            hx::rec("  deliver message from %u [%s]", e.slot, hx::ss::hex(e.data.data(), e.data.size()).c_str());
            if (on_message) on_message(e.slot, e.data.data(), e.data.size());
        }
    }
}
void Transport::send(uint8_t slot, bool reliable, const uint8_t *data, size_t size) {
    hx::rec("transport.send %u rel %d [%s]", slot, reliable ? 1 : 0, hx::ss::hex(data, size).c_str());
}
void Transport::broadcast(bool reliable, const uint8_t *data, size_t size) {
    hx::rec("transport.broadcast rel %d [%s]", reliable ? 1 : 0, hx::ss::hex(data, size).c_str());
}
bool Transport::connected() const { return true; }
bool Transport::peer_address(uint8_t slot, Address &out) const {
    hx::rec("transport.peer_address %u", slot);
    auto it = st.peer_addr.find(slot);
    if (it == st.peer_addr.end()) return false;
    out.ip = it->second.ip;
    out.port = it->second.port;
    return true;
}
}  // namespace net

// ---- gamenet.cpp -----------------------------------------------------------------------------------------
namespace gamenet {
void set_transport(net::Transport *t) { hx::rec("gamenet::set_transport %s", t ? "set" : "null"); }
int attach(uint8_t slot, uint32_t key, bool direct) {
    hx::rec("gamenet::attach %u %u %d", slot, key, direct ? 1 : 0);
    return 0;
}
void detach(uint8_t slot) { hx::rec("gamenet::detach %u", slot); }
uint64_t message_layer(uint8_t) { return 0; }
bool install() {
    hx::rec("gamenet::install");
    return st.gamenet_install;
}
void frame(double now) { hx::rec("gamenet::frame %.4f", now); }
double clock() {
    hx::rec("gamenet::clock");
    return st.clock;
}
void receive(uint8_t slot, const uint8_t *data, size_t size) {
    hx::rec("gamenet::receive %u [%s]", slot, hx::ss::hex(data, size).c_str());
}
}  // namespace gamenet

// ---- voice.cpp -------------------------------------------------------------------------------------------
namespace voice {
void set_transport(net::Transport *t) { hx::rec("voice::set_transport %s", t ? "set" : "null"); }
void frame() { hx::rec("voice::frame"); }
void receive(uint8_t slot, const uint8_t *data, size_t size) {
    hx::rec("voice::receive %u [%s]", slot, hx::ss::hex(data, size).c_str());
}
void stop() { hx::rec("voice::stop"); }
bool talking(uint32_t id) {
    hx::rec("voice::talking %u", id);
    return st.talking.count(id) != 0;
}
bool muted(uint32_t id) {
    hx::rec("voice::muted %u", id);
    return st.muted.count(id) != 0;
}
}  // namespace voice

// ---- online.cpp ------------------------------------------------------------------------------------------
namespace online {
net::Link *start() {
    hx::rec("online::start");
    return st.link ? &fake_link : nullptr;
}
bool active() {
    hx::rec("online::active");
    return st.online_active;
}
void frame() { hx::rec("online::frame"); }
bool create_match_room() {
    hx::rec("online::create_match_room");
    return st.create_room;
}
uint64_t match_room() {
    hx::rec("online::match_room");
    return st.match_room;
}
void follow_match_room(uint64_t room) { hx::rec("online::follow_match_room %llx", (unsigned long long)room); }
void on_host_reached(uint32_t id) { hx::rec("online::on_host_reached %u", id); }
bool host_change_expected() {
    hx::rec("online::host_change_expected");
    return st.host_change;
}
void on_transport_closed(const char *why) { hx::rec("online::on_transport_closed [%s]", why ? why : "(NULL)"); }
void on_message(uint8_t from, const uint8_t *data, size_t size) {
    hx::rec("online::on_message %u [%s]", from, hx::ss::hex(data, size).c_str());
}
bool on_host_lost() {
    hx::rec("online::on_host_lost");
    if (st.on_host_lost_cb) st.on_host_lost_cb();
    return st.host_lost_taken;
}
std::string my_npid_text() {
    hx::rec("online::my_npid_text");
    return st.my_npid;
}
std::string npid_at(const net::Address &a) {
    hx::rec("online::npid_at %s", at(a).c_str());
    auto it = st.npid_by_ip.find(a.ip);
    return it == st.npid_by_ip.end() ? std::string() : it->second;
}
}  // namespace online

// ---- frontend3.cpp / frontend6.cpp -----------------------------------------------------------------------
namespace matchmaking {
bool installed() {
    hx::rec("matchmaking::installed");
    return st.mm_installed;
}
void leader_start() { hx::rec("matchmaking::leader_start"); }
void on_message(uint8_t from, const uint8_t *data, size_t size) {
    hx::rec("matchmaking::on_message %u [%s]", from, hx::ss::hex(data, size).c_str());
}
void on_peer_joined(uint8_t slot) { hx::rec("matchmaking::on_peer_joined %u", slot); }
void on_migrated(bool host) { hx::rec("matchmaking::on_migrated %d", host ? 1 : 0); }
}  // namespace matchmaking
namespace frontend6 {
bool leader_starting(bool mm) {
    hx::rec("frontend6::leader_starting %d", mm ? 1 : 0);
    return st.leader_starting;
}
}  // namespace frontend6

// ---- the rest ----------------------------------------------------------------------------------------------
void show_notice(const char *text) { hx::rec("show_notice [%s]", text ? text : "(NULL)"); }
void show_page(uint32_t page) { hx::rec("show_page 0x%x", page); }
void netheap_install() { hx::rec("netheap_install"); }
void *netheap_alloc(size_t size, size_t align) {
    hx::rec("netheap_alloc 0x%zx %zu", size, align);
    if (st.netheap_null) return nullptr;
    std::memset(hx::mem + st.netheap_off, 0xcd, size);   // the module must zero it
    return hx::mem + st.netheap_off;
}
void netinit_prepare() { hx::rec("netinit_prepare"); }
void netinit_match() { hx::rec("netinit_match"); }
void netinit_dump(const char *when) { hx::rec("netinit_dump [%s]", when ? when : "(NULL)"); }
int netpatch_load() {
    hx::rec("netpatch_load");
    return 0;
}
const char *extra_net_package() {
    hx::rec("extra_net_package");
    return st.extra_net ? "net-u2mp" : nullptr;
}
int32_t playlist_pick_type() {
    hx::rec("playlist_pick_type");
    return st.playlist_type;
}
bool dlc_net_game_available(int32_t i) {
    hx::rec("dlc_net_game_available %d", i);
    return st.dlc_off.count(i) == 0;
}
uint32_t unlocks_skin_fingerprint() {
    hx::rec("unlocks_skin_fingerprint");
    return st.fingerprint;
}
void progression_publish() { hx::rec("progression_publish"); }

// ---- the entry table for test_session.cpp ---------------------------------------------------------------
void hx_session_api(hx::ss::Api &a) {
    a.install = session::install;
    a.member_at = session::member_at;
    a.member_name = session::member_name;
    a.local_id = session::local_id;
    a.is_host = session::is_host;
    a.kick = session::kick;
    a.local_name = session::local_name;
    a.rank = session::rank;
    a.member_count = session::member_count;
    a.send_message = session::send_message;
    a.net_ticks = session::net_ticks;
    a.create_match = session::create_match;
    a.member_rank = session::member_rank;
    a.online_reopen = [](const hx::ss::Addr *o) {
        net::Address owner;
        if (o) owner.ip = o->ip, owner.port = o->port;
        session::online_reopen(&fake_link, o ? &owner : nullptr);
    };
    a.online_match_ready = session::online_match_ready;
    a.set_local_name = session::set_local_name;
    a.member_address = [](uint32_t id, hx::ss::Addr &out) {
        net::Address a;
        a.ip = 0x11111111, a.port = 1111;   // untouched on false
        const bool r = session::member_address(id, a);
        out.ip = a.ip, out.port = a.port;
        return r;
    };
    a.online_migrated = session::online_migrated;
    a.online_host_gone = session::online_host_gone;
    a.match_migration_ok = session::match_migration_ok;
    a.net_id_of = session::net_id_of;
    a.online_rehost = [](const hx::ss::Addr *o, uint32_t id) {
        net::Address owner;
        if (o) owner.ip = o->ip, owner.port = o->port;
        session::online_rehost(&fake_link, o ? &owner : nullptr, id);
    };
    a.package_by_id = package_by_id;
}

}  // namespace u2mp

// gpu/shim: the runtime's guest path translation (renamed *_old for the old build by flags_session.sh)
extern "C" int runtime_file_translate(const char *guest, char *out, size_t size) {
    hx::rec("runtime_file_translate [%s] %zu", guest ? guest : "(NULL)", size);
    if (size) std::snprintf(out, size, "%s", st.file_path.c_str());
    return st.translate_result;
}
