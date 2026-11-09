// u2mp online: Uncharted 2 online play (rooms + peer-to-peer), the PC counterpart of what the PS3 GOTY
// build (NPUA80698 1.10) did with NP Matching2, NP Signaling and NP Basic (u2mp/docs/PORT-MATCHMAKING.md;
// this port: u2mp/docs/PORT-ONLINE.md).
//
// The room service is u2_online.dll (u2_online_abi.h, loaded at run time; it ships separately). The
// launcher signs the player in and hands the session to the game through UNCHARTED_U2MP_RELAY; the DLL
// path is UNCHARTED_U2MP_ONLINE_DLL. Without either, every function here is a no-op and the session layer
// plays over LAN as before.
//
// Model (the LAN session layer, session.cpp, stays the core): the transport (net::Transport, a star with
// the party leader / match host in the middle) runs over the DLL's peer-to-peer socket instead of a UDP
// socket of its own (net::Link below). The room owner hosts the transport, the other members are its
// clients. So everything that already works over LAN (roster, follow, matchmaking vote, game events,
// voice) works unchanged online; this file only decides who hosts and connects the players:
//   - Multiplayer entered: link up (u2o_open, U2O_READY, u2o_find_world), then this PC's party room
//     (PS3 0x4c48d0: 10 slots, OWNER_AUTO_GRANT) - searchable here, PS3 hid it and used invites.
//   - Start Match, custom game (PS3 0x444bb0 -> CreateJoinRoom 0x4d421c): the match room with the PS3
//     attribute set (1.3), then match+0x99 (session::online_match_ready). Party members join it too.
//   - Invites button -> join list (PS3 BUTTON_MULTIPLAYER_JOIN 0x4655b0 = PS4 0x6095d0 + SearchRoom
//     0x4d3c50, 0x4C == version): the rooms found; selecting one (SELECT_JOIN_GAME 0x465808 = 0x609680)
//     checks its free slots (the PS3 reservation 0x100/0x101, simplified) and joins it (JoinRoom 0x4d4578).
//     The transport then joins the room owner; a party leader takes its party along.
//   - Member data 0x59 (48 bytes, PS3 0x4af240 = PS4 0x64e810), room hide (0x4d5338) and state 0x53
//     (0x4d3af8) follow NET_SM.
// No owner change or kick from the service: a host leaving ends the match as over LAN (the transport
// loses its host); kicks travel as session messages (session.cpp kKick).
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "frontend3.h"
#include "online.h"
#include "session_api.h"
#include "u2_online_abi.h"
#include "u2mp_internal.h"

namespace u2mp::online {
namespace {

// ---- guest data (PS4 1.02 image offsets) ------------------------------------------------------
constexpr uint64_t kNetState = 0x14ce97c;     // NET_SM (PS3 0xf11764)
constexpr uint64_t kMatch = 0x1065b08;        // g_matchSession (PS3 0xf20c48)
constexpr uint64_t kNetInfo = 0x104f1c8;      // PS3 0xf1fa48
constexpr uint64_t kNpd = 0x11474f8;          // NetPlayerData
constexpr uint64_t M_SEARCH_DONE = 0xa8, M_SEARCH_RESULT = 0xac;   // read by PAGE_MULTIPLAYER_JOIN 0x609640
constexpr uint64_t NI_VERSION = 0x00, NI_ERROR = 0x1c, NI_MODE = 0x2c4;
constexpr uint64_t NPD_TEAM = 0x114;          // custom game team preference 1 / 2 (PS3 NetPlayerData+211)
constexpr uint64_t fButtonJoin = 0x6095d0;    // BUTTON_MULTIPLAYER_JOIN (PS3 0x4655b0), intact but SearchRoom
constexpr uint64_t fPopPage = 0x5cc6f0;
constexpr uint32_t kPageJoin = 0xe8d59d98;    // PAGE_MULTIPLAYER_JOIN
static_assert(sid("PAGE_MULTIPLAYER_JOIN") == kPageJoin, "page id");
// NET_SM values
enum : int32_t { kServerLobby = 16, kChooseHostJoin = 18, kUpdate = 26, kReset = 27, kReplay = 28, kResults = 29,
                 kRoundResults = 30 };

// ---- room service values (PORT-MATCHMAKING.md 1.3) ---------------------------------------------
constexpr char kCommunicationId[] = "NPWR00462_00";   // retail NPWR00462 (PS3 0xe161b0)
constexpr uint16_t kVport = 1;                       // the one virtual port of the match traffic
constexpr uint32_t kFlagOwnerAutoGrant = 0x80000000u, kFlagClosed = 0x40000000u, kFlagHidden = 0x10000000u;
constexpr uint16_t A_KEY = 0x4c, A_PARTY_SIZE = 0x4d, A_LAT = 0x4e, A_LON = 0x4f, A_REGION = 0x50, A_KIND = 0x52,
                   A_STATE = 0x53, A_LATLON = 0x55, A_NAME = 0x58;
// 0x52 (searchable int 7) is unused by PS3; here it tells the join list what a room is.
enum : uint32_t { kKindParty = 1, kKindCustom = 2 };
// The 0x4C key: PS3 netInfo+0 (DC 6f1c3f87[0] | 0x90000); PS4 never fills it, then this PC value.
constexpr uint32_t kPcNetVersion = 0x00090000u | 0x0102u;
constexpr uint32_t kMaxSlot = 10;                    // custom (0x49e620, inlined on PS4) and party: 10 players
constexpr size_t kBlobSize = 48;                     // member bin 0x59 (PS3 0x4af240)
constexpr double kJoinTimeout = 20.0, kCreateTimeout = 15.0, kSwitchDelay = 0.3, kRttLogPeriod = 10.0;
// online message ops (session message type session::kOnlineMessage)
enum : uint8_t { kOpFollowRoom = 1 };   // leader -> party: {u64 room id}: I joined that room, come along

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---- the DLL ----------------------------------------------------------------------------------
struct Api {
    HMODULE dll = nullptr;
    u2o_abi_fn abi = nullptr;
    u2o_open_fn open = nullptr;
    u2o_state_fn state = nullptr;
    u2o_poll_fn poll = nullptr;
    u2o_close_fn close = nullptr;
    u2o_find_world_fn find_world = nullptr;
    u2o_create_room_fn create_room = nullptr;
    u2o_join_room_fn join_room = nullptr;
    u2o_leave_room_fn leave_room = nullptr;
    u2o_search_rooms_fn search_rooms = nullptr;
    u2o_set_room_external_fn set_room_external = nullptr;
    u2o_set_room_internal_fn set_room_internal = nullptr;
    u2o_set_member_internal_fn set_member_internal = nullptr;
    u2o_peer_address_fn peer_address = nullptr;
    u2o_peer_connect_fn peer_connect = nullptr;
    u2o_peer_disconnect_fn peer_disconnect = nullptr;
    u2o_peer_state_fn peer_state = nullptr;
    u2o_send_fn send = nullptr;
    u2o_recv_fn recv = nullptr;
} api;

template <typename F> bool resolve(F &out, const char *name) {
    out = reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(api.dll, name)));
    if (!out) log("online: off (u2_online.dll has no %s): LAN only", name);
    return out != nullptr;
}

