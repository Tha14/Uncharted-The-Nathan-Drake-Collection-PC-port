// u2mp session layer: what the PS3 game's NetInit, Matching2 sessions and lobby code did, rebuilt for
// PCs over net::Transport. Blueprint and every address: u2mp/docs/SESSION-INTERFACE.md (PS4 1.02
// image offsets; PS3 1.10 originals in the comments).
//
// Roles: the PC that opens Multiplayer without UNCHARTED_U2MP_JOIN hosts the party (and later the
// match); the others join it by address (UNCHARTED_U2MP_JOIN=ip[:port]). Party and match use one
// transport: the party leader is the match host.
//
// Net ids: host 1, a client in transport slot s has s + 1 (also its connection-record key).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <utility>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "frontend3.h"
#include "gamenet.h"
#include "progression.h"
#include "net.h"
#include "session_api.h"
#include "u2mp_internal.h"
#include "voice.h"

namespace u2mp::session {
namespace {

// ---- guest data -----------------------------------------------------------------------------
constexpr uint64_t kNetState = 0x14ce97c;     // PS3 NET_SM (0xf11764); IsOnline = state > 1
constexpr uint64_t kParty = 0x105efb8;        // party session (PS3 0xf2c8a0)
constexpr uint64_t kMatch = 0x1065b08;        // g_matchSession (PS3 0xf20c48)
constexpr uint64_t kNetInfo = 0x104f1c8;      // PS3 0xf1fa48
constexpr uint64_t kNgm = 0x113b760;          // g_netGameManager (PS3 0xf1dc80)
// session fields
constexpr uint64_t S_CTX = 0x10, S_ME = 0xb00, S_HOST = 0xb04, S_MEMBERS = 0x228, S_MEMBER_SIZE = 0xd0;
constexpr uint64_t SM_VALID = 0x09, SM_ID = 0x10, SM_KEY = 0x18;
constexpr uint64_t M_CREATED = 0x99, M_CREATE_RESULT = 0x9c, M_JOINED = 0xb8, M_JOIN_RESULT = 0xbc,
                   M_JOIN_ROOM = 0xc0, M_LEFT = 0xc8;
// net info fields
constexpr uint64_t NI_CREATED_BY_ME = 0x08, NI_ERROR = 0x1c, NI_FROM_MATCHMAKING = 0x34, NI_MODE = 0x2c4;
// NetGameManager
constexpr uint64_t NGM_LOCAL = 0x00, NGM_HOST = 0x08, NGM_TRACKERS = 0x10, NGM_TRACKER_SIZE = 0x2c0,
                   NGM_GAME = 0x1ca0;
constexpr uint64_t T_NAME = 0x20, T_NETID = 0x120, T_MEMBER28 = 0x238, T_KEY = 0x23c, T_EXCLUDED = 0x26c,
                   T_CONNECTED = 0x275, T_TALKS_TO_ALL = 0x27c;
constexpr int kTrackers = 10;
// functions
constexpr uint64_t fPopPage = 0x5cc6f0, fClearNetPlayers = 0x63db90, fTrackerClear = 0x662a30,
                   fHostCreate = 0x5eb830, fCreateEvent = 0x6180f0, fMessageApply = 0x62dc50,
                   fRequestPackage = 0x56ad90, kPackageTable = 0x1130390;
// menu page-event handlers (dispatcher 0x5cd5c0): count at menu+0x24f0, entries at menu+0x24f8 (0x18
// each: u32 page id, 0 = every page; fn at +8; arg at +0x10), at most 32. A handler gets
// {u32 event, u32 page, u64 arg, ...}; event 6 is the per-frame update.
constexpr uint64_t MENU_HANDLER_COUNT = 0x24f0, MENU_HANDLERS = 0x24f8, MENU_HANDLER_SIZE = 0x18;
constexpr uint32_t kMaxPageHandlers = 32, kEventUpdate = 6;
// TaskManagerOnline states (vtable, Update at +0x18; SESSION-INTERFACE.md 3.2): the match keeps
// the network going through them while no menu page is updated.
struct StateUpdate { uint64_t slot, update; };
constexpr StateUpdate kStateUpdates[] = {
    {0x100ac40 + 0x18, 0x5f2ea0}, {0x100ac00 + 0x18, 0x5f2dc0}, {0x100abc0 + 0x18, 0x5f26d0},
    {0x100ab80 + 0x18, 0x5f14d0}, {0x100ab40 + 0x18, 0x5f1390}, {0x100ab00 + 0x18, 0x5f1000},
    {0x100aa80 + 0x18, 0x5f0ec0}, {0x100aa40 + 0x18, 0x5f0b30}, {0x100aa00 + 0x18, 0x5f0a90},
    {0x100a9c0 + 0x18, 0x5eff30}};
constexpr size_t kStateCount = sizeof(kStateUpdates) / sizeof(*kStateUpdates);
// stubs we replace
constexpr uint64_t cbMultiplayer = 0x5eb160, qPartyRows = 0x659bb0, qPlayerRows = 0x659bc0,
                   cbStartMatch = 0x5eb780, cbLeaveMultiplayer = 0x5eb180;
// NET_SM values
enum : int32_t { kStateOff = 0, kCreateGameWait = 3, kClientWaitPartyHost = 12, kServerLobby = 16,
                 kClientLobby = 17, kChooseHostJoin = 18, kLeaveGame = 35 };
// mailbox codes (NetEventMessage, type 46)
enum : uint32_t { kMsgPartyHostJoined = 0x10e, kMsgMode = 0x110 };

constexpr uint16_t kDefaultPort = 27210;
constexpr uint64_t kRoomContext = 0x55324d50000001ull;   // any non-zero session context

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
uint64_t guest(uint64_t offset) { return uint64_t(uintptr_t(image.base)) + offset; }
template <typename F> F fn(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }

// ---- session state --------------------------------------------------------------------------
struct Member {
    bool used = false;
    uint32_t net_id = 0;
    std::string name;
    uint8_t rank = 0;   // rank badge index (PS3 room member data +8, roster query 0x4bc728)
};
Member members[net::kMaxPeers + 1];   // by transport slot; slot 0 = the host
net::Transport transport;
bool active = false, hosting = false, in_match_setup = false;
uint32_t my_id = 0;
std::string my_name = "Drake";
int32_t sent_mode = -1;
int32_t last_state = 0;
char roster_text[net::kMaxPeers + 1][64];
unsigned frames = 0;
bool kicked = false;   // the host kicked us: leave the party after this pump
bool lost_host = false;   // the host is gone: close the transport after this pump (PS3 teardown 0x4386d0)
uint8_t sent_rank = 0xff;   // the rank this PC last published
double next_rate_log = 0;

uint32_t id_of_slot(uint8_t slot) { return uint32_t(slot) + 1; }

void set_state(int32_t s) { G<int32_t>(kNetState) = s; }
int32_t state() { return G<int32_t>(kNetState); }

// This PC's rank badge index: PS3 0x0cfa58 over the cash in NetPlayerData (+0xd8) and the rank table
// c85e199d (+0x50 count, +0x58 thresholds), as the profile panel and progression.cpp compute it.
uint8_t local_rank() {
    struct Table { uint8_t pad[0x50]; int32_t count; int32_t pad2; int32_t *thresholds; };
    const auto *t = fn<const Table *(U2MP_GUEST *)(uint32_t)>(0xa15e70)(0xc85e199d);
    if (!t || t->count <= 1 || !t->thresholds) return 0;
    const int32_t money = G<int32_t>(0x11474f8 + 0xd8), last = t->count - 1;
    for (int32_t i = 0; i < last; ++i)
        if (t->thresholds[i] <= money && t->thresholds[i + 1] > money) return uint8_t(std::min(i, 255));
    return uint8_t(std::min(last, 255));
}

void session_set(uint64_t s, uint64_t ctx, uint32_t me, uint32_t host) {
    G<uint64_t>(s + S_CTX) = ctx;
    G<uint32_t>(s + S_ME) = me;
    G<uint32_t>(s + S_HOST) = host;
}

void session_members(uint64_t s) {
    int i = 0;
    for (uint8_t slot = 0; slot <= net::kMaxPeers; ++slot) {
        const uint64_t m = s + S_MEMBERS + uint64_t(i) * S_MEMBER_SIZE;
        if (!members[slot].used) continue;
        if (i >= kTrackers) break;
        G<uint8_t>(m + SM_VALID) = 1;
        G<uint32_t>(m + SM_ID) = members[slot].net_id;
        G<uint32_t>(m + SM_KEY) = members[slot].net_id;
        ++i;
    }
    for (; i < kTrackers; ++i) {
        const uint64_t m = s + S_MEMBERS + uint64_t(i) * S_MEMBER_SIZE;
        G<uint8_t>(m + SM_VALID) = 0;
        G<uint32_t>(m + SM_ID) = 0;
        G<uint32_t>(m + SM_KEY) = 0;
    }
}

void session_clear(uint64_t s) {
    session_set(s, 0, 0, 0);
    for (int i = 0; i < kTrackers; ++i) {
        const uint64_t m = s + S_MEMBERS + uint64_t(i) * S_MEMBER_SIZE;
        G<uint8_t>(m + SM_VALID) = 0;
        G<uint32_t>(m + SM_ID) = 0;
    }
}

// Port of AddOrUpdateNetPlayer (PS3 0x49ada8; PS4 0x642370 is dead and copies its name from NULL):
// the tracker with this net id, or the first free one.
void add_net_player(uint32_t net_id, const std::string &name, bool local) {
    const uint64_t ngm = kNgm;
    int slot = -1;
    for (int i = 0; i < kTrackers && slot < 0; ++i) {
        const uint64_t t = ngm + NGM_TRACKERS + uint64_t(i) * NGM_TRACKER_SIZE;
        if (G<char>(t + T_NAME) && G<uint32_t>(t + T_NETID) == net_id) slot = i;
    }
    for (int i = 0; i < kTrackers && slot < 0; ++i)
        if (!G<char>(ngm + NGM_TRACKERS + uint64_t(i) * NGM_TRACKER_SIZE + T_NAME)) slot = i;
    if (slot < 0) {
        log("no free player slot for %s", name.c_str());
        return;
    }
    const uint64_t t = ngm + NGM_TRACKERS + uint64_t(slot) * NGM_TRACKER_SIZE;
    fn<void(U2MP_GUEST *)(void *)>(fTrackerClear)(image.at<void>(t));
    std::snprintf(image.at<char>(t + T_NAME), 64, "%s", name.empty() ? "Player" : name.c_str());
    G<uint32_t>(t + T_NETID) = net_id;
    G<uint32_t>(t + T_MEMBER28) = 0;
    G<int32_t>(t + T_KEY) = local ? -1 : int32_t(net_id);
    G<uint8_t>(t + T_EXCLUDED) = 0;
    G<uint8_t>(t + T_CONNECTED) = 1;      // what ConnectionDone's Apply did (PS3 0x43dcd8)
    G<uint8_t>(t + T_TALKS_TO_ALL) = 1;   // every client reaches the others through the host
    if (local) G<uint64_t>(ngm + NGM_LOCAL) = guest(t);
    if (net_id == 1) G<uint64_t>(ngm + NGM_HOST) = guest(t);
    log("player %s: net id %u, slot %d%s", name.c_str(), net_id, slot, local ? " (local)" : "");
}

void fill_net_players() {
    for (uint8_t slot = 0; slot <= net::kMaxPeers; ++slot)
        if (members[slot].used) add_net_player(members[slot].net_id, members[slot].name, members[slot].net_id == my_id);
}

void release_event(uint8_t *ev) {
    if (--ev[0x0a] == 0) {   // refcount; vtable slot 1 = deleting destructor
        auto vt = *reinterpret_cast<uint64_t **>(ev);
        reinterpret_cast<void(U2MP_GUEST *)(void *)>(vt[1])(ev);
    }
}

// PS3 member-left handler 0x43d9e0 sent NetEventPlayerLeft (type 79; PS4 Apply 0x619db0 frees the
// player's tracker). Nothing on PS4 sends it: the host does, to everyone, and applies it itself.
void player_left(uint32_t net_id) {
    if (!net_id) return;
    auto *ev = fn<uint8_t *(U2MP_GUEST *)(int)>(fCreateEvent)(79);
    if (!ev) return;
    *reinterpret_cast<uint32_t *>(ev + 0x0c) = net_id;
    fn<void(U2MP_GUEST *)(void *, void *, uint32_t)>(0x644ab0)(image.at<void>(kNgm), ev, 0xffffffffu);
    auto vt = *reinterpret_cast<uint64_t **>(ev);
    reinterpret_cast<void(U2MP_GUEST *)(void *, void *)>(vt[4])(ev, nullptr);   // Apply locally
    release_event(ev);
}

// Delivers a mailbox message as if a peer had sent a NetEventMessage (type 46; Apply 0x62dc50
// copies the payload and records the sender from the connection's +0x160).
void mailbox_post(uint32_t code, uint32_t sender, const void *data, uint32_t size) {
    auto *ev = fn<uint8_t *(U2MP_GUEST *)(int)>(fCreateEvent)(46);
    if (!ev) return;
    std::vector<uint8_t> payload(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
    *reinterpret_cast<uint32_t *>(ev + 0x0c) = code;
    *reinterpret_cast<uint64_t *>(ev + 0x10) = uint64_t(uintptr_t(payload.data()));
    *reinterpret_cast<uint32_t *>(ev + 0x18) = size;
    alignas(16) uint8_t connection[0x200] = {};
    *reinterpret_cast<uint32_t *>(connection + 0x160) = sender;
    fn<void(U2MP_GUEST *)(void *, void *)>(fMessageApply)(ev, connection);
    *reinterpret_cast<uint64_t *>(ev + 0x10) = 0;   // the payload was ours
    *reinterpret_cast<uint32_t *>(ev + 0x18) = 0;
    release_event(ev);
}

// ---- our session messages (transport tag kSessionMessage) ------------------------------------
enum : uint8_t { kInfo = 1, kRoster = 2, kMode = 3, kFollow = 4, kKick = 5 };
// types >= session::kFrontendMessages (frontend3.h) belong to frontend3.cpp (matchmaking)

void send_session(int slot, std::vector<uint8_t> body, int except = -1) {
    body.insert(body.begin(), gamenet::kSessionMessage);
    if (slot >= 0) transport.send(uint8_t(slot), true, body.data(), body.size());
    else if (except < 0) transport.broadcast(true, body.data(), body.size());
    else
        for (uint8_t s = 1; s <= net::kMaxPeers; ++s)
            if (members[s].used && s != except) transport.send(s, true, body.data(), body.size());
}

// kInfo: {len, name, rank} (client -> host).
void send_info() {
    std::vector<uint8_t> b{kInfo, uint8_t(std::min<size_t>(my_name.size(), 31))};
    b.insert(b.end(), my_name.begin(), my_name.begin() + b[1]);
    sent_rank = local_rank();
    b.push_back(sent_rank);
    send_session(0, b);
}

void broadcast_roster() {
    std::vector<uint8_t> b{kRoster};
    for (uint8_t slot = 0; slot <= net::kMaxPeers; ++slot) {   // {slot, len, rank, name}
        if (!members[slot].used) continue;
        b.push_back(slot);
        const std::string &n = members[slot].name;
        b.push_back(uint8_t(std::min<size_t>(n.size(), 31)));
        b.push_back(members[slot].rank);
        b.insert(b.end(), n.begin(), n.begin() + std::min<size_t>(n.size(), 31));
    }
    send_session(-1, b);
}

void refresh_sessions() {
    session_members(kParty);
    if (G<uint64_t>(kMatch + S_CTX)) session_members(kMatch);
}

void on_session_message(uint8_t from, const uint8_t *d, size_t n) {
    if (!n) return;
    switch (d[0]) {
    case kInfo:   // client -> host: its name
        if (hosting && from >= 1 && from <= net::kMaxPeers && n >= 2) {
            members[from].name.assign(reinterpret_cast<const char *>(d + 2), std::min<size_t>(d[1], n - 2));
            if (n >= size_t(2) + d[1] + 1) members[from].rank = d[2 + d[1]];
            refresh_sessions();
            broadcast_roster();
        }
        break;
    case kRoster:   // host -> clients: everyone in the party
        if (!hosting) {
            for (auto &m : members) m = Member{};
            for (size_t at = 1; at + 3 <= n;) {
                const uint8_t slot = d[at], len = d[at + 1], rank = d[at + 2];
                if (slot > net::kMaxPeers || at + 3 + len > n) break;
                members[slot] = Member{true, id_of_slot(slot), std::string(reinterpret_cast<const char *>(d + at + 3), len), rank};
                at += 3 + len;
            }
            refresh_sessions();
        }
        break;
    case kMode:   // host -> clients: the multiplayer mode (PAGE_LOBBY takes it from message 0x110)
        if (!hosting && n >= 5) {
            int32_t mode;
            std::memcpy(&mode, d + 1, 4);
            mailbox_post(kMsgMode, 1, &mode, 4);
        }
        break;
    case kKick:   // host -> one client: kicked (PS3 KickoutRoomMember 0x4d8a98)
        if (!hosting && from == 0) {
            log("party: the host kicked us");
            G<uint8_t>(kNetInfo + NI_ERROR) = 1;   // every multiplayer page falls back to the error box
            kicked = true;
        }
        break;
    case kFollow:   // host -> clients: the host created the match; join it (PS3: party follow)
        if (!hosting) {
            log("following the host into the match");
            netinit_match();
            G<uint8_t>(kNetInfo + NI_CREATED_BY_ME) = 0;
            fn<void(U2MP_GUEST *)(void *)>(fClearNetPlayers)(image.at<void>(kNgm));
            session_set(kMatch, kRoomContext, my_id, 1);
            session_members(kMatch);
            G<int32_t>(kMatch + M_JOIN_RESULT) = 0;
            G<uint8_t>(kMatch + M_LEFT) = 0;
            G<uint64_t>(kMatch + M_JOIN_ROOM) = kRoomContext;
            G<uint8_t>(kMatch + M_JOINED) = 1;
            fill_net_players();
            show_page(sid("PAGE_MULTIPLAYER_JOINING"));
            set_state(kClientWaitPartyHost);
            const uint8_t yes = 1;
            mailbox_post(kMsgPartyHostJoined, 1, &yes, 1);   // JOINING 12 -> 14 -> 17
        }
        break;
    default:   // matchmaking, vote, leader back-out (frontend3.cpp)
        if (d[0] >= kFrontendMessages) matchmaking::on_message(from, d, n);
        break;
    }
}

void on_peer(uint8_t slot, bool joined) {
    if (hosting) {
        if (joined) {
            members[slot] = Member{true, id_of_slot(slot), "Player " + std::to_string(slot + 1)};
            gamenet::attach(slot, id_of_slot(slot), true);
            log("party: peer in slot %u joined", slot);
        } else {
            log("party: %s left", members[slot].name.c_str());
            player_left(members[slot].net_id);
            members[slot] = Member{};
            gamenet::detach(slot);
        }
        refresh_sessions();
        broadcast_roster();
        return;
    }
    if (slot != 0) return;
    if (joined) {   // we reached the host
        my_id = id_of_slot(transport.local_slot());
        session_set(kParty, kRoomContext, my_id, 1);
        gamenet::attach(0, 1, true);
        send_info();
        log("party: joined the host as net id %u", my_id);
    } else {        // host gone: every multiplayer page falls back to the error box
        log("party: lost the host");
        gamenet::detach(0);
        G<uint8_t>(kNetInfo + NI_ERROR) = 1;
        // PS3 PAGE_MULTIPLAYER event 3 ran the teardown 0x4386d0 on this error; PS4 lost it, so
        // `active` stayed true over a dead transport and the next Multiplayer skipped
        // start_transport. Closed after this pump, as for a kick (not inside it).
        lost_host = true;
    }
}

bool start_transport() {
    if (const char *n = std::getenv("UNCHARTED_U2MP_NAME"); n && *n) my_name.assign(n, std::min<size_t>(std::strlen(n), 31));
    uint16_t port = kDefaultPort;
    if (const char *p = std::getenv("UNCHARTED_U2MP_PORT"); p && std::atoi(p) > 0) port = uint16_t(std::atoi(p));
    transport.on_peer = on_peer;
    transport.on_message = [](uint8_t slot, const uint8_t *d, size_t n) {
        if (!n) return;
        if (d[0] == gamenet::kGamePacket) gamenet::receive(slot, d + 1, n - 1);
        else if (d[0] == gamenet::kSessionMessage) on_session_message(slot, d + 1, n - 1);
        else if (d[0] == gamenet::kVoice) voice::receive(slot, d + 1, n - 1);
    };
    gamenet::set_transport(&transport);
    voice::set_transport(&transport);
    for (auto &m : members) m = Member{};
    const char *join = std::getenv("UNCHARTED_U2MP_JOIN");
    if (join && *join) {
        net::Address host;
        if (!net::Address::parse(join, port, host) || !transport.join(host)) {
            log("cannot join %s", join);
            return false;
        }
        hosting = false;
        my_id = 0;
        log("joining the party at %s", host.text().c_str());
    } else {
        if (!transport.host(port)) {
            log("cannot listen on UDP port %u", port);
            return false;
        }
        hosting = true;
        my_id = 1;
        members[0] = Member{true, 1, my_name, local_rank()};
        log("hosting a party on UDP port %u", port);
    }
    return true;
}

// ---- per frame ----------------------------------------------------------------------------------
// The menu's top page id (menu +0x1010 top index, +0x9d0 page records, record +0x10 id).
uint32_t top_page_id() {
    const uint64_t menu = addr::menu_manager;
    const int32_t top = G<int32_t>(menu + 0x1010);
    if (top < 0 || top > 0x40) return 0;
    const uint64_t page = G<uint64_t>(menu + 0x9d0 + 8 * uint64_t(top));
    return page ? *reinterpret_cast<const uint32_t *>(uintptr_t(page) + 0x10) : 0;
}

// PAGE_MULTIPLAYER_LEAVING_GAME (handler 0x5eb7d0, PS3 0x43ea8c) waits for match+0xc8 ("left the
// room": PS3 0x4d26b0(match) != 1 after the Matching2 LeaveRoom 0x4d8540 that "Leave Game" 0x44026c
// and the error paths started). PS4 lost the LeaveRoom and nothing sets the byte, so the page never
// closed (Leave Game from the pause menu, an error in the client wait page 0x609d00, Leave::Enter's
// error path). The PC has no room to leave: the byte is set while the page is up. The game then
// clears the players, pops and goes to NET_SM 18 (netInfo+0x34 cleared first: back in the lobby; the
// party stays together, the PC's match is the party's).
constexpr uint32_t kPageLeavingGame = 0xb808d03c;
static_assert(sid("PAGE_MULTIPLAYER_LEAVING_GAME") == kPageLeavingGame, "page id");
void finish_leaving_game() {
    if (top_page_id() != kPageLeavingGame || G<uint8_t>(kMatch + M_LEFT)) return;
    log("leaving the match: LEAVING_GAME may close (match +0xc8 = 1)");
    G<int32_t>(kNetInfo + NI_FROM_MATCHMAKING) = 0;
    G<uint8_t>(kMatch + M_LEFT) = 1;
}

void tick();
void host_create();
void log_match_choice();

// Page-event handler for every page (page id 0): event 6 every frame while a menu page is up.
U2MP_GUEST void page_event(uint32_t *event) {
    if (!event || event[0] != kEventUpdate) return;
    finish_leaving_game();   // also after the transport closed (host lost: the client wait page's error path)
    tick();
}

// ---- the "net" package (MP menu art: mode buttons, game types, map cards, playlists, badges) ----
// Its images form the net image table 0x1130008+0x70, which 0x554090 (every frame) takes from the
// package "net" (sid 4240ef2e) once it is logged in (package +0x30 == 7), else NULL: then every type 3
// image of the multiplayer menus (mode buttons 0x655f11, game type 0x656864, map 0x656ac8) finds
// nothing, is drawn with size 0, and the rich rows of PAGE_LOBBY collapse onto one line. The game's
// own request (PAGE_MULTIPLAYER_CONNECTING 0x5ed839) marks the package with flag 0x40 right after
// RequestPackage (as the hud request 0x4b8c7f does); our request did not (the likely reason the menu
// images were missing; check_net_package logs what the table holds). Requested on entry and again
// every second while the menus are up (RequestPackage returns the package when it exists).
constexpr uint64_t PKG_FLAGS = 0x18, PKG_STATE = 0x30, kNetImageTable = 0x1130008 + 0x70;
uint8_t *request_package(const char *name) {
    auto *pkg = fn<uint8_t *(U2MP_GUEST *)(void *, const char *, int)>(fRequestPackage)(image.at<void>(kPackageTable),
                                                                                    name, 0);
    if (pkg) pkg[PKG_FLAGS] |= 0x40;
    return pkg;
}
// With it the imported "net-u2mp" (the PS3 menu images PS4's net.pak lacks, menuimages.cpp), when present.
uint8_t *keep_net_package() {
    static const char net_package[] = "net";
    if (const char *extra = extra_net_package()) request_package(extra);
    return request_package(net_package);
}
// The package table lookup without a request (as 0x5540b9: FNV-1a over the id's bytes; buckets
// +0x10, bucket count +0x4c, node {+0 next, +0x10 id, +0x18 package}, buckets[count] = end node).
const uint8_t *find_package(uint32_t id) {
    const uint64_t buckets = G<uint64_t>(kPackageTable + 0x10);
    const int32_t count = G<int32_t>(kPackageTable + 0x4c);
    if (!buckets || count <= 0) return nullptr;
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 4; ++i) h = (h ^ ((id >> (8 * i)) & 0xff)) * 0x1000193u;
    const auto *table = reinterpret_cast<const uint64_t *>(uintptr_t(buckets));
    uint64_t node = table[h % uint32_t(count)];
    while (node && *reinterpret_cast<const uint32_t *>(uintptr_t(node) + 0x10) != id)
        node = *reinterpret_cast<const uint64_t *>(uintptr_t(node));
    if (!node || node == table[count]) return nullptr;
    return reinterpret_cast<const uint8_t *>(uintptr_t(*reinterpret_cast<const uint64_t *>(uintptr_t(node) + 0x18)));
}
// Keeps the package while the multiplayer menus are up (in a match the level owns memory; there it
// is only looked at), and logs what the test needs to tell why menu images are missing.
void skip_net_fade() {
    // Net images fade in over 300 frames from the package's login frame (0x5bca18: frame counter
    // 0x1116180+0x144 minus package +0x924). PS3 kept the package loaded, so the Multiplayer page
    // showed its icons at once; here the package loads when Multiplayer is entered, and the icons
    // faded in seconds later. Once logged in, the fade counts as done.
    for (const char *name : {"net", "net-u2mp"}) {
        auto *p = const_cast<uint8_t *>(find_package(sid(name)));
        if (!p || *reinterpret_cast<const int32_t *>(p + PKG_STATE) != 7) continue;
        const int32_t now = G<int32_t>(0x1116180 + 0x144);
        auto &login = *reinterpret_cast<int32_t *>(p + 0x924);
        if (now - login < 300) login = now - 300;
    }
}
void check_net_package(bool menus) {
    static int32_t last_state = -100;
    static uint64_t last_table = ~0ull;
    static uint32_t last_count = ~0u;
    const uint8_t *pkg = menus ? keep_net_package() : find_package(sid("net"));
    const int32_t st = pkg ? *reinterpret_cast<const int32_t *>(pkg + PKG_STATE) : -1;
    const uint64_t table = G<uint64_t>(kNetImageTable);
    const uint32_t count = table ? *reinterpret_cast<const uint16_t *>(uintptr_t(table) + 2) : 0;
    if (st != last_state || (table != 0) != (last_table != 0) || count != last_count) {
        log("net package: %s, state %d, flags 0x%x; net image table %s, %u images", pkg ? "present" : "missing", st,
            pkg ? unsigned(*reinterpret_cast<const uint16_t *>(pkg + PKG_FLAGS)) : 0u, table ? "set" : "NULL", count);
        last_state = st;
        last_table = table;
        last_count = count;
    }
}

// PS3 PAGE_MULTIPLAYER_CONNECTING (0x445020, at 0x4454ac) also requested the packages of the name
// list at 0xe155d0 that were not loaded yet (0xaf220), each with the flag the "net" request sets
// (PS3 +0x10 |= 0x200 << 32, PS4 +0x18 |= 0x40 as 0x5ed843): the multiplayer weapons and the
// multiplayer animation sets. PS4 has no copy of the list (none of its strings is referenced); all
// 21 packages exist in the PS4 data (pak22_ps4.txt, actor27). UNCHARTED_U2MP_PRELOAD=0 skips it.
void preload_ps3_packages() {
    static const char *const kNames[] = {
        "beretta-pistol", "beretta-pistol-magazine", "colt-defender-pistol", "colt-defender-magazine",
        "desert-eagle", "desert-eagle-magazine", "dragunov-magazine", "dragunov-rifle", "fn-fal-clip",
        "fn-fal-rifle", "m4-rifle", "m4-rifle-magazine", "shotgun", "spas12-shotgun", "machine-gun",
        "machine-gun-clip", "rpg", "anim-proto-multiplayer", "anim-proto-shield", "anim-proto-carry-objects",
        "m32-grenade-launcher"};
    if (const char *v = std::getenv("UNCHARTED_U2MP_PRELOAD"); v && std::strcmp(v, "0") == 0) return;
    int requested = 0, present = 0, failed = 0;
    for (const char *name : kNames) {
        if (find_package(sid(name))) {
            ++present;
            continue;
        }
        if (request_package(name)) ++requested;
        else ++failed;
    }
    log("package preload (PS3 0xe155d0): %d requested, %d already present, %d not created", requested, present, failed);
}

void register_frame_handler() {
    const uint64_t menu = addr::menu_manager;
    uint32_t &count = G<uint32_t>(menu + MENU_HANDLER_COUNT);
    for (uint32_t i = 0; i < count; ++i)
        if (G<uint64_t>(menu + MENU_HANDLERS + i * MENU_HANDLER_SIZE + 8) == uint64_t(uintptr_t(&page_event))) return;
    if (count >= kMaxPageHandlers) {
        log("menu page handler table full: no per-frame updates in menus");
        return;
    }
    const uint64_t e = menu + MENU_HANDLERS + count * MENU_HANDLER_SIZE;
    G<uint32_t>(e) = 0;
    G<uint64_t>(e + 8) = uint64_t(uintptr_t(&page_event));
    G<uint64_t>(e + 0x10) = 0;
    ++count;
    log("per-frame page handler registered (%u handlers)", count);
}

void unregister_frame_handler() {
    const uint64_t menu = addr::menu_manager;
    uint32_t &count = G<uint32_t>(menu + MENU_HANDLER_COUNT);
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t e = menu + MENU_HANDLERS + i * MENU_HANDLER_SIZE;
        if (G<uint64_t>(e + 8) != uint64_t(uintptr_t(&page_event))) continue;
        for (uint32_t j = i; j + 1 < count; ++j)
            std::memcpy(image.at<void>(menu + MENU_HANDLERS + j * MENU_HANDLER_SIZE),
                        image.at<void>(menu + MENU_HANDLERS + (j + 1) * MENU_HANDLER_SIZE), MENU_HANDLER_SIZE);
        --count;
        return;
    }
}

// The match states' Update, wrapped: original, then the network.
using StateUpdateFn = uint64_t(U2MP_GUEST *)(void *state);
StateUpdateFn original_update[kStateCount];
// Diagnostics: which TaskManagerOnline state runs (logged when it changes, and every 10 s).
std::atomic<int> last_online_state{-1};
uint64_t next_online_log = 0;
// LoadLevels waits for 0x64b770(ngm, task): every package loaded (0x56ce50 on the package table),
// the task's level (task+0x80 -> +0x88 -> +0x28) and the net game's second level (record +0x78) at
// state 8 in the level list 0x1534140 (node +0 next, +0x10 state, +0x98 id), and 0xa18010 false.
// Logs each part (test run 19 stayed in LoadLevels).
//
// Test run 21: the wait list holds the package "<map>-pak" (the name of the net game's level record,
// *levels* 0dec97a4 +0x18, e.g. ice-cave-multi-pak) next to the map and its sky. No registry defines
// those names (PS3 1.10's pak22.txt defines 5 of 13, as empty level groups; PS4 none) and no file
// exists, so the package is never created and LoadLevels waits forever. Its entry in the first list
// gets the list's own skip flag (bit 1, which 0x56ce50 honours there) while no such package exists;
// the map and sky packages are still waited for.
void skip_missing_level_pak() {
    struct Table { int32_t count; int32_t pad; uint8_t *records; };
    auto lookup = fn<Table *(U2MP_GUEST *)(uint32_t)>(0xa15e70);
    Table *games = lookup(0x3b9a067d), *levels = lookup(0x0dec97a4);
    const int16_t game = G<int16_t>(kNgm + NGM_GAME);
    if (!games || !levels || !games->records || !levels->records || game < 0 || game >= games->count) return;
    const uint32_t level_id = *reinterpret_cast<const uint32_t *>(games->records + 0x90 * game + 4);
    const char *name = nullptr;
    for (int32_t i = 0; i < levels->count; ++i)
        if (*reinterpret_cast<const uint32_t *>(levels->records + 0x38 * i) == level_id)
            name = *reinterpret_cast<const char *const *>(levels->records + 0x38 * i + 0x18);
    if (!name) return;
    const size_t n = std::strlen(name);
    if (n < 4 || std::strcmp(name + n - 4, "-pak") != 0) return;
    const uint32_t id = sid(name);
    if (find_package(id)) return;
    const uint64_t table = uint64_t(uintptr_t(image.at<uint8_t>(0x1130390)));
    const uint32_t count = *reinterpret_cast<const uint32_t *>(uintptr_t(table + 0x5a98));
    auto *e = *reinterpret_cast<uint8_t **>(uintptr_t(table + 0x5aa0));
    for (uint32_t i = 0; e && i < count && i < 256; ++i, e += 0x10) {
        auto &fl = *reinterpret_cast<uint32_t *>(e + 0xc);
        if (*reinterpret_cast<const uint32_t *>(e + 8) == id && !(fl & 1)) {
            fl |= 1;
            log("load levels: %s is defined nowhere (no registry entry, no file): not waited for", name);
        }
    }
}
constexpr uint64_t kAbortSlot = 0x103fa40 + 0x30, kAbortByte = 0x113a270;
using AbortFn = U2MP_GUEST void (*)(void *);
AbortFn original_abort = nullptr;
U2MP_GUEST void abort_logged(void *self) {
    const uint64_t ra = uint64_t(uintptr_t(__builtin_return_address(0))), base = uint64_t(uintptr_t(image.base));
    log("match abort (0x113a270 = 1) from %s0x%llx, session %p", ra >= base && ra < base + image.size ? "guest " : "",
        (unsigned long long)(ra >= base && ra < base + image.size ? ra - base : ra), self);
    original_abort(self);
}
// What LoadLevels / WaitReady test before going to Leave: the abort byte, the host tracker (ngm+8)
// and its excluded flag (+0x26c).
void log_leave_reason(const char *when) {
    const uint64_t host = G<uint64_t>(kNgm + NGM_HOST);
    log("%s: abort byte %u, host tracker %s, host excluded %u", when, unsigned(G<uint8_t>(kAbortByte)),
        host ? "set" : "NULL", host ? unsigned(*reinterpret_cast<const uint8_t *>(uintptr_t(host) + 0x26c)) : 0u);
    // Playing::Update (0x5f14d0) exits (run 39): leave flag 0x104f1c0, error box 0x11474f8 (+0x44 kind,
    // +0x6c0 state), idle timer 0x14cec0c (kick at 120 s, 0x5ec010), present players 0x63ffb0(0x113b760).
    const int players = fn<int(U2MP_GUEST *)(void *)>(0x63ffb0)(image.at<void>(0x113b760));
    log("%s: leave flag %u, error box kind %d state %d, idle %.1f s, players %d, room %d", when,
        unsigned(G<uint8_t>(0x104f1c0)), G<int32_t>(0x11474f8 + 0x44), G<int32_t>(0x11474f8 + 0x6c0),
        double(G<float>(0x14cec0c)), players, G<int32_t>(0x14ce97c));
}
void log_load_levels(void *state) {
    log_leave_reason("load levels");
    auto *task = *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(state) + 8);
    uint32_t task_level = 0;
    if (task) {
        auto *a = *reinterpret_cast<uint8_t **>(task + 0x80);
        auto *b = a ? *reinterpret_cast<uint8_t **>(a + 0x88) : nullptr;
        if (b) task_level = *reinterpret_cast<uint32_t *>(b + 0x28);
    }
    const bool packages = fn<bool(U2MP_GUEST *)(void *)>(0x56ce50)(image.at<void>(0x1130390));
    const bool busy = fn<bool(U2MP_GUEST *)()>(0xa18010)();
    uint32_t second = 0;
    struct Table { int32_t count; int32_t pad; uint8_t *records; };
    auto *games = fn<Table *(U2MP_GUEST *)(uint32_t)>(0xa15e70)(0x3b9a067d);
    const int16_t game = G<int16_t>(kNgm + NGM_GAME);
    if (games && games->records && game >= 0 && game < games->count)
        second = *reinterpret_cast<uint32_t *>(games->records + 0x90 * game + 0x78);
    char list[512] = "";
    size_t used = 0;
    const uint64_t head = uint64_t(uintptr_t(image.at<uint8_t>(0x1534140)));
    uint64_t node = G<uint64_t>(0x1534140);
    for (int n = 0; node && node != head && n < 24; ++n, node = *reinterpret_cast<uint64_t *>(uintptr_t(node))) {
        const auto *p = reinterpret_cast<const uint8_t *>(uintptr_t(node));
        used += size_t(std::snprintf(list + used, sizeof list - used, " %08x:%d", *reinterpret_cast<const uint32_t *>(p + 0x98),
                                     *reinterpret_cast<const int32_t *>(p + 0x10)));
        if (used >= sizeof list - 16) break;
    }
    log("load levels: packages loaded %d, busy %d, task level %08x, second level %08x; levels%s", packages, busy,
        task_level, second, list);
    // 0x56ce50: two request lists of the package table ({count +0x5a98, entries +0x5aa0} and {+0x5aa8,
    // +0x5ab0}, 0x10 bytes: +8 package id, +0xc flags; flag 1 = skip in the first list): each package
    // must exist with state 7 and flags & 0x80. Logs the ones that fail (id = sid of the package name).
    const uint64_t table = uint64_t(uintptr_t(image.at<uint8_t>(0x1130390)));
    for (int l = 0; l < 2 && !packages; ++l) {
        const uint32_t count = *reinterpret_cast<const uint32_t *>(uintptr_t(table + (l ? 0x5aa8 : 0x5a98)));
        const auto *e = *reinterpret_cast<const uint8_t *const *>(uintptr_t(table + (l ? 0x5ab0 : 0x5aa0)));
        for (uint32_t i = 0; e && i < count && i < 256; ++i, e += 0x10) {
            const uint32_t id = *reinterpret_cast<const uint32_t *>(e + 8), fl = *reinterpret_cast<const uint32_t *>(e + 0xc);
            if (l == 0 && (fl & 1)) continue;
            const uint8_t *p = find_package(id);
            const int32_t st = p ? *reinterpret_cast<const int32_t *>(p + PKG_STATE) : -1;
            const uint32_t pf = p ? *reinterpret_cast<const uint16_t *>(p + PKG_FLAGS) : 0;
            if (!p || st != 7 || !(pf & 0x80))
                log("load levels: list %d package %08x (flags %x): %s, state %d, package flags 0x%x", l, id, fl,
                    p ? "present" : "missing", st, pf);
        }
    }
}
template <size_t I> U2MP_GUEST uint64_t state_update(void *state) {
    const uint64_t now_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (last_online_state.exchange(int(I)) != int(I) || now_ms >= next_online_log) {
        next_online_log = now_ms + 10000;
        log("online state: update 0x%llx (vtable slot 0x%llx)", (unsigned long long)kStateUpdates[I].update,
            (unsigned long long)(kStateUpdates[I].slot - 0x18));
        if (kStateUpdates[I].update == 0x5f2dc0) log_load_levels(state);
        if (kStateUpdates[I].update == 0x5eff30) log_leave_reason("leave");
    }
    if (kStateUpdates[I].update == 0x5f2dc0) skip_missing_level_pak();
    // Test runs 39-40: the match-over byte 0x104f1c0 is 1 in the image's data and only Leave::Enter
    // (0x5f00bb, with 0x14ce978) and the replay Leave clear it, so the first match of a session saw it
    // set and Playing went to Leave at once (later matches were fine). Clear both while the match is
    // being set up, as Leave::Enter leaves them for the next match.
    if (kStateUpdates[I].update == 0x5f2ea0 && (G<uint8_t>(0x104f1c0) || G<uint8_t>(0x14ce978))) {
        log("match setup: match-over flag was %u (0x14ce978 %u), cleared", unsigned(G<uint8_t>(0x104f1c0)),
            unsigned(G<uint8_t>(0x14ce978)));
        G<uint8_t>(0x104f1c0) = 0;
        G<uint8_t>(0x14ce978) = 0;
    }
    // Test run 24: LoadLevels::Enter (0x5f2e50 -> 0x640fc0) resets the NetGameManager, so the players
    // Start Match registered are gone (ngm+8, the host tracker, NULL) and LoadLevels / WaitReady go to
    // Leave. PS3's session layer registered them again; here the party's members are put back while
    // the match is being set up (EstablishConnection, LoadLevels, WaitReady).
    if (active && (kStateUpdates[I].update == 0x5f2ea0 || kStateUpdates[I].update == 0x5f2dc0 ||
                   kStateUpdates[I].update == 0x5f26d0) &&
        !G<uint64_t>(kNgm + NGM_HOST)) {
        log("match setup: the net players were cleared, registering the party again");
        fill_net_players();
    }
    const uint64_t result = original_update[I](state);
    tick();
    return result;
}
template <size_t... I> constexpr std::array<void *, sizeof...(I)> make_state_updates(std::index_sequence<I...>) {
    return {reinterpret_cast<void *>(&state_update<I>)...};
}
const std::array<void *, kStateCount> state_update_fns = make_state_updates(std::make_index_sequence<kStateCount>{});

