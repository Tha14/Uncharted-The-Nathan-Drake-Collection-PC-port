// u2mp session: the party and match session layer of the Uncharted 2 multiplayer port.
//
// The PS4 build of the game lost the PS3 multiplayer bootstrap, the party and match sessions, the
// lobby roster queries, Start Match, Leave Multiplayer and the per-frame network update. This file
// stands in for all of them on PC: it owns the module's one transport (one PC hosts, the others join
// it by LAN address or over the online link), keeps the party member list and copies it into the
// game's session objects and player trackers, replaces the five stripped menu functions, drives the
// network once per game frame (from a menu page handler and from the wrapped match state updates),
// owns the net clock the game reads, and logs diagnostics along the match start path.
//
// Reimplemented from the behaviour description u2mp/docs/spec/session.md.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

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

extern "C" int runtime_file_translate(const char *guest, char *out, size_t size);

namespace u2mp::session {
void install(int &installed);
}

namespace u2mp {
namespace {

// ---- game memory ------------------------------------------------------------------------------------
// Image offsets of the objects and values this file reads or writes.
constexpr uint64_t kNetSm = 0x14ce97c;
constexpr uint64_t kPartySession = 0x105efb8;
constexpr uint64_t kMatchSession = 0x1065b08;
constexpr uint64_t kNetInfo = 0x104f1c8;
constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kNpd = 0x11474f8;
constexpr uint64_t kPackageTable = 0x1130390;
constexpr uint64_t kNetImageTable = 0x1130078;
constexpr uint64_t kGameFrame = 0x11162c4;
constexpr uint64_t kNetClockPtr = 0x113a040;
constexpr uint64_t kAbortByte = 0x113a270;
constexpr uint64_t kMatchOver = 0x104f1c0;
constexpr uint64_t kMatchOverCompanion = 0x14ce978;
constexpr uint64_t kIdleTimer = 0x14cec0c;
constexpr uint64_t kCustomChoiceCount = 0x113ddb0;
constexpr uint64_t kLevelListHead = 0x1534140;
constexpr uint64_t kAbortSlot = 0x103fa70;
constexpr uint64_t kAbortOriginal = 0x5e9f10;

// Guest functions.
constexpr uint64_t kPopPage = 0x5cc6f0;
constexpr uint64_t kClearNetPlayers = 0x63db90;
constexpr uint64_t kClearTracker = 0x662a30;
constexpr uint64_t kCreateEvent = 0x6180f0;
constexpr uint64_t kMessageApply = 0x62dc50;
constexpr uint64_t kSendEventAll = 0x644ab0;
constexpr uint64_t kRequestPackage = 0x56ad90;
constexpr uint64_t kPackagesLoaded = 0x56ce50;
constexpr uint64_t kLoaderBusy = 0xa18010;
constexpr uint64_t kLookup = 0xa15e70;
constexpr uint64_t kPresentPlayers = 0x63ffb0;
constexpr uint64_t kSetNetGame = 0x63dc30;
constexpr uint64_t kMenuReset = 0x5cd1e0;
constexpr uint64_t kTeamColor = 0x64aeb0;
constexpr uint64_t kOwnerChanged = 0x5e9f30;

// Ids and page ids.
constexpr uint32_t kNetGamesId = 0x3b9a067d;
constexpr uint32_t kLevelsId = 0x0dec97a4;
constexpr uint32_t kRankTableId = 0xc85e199d;
constexpr uint32_t kPageMultiplayer = 0x53cbb5f2;
constexpr uint32_t kPageJoining = 0x1af4bdd2;
constexpr uint32_t kPageLeavingGame = 0xb808d03c;
constexpr uint32_t kPageHostCreate = 0x471007a1;
constexpr uint64_t kHostCreateHandler = 0x6093f0;
constexpr uint32_t kWaitText = 0x69deab50;
constexpr uint32_t kNetPackage = 0x4240ef2e;
constexpr uint32_t kNetU2mpPackage = 0x5e516de8;
constexpr uint32_t kIconTalking = 0xffa98ec5;
constexpr uint32_t kIconMuted = 0x7a5f5bf1;
constexpr uint16_t kDefaultPort = 27210;
constexpr uint64_t kCtx = 0x55324d50000001ull;   // any non-zero session context; this one is ours

static_assert(sid("PAGE_MULTIPLAYER_JOINING") == kPageJoining, "page id");
static_assert(sid("net") == kNetPackage && sid("net-u2mp") == kNetU2mpPackage, "package ids");

// Session message types (the byte after the layer tag).
enum : uint8_t { kInfo = 1, kRoster = 2, kMode = 3, kFollow = 4, kKick = 5, kTeams = 6, kIds = 8 };

// ---- raw memory helpers -----------------------------------------------------------------------------
template <typename T> T get(const uint8_t *p) { T v; std::memcpy(&v, p, sizeof v); return v; }
template <typename T> void put(uint8_t *p, T v) { std::memcpy(p, &v, sizeof v); }
uint8_t *g(uint64_t offset) { return image.base + offset; }
uint8_t *ptr_at(const uint8_t *p) { return reinterpret_cast<uint8_t *>(get<uint64_t>(p)); }
uint64_t addr_of(const void *p) { return reinterpret_cast<uint64_t>(p); }
int32_t net_sm() { return get<int32_t>(g(kNetSm)); }
void set_net_sm(int32_t v) { put<int32_t>(g(kNetSm), v); }
uint8_t *menu() { return g(addr::menu_manager); }
uint8_t *ngm() { return g(kNgm); }
int16_t current_game() { return get<int16_t>(ngm() + 0x1ca0); }

template <typename Fn> Fn guest(uint64_t offset) { return reinterpret_cast<Fn>(image.base + offset); }

uint8_t *lookup(uint32_t id) {
    return guest<uint8_t *(U2MP_GUEST *)(uint32_t)>(kLookup)(id);
}

// A script table {s32 count; pad; records}: the record pointer, or NULL if the table is unusable.
struct Table {
    int32_t count = 0;
    uint8_t *records = nullptr;
};
Table table_of(uint8_t *t) {
    Table out;
    if (!t) return out;
    out.count = get<int32_t>(t);
    out.records = ptr_at(t + 8);
    return out;
}

bool coop_type(int32_t type) { return type == 4 || type == 8 || type == 9 || type == 12; }

// ---- module state -----------------------------------------------------------------------------------
constexpr int kSlots = int(net::kMaxPeers) + 1;

struct Member {
    bool used = false;
    uint32_t net_id = 0;
    std::string name;
    uint8_t rank = 0;
    uint8_t team = 0;
};

Member members[kSlots];
net::Transport transport;
bool active = false;
bool hosting = false;
uint32_t my_id = 0;
std::string my_name = "Drake";
uint32_t host_id = 1;
int32_t sent_mode = -1;
int32_t last_sm = 0;
uint32_t frames = 0;
bool kicked = false;
bool host_lost = false;
uint8_t sent_rank = 0xff;
uint8_t sent_team = 0xff;
double next_rate_log = 0;
double last_clock = -1;
bool rehost_pending = false;
std::map<uint32_t, std::string> npids;   // net id -> NpId (ordered, so the smallest id is found first)
int closed_frames = 0;
uint8_t *clock_object = nullptr;
std::chrono::steady_clock::time_point clock_base;
int last_state = -1;
int64_t next_online_log = 0;
char row_text[kSlots][64];

void clear_members() {
    for (Member &m : members) m = Member();
}

uint32_t slot_net_id(int slot) { return slot == 0 ? host_id : uint32_t(slot) + 1; }

// The row-th used member in slot order, or -1.
int slot_of_row(int row) {
    if (row < 0) return -1;
    for (int s = 0; s < kSlots; ++s)
        if (members[s].used && row-- == 0) return s;
    return -1;
}

std::string cut(const std::string &s, size_t n) { return s.size() > n ? s.substr(0, n) : s; }

// ---- sessions and trackers --------------------------------------------------------------------------
void set_session(uint64_t session, uint64_t context, uint32_t me, uint32_t host) {
    uint8_t *s = g(session);
    put<uint64_t>(s + 0x10, context);
    put<uint32_t>(s + 0xb00, me);
    put<uint32_t>(s + 0xb04, host);
}

void publish(uint64_t session) {
    // The game's session holds only 10 member records; an 11th member never reaches it.
    uint8_t *records = g(session) + 0x228;
    int written = 0;
    for (int s = 0; s < kSlots && written < 10; ++s) {
        if (!members[s].used) continue;
        uint8_t *r = records + written * 0xd0;
        r[9] = 1;
        put<uint32_t>(r + 0x10, members[s].net_id);
        put<uint32_t>(r + 0x18, members[s].net_id);
        ++written;
    }
    for (int i = written; i < 10; ++i) {
        uint8_t *r = records + i * 0xd0;
        r[9] = 0;
        put<uint32_t>(r + 0x10, 0);
        put<uint32_t>(r + 0x18, 0);
    }
}

void clear_session(uint64_t session) {
    set_session(session, 0, 0, 0);
    uint8_t *records = g(session) + 0x228;
    for (int i = 0; i < 10; ++i) {
        records[i * 0xd0 + 9] = 0;
        put<uint32_t>(records + i * 0xd0 + 0x10, 0);
    }
}

void refresh_sessions() {
    publish(kPartySession);
    if (get<uint64_t>(g(kMatchSession) + 0x10) != 0) publish(kMatchSession);
}

void add_tracker(uint32_t net_id, const std::string &name, bool local) {
    uint8_t *base = ngm() + 0x10;
    int index = -1;
    for (int i = 0; i < 10 && index < 0; ++i) {
        uint8_t *t = base + i * 0x2c0;
        if (t[0x20] != 0 && get<uint32_t>(t + 0x120) == net_id) index = i;
    }
    for (int i = 0; i < 10 && index < 0; ++i)
        if (base[i * 0x2c0 + 0x20] == 0) index = i;
    if (index < 0) {
        log("no free player slot for %s", name.c_str());
        return;
    }
    uint8_t *t = base + index * 0x2c0;
    guest<void(U2MP_GUEST *)(void *)>(kClearTracker)(t);
    const std::string shown = name.empty() ? std::string("Player") : cut(name, 63);
    std::memcpy(t + 0x20, shown.c_str(), shown.size() + 1);
    put<uint32_t>(t + 0x120, net_id);
    put<uint32_t>(t + 0x238, 0);
    put<int32_t>(t + 0x23c, local ? -1 : int32_t(net_id));
    t[0x26c] = 0;
    t[0x275] = 1;
    t[0x27c] = 1;
    if (local) put<uint64_t>(ngm(), addr_of(t));
    if (net_id == host_id) put<uint64_t>(ngm() + 8, addr_of(t));
    log("player %s: net id %u, slot %d%s", name.c_str(), net_id, index, local ? " (local)" : "");
}

void fill_trackers() {
    for (int s = 0; s < kSlots; ++s)
        if (members[s].used) add_tracker(members[s].net_id, members[s].name, members[s].net_id == my_id);
}

// ---- net events -------------------------------------------------------------------------------------
using EventDtor = void(U2MP_GUEST *)(void *);
using EventApply = void(U2MP_GUEST *)(void *, void *);

uint8_t *create_event(int32_t type) {
    return guest<uint8_t *(U2MP_GUEST *)(int32_t)>(kCreateEvent)(type);
}

void release_event(uint8_t *ev) {
    // Events are reference counted; the last reference runs the deleting destructor.
    if (--ev[0x0a] == 0) reinterpret_cast<EventDtor>(get<uint64_t>(ptr_at(ev) + 0x08))(ev);
}

void player_left(uint32_t net_id) {
    if (net_id == 0) return;
    uint8_t *ev = create_event(79);
    if (!ev) return;
    put<uint32_t>(ev + 0xc, net_id);
    guest<void(U2MP_GUEST *)(void *, void *, uint32_t)>(kSendEventAll)(ngm(), ev, 0xffffffffu);
    reinterpret_cast<EventApply>(get<uint64_t>(ptr_at(ev) + 0x20))(ev, nullptr);
    release_event(ev);
}

// The game's party mailbox: a NetEventMessage applied as if it came from `sender`.
std::vector<uint8_t> mailbox_payload;
void mailbox_post(uint32_t code, uint32_t sender, const uint8_t *data, size_t size) {
    uint8_t *ev = create_event(46);
    if (!ev) return;
    mailbox_payload.assign(data, data + size);
    alignas(16) uint8_t connection[0x200] = {};
    put<uint32_t>(connection + 0x160, sender);
    put<uint32_t>(ev + 0xc, code);
    put<uint64_t>(ev + 0x10, addr_of(mailbox_payload.data()));
    put<uint32_t>(ev + 0x18, uint32_t(size));
    guest<void(U2MP_GUEST *)(void *, void *)>(kMessageApply)(ev, connection);
    // The payload is ours: the event must not point at it once we let go.
    put<uint64_t>(ev + 0x10, 0);
    put<uint32_t>(ev + 0x18, 0);
    release_event(ev);
}

// ---- local player facts -----------------------------------------------------------------------------
int local_rank() {
    uint8_t *t = lookup(kRankTableId);
    if (!t) return 0;
    int32_t count = get<int32_t>(t + 0x50);
    uint8_t *thresholds = ptr_at(t + 0x58);
    if (count <= 1 || !thresholds) return 0;
    int32_t money = get<int32_t>(g(kNpd) + 0xd8);
    int32_t last = count - 1;
    for (int32_t i = 0; i < last; ++i) {
        int32_t lo = get<int32_t>(thresholds + 4 * i), hi = get<int32_t>(thresholds + 4 * (i + 1));
        if (lo <= money && money < hi) return i < 255 ? i : 255;
    }
    return last < 255 ? last : 255;
}

int local_team() {
    int32_t team = get<int32_t>(g(kNpd) + 0x114);
    return team == 1 || team == 2 ? team : 0;
}

// ---- menu ---------------------------------------------------------------------------------------------
uint32_t top_page_id() {
    int32_t top = get<int32_t>(menu() + 0x1010);
    if (top < 0 || top > 0x40) return 0;
    uint8_t *page = ptr_at(menu() + 0x9d0 + 8 * top);
    return page ? get<uint32_t>(page + 0x10) : 0;
}

bool multiplayer_page_open() {
    int32_t top = get<int32_t>(menu() + 0x1010);
    if (top < 0 || top >= 200) return true;
    for (int32_t i = 0; i <= top; ++i) {
        uint8_t *page = ptr_at(menu() + 0x9d0 + 8 * i);
        if (page && get<uint32_t>(page + 0x10) == kPageMultiplayer) return true;
    }
    return false;
}

void finish_leaving_game() {
    // The LEAVING_GAME page waits for "left the room", which nothing on PC sets otherwise.
    uint8_t *match = g(kMatchSession);
    if (top_page_id() == kPageLeavingGame && match[0xc8] == 0) {
        log("leaving the match: LEAVING_GAME may close (match +0xc8 = 1)");
        put<int32_t>(g(kNetInfo) + 0x34, 0);
        match[0xc8] = 1;
    }
}

// ---- packages -----------------------------------------------------------------------------------------
uint8_t *find_package(uint32_t id) {
    uint8_t *table = g(kPackageTable);
    uint8_t *buckets = ptr_at(table + 0x10);
    int32_t count = get<int32_t>(table + 0x4c);
    if (!buckets || count <= 0) return nullptr;
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 4; ++i) {
        h ^= (id >> (8 * i)) & 0xff;
        h *= 0x01000193u;
    }
    uint8_t *node = ptr_at(buckets + 8 * (h % uint32_t(count)));
    while (node && get<uint32_t>(node + 0x10) != id) node = ptr_at(node);
    if (!node || node == ptr_at(buckets + 8 * uint64_t(count))) return nullptr;
    return ptr_at(node + 0x18);
}

uint8_t *request_package(const char *name) {
    uint8_t *pkg = guest<uint8_t *(U2MP_GUEST *)(void *, const char *, int32_t)>(kRequestPackage)(
        g(kPackageTable), name, 0);
    if (pkg) pkg[0x18] |= 0x40;   // what the game's own requests set
    return pkg;
}

uint8_t *keep_net_package() {
    if (const char *extra = extra_net_package()) request_package(extra);
    return request_package("net");
}

void preload_packages() {
    const char *env = std::getenv("UNCHARTED_U2MP_PRELOAD");
    if (env && std::strcmp(env, "0") == 0) return;
    // The weapons and animation packages the PS3 game loaded on entering Multiplayer.
    static const char *const names[] = {
        "beretta-pistol", "beretta-pistol-magazine", "colt-defender-pistol", "colt-defender-magazine",
        "desert-eagle", "desert-eagle-magazine", "dragunov-magazine", "dragunov-rifle", "fn-fal-clip",
        "fn-fal-rifle", "m4-rifle", "m4-rifle-magazine", "shotgun", "spas12-shotgun", "machine-gun",
        "machine-gun-clip", "rpg", "anim-proto-multiplayer", "anim-proto-shield", "anim-proto-carry-objects",
        "m32-grenade-launcher"};
    int requested = 0, present = 0, missing = 0;
    for (const char *name : names) {
        if (find_package(sid(name))) ++present;
        else if (request_package(name)) ++requested;
        else ++missing;
    }
    log("package preload (PS3 0xe155d0): %d requested, %d already present, %d not created", requested, present,
        missing);
}

// ---- messages -----------------------------------------------------------------------------------------
void send_raw(int slot, const std::vector<uint8_t> &body, int except) {
    std::vector<uint8_t> msg;
    msg.reserve(body.size() + 1);
    msg.push_back(gamenet::kSessionMessage);
    msg.insert(msg.end(), body.begin(), body.end());
    if (slot >= 0) {
        transport.send(uint8_t(slot), true, msg.data(), msg.size());
    } else if (except < 0) {
        transport.broadcast(true, msg.data(), msg.size());
    } else {
        for (int s = 1; s <= int(net::kMaxPeers); ++s)
            if (members[s].used && s != except) transport.send(uint8_t(s), true, msg.data(), msg.size());
    }
}

void send_info() {
    std::string name = cut(my_name, 31);
    std::vector<uint8_t> b{kInfo, uint8_t(name.size())};
    b.insert(b.end(), name.begin(), name.end());
    sent_rank = uint8_t(local_rank());
    sent_team = uint8_t(local_team());
    b.push_back(sent_rank);
    b.push_back(sent_team);
    uint32_t fp = unlocks_skin_fingerprint();
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(fp >> (8 * i)));
    send_raw(0, b, -1);
}