bool load() {
    static int tried = 0;   // the environment is fixed for the process: decide once
    if (tried) return api.dll != nullptr;
    tried = 1;
    if (const char *join = std::getenv("UNCHARTED_U2MP_JOIN"); join && *join) {
        log("online: off (UNCHARTED_U2MP_JOIN is set: LAN join by address)");
        return false;
    }
    wchar_t path[1024];
    const DWORD n = GetEnvironmentVariableW(L"UNCHARTED_U2MP_ONLINE_DLL", path, DWORD(sizeof path / sizeof *path));
    if (!n || n >= sizeof path / sizeof *path) {
        log("online: off (UNCHARTED_U2MP_ONLINE_DLL not set: no online extension): LAN only");
        return false;
    }
    if (const char *relay = std::getenv("UNCHARTED_U2MP_RELAY"); !relay || !*relay) {
        log("online: off (UNCHARTED_U2MP_RELAY not set: not signed in to RPCN in the launcher): LAN only");
        return false;
    }
    api.dll = LoadLibraryW(path);
    if (!api.dll) {
        log("online: off (u2_online.dll did not load, Windows error %lu): LAN only", (unsigned long)GetLastError());
        return false;
    }
    const bool ok = resolve(api.abi, "u2o_abi") && resolve(api.open, "u2o_open") && resolve(api.state, "u2o_state") &&
                    resolve(api.poll, "u2o_poll") && resolve(api.close, "u2o_close") &&
                    resolve(api.find_world, "u2o_find_world") && resolve(api.create_room, "u2o_create_room") &&
                    resolve(api.join_room, "u2o_join_room") && resolve(api.leave_room, "u2o_leave_room") &&
                    resolve(api.search_rooms, "u2o_search_rooms") &&
                    resolve(api.set_room_external, "u2o_set_room_external") &&
                    resolve(api.set_room_internal, "u2o_set_room_internal") &&
                    resolve(api.set_member_internal, "u2o_set_member_internal") &&
                    resolve(api.peer_address, "u2o_peer_address") && resolve(api.peer_connect, "u2o_peer_connect") &&
                    resolve(api.peer_disconnect, "u2o_peer_disconnect") && resolve(api.peer_state, "u2o_peer_state") &&
                    resolve(api.send, "u2o_send") && resolve(api.recv, "u2o_recv");
    if (ok && api.abi() != U2O_ABI) log("online: off (u2_online.dll ABI %d, this build needs %d): LAN only", api.abi(), int(U2O_ABI));
    if (!ok || api.abi() != U2O_ABI) {
        FreeLibrary(api.dll);
        api = Api{};
        return false;
    }
    log("online: u2_online.dll loaded (ABI %d)", int(U2O_ABI));
    return true;
}

// ---- state -------------------------------------------------------------------------------------
struct Member {
    uint16_t id = 0;
    std::string npid, name;
};
struct Room {
    uint64_t id = 0;
    uint32_t kind = 0;
    uint16_t my_id = 0, owner_id = 0, max_slot = 0;
    uint32_t flags = 0;
    bool hidden = false;
    int32_t state53 = 0;
    bool lobby_started = false;   // 0x53 = 1 sent for this lobby (PS3 0x465fe8)
    std::array<uint8_t, kBlobSize> sent_blob{};
    bool blob_sent = false;
    std::vector<Member> members;
    bool owned() const { return id && my_id && my_id == owner_id; }
    const Member *member(uint16_t mid) const {
        for (const Member &m : members)
            if (m.id == mid) return &m;
        return nullptr;
    }
};

enum class Req { World, CreateParty, CreateMatch, JoinList, JoinLeader, JoinMatchMember, Leave, Search, SetExternal,
                 SetInternal, SetMember };
struct Pending {
    Req kind;
    uint64_t room;
    uint32_t room_kind = 0;
    bool lead_party = false;   // JoinList: we led a party with others, it follows
    std::string what;
};

struct Peer {
    std::string npid;
    int32_t state = -1;
    uint32_t rtt_us = 0;
};

bool started = false;          // Multiplayer entered with online on (the transport runs over the link)
int32_t link_state = U2O_OFF;
bool link_opened = false;
uint32_t world = 0;
uint16_t world_server = 0;
uint64_t world_req = 0;
double next_world_try = 0;
Room party_room, match_room_;
std::map<uint64_t, Pending> pending;
std::vector<Peer> peers;
std::string my_npid;

// join list
std::vector<U2oRoomExt> found;
bool searching = false;
uint64_t search_req = 0;
char row_text[U2O_MAX_ROOMS][96];

// timers / flows
uint64_t create_party_req = 0, create_match_req = 0;
bool match_waiting = false;    // Start Match waits for the match room
double match_deadline = 0;
bool waiting_host = false;     // the transport joins a room owner
double host_deadline = 0;
uint64_t waiting_room = 0;
bool following_leader = false; // the old host closes on purpose (session.cpp host_change_expected)
net::Address switch_owner{};
double switch_at = 0;          // the leader switches its transport once the follow went out
double next_rtt_log = 0, next_blob_check = 0;
double join_page_deadline = 0;  // reached a host from the join list: no follow in 3 s -> back to PAGE_MULTIPLAYER
int32_t last_net_state = -1;
bool in_match = false;