// The network memory context (0x14): the allocator (0xb332a0) takes context n's heap from the table at
// [0x104ae00] + n*8. PS3 had a network heap (ALLOCATION_NETWORK_HEAP); PS4 never creates it, so the
// slot is null and every network allocation (PlayerInfo 0x6144b0 and others) faults. Point it at the
// default heap (the one context 0x46 resolves to, 0x11c6630+0x198); frees use the same table.
void ensure_network_heap() {
    constexpr uint64_t kAllocTablePtr = 0x104ae00, kDefaultHeap = 0x11c6630 + 0x198, kNetContext = 0x14;
    const uint64_t table = G<uint64_t>(kAllocTablePtr);
    if (!table) return;
    auto *slot = reinterpret_cast<uint64_t *>(uintptr_t(table) + kNetContext * 8);
    if (*slot) return;
    const uint64_t heap = G<uint64_t>(kDefaultHeap);
    if (!heap) {
        log("network heap: no default heap to share");
        return;
    }
    *slot = heap;
    log("network heap: context 0x14 now uses the default heap");
}

// ---- restored callbacks and queries ------------------------------------------------------------

// BUTTON_MULTIPLAYER (PS3 0x43b150 -> bootstrap 0x438bc8 -> NetInit 0x447a5c, which ended with
// SetNetState(18)). PS4 0x5eb160 was a bare ret.
U2MP_GUEST void multiplayer_button(void *, void *, void *) {
    log("Multiplayer selected");
    netheap_install();   // context 0x14: a heap of its own (netheap.cpp)
    netinit_prepare();   // what PS3 NetGameManager::Init / NetInit built (netinit.cpp)
    netpatch_load();     // the server live-data module net-patch9 (weapon sets ...; PS3 NetInit 0x447a5c)
    if (!active) {
        if (!start_transport()) {
            G<uint8_t>(kNetInfo + NI_ERROR) = 1;
            show_page(sid("PAGE_MULTIPLAYER"));   // shows the network error box and goes back
            return;
        }
        active = true;
    }
    G<uint8_t>(kNetInfo + NI_ERROR) = 0;
    G<int32_t>(kNetInfo + NI_FROM_MATCHMAKING) = 0;
    G<int16_t>(kNgm + NGM_GAME) = -1;
    if (hosting) session_set(kParty, kRoomContext, 1, 1);   // party leader
    else session_set(kParty, kRoomContext, my_id ? my_id : 0xfff, 1);
    session_members(kParty);
    sent_mode = -1;
    set_state(kChooseHostJoin);   // IsOnline: state > 1
    register_frame_handler();
    // PS3 showed PAGE_MULTIPLAYER_CONNECTING while NetInit ran; its PS4 handler waits for the "net"
    // package forever (the request never reaches the loader). Request it here and go straight on.
    keep_net_package();
    preload_ps3_packages();
    show_page(sid("PAGE_MULTIPLAYER"));
}

