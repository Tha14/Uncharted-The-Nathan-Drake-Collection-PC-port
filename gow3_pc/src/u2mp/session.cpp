// u2mp session layer: what the PS3 game's NetInit, Matching2 sessions and lobby code did, rebuilt for
// PCs over net::Transport. Blueprint and every address: u2mp/docs/SESSION-INTERFACE.md (PS4 1.02
// image offsets; PS3 1.10 originals in the comments).
//
// Roles: the PC that opens Multiplayer without UNCHARTED_U2MP_JOIN hosts the party (and later the
// match); the others join it by address (UNCHARTED_U2MP_JOIN=ip[:port]). Party and match use one
// transport: the party leader is the match host.
//
// Net ids: host 1, a client in transport slot s has s + 1 (also its connection-record key).
#include <array>
#include <cstdio>
#include <utility>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
double next_rate_log = 0;

uint32_t id_of_slot(uint8_t slot) { return uint32_t(slot) + 1; }

void set_state(int32_t s) { G<int32_t>(kNetState) = s; }
int32_t state() { return G<int32_t>(kNetState); }

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

void send_session(int slot, std::vector<uint8_t> body) {
    body.insert(body.begin(), gamenet::kSessionMessage);
    if (slot < 0) transport.broadcast(true, body.data(), body.size());
    else transport.send(uint8_t(slot), true, body.data(), body.size());
}

void broadcast_roster() {
    std::vector<uint8_t> b{kRoster};
    for (uint8_t slot = 0; slot <= net::kMaxPeers; ++slot) {
        if (!members[slot].used) continue;
        b.push_back(slot);
        const std::string &n = members[slot].name;
        b.push_back(uint8_t(std::min<size_t>(n.size(), 31)));
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
            refresh_sessions();
            broadcast_roster();
        }
        break;
    case kRoster:   // host -> clients: everyone in the party
        if (!hosting) {
            for (auto &m : members) m = Member{};
            for (size_t at = 1; at + 2 <= n;) {
                const uint8_t slot = d[at], len = d[at + 1];
                if (slot > net::kMaxPeers || at + 2 + len > n) break;
                members[slot] = Member{true, id_of_slot(slot), std::string(reinterpret_cast<const char *>(d + at + 2), len)};
                at += 2 + len;
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
            G<uint8_t>(kNetInfo + NI_CREATED_BY_ME) = 0;
            fn<void(U2MP_GUEST *)(void *)>(fClearNetPlayers)(image.at<void>(kNgm));
            session_set(kMatch, kRoomContext, my_id, 1);
            session_members(kMatch);
            G<int32_t>(kMatch + M_JOIN_RESULT) = 0;
            G<uint64_t>(kMatch + M_JOIN_ROOM) = kRoomContext;
            G<uint8_t>(kMatch + M_JOINED) = 1;
            fill_net_players();
            show_page(sid("PAGE_MULTIPLAYER_JOINING"));
            set_state(kClientWaitPartyHost);
            const uint8_t yes = 1;
            mailbox_post(kMsgPartyHostJoined, 1, &yes, 1);   // JOINING 12 -> 14 -> 17
        }
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
        std::vector<uint8_t> b{kInfo, uint8_t(std::min<size_t>(my_name.size(), 31))};
        b.insert(b.end(), my_name.begin(), my_name.begin() + b[1]);
        send_session(0, b);
        log("party: joined the host as net id %u", my_id);
    } else {        // host gone: every multiplayer page falls back to the error box
        log("party: lost the host");
        gamenet::detach(0);
        G<uint8_t>(kNetInfo + NI_ERROR) = 1;
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
        members[0] = Member{true, 1, my_name};
        log("hosting a party on UDP port %u", port);
    }
    return true;
}

// ---- per frame ----------------------------------------------------------------------------------
void tick();
void host_create();