const char *link_name(int32_t s) {
    switch (s) {
    case U2O_OFF: return "off";
    case U2O_CONNECTING: return "connecting";
    case U2O_READY: return "ready";
    case U2O_FAILED: return "failed";
    default: return "?";
    }
}
const char *peer_state_name(uint32_t s) {
    switch (s) {
    case U2O_PEER_INACTIVE: return "inactive";
    case U2O_PEER_PENDING: return "pending";
    case U2O_PEER_ACTIVE: return "active";
    case U2O_PEER_DEAD: return "dead";
    default: return "?";
    }
}
const char *kind_name(uint32_t k) { return k == kKindParty ? "party" : k == kKindCustom ? "custom game" : "room"; }
const char *error_hint(int32_t e) {
    switch (e) {
    case 0: return "ok";
    case 14: return "no such room";
    case 16: return "room full";
    case 26: return "player not online";
    default: return "room service error";
    }
}
// Peer addresses are logged without their last two bytes.
std::string masked(const uint8_t ip[4], uint16_t port) {
    char s[40];
    std::snprintf(s, sizeof s, "%u.%u.x.x:%u", ip[0], ip[1], port);
    return s;
}
std::string npid_text(const char *npid) { return std::string(npid, strnlen(npid, 20)); }

uint32_t room_key() {
    const uint32_t v = G<uint32_t>(kNetInfo + NI_VERSION);
    return v ? v : kPcNetVersion;
}

uint32_t top_page_id() {
    const uint64_t menu = addr::menu_manager;
    const int32_t top = G<int32_t>(menu + 0x1010);
    if (top < 0 || top > 0x40) return 0;
    const uint64_t page = G<uint64_t>(menu + 0x9d0 + 8 * uint64_t(top));
    return page ? *reinterpret_cast<const uint32_t *>(uintptr_t(page) + 0x10) : 0;
}

// ---- peers and the link --------------------------------------------------------------------------
int peer_index(const std::string &npid, bool create) {
    for (size_t i = 0; i < peers.size(); ++i)
        if (peers[i].npid == npid) return int(i);
    if (!create || npid.empty()) return -1;
    peers.push_back(Peer{npid});
    return int(peers.size() - 1);
}
net::Address address_of(int index) { return net::Address{uint32_t(index + 1), kVport}; }
const Peer *peer_at(const net::Address &a) {
    if (a.port != kVport || a.ip == 0 || a.ip > peers.size()) return nullptr;
    return &peers[a.ip - 1];
}

class OnlineLink final : public net::Link {
public:
    void send(const net::Address &to, const uint8_t *data, size_t size) override {
        const Peer *p = peer_at(to);
        if (!p || !api.send) return;
        api.send(p->npid.c_str(), kVport, kVport, data, uint32_t(size));
    }
    int receive(net::Address &from, uint8_t *data, size_t capacity) override {
        if (!api.recv) return 0;
        char npid[20] = {};
        const int n = api.recv(kVport, data, uint32_t(capacity), npid);
        if (n == -1) {
            static bool once = false;
            if (!once) log("online: a datagram larger than %zu bytes was dropped", capacity);
            once = true;
            return 0;
        }
        if (n <= 0) return 0;
        from = address_of(peer_index(npid_text(npid), true));
        return n;
    }
} link;

void connect_member(const U2oMember &m, uint16_t my_member_id, uint64_t room) {
    const std::string npid = npid_text(m.npid);
    if (npid.empty() || m.id == my_member_id || npid == my_npid) return;
    peer_index(npid, true);
    const bool no_address = !m.ip[0] && !m.ip[1] && !m.ip[2] && !m.ip[3];
    if (no_address) {
        const uint64_t r = api.peer_address(npid.c_str());
        log("online: peer %s (%s) in room %llx: no address in the room data, asked for it (request %llu)", npid.c_str(),
            m.name, (unsigned long long)room, (unsigned long long)r);
        return;
    }
    const int r = api.peer_connect(npid.c_str(), m.ip, m.port);
    log("online: peer %s (%s) in room %llx: connecting to %s (u2o_peer_connect %d)", npid.c_str(), m.name,
        (unsigned long long)room, masked(m.ip, m.port).c_str(), r);
}

// ---- rooms ---------------------------------------------------------------------------------------
void take_room(Room &r, const U2oRoom &src, uint32_t kind) {
    r.id = src.room_id;
    r.kind = kind;
    r.my_id = src.my_id;
    r.owner_id = src.owner_id;
    r.max_slot = src.max_slot;
    r.flags = src.flag_attr;
    r.hidden = (src.flag_attr & kFlagHidden) != 0;
    r.state53 = 0;
    r.lobby_started = false;
    r.blob_sent = false;
    r.members.clear();
    for (uint32_t i = 0; i < src.member_count && i < U2O_MAX_MEMBERS; ++i)
        r.members.push_back(Member{src.members[i].id, npid_text(src.members[i].npid), src.members[i].name});
}
bool in_room(const Room &r, const std::string &npid) {
    for (const Member &m : r.members)
        if (m.npid == npid) return true;
    return false;
}
Room *room_by_id(uint64_t id) {
    if (id && party_room.id == id) return &party_room;
    if (id && match_room_.id == id) return &match_room_;
    return nullptr;
}
void log_room(const char *what, const Room &r) {
    log("online: %s %s room %llx: %zu member(s) of %u, owner member %u, our member id %u, flags 0x%x", what,
        kind_name(r.kind), (unsigned long long)r.id, r.members.size(), unsigned(r.max_slot), unsigned(r.owner_id),
        unsigned(r.my_id), r.flags);
    for (const Member &m : r.members)
        log("online:   member %u %s (%s)%s", unsigned(m.id), m.name.c_str(), m.npid.c_str(),
            m.id == r.owner_id ? " owner" : "");
}

uint64_t request(uint64_t id, Pending p) {
    if (id) pending[id] = std::move(p);
    return id;
}

void leave_room(Room &r, const char *why) {
    if (!r.id) return;
    const uint64_t id = api.leave_room(r.id);
    log("online: leaving %s room %llx (%s): request %llu", kind_name(r.kind), (unsigned long long)r.id, why,
        (unsigned long long)id);
    request(id, Pending{Req::Leave, r.id, r.kind, false, "leave"});
    r = Room{};
}