// Party rows PARTY_PLAYER0..9 (PS3 roster query 0x4bc728) and the match/post-game rows PLAYER0..9
// (cond 6..15): the query object is {+0 op, +4 value, +0x40 text, +0x48 table arg}. The party rows
// are two sets: PAGE_MULTIPLAYER cond 16..25 (arg 0..9) and PAGE_LOBBY cond 26..35 (arg 10..19);
// PS3 takes the row as arg % 10 (0x4bc780), so both pages list the same party.
void roster_query(uint8_t *query) {
    const uint32_t op = *reinterpret_cast<uint32_t *>(query);
    const uint64_t row = *reinterpret_cast<uint64_t *>(query + 0x48) % 10;
    int seen = 0;
    const Member *m = nullptr;
    uint8_t mslot = 0;
    for (uint8_t slot = 0; slot <= net::kMaxPeers && !m; ++slot)
        if (members[slot].used && uint64_t(seen++) == row) { m = &members[slot]; mslot = slot; }
    if (op == 7) *reinterpret_cast<uint32_t *>(query + 4) = m ? 1 : 0;
    else if (op == 3) {
        // PS3 0x4bc9f0: image 0 (+4 == 0) is the member's rank badge, type 0x27 (menuimages.cpp draws
        // it) with the rank at +0x18 (PS3 +16); image 1 was the cellVoice talking icon (not here).
        if (m && *reinterpret_cast<uint32_t *>(query + 4) == 0) {
            *reinterpret_cast<uint32_t *>(query + 0x08) = 0x27;
            *reinterpret_cast<uint64_t *>(query + 0x10) = 0;
            *reinterpret_cast<uint32_t *>(query + 0x18) = m->rank;
            *reinterpret_cast<uint64_t *>(query + 0x1c) = 0;
            *reinterpret_cast<int32_t *>(query + 0x24) = -1;
            *reinterpret_cast<uint32_t *>(query + 0x28) = 0;
            *reinterpret_cast<int32_t *>(query + 0x2c) = -1;
            *reinterpret_cast<float *>(query + 0x30) = 1.0f;
        }
    } else if (op == 1) {
        std::snprintf(roster_text[mslot], sizeof roster_text[mslot], "%s", m ? m->name.c_str() : "");
        *reinterpret_cast<uint64_t *>(query + 0x40) = m ? uint64_t(uintptr_t(roster_text[mslot])) : uint64_t(uintptr_t(""));
    } else
        *reinterpret_cast<uint32_t *>(query + 4) = 0;
}
U2MP_GUEST void party_rows(uint8_t *query) { roster_query(query); }
U2MP_GUEST void player_rows(uint8_t *query) { roster_query(query); }