void broadcast_roster() {
    std::vector<uint8_t> b{kRoster};
    for (int s = 0; s < kSlots; ++s) {
        if (!members[s].used) continue;
        std::string name = cut(members[s].name, 31);
        b.push_back(uint8_t(s));
        b.push_back(uint8_t(name.size()));
        b.push_back(members[s].rank);
        b.insert(b.end(), name.begin(), name.end());
    }
    send_raw(-1, b, -1);
    if (!online::active()) return;
    // Online, clients also need everyone's NpId to map room members to net ids.
    npids.clear();
    std::vector<uint8_t> ids{kIds};
    for (int s = 0; s < kSlots; ++s) {
        if (!members[s].used) continue;
        std::string npid;
        net::Address a;
        if (s == 0) npid = online::my_npid_text();
        else if (transport.peer_address(uint8_t(s), a)) npid = online::npid_at(a);
        if (npid.empty()) continue;
        uint32_t id = members[s].net_id;
        npids[id] = npid;
        std::string shown = cut(npid, 16);
        ids.push_back(uint8_t(id));
        ids.push_back(uint8_t(shown.size()));
        ids.insert(ids.end(), shown.begin(), shown.end());
    }
    send_raw(-1, ids, -1);
}

void broadcast_teams() {
    std::vector<uint8_t> b{kTeams};
    for (int s = 0; s < kSlots; ++s) {
        if (!members[s].used) continue;
        b.push_back(uint8_t(s));
        b.push_back(members[s].team);
    }
    send_raw(-1, b, -1);
}