std::array<uint8_t, kBlobSize> member_blob() {
    // PS3 0x4af240 (PORT-MATCHMAKING.md 1.8): +0 party room id (party of more than one), +8 rank badge,
    // +9 team preference. +10..+47 (NetPlayerData stats / address words, G) are not known on PS4: 0.
    std::array<uint8_t, kBlobSize> b{};
    const uint64_t party = party_room.id && party_room.members.size() > 1 ? party_room.id : 0;
    std::memcpy(b.data(), &party, 8);
    b[8] = uint8_t(session::member_rank(session::local_id()));
    const int32_t team = G<int32_t>(kNpd + NPD_TEAM);
    b[9] = team == 1 || team == 2 ? uint8_t(team) : 0;
    return b;
}

void publish_blob(Room &r, bool force) {
    if (!r.id || link_state != U2O_READY) return;
    const auto b = member_blob();
    if (!force && r.blob_sent && b == r.sent_blob) return;
    const uint64_t id = api.set_member_internal(r.id, r.my_id, 0, b.data(), uint32_t(b.size()));
    log("online: member data 0x59 to %s room %llx: party room %llx, rank %u, team %u (request %llu)", kind_name(r.kind),
        (unsigned long long)r.id, (unsigned long long)(party_room.members.size() > 1 ? party_room.id : 0),
        unsigned(b[8]), unsigned(b[9]), (unsigned long long)id);
    request(id, Pending{Req::SetMember, r.id, r.kind, false, "member data 0x59"});
    if (id) {
        r.sent_blob = b;
        r.blob_sent = true;
    }
}

uint64_t create_room(uint32_t kind) {
    // PS3 CreateJoinRoom 0x4d421c: searchable ints 0x4C key, 0x4D party size, 0x4E/0x4F lat/lon (no
    // location service: 0 on every PC), 0x50 region (0), 0x52 kind (PC), 0x53 state; bin 0x55 lat/lon
    // floats (8 bytes, 0), bin 0x58 room name "%s.%lld"; member bin 0x59.
    static uint8_t latlon[8];
    static char name[64];
    std::snprintf(name, sizeof name, "%s.%lld", session::local_name().c_str(), (long long)std::time(nullptr));
    const uint32_t party_size = uint32_t(std::max(1, session::member_count()));
    const U2oAttr attrs[] = {
        {A_KEY, 0, room_key(), nullptr},          {A_PARTY_SIZE, 0, party_size, nullptr},
        {A_LAT, 0, 0, nullptr},                   {A_LON, 0, 0, nullptr},
        {A_REGION, 0, 0, nullptr},                {A_KIND, 0, kind, nullptr},
        {A_STATE, 0, 0, nullptr},                 {A_LATLON, uint16_t(sizeof latlon), 0, latlon},
        {A_NAME, uint16_t(std::strlen(name)), 0, reinterpret_cast<const uint8_t *>(name)},
    };
    const auto blob = member_blob();
    U2oCreate c{};
    c.max_slot = kMaxSlot;
    c.flag_attr = kind == kKindParty ? kFlagOwnerAutoGrant : 0;   // match rooms: no auto grant (PS3)
    c.attrs = attrs;
    c.attr_count = uint32_t(sizeof attrs / sizeof *attrs);
    c.member_bin = blob.data();
    c.member_bin_len = uint32_t(blob.size());
    c.team = 0;
    c.signaling = 1;   // mesh: the room service hands out every member's peer address
    const uint64_t id = api.create_room(world, &c);
    log("online: creating the %s room in world %u: key 0x%x, party size %u, %u slots, name %s (request %llu)",
        kind_name(kind), world, room_key(), party_size, kMaxSlot, name, (unsigned long long)id);
    request(id, Pending{kind == kKindParty ? Req::CreateParty : Req::CreateMatch, 0, kind, false, "create"});
    return id;
}

void set_hidden(Room &r, bool on) {
    // PS3 Hide 0x4d5338: flag filter CLOSED | HIDDEN, attr both or none.
    const uint64_t id = api.set_room_internal(r.id, kFlagClosed | kFlagHidden, on ? kFlagClosed | kFlagHidden : 0, nullptr, 0);
    log("online: %s room %llx %s (request %llu)", kind_name(r.kind), (unsigned long long)r.id,
        on ? "hidden and closed (match running)" : "shown and open again (lobby)", (unsigned long long)id);
    request(id, Pending{Req::SetInternal, r.id, r.kind, false, on ? "hide" : "show"});
    r.hidden = on;
}

void set_state53(Room &r, int32_t value) {
    // PS3 SetRoomDataExternal 0x4d3af8: searchable int 0x53 (1 at SERVER_LOBBY 0x465fe8, 0 at Leave::Enter).
    const U2oAttr a{A_STATE, 0, uint32_t(value), nullptr};
    const uint64_t id = api.set_room_external(r.id, &a, 1);
    log("online: %s room %llx: state 0x53 = %d (request %llu)", kind_name(r.kind), (unsigned long long)r.id, value,
        (unsigned long long)id);
    request(id, Pending{Req::SetExternal, r.id, r.kind, false, "state 0x53"});
    r.state53 = value;
}

// ---- join list -----------------------------------------------------------------------------------
void search_failed() {
    G<uint8_t>(kMatch + M_SEARCH_DONE) = 1;     // PAGE_MULTIPLAYER_JOIN 0x609640: pop, NET_SM 18
    G<int32_t>(kMatch + M_SEARCH_RESULT) = -1;
}

void start_search() {
    if (link_state != U2O_READY || !world) {
        log("online: join list: link %s, world %u: no search", link_name(link_state), world);
        search_failed();
        return;
    }
    // PS3 0x4655b0 -> SearchRoom 0x4d3c50(0, 0, -1, 0, 0, 0, 0): only 0x4C == netInfo+0; open rooms only.
    U2oSearch s{};
    s.flag_filter = kFlagHidden | kFlagClosed;
    s.flag_attr = 0;
    s.ints[0] = U2oIntFilter{1, 0, A_KEY, room_key()};
    s.int_count = 1;
    s.start = 1;
    s.max = U2O_MAX_ROOMS;
    search_req = api.search_rooms(world, &s);
    searching = search_req != 0;
    found.clear();
    G<uint8_t>(kMatch + M_SEARCH_DONE) = 0;
    G<int32_t>(kMatch + M_SEARCH_RESULT) = 0;
    log("online: join list: searching world %u for key 0x%x (request %llu)", world, room_key(), (unsigned long long)search_req);
    if (!searching) search_failed();
    else request(search_req, Pending{Req::Search, 0, 0, false, "search"});
}