// BUTTON_MULTIPLAYER_STARTMATCH (PS4 0x5eb780 was a bare ret; the query enables it for the party
// leader only). PS3: lobby -> CREATE_GAME_WAIT (0x444bb0 = PS4 0x5eb830, intact) -> room created.
void create_match_now();
U2MP_GUEST void start_match(void *, void *, void *) {
    if (!hosting) return;
    // Matchmaking (mode 1): PS3 0x43aab0 -> START_MATCHMAKING -> the matchmaking page 0x4b0a58
    // (PS4 0x64e910 = ret), ported in frontend3.cpp (gather, vote, countdown; it then calls
    // create_match). Without that port: the host picks a random game of the playlist here
    // (PS3's random pick 0x49bb00 has no PS4 pair; SetNetGame 0x63dc30).
    if (G<int32_t>(kNetInfo + NI_MODE) == 1 && matchmaking::installed()) {
        matchmaking::leader_start();
        return;
    }
    if (G<int32_t>(kNetInfo + NI_MODE) == 1) {
        struct Table { int32_t count; int32_t pad; uint8_t *records; };
        auto *table = fn<Table *(U2MP_GUEST *)(uint32_t)>(0xa15e70)(0x3b9a067d);
        const int16_t current = G<int16_t>(kNgm + NGM_GAME);
        if (table && table->count > 0 && table->records) {
            const int32_t chosen = playlist_pick_type();   // the playlist's game types (menus.cpp)
            const int32_t type = chosen >= 0 ? chosen
                                             : *reinterpret_cast<int32_t *>(table->records + 0x90 * (current >= 0 ? current : 0) + 0x0c);
            std::vector<int16_t> same;
            for (int32_t i = 0; i < table->count; ++i)
                if (*reinterpret_cast<int32_t *>(table->records + 0x90 * i + 0x0c) == type && dlc_net_game_available(i))
                    same.push_back(int16_t(i));   // DLC maps only when imported (dlc.cpp)
            if (!same.empty()) {
                const int16_t pick = same[size_t(std::rand()) % same.size()];
                fn<void(U2MP_GUEST *)(void *, int16_t, const void *, int)>(0x63dc30)(image.at<void>(kNgm), pick, nullptr, 10);
                log("matchmaking: net game %d of %zu of type %d", pick, same.size(), type);
            }
        }
    }
    create_match_now();
}