void broadcast_mode(int32_t mode) {
    std::vector<uint8_t> b{kMode};
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(uint32_t(mode) >> (8 * i)));
    send_raw(-1, b, -1);
}

std::vector<uint8_t> follow_body() {
    std::vector<uint8_t> b{kFollow};
    if (uint64_t room = online::match_room())
        for (int i = 0; i < 8; ++i) b.push_back(uint8_t(room >> (8 * i)));
    return b;
}

void on_info(uint8_t from, const uint8_t *d, size_t size) {
    if (!hosting || from < 1 || from > 10 || size < 2) return;
    Member &m = members[from];
    size_t len = d[1];
    m.name.assign(reinterpret_cast<const char *>(d + 2), len < size - 2 ? len : size - 2);
    if (size >= 3 + len) m.rank = d[2 + len];
    if (size >= 4 + len) m.team = d[3 + len];
    if (size >= 8 + len) {
        uint32_t theirs = get<uint32_t>(d + 4 + len);
        uint32_t mine = unlocks_skin_fingerprint();
        if (theirs != 0 && mine != 0 && theirs != mine) {
            log("party: %s's skin table differs (fingerprint %08x, ours %08x): a different import; skins may show "
                "as others",
                m.name.c_str(), theirs, mine);
            std::string notice = m.name +
                                 " has a different Uncharted 2 multiplayer import. Their character skins may show "
                                 "differently. Both players should run the importer again.";
            show_notice(notice.c_str());
        }
    }
    refresh_sessions();
    broadcast_roster();
    broadcast_teams();
}

void on_roster(const uint8_t *d, size_t size) {
    if (hosting) return;
    // A roster carries no teams; those come with Teams messages and must survive a new roster.
    uint8_t teams[kSlots];
    for (int s = 0; s < kSlots; ++s) teams[s] = members[s].team;
    clear_members();
    size_t at = 1;
    while (size - at >= 3) {
        uint8_t slot = d[at], len = d[at + 1], rank = d[at + 2];
        if (slot > 10 || at + 3 + len > size) break;
        Member &m = members[slot];
        m.used = true;
        m.net_id = slot_net_id(slot);
        m.name.assign(reinterpret_cast<const char *>(d + at + 3), len);
        m.rank = rank;
        m.team = teams[slot];
        at += 3 + len;
    }
    refresh_sessions();
}

void on_teams(const uint8_t *d, size_t size) {
    if (hosting) return;
    for (size_t at = 1; size - at >= 2; at += 2)
        if (d[at] <= 10) members[d[at]].team = d[at + 1];
}

void on_follow(const uint8_t *d, size_t size) {
    if (hosting) return;
    log("following the host into the match");
    if (size >= 9) online::follow_match_room(get<uint64_t>(d + 1));
    netinit_match();
    g(kNetInfo)[0x08] = 0;
    guest<void(U2MP_GUEST *)(void *)>(kClearNetPlayers)(ngm());
    set_session(kMatchSession, kCtx, my_id, 1);
    publish(kMatchSession);
    uint8_t *match = g(kMatchSession);
    put<int32_t>(match + 0xbc, 0);
    match[0xc8] = 0;
    put<uint64_t>(match + 0xc0, kCtx);
    match[0xb8] = 1;
    fill_trackers();
    show_page(kPageJoining);
    set_net_sm(12);
    const uint8_t joined = 1;
    mailbox_post(0x10e, 1, &joined, 1);
}

void on_ids(const uint8_t *d, size_t size) {
    if (hosting) return;
    npids.clear();
    for (size_t at = 1; size - at >= 2;) {
        uint8_t id = d[at], len = d[at + 1];
        if (at + 2 + len > size) break;
        npids[id] = std::string(reinterpret_cast<const char *>(d + at + 2), len);
        at += 2 + len;
    }
}