// ---- transport switching -------------------------------------------------------------------------
void own_party(const char *why) {
    log("online: back to a party of our own (%s)", why);
    following_leader = false;
    waiting_host = false;
    switch_at = 0;
    session::online_reopen(&link, nullptr);
}

void switch_to_owner(const net::Address &owner, uint64_t room) {
    waiting_host = true;
    waiting_room = room;
    host_deadline = now() + kJoinTimeout;
    session::online_reopen(&link, &owner);
}

void join_succeeded(const Pending &p, const U2oRoom &src) {
    const uint32_t kind = p.room_kind ? p.room_kind : kKindCustom;
    // The rooms we were in are left: the transport goes to the new room's owner.
    if (party_room.id && party_room.id != src.room_id) leave_room(party_room, "joined another room");
    if (match_room_.id && match_room_.id != src.room_id) leave_room(match_room_, "joined another room");
    Room &r = kind == kKindParty ? party_room : match_room_;
    take_room(r, src, kind);
    log_room("joined", r);
    for (uint32_t i = 0; i < src.member_count && i < U2O_MAX_MEMBERS; ++i) connect_member(src.members[i], src.my_id, r.id);
    const Member *owner = r.member(r.owner_id);
    if (!owner || owner->npid.empty()) {
        log("online: room %llx has no owner in its member list: leaving it", (unsigned long long)r.id);
        leave_room(r, "no owner");
        own_party("join failed");
        if (top_page_id() == kPageJoin) search_failed();
        return;
    }
    const net::Address addr = address_of(peer_index(owner->npid, true));
    log("online: the room owner is %s (%s): the transport joins it", owner->name.c_str(), owner->npid.c_str());
    publish_blob(r, true);
    following_leader = false;
    if (p.lead_party) {
        // PS3: the leader's party followed through member data +0 / mailbox; here a session message,
        // then this PC switches once it went out.
        std::vector<uint8_t> b{session::kOnlineMessage, kOpFollowRoom, 0, 0, 0, 0, 0, 0, 0, 0};
        std::memcpy(b.data() + 2, &r.id, 8);
        session::send_message(-1, b);
        log("online: told the party (%d member(s)) to follow into room %llx", session::member_count() - 1,
            (unsigned long long)r.id);
        switch_owner = addr;
        switch_at = now() + kSwitchDelay;
        waiting_room = r.id;
    } else {
        switch_to_owner(addr, r.id);
    }
    if (kind == kKindParty && p.kind == Req::JoinList && top_page_id() == kPageJoin) {
        // A party: back to PAGE_MULTIPLAYER (PS3 party join 0x4c4704 ended in NET_SM 18).
        guest<void(U2MP_GUEST *)(void *)>(fPopPage)(image.at<void>(addr::menu_manager));
        G<int32_t>(kNetState) = kChooseHostJoin;
    }
}