// Host: the match of the current net game (the custom-game create path); the party follows.
void create_match_now() {
    log("Start Match: creating the match");
    G<uint8_t>(kMatch + M_LEFT) = 0;
    log_match_choice();
    netinit_match();
    netinit_dump("before create");
    host_create();
    netinit_dump("after create");   // state 3, page handler 0x6093f0
    session_set(kMatch, kRoomContext, 1, 1);
    session_members(kMatch);
    fill_net_players();
    G<int32_t>(kMatch + M_CREATE_RESULT) = 0;
    G<uint8_t>(kMatch + M_CREATED) = 1;   // 0x6093f0: 3 -> 10 -> 16 -> PlayTask("deathmatch")
    in_match_setup = true;
    send_session(-1, {kFollow});
}

// Diagnostics: what Start Match is about to start (mode, the custom choice list, the net game and its level).
void log_match_choice() {
    struct Table { int32_t count; int32_t pad; uint8_t *records; };
    auto lookup = fn<Table *(U2MP_GUEST *)(uint32_t)>(0xa15e70);
    const int32_t mode = G<int32_t>(kNetInfo + NI_MODE);
    const int32_t choices = G<int32_t>(0x113ddb0);
    const int16_t game = G<int16_t>(kNgm + NGM_GAME);
    log("match choice: mode %d, custom choice list %d entries, net game %d", mode, choices, game);
    Table *games = lookup(0x3b9a067d);   // *net-games*
    if (!games || game < 0 || game >= games->count || !games->records) {
        log("match choice: net game %d is not in *net-games* (%d records)", game, games ? games->count : -1);
        return;
    }
    const uint8_t *rec = games->records + 0x90 * game;
    const uint32_t level_id = *reinterpret_cast<const uint32_t *>(rec + 4);
    const int32_t type = *reinterpret_cast<const int32_t *>(rec + 0x0c);
    const char *level = "?";
    if (Table *levels = lookup(0x0dec97a4))
        for (int32_t i = 0; levels->records && i < levels->count; ++i) {
            const uint8_t *l = levels->records + 0x38 * i;
            if (*reinterpret_cast<const uint32_t *>(l) == level_id) {
                const char *name = *reinterpret_cast<const char *const *>(l + 0x18);
                if (name) level = name;
                break;
            }
        }
    log("match choice: game type %d, level id %08x, package %s", type, level_id, level);
}