void on_session_message(uint8_t from, const uint8_t *d, size_t size) {
    if (size == 0) return;
    uint8_t type = d[0];
    if (type >= session::kFrontendMessages) {
        matchmaking::on_message(from, d, size);
        return;
    }
    switch (type) {
    case kInfo: on_info(from, d, size); break;
    case kRoster: on_roster(d, size); break;
    case kTeams: on_teams(d, size); break;
    case kMode:
        if (!hosting && size >= 5) mailbox_post(0x110, 1, d + 1, 4);
        break;
    case kKick:
        if (!hosting && from == 0) {
            log("party: the host kicked us");
            g(kNetInfo)[0x1c] = 1;
            kicked = true;   // the transport closes on the next frame, outside the receive loop
        }
        break;
    case kFollow: on_follow(d, size); break;
    case kIds: on_ids(d, size); break;
    case session::kOnlineMessage: online::on_message(from, d + 1, size - 1); break;
    default: break;
    }
}

void on_transport_message(uint8_t slot, const uint8_t *data, size_t size) {
    if (size == 0) return;
    switch (data[0]) {
    case gamenet::kGamePacket: gamenet::receive(slot, data + 1, size - 1); break;
    case gamenet::kSessionMessage: on_session_message(slot, data + 1, size - 1); break;
    case gamenet::kVoice: voice::receive(slot, data + 1, size - 1); break;
    default: break;
    }
}

void on_peer(uint8_t slot, bool joined) {
    if (hosting) {
        if (slot < kSlots) {
            Member &m = members[slot];
            if (joined) {
                uint32_t id = slot_net_id(slot);
                m = Member{true, id, "Player " + std::to_string(slot + 1), 0, 0};
                gamenet::attach(slot, id, true);
                log("party: peer in slot %u joined", unsigned(slot));
                if (online::active()) {
                    online::on_host_reached(id);
                    // Someone who arrives while the match lobby is up must be told to follow too.
                    if (g(kMatchSession)[0x99] != 0 && net_sm() == 16) {
                        log("online: slot %u joined while the lobby is up: sending the follow", unsigned(slot));
                        send_raw(slot, follow_body(), -1);
                    }
                    matchmaking::on_peer_joined(slot);
                }
            } else {
                log("party: %s left", m.name.c_str());
                player_left(m.net_id);
                m = Member();
                gamenet::detach(slot);
            }
        }
        refresh_sessions();
        broadcast_roster();
        return;
    }
    if (slot != 0) return;
    if (joined) {
        my_id = slot_net_id(transport.local_slot());
        set_session(kPartySession, kCtx, my_id, host_id);
        gamenet::attach(0, host_id, true);
        send_info();
        log("party: joined the host as net id %u", my_id);
        online::on_host_reached(my_id);
        if (rehost_pending) {
            // A match migration finishes once the new host is reached: the round restarts under it.
            rehost_pending = false;
            set_session(kMatchSession, kCtx, my_id, host_id);
            publish(kMatchSession);
            guest<void(U2MP_GUEST *)(void *)>(kOwnerChanged)(nullptr);
            log("match migration: reached the new host (net id %u); the round restarts under it", host_id);
        }
        return;
    }
    if (online::host_change_expected()) {
        log("online: the old host closed the link; following the leader to its new room");
        gamenet::detach(0);
    } else if (online::on_host_lost()) {
        log("party: lost the host; the room's new owner takes over");
        gamenet::detach(0);
    } else {
        log("party: lost the host");
        gamenet::detach(0);
        g(kNetInfo)[0x1c] = 1;
        host_lost = true;
    }
}

// ---- transport start ----------------------------------------------------------------------------------
bool start_transport() {
    const char *env_name = std::getenv("UNCHARTED_U2MP_NAME");
    if (env_name && *env_name) my_name = cut(env_name, 31);
    uint16_t port = kDefaultPort;
    if (const char *p = std::getenv("UNCHARTED_U2MP_PORT")) {
        int v = std::atoi(p);
        if (v > 0) port = uint16_t(v);
    }
    transport.on_peer = on_peer;
    transport.on_message = on_transport_message;
    gamenet::set_transport(&transport);
    voice::set_transport(&transport);
    clear_members();

    if (net::Link *link = online::start()) {
        if (!transport.host_link(link)) return false;
        hosting = true;
        my_id = 1;
        host_id = 1;
        members[0] = Member{true, 1, my_name, uint8_t(local_rank()), 0};
        log("online: hosting a party on the online link as %s", my_name.c_str());
        return true;
    }
    const char *join = std::getenv("UNCHARTED_U2MP_JOIN");
    if (join && *join) {
        net::Address address;
        if (!net::Address::parse(join, port, address) || !transport.join(address)) {
            log("cannot join %s", join);
            return false;
        }
        hosting = false;
        my_id = 0;
        log("joining the party at %s", address.text().c_str());
        return true;
    }
    if (!transport.host(port)) {
        log("cannot listen on UDP port %u", unsigned(port));
        return false;
    }
    hosting = true;
    my_id = 1;
    members[0] = Member{true, 1, my_name, uint8_t(local_rank()), 0};
    log("hosting a party on UDP port %u", unsigned(port));
    return true;
}

// ---- page handler -------------------------------------------------------------------------------------
U2MP_GUEST void page_handler(uint32_t *event);

uint8_t *handler_entry(uint32_t i) { return menu() + 0x24f8 + uint64_t(i) * 0x18; }

void register_handler() {
    uint32_t count = get<uint32_t>(menu() + 0x24f0);
    uint64_t fn = addr_of(reinterpret_cast<void *>(&page_handler));
    for (uint32_t i = 0; i < count; ++i)
        if (get<uint64_t>(handler_entry(i) + 8) == fn) return;
    if (count >= 32) {
        log("menu page handler table full: no per-frame updates in menus");
        return;
    }
    uint8_t *e = handler_entry(count);
    put<uint32_t>(e, 0);   // page 0: called for every page
    put<uint64_t>(e + 8, fn);
    put<uint64_t>(e + 0x10, 0);
    put<uint32_t>(menu() + 0x24f0, count + 1);
    log("per-frame page handler registered (%u handlers)", count + 1);
}

void unregister_handler() {
    uint32_t count = get<uint32_t>(menu() + 0x24f0);
    uint64_t fn = addr_of(reinterpret_cast<void *>(&page_handler));
    for (uint32_t i = 0; i < count; ++i) {
        if (get<uint64_t>(handler_entry(i) + 8) != fn) continue;
        for (uint32_t j = i + 1; j < count; ++j) std::memcpy(handler_entry(j - 1), handler_entry(j), 0x18);
        put<uint32_t>(menu() + 0x24f0, count - 1);
        return;
    }
}

// ---- teardown -----------------------------------------------------------------------------------------
void teardown(bool pop) {
    log("leaving multiplayer");
    unregister_handler();
    for (int s = 0; s < kSlots; ++s) gamenet::detach(uint8_t(s));
    voice::stop();
    transport.close();
    online::on_transport_closed("Multiplayer left");
    active = false;
    clear_session(kPartySession);
    clear_session(kMatchSession);
    put<int32_t>(g(kNetInfo) + 0x2c4, 0);
    put<int16_t>(ngm() + 0x1ca0, -1);
    guest<void(U2MP_GUEST *)(void *)>(kClearNetPlayers)(ngm());
    progression_publish();
    set_net_sm(0);
    clear_members();
    if (pop) guest<void(U2MP_GUEST *)(void *)>(kPopPage)(menu());
}

// ---- net clock ----------------------------------------------------------------------------------------
uint32_t own_ticks() {
    using namespace std::chrono;
    static const steady_clock::time_point origin = steady_clock::now();
    steady_clock::time_point since = clock_object ? clock_base : origin;
    int64_t us = duration_cast<microseconds>(steady_clock::now() - since).count();
    return uint32_t(us * 3 / 10000);   // 300 ticks per second
}