// Page-event handler for every page (page id 0): event 6 every frame while a menu page is up.
U2MP_GUEST void page_event(uint32_t *event) {
    if (event && event[0] == kEventUpdate) tick();
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
template <size_t I> U2MP_GUEST uint64_t state_update(void *state) {
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
    ensure_network_heap();
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
    static const char net_package[] = "net";
    fn<void(U2MP_GUEST *)(void *, const char *, int)>(fRequestPackage)(image.at<void>(kPackageTable), net_package, 0);
    show_page(sid("PAGE_MULTIPLAYER"));
}

// Party rows PARTY_PLAYER0..9 (cond 26..35; PS3 roster query 0x4bc728) and the match/post-game rows
// PLAYER0..9 (cond 6..15): the query object is {+0 op, +4 value, +0x40 text, +0x48 row}.
void roster_query(uint8_t *query) {
    const uint32_t op = *reinterpret_cast<uint32_t *>(query);
    const uint64_t row = *reinterpret_cast<uint64_t *>(query + 0x48);
    int seen = 0;
    const Member *m = nullptr;
    uint8_t mslot = 0;
    for (uint8_t slot = 0; slot <= net::kMaxPeers && !m; ++slot)
        if (members[slot].used && uint64_t(seen++) == row) { m = &members[slot]; mslot = slot; }
    if (op == 7) *reinterpret_cast<uint32_t *>(query + 4) = m ? 1 : 0;
    else if (op == 1) {
        std::snprintf(roster_text[mslot], sizeof roster_text[mslot], "%s", m ? m->name.c_str() : "");
        *reinterpret_cast<uint64_t *>(query + 0x40) = m ? uint64_t(uintptr_t(roster_text[mslot])) : uint64_t(uintptr_t(""));
    } else
        *reinterpret_cast<uint32_t *>(query + 4) = 0;
}
U2MP_GUEST void party_rows(uint8_t *query) { roster_query(query); }
U2MP_GUEST void player_rows(uint8_t *query) { roster_query(query); }

// BUTTON_MULTIPLAYER_STARTMATCH (PS4 0x5eb780 was a bare ret; the query enables it for the party
// leader only). PS3: lobby -> CREATE_GAME_WAIT (0x444bb0 = PS4 0x5eb830, intact) -> room created.
U2MP_GUEST void start_match(void *, void *, void *) {
    if (!hosting) return;
    // Matchmaking (mode 1): PS3 picked a random game (0x49bb00, no PS4 pair) and the matchmaking
    // state machine (0x4b0a58, PS4 0x64e910 = ret) is gone. Here the party is the match: the host
    // picks a random game of the current game type from *net-games* (SetNetGame 0x63dc30).
    if (G<int32_t>(kNetInfo + NI_MODE) == 1) {
        struct Table { int32_t count; int32_t pad; uint8_t *records; };
        auto *table = fn<Table *(U2MP_GUEST *)(uint32_t)>(0xa15e70)(0x3b9a067d);
        const int16_t current = G<int16_t>(kNgm + NGM_GAME);
        if (table && table->count > 0 && table->records) {
            const int32_t type = *reinterpret_cast<int32_t *>(table->records + 0x90 * (current >= 0 ? current : 0) + 0x0c);
            std::vector<int16_t> same;
            for (int32_t i = 0; i < table->count; ++i)
                if (*reinterpret_cast<int32_t *>(table->records + 0x90 * i + 0x0c) == type) same.push_back(int16_t(i));
            if (!same.empty()) {
                const int16_t pick = same[size_t(std::rand()) % same.size()];
                fn<void(U2MP_GUEST *)(void *, int16_t, const void *, int)>(0x63dc30)(image.at<void>(kNgm), pick, nullptr, 10);
                log("matchmaking: net game %d of %zu of type %d", pick, same.size(), type);
            }
        }
    }
    log("Start Match: creating the match");
    host_create();   // state 3, page handler 0x6093f0
    session_set(kMatch, kRoomContext, 1, 1);
    session_members(kMatch);
    fill_net_players();
    G<int32_t>(kMatch + M_CREATE_RESULT) = 0;
    G<uint8_t>(kMatch + M_CREATED) = 1;   // 0x6093f0: 3 -> 10 -> 16 -> PlayTask("deathmatch")
    in_match_setup = true;
    send_session(-1, {kFollow});
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
void tick() {
    if (!active) return;
    const double now = gamenet::clock();
    if (now == last_tick) return;   // already done this frame
    last_tick = now;
    transport.pump(now);
    gamenet::frame(now);
    voice::frame();
    transport.pump(now);
    if (kicked) {   // not inside the pump: closing resets the connections it walks
        kicked = false;
        for (uint8_t slot = 0; slot <= net::kMaxPeers; ++slot) gamenet::detach(slot);
        voice::stop();
    transport.close();
        active = false;
        for (auto &m : members) m = Member{};
        return;
    }
    ++frames;
    if (now >= next_rate_log) {   // proves the hook runs every frame (expect ~60 per second)
        if (next_rate_log > 0) log("frame hook: %u calls in 10 s", frames);
        frames = 0;
        next_rate_log = now + 10;
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

bool is_host() { return active && hosting; }

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
}

}  // namespace u2mp::session