// The custom game create path (PS3 0x444bb0 = PS4 0x5eb830) without its room-attribute pick: that
// part reads a list (0x14ce990 count / 0x14ce9b8 array) the PS3 network init filled; on PS4 it is
// empty and 0x5eb830 dereferences null. The rest is the same, in the same order.
void host_create() {
    G<uint8_t>(kNetInfo + NI_CREATED_BY_ME) = 1;
    fn<void(U2MP_GUEST *)(void *)>(fClearNetPlayers)(image.at<void>(kNgm));
    G<int32_t>(kNetInfo + NI_FROM_MATCHMAKING) = 0;
    const uint64_t menu = addr::menu_manager;
    fn<void(U2MP_GUEST *)(void *)>(0x5cd1e0)(image.at<void>(menu));        // menu reset
    G<uint32_t>(G<uint64_t>(menu + 0x1828) - uint64_t(uintptr_t(image.base)) + 8) = 0x69deab50;   // wait text
    constexpr uint32_t kHostPage = 0x471007a1;
    uint32_t &count = G<uint32_t>(menu + MENU_HANDLER_COUNT);
    uint64_t entry = 0;
    for (uint32_t i = 0; i < count && !entry; ++i)
        if (G<uint32_t>(menu + MENU_HANDLERS + i * MENU_HANDLER_SIZE) == kHostPage) entry = menu + MENU_HANDLERS + i * MENU_HANDLER_SIZE;
    if (!entry && count < kMaxPageHandlers) entry = menu + MENU_HANDLERS + (count++) * MENU_HANDLER_SIZE;
    if (entry) {
        G<uint32_t>(entry) = kHostPage;
        G<uint64_t>(entry + 8) = guest(0x6093f0);
        G<uint64_t>(entry + 0x10) = 0;
    }
    register_frame_handler();   // in case the reset cleared it
    show_page(kHostPage);
    set_state(kCreateGameWait);
}