void update_net_clock() {
    uint8_t *slot = g(kNetClockPtr);
    if (!clock_object) {
        if (get<uint64_t>(slot) != 0) return;   // the game's own (or someone else's) clock: leave it
        auto *object = static_cast<uint8_t *>(netheap_alloc(0x40, 16));
        if (!object) return;
        std::memset(object, 0, 0x40);
        clock_base = std::chrono::steady_clock::now();
        clock_object = object;
        put<uint64_t>(slot, addr_of(object));
        log("net clock created (300 ticks per second)");
    }
    if (get<uint64_t>(slot) == addr_of(clock_object)) put<int32_t>(clock_object + 0xc, int32_t(own_ticks()));
}

// ---- per-frame work -----------------------------------------------------------------------------------
void skip_net_fade() {
    // Net images fade in from the package's login frame; in menus that reads as a 1 s flicker.
    int32_t now = get<int32_t>(g(kGameFrame));
    for (uint32_t id : {kNetPackage, kNetU2mpPackage}) {
        uint8_t *pkg = find_package(id);
        if (!pkg || get<int32_t>(pkg + 0x30) != 7) continue;
        int32_t age = int32_t(uint32_t(now) - uint32_t(get<int32_t>(pkg + 0x924)));
        if (age < 300) put<int32_t>(pkg + 0x924, int32_t(uint32_t(now) - 300u));
    }
}

void check_net_package() {
    static int32_t logged_state = -100;
    static bool logged_table_null = false;
    static uint32_t logged_count = 0xffffffffu;
    int32_t sm = net_sm();
    bool menus = sm == 18 || sm == 3 || sm == 12;
    uint8_t *pkg = menus ? keep_net_package() : find_package(kNetPackage);
    int32_t state = pkg ? get<int32_t>(pkg + 0x30) : -1;
    uint8_t *table = ptr_at(g(kNetImageTable));
    uint32_t count = table ? get<uint16_t>(table + 2) : 0;
    bool table_null = table == nullptr;
    if (state != logged_state || table_null != logged_table_null || count != logged_count)
        log("net package: %s, state %d, flags 0x%x; net image table %s, %u images", pkg ? "present" : "missing",
            state, pkg ? unsigned(get<uint16_t>(pkg + 0x18)) : 0u, table ? "set" : "NULL", count);
    logged_state = state;
    logged_table_null = table_null;
    logged_count = count;
}

void frame_work() {
    if (active || clock_object) update_net_clock();
    online::frame();
    if (!active) return;
    double now = gamenet::clock();
    if (now == last_clock) return;   // the page handler and the state wrappers share one frame
    last_clock = now;

    transport.pump(now);
    gamenet::frame(now);
    voice::frame();
    transport.pump(now);

    if (kicked || host_lost) {
        const char *why = kicked ? "kicked" : "host lost";
        log("party: closing the transport (%s); Multiplayer starts a new one", why);
        kicked = host_lost = false;
        for (int s = 0; s < kSlots; ++s) gamenet::detach(uint8_t(s));
        voice::stop();
        transport.close();
        online::on_transport_closed(why);
        active = false;
        clear_members();
        return;
    }

    ++frames;
    skip_net_fade();
    finish_leaving_game();
    if (frames % 60 == 1) check_net_package();
    if (now >= next_rate_log) {
        if (next_rate_log > 0) log("frame hook: %u calls in 10 s", frames);
        frames = 0;
        next_rate_log = now + 10;
    }

    if (frames % 120 == 7) {
        int r = local_rank();
        int t = local_team();
        if (hosting && members[0].used && members[0].rank != r) {
            members[0].rank = uint8_t(r);
            broadcast_roster();
        } else if (!hosting && my_id != 0 && (r != sent_rank || t != sent_team)) {
            send_info();
        }
        if (hosting && members[0].used && members[0].team != t) {
            members[0].team = uint8_t(t);
            broadcast_teams();
        }
    }

    if (hosting) {
        int32_t mode = get<int32_t>(g(kNetInfo) + 0x2c4);
        if (mode != sent_mode) {
            sent_mode = mode;
            broadcast_mode(mode);
        }
    }

    int32_t s = net_sm();
    if (s != last_sm) {
        log("net state %d -> %d", last_sm, s);
        if (s == 16) netinit_dump("state 16");
        if (s == 3) netinit_dump("state 3");
        // Back from a left match to the lobby: the host create page must wait for a new room.
        if (hosting && s == 3 && last_sm == 35) g(kMatchSession)[0x99] = 0;
        last_sm = s;
    }
}

U2MP_GUEST void page_handler(uint32_t *event) {
    if (!event || event[0] != 6) return;   // 6: per-frame update
    finish_leaving_game();
    if (active && net_sm() == 18 && !multiplayer_page_open()) {
        // The player backed out of the party menus without Leave Multiplayer (PS3 tore down here too).
        if (++closed_frames >= 30) {
            closed_frames = 0;
            log("PAGE_MULTIPLAYER closed without Leave Multiplayer: multiplayer teardown (PS3 0x4386d0)");
            teardown(false);
            return;
        }
    } else {
        closed_frames = 0;
    }
    frame_work();
}

// ---- match state wrappers -----------------------------------------------------------------------------
struct StateSlot {
    uint64_t slot;
    uint64_t original;
};
constexpr StateSlot kStates[10] = {
    {0x100ac58, 0x5f2ea0},   // EstablishConnection
    {0x100ac18, 0x5f2dc0},   // LoadLevels
    {0x100abd8, 0x5f26d0},   // WaitReady
    {0x100ab98, 0x5f14d0},   // Playing
    {0x100ab58, 0x5f1390},   // PreResetting
    {0x100ab18, 0x5f1000},   // Resetting
    {0x100aa98, 0x5f0ec0},   // ResettingReady
    {0x100aa58, 0x5f0b30},   // ShowResults
    {0x100aa18, 0x5f0a90},   // ShowRoundResults
    {0x100a9d8, 0x5eff30},   // Leave
};
enum { kEstablish = 0, kLoadLevels = 1, kWaitReady = 2, kLeave = 9 };
using UpdateFn = uint64_t(U2MP_GUEST *)(void *);
UpdateFn originals[10];

void leave_reason(const char *when) {
    uint8_t *host = ptr_at(ngm() + 8);
    log("%s: abort byte %u, host tracker %s, host excluded %u", when, unsigned(*g(kAbortByte)),
        host ? "set" : "NULL", host ? unsigned(host[0x26c]) : 0u);
    int32_t players = guest<int32_t(U2MP_GUEST *)(void *)>(kPresentPlayers)(ngm());
    uint8_t *npd = g(kNpd);
    log("%s: leave flag %u, error box kind %d state %d, idle %.1f s, players %d, room %d", when,
        unsigned(*g(kMatchOver)), get<int32_t>(npd + 0x44), get<int32_t>(npd + 0x6c0),
        double(get<float>(g(kIdleTimer))), players, net_sm());
}

void log_wait_list(int list, bool skip_flagged) {
    uint8_t *table = g(kPackageTable);
    uint64_t at = list == 0 ? 0x5a98 : 0x5aa8;
    uint32_t count = get<uint32_t>(table + at);
    uint8_t *entries = ptr_at(table + at + 8);
    if (!entries) return;
    if (count > 256) count = 256;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *e = entries + i * 0x10;
        uint32_t id = get<uint32_t>(e + 8), flags = get<uint32_t>(e + 0xc);
        if (skip_flagged && (flags & 1)) continue;
        uint8_t *pkg = find_package(id);
        if (pkg && get<int32_t>(pkg + 0x30) == 7 && (get<uint16_t>(pkg + 0x18) & 0x80)) continue;
        log("load levels: list %d package %08x (flags %x): %s, state %d, package flags 0x%x", list, id, flags,
            pkg ? "present" : "missing", pkg ? get<int32_t>(pkg + 0x30) : -1,
            pkg ? unsigned(get<uint16_t>(pkg + 0x18)) : 0u);
    }
}