// ---- events --------------------------------------------------------------------------------------
void on_event(const U2oEvent &e) {
    Pending p{Req::World, 0, 0, false, "request"};
    bool known = false;
    if (e.request) {
        auto it = pending.find(e.request);
        if (it != pending.end()) {
            p = it->second;
            known = true;
            pending.erase(it);
        }
    }
    switch (e.kind) {
    case U2O_EV_LINK_LOST:
        log("online: link lost (%s): every room is gone", e.text ? e.text : "no reason given");
        party_room = Room{};
        match_room_ = Room{};
        pending.clear();
        world = 0;
        world_req = 0;
        if (match_waiting) {
            match_waiting = false;
            session::online_match_ready(0);
        }
        break;
    case U2O_EV_WORLD:
        world_req = 0;
        if (e.error) {
            log("online: no world: error %d (%s); trying again in 10 s", e.error, error_hint(e.error));
            next_world_try = now() + 10;
            break;
        }
        world = e.world;
        world_server = e.server;
        log("online: world %u on server %u", world, unsigned(world_server));
        break;
    case U2O_EV_ROOM_CREATED: {
        const bool match = known ? p.kind == Req::CreateMatch : e.request == create_match_req;
        if (match) create_match_req = 0;
        else create_party_req = 0;
        if (e.error || !e.room) {
            log("online: creating the %s room failed: error %d (%s)", match ? "custom game" : "party", e.error,
                error_hint(e.error));
            if (match && match_waiting) {
                match_waiting = false;
                log("online: Start Match goes on without a room (the party plays; nobody else can join)");
                session::online_match_ready(0);
            }
            break;
        }
        Room &r = match ? match_room_ : party_room;
        take_room(r, *e.room, match ? kKindCustom : kKindParty);
        log_room("created", r);
        if (match) {
            if (match_waiting) {
                match_waiting = false;
                session::online_match_ready(r.id);
            } else {
                log("online: the match room came after Start Match had given up waiting; kept");
            }
        }
        break;
    }
    case U2O_EV_ROOM_JOINED:
        if (!known) {
            log("online: room %llx joined for an unknown request %llu", (unsigned long long)e.room_id,
                (unsigned long long)e.request);
            break;
        }
        if (e.error || !e.room) {
            log("online: joining room %llx failed: error %d (%s)", (unsigned long long)p.room, e.error, error_hint(e.error));
            if (p.kind == Req::JoinList) {
                if (top_page_id() == kPageJoin) search_failed();
            } else if (p.kind == Req::JoinLeader) {
                leave_room(party_room, "could not follow the leader");
                leave_room(match_room_, "could not follow the leader");
                own_party("could not follow the leader");
                G<uint8_t>(kNetInfo + NI_ERROR) = 1;   // the multiplayer page shows the network error box
            }
            break;
        }
        if (p.kind == Req::JoinMatchMember) {   // a party member joins its leader's match room (no transport change)
            if (match_room_.id && match_room_.id != e.room->room_id) leave_room(match_room_, "the leader's new match room");
            take_room(match_room_, *e.room, kKindCustom);
            log_room("joined (party follow)", match_room_);
            for (uint32_t i = 0; i < e.room->member_count && i < U2O_MAX_MEMBERS; ++i)
                connect_member(e.room->members[i], e.room->my_id, match_room_.id);
            publish_blob(match_room_, true);
            break;
        }
        join_succeeded(p, *e.room);
        break;
    case U2O_EV_ROOM_LEFT:
        log("online: left room %llx: error %d (%s)", (unsigned long long)e.room_id, e.error, error_hint(e.error));
        break;
    case U2O_EV_SEARCH: {
        if (e.request != search_req) break;   // an older search
        searching = false;
        if (e.error) {
            log("online: join list: search failed: error %d (%s)", e.error, error_hint(e.error));
            if (top_page_id() == kPageJoin) search_failed();
            break;
        }
        found.clear();
        for (uint32_t i = 0; i < e.count && e.rooms && i < U2O_MAX_ROOMS; ++i) {
            const U2oRoomExt &x = e.rooms[i];
            const bool ours = room_by_id(x.room_id) != nullptr;
            log("online: join list: room %llx %s of %s, %u/%u members, kind %u, state %u, flags 0x%x%s",
                (unsigned long long)x.room_id, x.int_attr[A_KIND - A_KEY] == kKindParty ? "party" : "custom game",
                x.owner_name, unsigned(x.cur_members), unsigned(x.max_slot), x.int_attr[A_KIND - A_KEY],
                x.int_attr[A_STATE - A_KEY], x.flag_attr, ours ? " (ours, not listed)" : "");
            if (!ours) found.push_back(x);
        }
        log("online: join list: %zu room(s) listed (%u found, %u in total)", found.size(), e.count, e.total);
        G<uint8_t>(kMatch + M_SEARCH_DONE) = 1;
        G<int32_t>(kMatch + M_SEARCH_RESULT) = 0;
        break;
    }
    case U2O_EV_DONE:
        if (e.error)
            log("online: %s on room %llx failed: error %d (%s)", known ? p.what.c_str() : "request",
                (unsigned long long)p.room, e.error, error_hint(e.error));
        else
            log("online: %s on room %llx done", known ? p.what.c_str() : "request", (unsigned long long)p.room);
        break;
    case U2O_EV_MEMBER_JOINED: {
        Room *r = room_by_id(e.room_id);
        if (!e.member) break;
        log("online: %s (%s) joined %s room %llx as member %u", e.member->name, npid_text(e.member->npid).c_str(),
            r ? kind_name(r->kind) : "an unknown", (unsigned long long)e.room_id, unsigned(e.member->id));
        if (!r) break;
        r->members.push_back(Member{e.member->id, npid_text(e.member->npid), e.member->name});
        connect_member(*e.member, r->my_id, r->id);
        if (r == &party_room) publish_blob(party_room, false);   // +0 party room id once the party has 2
        break;
    }
    case U2O_EV_MEMBER_LEFT: {
        Room *r = room_by_id(e.room_id);
        if (!e.member) break;
        log("online: %s (%s) left room %llx (cause %u: %s)", e.member->name, npid_text(e.member->npid).c_str(),
            (unsigned long long)e.room_id, e.cause, e.cause == 1 ? "left" : e.cause == 5 ? "disappeared" : "other");
        if (!r) break;
        for (size_t i = 0; i < r->members.size(); ++i)
            if (r->members[i].id == e.member->id) {
                r->members.erase(r->members.begin() + ptrdiff_t(i));
                break;
            }
        if (e.member->id == r->owner_id)
            log("online: the owner of room %llx left; the new owner comes with the room update (no host migration: "
                "the transport ends like a LAN host leaving)", (unsigned long long)r->id);
        if (e.room) r->owner_id = e.room->owner_id;
        const std::string npid = npid_text(e.member->npid);
        const bool elsewhere = (r != &party_room && in_room(party_room, npid)) || (r != &match_room_ && in_room(match_room_, npid));
        if (!elsewhere && !npid.empty()) {
            api.peer_disconnect(npid.c_str());
            log("online: peer %s disconnected (in none of our rooms now)", npid.c_str());
        }
        if (r == &party_room) publish_blob(party_room, false);
        break;
    }
    case U2O_EV_ROOM_UPDATED: {
        Room *r = room_by_id(e.room_id ? e.room_id : e.room ? e.room->room_id : 0);
        if (!r || !e.room) break;
        const uint16_t old_owner = r->owner_id;
        r->owner_id = e.room->owner_id;
        r->flags = e.room->flag_attr;
        const Member *o = r->member(r->owner_id);
        log("online: %s room %llx updated: owner member %u (%s), flags 0x%x", kind_name(r->kind), (unsigned long long)r->id,
            unsigned(r->owner_id), o ? o->name.c_str() : "?", r->flags);
        if (old_owner != r->owner_id)
            log("online: room %llx owner changed %u -> %u%s", (unsigned long long)r->id, unsigned(old_owner),
                unsigned(r->owner_id), r->owned() ? " (this PC now)" : "");
        break;
    }
    case U2O_EV_MEMBER_UPDATED:
        if (e.member && e.member->bin_len >= 10) {
            uint64_t party = 0;
            std::memcpy(&party, e.member->bin, 8);
            log("online: member data of %s in room %llx: party room %llx, rank %u, team %u", e.member->name,
                (unsigned long long)e.room_id, (unsigned long long)party, unsigned(e.member->bin[8]),
                unsigned(e.member->bin[9]));
        }
        break;
    case U2O_EV_PEER_ADDRESS: {
        const std::string npid = npid_text(e.npid);
        if (e.error) {
            log("online: no address for peer %s: error %d (%s)", npid.c_str(), e.error, error_hint(e.error));
            break;
        }
        peer_index(npid, true);
        const int r = api.peer_connect(npid.c_str(), e.ip, e.port);
        log("online: peer %s: address %s, connecting (u2o_peer_connect %d)", npid.c_str(), masked(e.ip, e.port).c_str(), r);
        break;
    }
    case U2O_EV_PEER_STATE: {
        const std::string npid = npid_text(e.npid);
        const int i = peer_index(npid, true);
        if (i >= 0) {
            peers[size_t(i)].state = int32_t(e.state);
            peers[size_t(i)].rtt_us = e.rtt_us;
        }
        log("online: peer %s: %s, rtt %u ms", npid.c_str(), peer_state_name(e.state), e.rtt_us / 1000);
        if (waiting_host && e.state == U2O_PEER_DEAD) {
            Room *r = room_by_id(waiting_room);
            const Member *o = r ? r->member(r->owner_id) : nullptr;
            if (o && o->npid == npid) {
                log("online: the link to the room owner died before the transport reached it: leaving room %llx",
                    (unsigned long long)waiting_room);
                if (r) leave_room(*r, "owner unreachable");
                own_party("room owner unreachable");
                if (top_page_id() == kPageJoin) search_failed();
            }
        }
        break;
    }
    case U2O_EV_MEMBERS_EXT:
        log("online: %u external member(s) of room %llx", e.count, (unsigned long long)e.room_id);
        break;
    case U2O_EV_PUBLIC_ADDRESS:
        log("online: the room service sees our peer socket at %s", masked(e.ip, e.port).c_str());
        break;
    default:
        log("online: event %d (error %d) ignored", e.kind, e.error);
        break;
    }
}