// LEAVE_MULTIPLAYER (PS4 0x5eb180 only popped the page; PS3 teardown 0x4386d0).
U2MP_GUEST void leave_multiplayer(void *, void *, void *) {
    log("leaving multiplayer");
    unregister_frame_handler();
    for (uint8_t slot = 0; slot <= net::kMaxPeers; ++slot) gamenet::detach(slot);
    voice::stop();
    transport.close();
    active = false;
    session_clear(kParty);
    session_clear(kMatch);
    G<int32_t>(kNetInfo + NI_MODE) = 0;
    G<int16_t>(kNgm + NGM_GAME) = -1;
    fn<void(U2MP_GUEST *)(void *)>(fClearNetPlayers)(image.at<void>(kNgm));
    progression_publish();   // unlocks bought in the menus since the last match
    set_state(kStateOff);
    for (auto &m : members) m = Member{};
    fn<void(U2MP_GUEST *)(void *)>(fPopPage)(image.at<void>(addr::menu_manager));
}

// Once per game frame (menu page handler or match state update, whichever runs first).
double last_tick = -1;
// The net clock (PS3 [0xf116d4], "Net Clock = %f": frames at +0xc, times a float = seconds; PS4
// pointer 0x113a040). PS4 has ~40 readers of [ptr]+0xc and one reset of byte +0 (0x5e8b22), and
// nothing that creates it: WaitReady (0x5f276c) read through NULL (test run 27). Its unit is 1/300 s:
// PS3 "Net Clock = %f" prints +0xc times 1/300 (0xdb04c0), PS4's readers convert the same way (the
// matchmaking status 0x65aab5 multiplies by 1/300 at 0xe62628), and remaining.cpp's kClockRate is
// 300. Ours counts 300 ticks per real second from its creation (it used to count frames, 60 per
// second, so every countdown read from it ran five times too slow).
constexpr uint64_t kNetClock = 0x113a040;
uint8_t *net_clock = nullptr;
std::chrono::steady_clock::time_point net_clock_base;
uint32_t own_ticks() {
    static const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    const auto since = std::chrono::steady_clock::now() - (net_clock ? net_clock_base : origin);
    return uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(since).count() * 3 / 10000);
}
void update_net_clock() {
    if (!net_clock) {
        if (G<uint64_t>(kNetClock)) return;   // something else made one
        net_clock = static_cast<uint8_t *>(netheap_alloc(0x40, 16));
        if (!net_clock) return;
        std::memset(net_clock, 0, 0x40);
        net_clock_base = std::chrono::steady_clock::now();
        G<uint64_t>(kNetClock) = uint64_t(uintptr_t(net_clock));
        log("net clock created (300 ticks per second)");
    }
    if (G<uint64_t>(kNetClock) == uint64_t(uintptr_t(net_clock)))
        *reinterpret_cast<int32_t *>(net_clock + 0xc) = int32_t(own_ticks());
}