void load_levels_diagnostics(uint8_t *state) {
    leave_reason("load levels");
    uint32_t task_level = 0;
    if (uint8_t *task = ptr_at(state + 8))
        if (uint8_t *a = ptr_at(task + 0x80))
            if (uint8_t *b = ptr_at(a + 0x88)) task_level = get<uint32_t>(b + 0x28);
    bool packages = guest<bool(U2MP_GUEST *)(void *)>(kPackagesLoaded)(g(kPackageTable));
    bool busy = guest<bool(U2MP_GUEST *)()>(kLoaderBusy)();
    uint32_t second = 0;
    Table games = table_of(lookup(kNetGamesId));
    int16_t game = current_game();
    if (games.records && game >= 0 && game < games.count) second = get<uint32_t>(games.records + game * 0x90 + 0x78);

    char text[512];
    size_t used = 0;
    text[0] = 0;
    uint8_t *head = g(kLevelListHead);
    uint8_t *node = ptr_at(head);
    for (int n = 0; n < 24 && node && node != head; ++n, node = ptr_at(node)) {
        if (used >= 496) break;
        int w = std::snprintf(text + used, sizeof text - used, " %08x:%d", get<uint32_t>(node + 0x98),
                              get<int32_t>(node + 0x10));
        if (w > 0) used += size_t(w) < sizeof text - used ? size_t(w) : sizeof text - used - 1;
    }
    log("load levels: packages loaded %d, busy %d, task level %08x, second level %08x; levels%s", int(packages),
        int(busy), task_level, second, text);
    if (!packages) {
        log_wait_list(0, true);
        log_wait_list(1, false);
    }
}

void missing_level_package() {
    static uint32_t waited_id = 0;
    static std::chrono::steady_clock::time_point waited_since;
    static bool file_present = false;
    static uint32_t last_told = 0;   // the id of the last 20 s log (0 none)

    Table games = table_of(lookup(kNetGamesId));
    Table levels = table_of(lookup(kLevelsId));
    int16_t game = current_game();
    if (!games.records || !levels.records || game < 0 || game >= games.count) return;
    uint32_t level = get<uint32_t>(games.records + game * 0x90 + 4);
    const char *name = nullptr;
    bool found = false;
    for (int32_t i = 0; i < levels.count; ++i) {
        uint8_t *r = levels.records + i * 0x38;
        if (get<uint32_t>(r) == level) {
            found = true;
            name = reinterpret_cast<const char *>(ptr_at(r + 0x18));
        }
    }
    if (!found || !name) return;
    size_t len = std::strlen(name);
    if (len < 4 || std::strcmp(name + len - 4, "-pak") != 0) return;
    uint32_t id = sid(name);
    if (find_package(id)) return;

    auto now = std::chrono::steady_clock::now();
    if (id != waited_id) {
        waited_id = id;
        waited_since = now;
        std::string guest_path = std::string("/app0/u2data/build/main/pak22/ps4/") + name + "-ingame10.pak";
        char host[1024];
        file_present = false;
        if (runtime_file_translate(guest_path.c_str(), host, sizeof host) == 0) {
            if (FILE *f = std::fopen(host, "rb")) {
                file_present = true;
                std::fclose(f);
            }
        }
    }
    if (file_present) {
        if (now - waited_since < std::chrono::seconds(20)) return;
        if (id != last_told) {
            last_told = id;
            log("load levels: %s has a file but no package after 20 s (not registered?): not waited for", name);
        }
    }
    // The loader would wait forever for a package nothing can produce: mark its wait entry skipped.
    uint8_t *table = g(kPackageTable);
    uint32_t count = get<uint32_t>(table + 0x5a98);
    uint8_t *entries = ptr_at(table + 0x5aa0);
    if (!entries) return;
    if (count > 256) count = 256;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *e = entries + i * 0x10;
        uint32_t flags = get<uint32_t>(e + 0xc);
        if (get<uint32_t>(e + 8) != id || (flags & 1)) continue;
        put<uint32_t>(e + 0xc, flags | 1);
        log("load levels: %s is defined nowhere (no registry entry, no file): not waited for", name);
    }
}

int64_t steady_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t state_update(int index, void *state) {
    int64_t ms = steady_ms();
    bool changed = last_state != index;
    last_state = index;
    if (changed || ms >= next_online_log) {
        next_online_log = ms + 10000;
        log("online state: update 0x%llx (vtable slot 0x%llx)", (unsigned long long)kStates[index].original,
            (unsigned long long)(kStates[index].slot - 0x18));
        if (index == kLoadLevels) load_levels_diagnostics(static_cast<uint8_t *>(state));
        if (index == kLeave) leave_reason("leave");
    }
    if (index == kLoadLevels) missing_level_package();
    if (index == kEstablish) {
        uint8_t over = *g(kMatchOver), companion = *g(kMatchOverCompanion);
        if (over || companion) {
            // A previous match's end flags would end the new one at once.
            log("match setup: match-over flag was %u (0x14ce978 %u), cleared", unsigned(over), unsigned(companion));
            *g(kMatchOver) = 0;
            *g(kMatchOverCompanion) = 0;
        }
    }
    if (active && (index == kEstablish || index == kLoadLevels || index == kWaitReady) &&
        get<uint64_t>(ngm() + 8) == 0) {
        log("match setup: the net players were cleared, registering the party again");
        fill_trackers();
    }
    uint64_t result = originals[index](state);
    frame_work();
    return result;
}

template <int I> U2MP_GUEST uint64_t state_wrapper(void *state) { return state_update(I, state); }

using WrapperFn = uint64_t(U2MP_GUEST *)(void *);
constexpr WrapperFn kWrappers[10] = {state_wrapper<0>, state_wrapper<1>, state_wrapper<2>, state_wrapper<3>,
                                     state_wrapper<4>, state_wrapper<5>, state_wrapper<6>, state_wrapper<7>,
                                     state_wrapper<8>, state_wrapper<9>};

U2MP_GUEST void abort_wrapper(void *self) {
    uint64_t caller = addr_of(__builtin_return_address(0));
    uint64_t base = addr_of(image.base);
    bool in_image = caller >= base && caller < base + image.size;
    log("match abort (0x113a270 = 1) from %s0x%llx, session %p", in_image ? "guest " : "",
        (unsigned long long)(in_image ? caller - base : caller), self);
    guest<void(U2MP_GUEST *)(void *)>(kAbortOriginal)(self);
}

// ---- Start Match --------------------------------------------------------------------------------------
void match_choice_diagnostics() {
    int16_t game = current_game();
    log("match choice: mode %d, custom choice list %d entries, net game %d", get<int32_t>(g(kNetInfo) + 0x2c4),
        get<int32_t>(g(kCustomChoiceCount)), int(game));
    uint8_t *raw = lookup(kNetGamesId);
    Table games = table_of(raw);
    if (!raw || game < 0 || game >= games.count || !games.records) {
        log("match choice: net game %d is not in *net-games* (%d records)", int(game), raw ? games.count : -1);
        return;
    }
    uint8_t *record = games.records + game * 0x90;
    uint32_t level = get<uint32_t>(record + 4);
    int32_t type = get<int32_t>(record + 0x0c);
    const char *name = "?";
    if (uint8_t *raw_levels = lookup(kLevelsId)) {
        Table levels = table_of(raw_levels);
        for (int32_t i = 0; levels.records && i < levels.count; ++i) {
            uint8_t *r = levels.records + i * 0x38;
            if (get<uint32_t>(r) != level) continue;
            if (const char *n = reinterpret_cast<const char *>(ptr_at(r + 0x18))) name = n;
            break;
        }
    }
    log("match choice: game type %d, level id %08x, package %s", type, level, name);
}