void open_link() {
    const char *relay = std::getenv("UNCHARTED_U2MP_RELAY");   // never logged
    if (link_opened) api.close();
    const int r = api.open(relay ? relay : "", kCommunicationId);
    link_opened = true;
    link_state = U2O_CONNECTING;
    world = 0;
    world_req = 0;
    log("online: opening the link (communication id %s): u2o_open %d", kCommunicationId, r);
}

// NET_SM-driven room state (PS3 call sites in PORT-MATCHMAKING.md 1.6 / 1.7 and section 6 item 9).
void room_state() {
    const int32_t s = G<int32_t>(kNetState);
    const bool match_running = s == kUpdate || s == kReset || s == kReplay || s == kResults || s == kRoundResults;
    if (s != last_net_state) {
        if (match_running && !in_match) log("online: NET_SM %d: match running", s);
        if (!match_running && in_match) log("online: NET_SM %d: back from the match", s);
        last_net_state = s;
    }
    const bool ended = in_match && !match_running;
    in_match = match_running;
    if (link_state != U2O_READY) return;
    for (Room *r : {&party_room, &match_room_}) {
        if (!r->owned()) continue;
        if (r->hidden != match_running) set_hidden(*r, match_running);   // 0x4d5338: closed during the match
    }
    if (match_room_.owned()) {
        if (s == kServerLobby && !match_room_.lobby_started) {   // 0x465fe8: the lobby task started
            match_room_.lobby_started = true;
            set_state53(match_room_, 1);
        }
        if (ended && match_room_.state53 != 0) set_state53(match_room_, 0);   // Leave::Enter 0x445e9c
    }
}

}  // namespace

// ---- online.h --------------------------------------------------------------------------------------
net::Link *start() {
    if (!load()) return nullptr;
    if (const char *n = std::getenv("UNCHARTED_U2MP_NPID"); n && *n) my_npid = std::string(n).substr(0, 16);
    if (!link_opened || link_state == U2O_FAILED || link_state == U2O_OFF) open_link();
    started = true;
    waiting_host = following_leader = false;
    switch_at = 0;
    log("online: Multiplayer entered online as %s (NpId %s): link %s, world %u", session::local_name().c_str(),
        my_npid.empty() ? "?" : my_npid.c_str(), link_name(link_state), world);
    return &link;
}

bool active() { return started; }

void frame() {
    if (!api.dll || !link_opened) return;
    const double t = now();
    char text[128] = {};
    const int32_t st = api.state(text, int(sizeof text));
    if (st != link_state) {
        log("online: link %s -> %s (%s)", link_name(link_state), link_name(st), st == U2O_READY ? text : (*text ? text : "-"));
        if (st == U2O_FAILED && match_waiting) {
            match_waiting = false;
            session::online_match_ready(0);
        }
        link_state = st;
    }
    if (link_state == U2O_READY && !world && !world_req && t >= next_world_try) {
        world_req = api.find_world();
        log("online: looking for a world (request %llu)", (unsigned long long)world_req);
        request(world_req, Pending{Req::World, 0, 0, false, "find world"});
        if (!world_req) next_world_try = t + 10;
    }
    U2oEvent e;
    for (int guard = 0; guard < 256; ++guard) {
        std::memset(&e, 0, sizeof e);
        if (api.poll(&e) <= 0) break;
        on_event(e);
    }
    if (!started) return;
    // our own party room, while this PC leads a party and is in no other room (PS3 0x4c48d0)
    if (link_state == U2O_READY && world && session::is_host() && !party_room.id && !match_room_.id && !create_party_req &&
        !waiting_host && !switch_at)
        create_party_req = create_room(kKindParty);
    if (switch_at && t >= switch_at) {
        switch_at = 0;
        log("online: transport switch: the party was told, joining the owner of room %llx", (unsigned long long)waiting_room);
        switch_to_owner(switch_owner, waiting_room);
    }
    if (waiting_host && t >= host_deadline) {
        log("online: the room owner was not reached in %.0f s: leaving room %llx", kJoinTimeout, (unsigned long long)waiting_room);
        if (Room *r = room_by_id(waiting_room)) leave_room(*r, "owner not reached");
        own_party("room owner not reached");
        if (top_page_id() == kPageJoin) search_failed();
    }
    if (join_page_deadline && t >= join_page_deadline) {
        join_page_deadline = 0;
        if (top_page_id() == kPageJoin) {   // the host is not in its custom game lobby: we are in its party now
            log("online: no follow from the host (not in its lobby): back to PAGE_MULTIPLAYER, NET_SM 18");
            guest<void(U2MP_GUEST *)(void *)>(fPopPage)(image.at<void>(addr::menu_manager));
            G<int32_t>(kNetState) = kChooseHostJoin;
        }
    }
    if (match_waiting && t >= match_deadline) {
        match_waiting = false;
        log("online: no answer to the match room create in %.0f s: Start Match goes on without a room", kCreateTimeout);
        session::online_match_ready(0);
    }
    room_state();
    if (t >= next_blob_check) {
        next_blob_check = t + 1.0;
        publish_blob(party_room, false);
        publish_blob(match_room_, false);
    }
    if (t >= next_rtt_log) {
        next_rtt_log = t + kRttLogPeriod;
        for (Peer &p : peers) {
            uint32_t rtt = 0;
            const int s = api.peer_state(p.npid.c_str(), &rtt);
            if (s == U2O_PEER_INACTIVE && p.state < 0) continue;
            p.state = s;
            p.rtt_us = rtt;
            log("online: peer %s: %s, rtt %u ms", p.npid.c_str(), peer_state_name(uint32_t(s)), rtt / 1000);
        }
    }
}