void tick() {
    // before the active test: once made, the clock keeps running after the host is lost (not made outside MP)
    if (active || net_clock) update_net_clock();
    if (!active) return;
    const double now = gamenet::clock();
    if (now == last_tick) return;   // already done this frame
    last_tick = now;
    transport.pump(now);
    gamenet::frame(now);
    voice::frame();
    transport.pump(now);
    if (kicked || lost_host) {   // not inside the pump: closing resets the connections it walks
        log("party: closing the transport (%s); Multiplayer starts a new one", kicked ? "kicked" : "host lost");
        kicked = false;
        lost_host = false;
        for (uint8_t slot = 0; slot <= net::kMaxPeers; ++slot) gamenet::detach(slot);
        voice::stop();
    transport.close();
        active = false;
        for (auto &m : members) m = Member{};
        return;
    }
    ++frames;
    skip_net_fade();
    finish_leaving_game();   // in a match the menu handler may not run
    if (frames % 60 == 1) {
        const int32_t st = state();
        check_net_package(st == kChooseHostJoin || st == kCreateGameWait || st == kClientWaitPartyHost);
    }
    if (now >= next_rate_log) {   // proves the hook runs every frame (expect ~60 per second)
        if (next_rate_log > 0) log("frame hook: %u calls in 10 s", frames);
        frames = 0;
        next_rate_log = now + 10;
    }
    if (frames % 120 == 7) {   // the rank badge of the party rows (PS3: room member data +8)
        const uint8_t r = local_rank();
        if (hosting && members[0].used && members[0].rank != r) {
            members[0].rank = r;
            broadcast_roster();
        } else if (!hosting && my_id && r != sent_rank) {
            send_info();
        }
    }
    if (hosting) {   // the party leader's mode reaches the members (PS3: party data)
        const int32_t mode = G<int32_t>(kNetInfo + NI_MODE);
        if (mode != sent_mode) {
            sent_mode = mode;
            std::vector<uint8_t> b{kMode, 0, 0, 0, 0};
            std::memcpy(b.data() + 1, &mode, 4);
            send_session(-1, b);
        }
    }
    const int32_t s = state();
    if (s != last_state) {
        log("net state %d -> %d", last_state, s);
        if (s == kServerLobby || s == kCreateGameWait) netinit_dump(s == kServerLobby ? "state 16" : "state 3");
        if (hosting && s == kCreateGameWait && last_state == kLeaveGame)
            G<uint8_t>(kMatch + M_CREATED) = 0;   // back in the lobby: wait for the next Start Match
        last_state = s;
    }
}

}  // namespace

// ---- session_api.h ----------------------------------------------------------------------------
bool member_at(int row, uint32_t &net_id, std::string &name) {
    int seen = 0;
    for (const Member &m : members)
        if (m.used && seen++ == row) {
            net_id = m.net_id;
            name = m.name;
            return true;
        }
    return false;
}

bool member_name(uint32_t net_id, std::string &name) {
    for (const Member &m : members)
        if (m.used && m.net_id == net_id) {
            name = m.name;
            return true;
        }
    return false;
}

uint32_t local_id() { return active ? my_id : 0; }

void send_message(int slot, const std::vector<uint8_t> &body, int except) {
    if (active && !body.empty()) send_session(slot, body, except);
}

uint32_t net_ticks() {
    const uint64_t clock = G<uint64_t>(kNetClock);
    if (clock) return *reinterpret_cast<const uint32_t *>(uintptr_t(clock) + 0xc);
    return own_ticks();
}

void create_match() {
    if (active && hosting) create_match_now();
}

uint32_t member_rank(uint32_t net_id) {
    for (const Member &m : members)
        if (m.used && m.net_id == net_id) return m.rank;
    return 0;
}

bool is_host() { return active && hosting; }

std::string local_name() {
    if (const char *n = std::getenv("UNCHARTED_U2MP_NAME"); n && *n) return std::string(n).substr(0, 31);
    return my_name;
}

int member_count() {
    int n = 0;
    if (active)
        for (const Member &m : members) n += m.used ? 1 : 0;
    return n;
}

bool kick(uint32_t net_id) {
    if (!active || !hosting || net_id < 2 || net_id > net::kMaxPeers + 1) return false;
    const uint8_t slot = uint8_t(net_id - 1);
    if (!members[slot].used) return false;
    log("party: kicking %s (net id %u)", members[slot].name.c_str(), net_id);
    send_session(slot, {kKick});
    return true;
}

void install(int &installed) {
    if (!gamenet::install()) return;
    struct { uint64_t at; void *fn; const char *name; } hooks[] = {
        {cbMultiplayer, reinterpret_cast<void *>(&multiplayer_button), "BUTTON_MULTIPLAYER"},
        {qPartyRows, reinterpret_cast<void *>(&party_rows), "party roster query"},
        {qPlayerRows, reinterpret_cast<void *>(&player_rows), "player roster query"},
        {cbStartMatch, reinterpret_cast<void *>(&start_match), "BUTTON_MULTIPLAYER_STARTMATCH"},
        {cbLeaveMultiplayer, reinterpret_cast<void *>(&leave_multiplayer), "LEAVE_MULTIPLAYER"},
    };
    // LEAVE_MULTIPLAYER is not a stub: lea rdi, [menu manager]; jmp PopPage.
    static const unsigned char leave_bytes[] = {0x48, 0x8d, 0x3d};
    for (auto &h : hooks) {
        const bool expected = h.at == cbLeaveMultiplayer ? image.matches(h.at, leave_bytes, sizeof leave_bytes)
                                                         : is_ret_stub(h.at);
        if (!expected) {
            log("%s at 0x%llx: not the stripped function this port expects, left alone", h.name,
                (unsigned long long)h.at);
            continue;
        }
        if (replace_function(h.at, 5, h.fn, h.name)) ++installed;
    }
    const uint64_t base = uint64_t(uintptr_t(image.base));
    bool states_ok = true;
    for (const auto &s : kStateUpdates) states_ok = states_ok && G<uint64_t>(s.slot) == base + s.update;
    if (!states_ok) {
        log("match state vtables differ: no per-frame network during matches");
        return;
    }
    for (size_t i = 0; i < kStateCount; ++i) {
        original_update[i] = reinterpret_cast<StateUpdateFn>(base + kStateUpdates[i].update);
        G<uint64_t>(kStateUpdates[i].slot) = uint64_t(uintptr_t(state_update_fns[i]));
    }
    log("match state updates wrapped (%zu states)", kStateCount);
    ++installed;
    // Diagnostics (test run 22: LoadLevels -> Leave): the match session's slot 6 (vtable 0x103fa40
    // +0x30 = 0x5e9f10) sets the abort byte 0x113a270; log who calls it.
    if (G<uint64_t>(kAbortSlot) == base + 0x5e9f10) {
        original_abort = reinterpret_cast<AbortFn>(base + 0x5e9f10);
        G<uint64_t>(kAbortSlot) = uint64_t(uintptr_t(&abort_logged));
    }
}

}  // namespace u2mp::session

namespace u2mp {
const uint8_t *package_by_id(uint32_t id) { return session::find_package(id); }
}  // namespace u2mp