void host_create() {
    g(kNetInfo)[0x08] = 1;
    guest<void(U2MP_GUEST *)(void *)>(kClearNetPlayers)(ngm());
    put<int32_t>(g(kNetInfo) + 0x34, 0);
    guest<void(U2MP_GUEST *)(void *)>(kMenuReset)(menu());
    put<uint32_t>(ptr_at(menu() + 0x1828) + 8, kWaitText);
    // The host create page's own handler (stripped from the PS4 menu set-up) waits for the room.
    uint32_t count = get<uint32_t>(menu() + 0x24f0);
    uint8_t *entry = nullptr;
    for (uint32_t i = 0; i < count && !entry; ++i)
        if (get<uint32_t>(handler_entry(i)) == kPageHostCreate) entry = handler_entry(i);
    if (!entry && count < 32) {
        entry = handler_entry(count);
        put<uint32_t>(menu() + 0x24f0, count + 1);
    }
    if (entry) {
        put<uint32_t>(entry, kPageHostCreate);
        put<uint64_t>(entry + 8, addr_of(g(kHostCreateHandler)));
        put<uint64_t>(entry + 0x10, 0);
    }
    register_handler();
    show_page(kPageHostCreate);
    set_net_sm(3);
}

void create_match_now() {
    log("Start Match: creating the match");
    g(kMatchSession)[0xc8] = 0;
    match_choice_diagnostics();
    netinit_match();
    netinit_dump("before create");
    host_create();
    netinit_dump("after create");
    set_session(kMatchSession, kCtx, 1, 1);
    publish(kMatchSession);
    fill_trackers();
    put<int32_t>(g(kMatchSession) + 0x9c, 0);
    if (online::create_match_room()) return;   // online_match_ready() finishes it
    g(kMatchSession)[0x99] = 1;
    send_raw(-1, follow_body(), -1);
}

void pick_matchmaking_game() {
    Table games = table_of(lookup(kNetGamesId));
    if (!games.records || games.count <= 0) return;
    int32_t chosen = playlist_pick_type();
    int16_t game = current_game();
    int32_t type = chosen >= 0 ? chosen : get<int32_t>(games.records + (game < 0 ? 0 : game) * 0x90 + 0x0c);
    std::vector<int32_t> candidates;
    for (int32_t i = 0; i < games.count; ++i)
        if (get<int32_t>(games.records + i * 0x90 + 0x0c) == type && dlc_net_game_available(i)) candidates.push_back(i);
    if (candidates.empty()) return;
    int32_t pick = candidates[size_t(std::rand()) % candidates.size()];
    guest<void(U2MP_GUEST *)(void *, int16_t, void *, int32_t)>(kSetNetGame)(ngm(), int16_t(pick), nullptr, 10);
    log("matchmaking: net game %d of %zu of type %d", pick, candidates.size(), type);
}

// ---- replaced menu functions --------------------------------------------------------------------------
U2MP_GUEST void button_multiplayer(void *, void *, void *) {
    log("Multiplayer selected");
    netheap_install();
    netinit_prepare();
    netpatch_load();
    if (!active) {
        if (!start_transport()) {
            g(kNetInfo)[0x1c] = 1;
            show_page(kPageMultiplayer);
            return;
        }
        active = true;
    }
    g(kNetInfo)[0x1c] = 0;
    put<int32_t>(g(kNetInfo) + 0x34, 0);
    put<int16_t>(ngm() + 0x1ca0, -1);
    if (hosting) set_session(kPartySession, kCtx, 1, 1);
    else set_session(kPartySession, kCtx, my_id ? my_id : 0xfff, 1);
    publish(kPartySession);
    sent_mode = -1;
    set_net_sm(18);
    register_handler();
    keep_net_package();
    preload_packages();
    show_page(kPageMultiplayer);
}

U2MP_GUEST void roster_query(uint8_t *q) {
    uint32_t op = get<uint32_t>(q);
    uint64_t arg = get<uint64_t>(q + 0x48);
    int slot = slot_of_row(int(arg % 10));
    const Member *m = slot >= 0 ? &members[slot] : nullptr;
    switch (op) {
    case 7: put<uint32_t>(q + 4, m ? 1 : 0); break;
    case 3: {
        uint32_t value = get<uint32_t>(q + 4);
        if (m && value == 1) {
            uint32_t icon = 0;
            if (voice::talking(m->net_id)) icon = kIconTalking;
            else if (voice::muted(m->net_id)) icon = kIconMuted;
            if (!icon) break;
            put<uint32_t>(q + 8, 3);
            put<uint64_t>(q + 0x10, 0);
            put<uint32_t>(q + 0x18, 0);
            put<uint32_t>(q + 0x1c, icon);
            put<uint32_t>(q + 0x20, 1);
            put<int32_t>(q + 0x24, -1);
            put<uint32_t>(q + 0x28, 0);
            put<int32_t>(q + 0x2c, -1);
            put<float>(q + 0x30, 1.0f);
        } else if (m && value == 0) {
            put<uint32_t>(q + 8, 0x27);   // rank badge
            put<uint64_t>(q + 0x10, 0);
            put<uint32_t>(q + 0x18, m->rank);
            put<uint64_t>(q + 0x1c, 0);
            put<int32_t>(q + 0x24, -1);
            put<uint32_t>(q + 0x28, 0);
            put<int32_t>(q + 0x2c, -1);
            put<float>(q + 0x30, 1.0f);
        }
        break;
    }
    case 17: {
        if (!m || arg <= 9 || get<int32_t>(g(kNetInfo) + 0x2c4) != 2 || (m->team != 1 && m->team != 2)) break;
        Table games = table_of(lookup(kNetGamesId));
        int16_t game = current_game();
        if (!games.records || game < 0 || game >= games.count) break;
        if (coop_type(get<int32_t>(games.records + game * 0x90 + 0x0c))) break;
        static int colour_ok = -1;   // -1 unchecked, 0 differs, 1 matches
        if (colour_ok < 0) {
            static const unsigned char a[] = {0x89, 0xcb, 0x41, 0x89, 0xd6, 0x49, 0x89, 0xf5, 0x49, 0x89, 0xfc};
            static const unsigned char b[] = {0xc7, 0x05, 0xc7, 0x3f, 0xe8, 0x00, 0x4b, 0x2a, 0x02, 0x6a};
            colour_ok = image.matches(0x64aec1, a, sizeof a) && image.matches(0x64af57, b, sizeof b) ? 1 : 0;
            if (!colour_ok) log("party rows: GetTeamColor 0x64aeb0 differs, no team colours");
        }
        if (!colour_ok) break;
        alignas(16) uint8_t out[0x20] = {};
        guest<void(U2MP_GUEST *)(void *, void *, int32_t, int32_t)>(kTeamColor)(out, ngm(), int32_t(m->team) - 1, 0);
        put<uint32_t>(q + 0x3c, (get<uint32_t>(out + 0xc) & 0xffffff) | 0x20000000u);
        break;
    }
    case 1: {
        static const char empty[1] = {0};
        char *buffer = row_text[m ? slot : 0];
        std::string name = m ? cut(m->name, 63) : std::string();
        std::memcpy(buffer, name.c_str(), name.size() + 1);
        put<uint64_t>(q + 0x40, m ? addr_of(buffer) : addr_of(empty));
        break;
    }
    default: put<uint32_t>(q + 4, 0); break;
    }
}

U2MP_GUEST void button_start_match(void *, void *, void *) {
    if (!hosting) return;
    int32_t mode = get<int32_t>(g(kNetInfo) + 0x2c4);
    if (mode == 1 && matchmaking::installed()) {
        if (!frontend6::leader_starting(true)) matchmaking::leader_start();
        return;
    }
    if (mode == 1) {
        pick_matchmaking_game();
    } else if (frontend6::leader_starting(false)) {
        return;   // the "Starting in" popup creates the match when it runs out
    }
    create_match_now();
}

U2MP_GUEST void leave_multiplayer(void *, void *, void *) { teardown(true); }

}  // namespace

// ---- public interface -----------------------------------------------------------------------------------
const uint8_t *package_by_id(uint32_t id) { return find_package(id); }