bool create_match_room() {
    if (!started) return false;
    if (G<int32_t>(kNetInfo + NI_MODE) != 2) {
        log("online: Start Match in mode %d: no match room (stranger matchmaking is not ported; the party plays)",
            G<int32_t>(kNetInfo + NI_MODE));
        return false;
    }
    if (match_room_.owned()) {
        log("online: Start Match: our match room %llx is still open, reused", (unsigned long long)match_room_.id);
        return false;
    }
    if (link_state != U2O_READY || !world) {
        log("online: Start Match: link %s, world %u: no match room (the party plays, nobody else can join)",
            link_name(link_state), world);
        return false;
    }
    if (match_room_.id) leave_room(match_room_, "a match room of our own");
    create_match_req = create_room(kKindCustom);
    if (!create_match_req) {
        log("online: Start Match: the match room request was refused: the party plays without one");
        return false;
    }
    match_waiting = true;
    match_deadline = now() + kCreateTimeout;
    return true;
}

uint64_t match_room() { return started ? match_room_.id : 0; }

void follow_match_room(uint64_t room_id) {
    if (!started || !room_id) return;
    if (match_room_.id == room_id) {
        log("online: the leader's match room %llx: already a member", (unsigned long long)room_id);
        return;
    }
    const auto blob = member_blob();
    const uint64_t id = api.join_room(room_id, blob.data(), uint32_t(blob.size()), 0);
    log("online: joining the leader's match room %llx as a member (the transport stays): request %llu",
        (unsigned long long)room_id, (unsigned long long)id);
    request(id, Pending{Req::JoinMatchMember, room_id, kKindCustom, false, "join (party follow)"});
}

void on_host_reached(uint32_t net_id) {
    if (!started) return;
    if (session::is_host()) {
        log("online: transport: net id %u connected to this host over the online link", net_id);
        return;
    }
    if (waiting_host) {
        waiting_host = false;
        log("online: transport switch done: reached the owner of room %llx, our net id %u", (unsigned long long)waiting_room,
            net_id);
        if (top_page_id() == kPageJoin) join_page_deadline = now() + 3.0;   // the host's follow normally comes first
    }
}

bool host_change_expected() { return started && (following_leader || waiting_host || switch_at); }

void on_transport_closed(const char *why) {
    if (!started) return;
    log("online: the transport closed (%s): leaving every room", why ? why : "?");
    leave_room(match_room_, why ? why : "closed");
    leave_room(party_room, why ? why : "closed");
    for (const Peer &p : peers)
        if (p.state == U2O_PEER_ACTIVE || p.state == U2O_PEER_PENDING) api.peer_disconnect(p.npid.c_str());
    for (Peer &p : peers) p.state = -1;
    started = false;
    waiting_host = following_leader = false;
    switch_at = 0;
    match_waiting = false;   // the match is gone with the transport: nothing to finish
}

void on_message(uint8_t from, const uint8_t *data, size_t size) {
    if (!started || !size) return;
    if (data[0] == kOpFollowRoom && size >= 9 && from == 0 && !session::is_host()) {
        uint64_t room = 0;
        std::memcpy(&room, data + 1, 8);
        following_leader = true;
        const auto blob = member_blob();
        const uint64_t id = api.join_room(room, blob.data(), uint32_t(blob.size()), 0);
        log("online: the party leader moved to room %llx: following (request %llu)", (unsigned long long)room,
            (unsigned long long)id);
        if (!id) {
            following_leader = false;
            log("online: could not follow the leader (request refused)");
            return;
        }
        request(id, Pending{Req::JoinLeader, room, kKindCustom, false, "join (follow the leader)"});
    }
}

bool open_join_list() {
    if (!started) return false;
    log("online: Invites -> join list (PAGE_MULTIPLAYER_JOIN, BUTTON_MULTIPLAYER_JOIN 0x6095d0)");
    G<uint8_t>(kMatch + M_SEARCH_DONE) = 0;
    G<int32_t>(kMatch + M_SEARCH_RESULT) = 0;
    found.clear();
    guest<void(U2MP_GUEST *)(void *, void *, void *)>(fButtonJoin)(nullptr, nullptr, nullptr);   // page, NET_SM 5
    start_search();
    return true;
}

int join_rows() {
    if (!started) return -1;
    return found.empty() ? 1 : int(found.size());
}

const char *join_row(int row) {
    if (!started || row < 0) return "";
    if (found.empty()) return searching ? "Searching for games..." : "No open games found. Select to search again.";
    if (size_t(row) >= found.size() || row >= U2O_MAX_ROOMS) return "";
    const U2oRoomExt &x = found[size_t(row)];
    std::snprintf(row_text[row], sizeof row_text[row], "%.40s - %s (%u/%u)", x.owner_name,
                  x.int_attr[A_KIND - A_KEY] == kKindParty ? "Party" : "Custom Game", unsigned(x.cur_members),
                  unsigned(x.max_slot));
    return row_text[row];
}

const char *join_title() { return "Online Games"; }

bool select_join(int row) {
    if (!started) return false;
    if (found.empty()) {
        if (!searching) start_search();
        return true;
    }
    if (row < 0 || size_t(row) >= found.size()) {
        log("online: join list row %d of %zu: nothing there", row, found.size());
        return true;
    }
    const U2oRoomExt &x = found[size_t(row)];
    const bool leader = session::is_host();
    const int party = leader ? std::max(1, session::member_count()) : 1;
    const int free_slots = int(x.max_slot) - int(x.cur_members);
    // PS3 RESERVE_SLOTS (mailbox 0x100 / 0x101 with the host's slot manager 0x4da278): the whole party
    // must fit; simplified to the room data of the search.
    if (free_slots < party) {
        log("online: join refused: room %llx has %d free slot(s), the party needs %d", (unsigned long long)x.room_id,
            free_slots, party);
        return true;
    }
    const auto blob = member_blob();
    const uint64_t id = api.join_room(x.room_id, blob.data(), uint32_t(blob.size()), 0);
    const uint32_t kind = x.int_attr[A_KIND - A_KEY] == kKindParty ? kKindParty : kKindCustom;
    log("online: joining %s room %llx of %s (%d free slot(s), party of %d): request %llu", kind_name(kind),
        (unsigned long long)x.room_id, x.owner_name, free_slots, party, (unsigned long long)id);
    if (!id) {
        if (top_page_id() == kPageJoin) search_failed();
        return true;
    }
    request(id, Pending{Req::JoinList, x.room_id, kind, leader && party > 1, "join"});
    return true;
}

void member_data_changed() {
    if (!started) return;
    publish_blob(party_room, false);
    publish_blob(match_room_, false);
}

}  // namespace u2mp::online