namespace session {

bool member_at(int row, uint32_t &net_id, std::string &name) {
    int slot = slot_of_row(row);
    if (slot < 0) return false;
    net_id = members[slot].net_id;
    name = members[slot].name;
    return true;
}

bool member_name(uint32_t net_id, std::string &name) {
    for (const Member &m : members)
        if (m.used && m.net_id == net_id) {
            name = m.name;
            return true;
        }
    return false;
}

uint32_t member_rank(uint32_t net_id) {
    for (const Member &m : members)
        if (m.used && m.net_id == net_id) return m.rank;
    return 0;
}

uint32_t local_id() { return active ? my_id : 0; }

bool is_host() { return active && hosting; }

int member_count() {
    if (!active) return 0;
    int n = 0;
    for (const Member &m : members) n += m.used;
    return n;
}

std::string local_name() {
    const char *env = std::getenv("UNCHARTED_U2MP_NAME");
    if (env && *env) return cut(env, 31);
    return my_name;
}

int rank() { return local_rank(); }

void send_message(int slot, const std::vector<uint8_t> &body, int except) {
    if (active && !body.empty()) send_raw(slot, body, except);
}

uint32_t net_ticks() {
    if (uint8_t *object = ptr_at(g(kNetClockPtr))) return get<uint32_t>(object + 0xc);
    return own_ticks();
}

void create_match() {
    if (active && hosting) create_match_now();
}

bool kick(uint32_t net_id) {
    if (!active || !hosting || net_id < 2 || net_id > 11 || !members[net_id - 1].used) return false;
    log("party: kicking %s (net id %u)", members[net_id - 1].name.c_str(), net_id);
    send_raw(int(net_id - 1), {kKick}, -1);
    return true;
}

bool member_address(uint32_t net_id, net::Address &out) {
    if (!active || !hosting || net_id < 2 || net_id > 11) return false;
    return transport.peer_address(uint8_t(net_id - 1), out);
}

uint32_t net_id_of(const std::string &npid) {
    for (const auto &entry : npids)
        if (entry.second == npid) return entry.first;
    return 0;
}

void set_local_name(const std::string &name) {
    if (name.empty()) return;
    std::string n = cut(name, 31);
    if (n == my_name && local_name() == n) return;
    my_name = n;
    _putenv_s("UNCHARTED_U2MP_NAME", n.c_str());
    log("player name: %s (RPCN)", n.c_str());
    if (!active) return;
    if (hosting && members[0].used) {
        members[0].name = n;
        refresh_sessions();
        broadcast_roster();
    } else if (!hosting && my_id != 0) {
        send_info();
    }
}

void online_reopen(net::Link *link, const net::Address *owner) {
    for (int s = 0; s < kSlots; ++s) gamenet::detach(uint8_t(s));
    transport.close();
    clear_members();
    kicked = host_lost = false;
    host_id = 1;
    rehost_pending = false;
    if (!owner) {
        hosting = transport.host_link(link);
        my_id = 1;
        members[0] = Member{true, 1, my_name, uint8_t(local_rank()), 0};
        set_session(kPartySession, kCtx, 1, 1);
        publish(kPartySession);
        log("online: transport switch: hosting a new party on the online link");
    } else {
        hosting = false;
        my_id = 0;
        set_session(kPartySession, kCtx, 0xfff, 1);
        publish(kPartySession);
        transport.join_link(link, *owner);
        log("online: transport switch: joining the room owner over the online link");
    }
    sent_mode = -1;
    active = true;
}

void online_match_ready(uint64_t room_id) {
    if (!active || !hosting) {
        log("online: match room %llx ready but this PC no longer hosts: ignored", (unsigned long long)room_id);
        return;
    }
    log("online: Start Match goes on (match room %llx)", (unsigned long long)room_id);
    g(kMatchSession)[0x99] = 1;
    send_raw(-1, follow_body(), -1);
}

void online_migrated(bool host, bool mm) {
    log("party: new leader (%s)%s", host ? "this PC" : "another player", mm ? ", matchmaking goes on" : "");
    if (mm) {
        matchmaking::on_migrated(host);
        return;
    }
    g(kMatchSession)[0x99] = 0;
    clear_session(kMatchSession);
}

void online_host_gone() {
    if (!active || hosting) return;
    log("party: lost the host (nobody took over)");
    g(kNetInfo)[0x1c] = 1;
    host_lost = true;
}

bool match_migration_ok() {
    if (!active || hosting || !online::active()) return false;
    const char *env = std::getenv("UNCHARTED_U2MP_HOST_MIGRATION");
    if (env && env[0] == '0') return false;
    if (net_sm() != 26) return false;
    Table games = table_of(lookup(kNetGamesId));
    int16_t game = current_game();
    if (games.records && game >= 0 && game < games.count && coop_type(get<int32_t>(games.records + game * 0x90 + 0x0c)))
        return false;
    return true;
}

void online_rehost(net::Link *link, const net::Address *owner, uint32_t new_host_net_id) {
    uint32_t old = host_id;
    log("match migration: host net id %u left; net id %u hosts now%s", old, new_host_net_id,
        owner ? "" : " (this PC)");
    for (int s = 0; s < kSlots; ++s) gamenet::detach(uint8_t(s));
    transport.close();
    clear_members();
    kicked = host_lost = false;
    host_id = new_host_net_id;
    if (!owner) {
        hosting = transport.host_link(link);
        transport.reserve_slot(uint8_t(my_id - 1));   // our old slot keeps our net id for us
        uint8_t r = uint8_t(local_rank());
        uint8_t t = uint8_t(local_team());
        members[0] = Member{true, my_id, my_name, r, t};
    } else {
        hosting = false;
        transport.join_link(link, *owner, uint8_t(my_id - 1));
    }
    set_session(kPartySession, kCtx, my_id, new_host_net_id);
    set_session(kMatchSession, kCtx, my_id, new_host_net_id);
    player_left(old);
    active = true;
    sent_mode = -1;
    if (!owner) {
        publish(kMatchSession);
        guest<void(U2MP_GUEST *)(void *)>(kOwnerChanged)(nullptr);
    } else {
        rehost_pending = true;   // finished by the joined event of the new host
    }
}

// ---- install ------------------------------------------------------------------------------------------
void install(int &installed) {
    if (!gamenet::install()) return;

    struct Replacement {
        uint64_t offset;
        const char *name;
        void *host;
    };
    const Replacement replacements[] = {
        {0x5eb160, "BUTTON_MULTIPLAYER", reinterpret_cast<void *>(&button_multiplayer)},
        {0x659bb0, "party roster query", reinterpret_cast<void *>(&roster_query)},
        {0x659bc0, "player roster query", reinterpret_cast<void *>(&roster_query)},
        {0x5eb780, "BUTTON_MULTIPLAYER_STARTMATCH", reinterpret_cast<void *>(&button_start_match)},
        {0x5eb180, "LEAVE_MULTIPLAYER", reinterpret_cast<void *>(&leave_multiplayer)},
    };
    static const unsigned char lea_rdi[] = {0x48, 0x8d, 0x3d};
    for (const Replacement &r : replacements) {
        bool expected = r.offset == 0x5eb180 ? image.matches(r.offset, lea_rdi, sizeof lea_rdi) : is_ret_stub(r.offset);
        if (!expected) {
            log("%s at 0x%llx: not the stripped function this port expects, left alone", r.name,
                (unsigned long long)r.offset);
            continue;
        }
        if (replace_function(r.offset, 5, r.host, r.name)) ++installed;
    }

    for (const StateSlot &s : kStates)
        if (get<uint64_t>(g(s.slot)) != addr_of(g(s.original))) {
            log("match state vtables differ: no per-frame network during matches");
            return;
        }
    for (size_t i = 0; i < 10; ++i) {
        originals[i] = reinterpret_cast<UpdateFn>(g(kStates[i].original));
        put<uint64_t>(g(kStates[i].slot), addr_of(reinterpret_cast<void *>(kWrappers[i])));
    }
    log("match state updates wrapped (%zu states)", size_t(10));
    ++installed;

    if (get<uint64_t>(g(kAbortSlot)) == addr_of(g(kAbortOriginal)))
        put<uint64_t>(g(kAbortSlot), addr_of(reinterpret_cast<void *>(&abort_wrapper)));
}

}  // namespace session
}  // namespace u2mp
