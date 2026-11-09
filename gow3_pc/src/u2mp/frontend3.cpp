// u2mp frontend3: matchmaking (PAGE_MULTIPLAYER_MATCHMAKING, the vote screen) and the party leader's
// "back out" callbacks, ported from PS3 1.10 onto the PC session layer (session.cpp). Audit and
// design: u2mp/docs/PORT-FRONTEND3.md. Addresses are PS4 1.02 image offsets, PS3 1.10 in comments.
//
// PS3: the party leader's Start Match in matchmaking mode (0x43aab0) sent party mailbox 0x114, and
// after the 5 s "Starting in" popup START_MATCHMAKING (0x43e99c, cb 4055f45f): mailbox 0x107 to the
// party (members follow: 0x43a444), menu reset, NET_SM 19, ShowPage(PAGE_MULTIPLAYER_MATCHMAKING).
// That page's handler is the matchmaking state machine 0x4b0a58 (net-matchmaking.cpp, 0x34e8 bytes;
// PS4 0x64e910 = ret): Matching2 room search / join / create (19 -> 5/6/7/9/14 or 3 -> 16), the
// gather wait in SERVER_LOBBY (16), then the vote: the host picks two net games (0x4b0868, mailbox
// 0x112), everyone votes (LIST_MATCHMAKING_VOTE 0x4b0670, mailbox 0x10a), the host sends the winner
// (0x109) and the "Game starting in" countdown ends in the match (PlayTask).
//
// PC: the party is the match (no Matching2, no strangers to find), so the search / join / create
// part has nothing to do; the rest is the PS3 flow over session messages:
//   leader: Start Match -> leader_start(): message kMmStart (0x107), START_MATCHMAKING 0x5eba40
//           (intact on PS4: menu reset, NET_SM 19, ShowPage(PAGE_MULTIPLAYER_MATCHMAKING)).
//   host:   19/16 gather ("Searching for players...", netInfo+0x34 = 1) until every member's page
//           is up (kMmReady) or 10 s; vote (NET_SM 20, phase 2, "Voting ends in", 15 s = PS3 4500
//           ticks): two games of the chosen playlist (kMmVoteOptions = 0x112); votes (kMmVote =
//           0x10a, the host relays them); winner (kMmResult = 0x109): SetNetGame, phase 3 "Game
//           starting in" (10 s); at 2 s left the host creates the match (session::create_match, the
//           custom-game path; the members follow it with the existing kFollow).
//   member: 19/17 -> kMmReady; vote options -> NET_SM 21 (CLIENT_VOTE_SCREEN); result -> phase 3.
// The page widgets are PS4's own and intact: status (cond 89, 0x65a900: netInfo+0x34 phase,
// netInfo+0x20 deadline in net-clock ticks, the playlist), vote rows (cond 90, 0x65ae50: the vote
// block 0x113db50 = PS3 0xf21dcc), players (cond 6..15, session.cpp roster).
// After a matchmaking match Leave::Enter (0x5f00d0) pushes this page again with netInfo+0x34 = 1 and
// NET_SM 16 (host) / 17 (members): the party votes on the next game, as on PS3.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "frontend3.h"
#include "frontend6.h"
#include "session_api.h"
#include "u2mp_internal.h"

namespace u2mp {
namespace {

// ---- guest data -----------------------------------------------------------------------------
constexpr uint64_t kNetState = 0x14ce97c;     // PS3 NET_SM 0xf11764
constexpr uint64_t kNetInfo = 0x104f1c8;      // PS3 0xf1fa48
constexpr uint64_t NI_CREATED_BY_ME = 0x08, NI_ERROR = 0x1c, NI_DEADLINE = 0x20, NI_PHASE = 0x34;
constexpr uint64_t kNgm = 0x113b760;          // g_netGameManager (PS3 0xf1dc80)
constexpr uint64_t NGM_GAME = 0x1ca0;
constexpr uint64_t kVoteBlock = 0x113db50;    // PS3 0xf21dcc
constexpr uint64_t kTipIndex = 0x113ddac;     // the status line's tip (DC b25ab071), PS3 0xf22630 cycle
constexpr uint32_t kTips = 0xb25ab071;
// menu manager fields
constexpr uint64_t MENU_TOP = 0x1010, MENU_PAGES = 0x9d0, PAGE_ID = 0x10;
constexpr uint64_t MENU_CALLBACK_COUNT = 0x18ec, MENU_CALLBACKS = 0x18f0, MENU_CALLBACK_SIZE = 0x18;
constexpr uint32_t kMaxCallbacks = 128;
constexpr uint64_t MENU_BOX = 0x17d0;         // the message box descriptor (PS3 menu+2536)
// functions
constexpr uint64_t fPopPage = 0x5cc6f0, fSetNetGame = 0x63dc30, fDcLookup = 0xa15e70,
                   fStartMatchmaking = 0x5eba40,   // cb 4055f45f (PS3 0x43e99c), intact part
                   fPlayerInfo = 0x64e810;         // PS3 0x4af240 (progression.cpp hooks it)
// replaced
constexpr uint64_t pMatchmaking = 0x64e910;   // PAGE_MULTIPLAYER_MATCHMAKING (PS3 0x4b0a58)
constexpr uint64_t cbVote = 0x64e8e0;         // LIST_MATCHMAKING_VOTE (PS3 0x4b0670)
constexpr uint64_t cbLeaderBack1 = 0x5eba80;  // bbb649ef (PS3 0x439a88)
constexpr uint64_t cbLeaderBack2 = 0x5eba30;  // bf6ce4d5 (PS3 0x439b04)
// ids
constexpr uint32_t kPageMatchmaking = 0xe35f0f1a, kMessageBoxPage = 0x719e9fec;
constexpr uint32_t kTextLeaveMatchmaking = 0xcca090f2, kTextYes = 0x0260a263, kTextNo = 0x075bc82d;
constexpr uint32_t kCbLeaveYes = sid("U2MP_MATCHMAKING_LEAVE");   // ours (PS3 used 24df6961)
constexpr uint32_t kCbLeaveNo = 0x2a2d147e;                       // PS3's, registered nowhere
static_assert(sid("PAGE_MULTIPLAYER_MATCHMAKING") == kPageMatchmaking, "page id");
// NET_SM
enum : int32_t { kStateServerLobby = 16, kStateClientLobby = 17, kStateChooseHostJoin = 18,
                 kStateStartMatchmaking = 19, kStateServerVote = 20, kStateClientVote = 21 };
// netInfo+0x34 (PS3 0x4a9c64): 1 searching / gathering, 2 voting, 3 game starting
enum : int32_t { kPhaseSearch = 1, kPhaseVote = 2, kPhaseStart = 3 };
// times in net-clock ticks (300 per second)
constexpr uint32_t kTicks = 300, kGatherMin = 3 * kTicks, kGatherMax = 10 * kTicks, kVoteTicks = 4500,
                   kStartTicks = 10 * kTicks, kStartAt = 2 * kTicks, kTipTicks = 8 * kTicks;
// session messages (session::kFrontendMessages and up); PS3 mailbox code in the comment
enum : uint8_t { kMmStart = 0x10,        // 0x107 leader -> party: follow into matchmaking
                 kMmReady = 0x11,        // member -> host: my matchmaking page is up
                 kMmVoteOptions = 0x12,  // 0x112 host -> all: {s16 game a, s16 game b, u32 ticks left}
                 kMmVote = 0x13,         // 0x10a a vote {u8 choice}; the host relays it
                 kMmResult = 0x14,       // 0x109 host -> all: {s16 game, u32 ticks left}
                 kMmLeave = 0x15,        // matchmaking ends for the party
                 kLeaderBack = 0x16 };   // 0x115 the leader backed out
// 0x17 = frontend6::kStartingMessage (0x114, the "Starting in" popup)
static_assert(frontend6::kStartingMessage == kLeaderBack + 1, "session message numbers");

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }

struct VoteBlock {   // PS3 0x4b0700 / 0x4b0868; PS4 readers 0x64e8e0, 0x65ae50
    uint8_t active, closed, pad[2];
    int32_t mine;    // -1: not voted (rows enabled while +4 > 1 unsigned)
    int32_t game[2]; // *net-games* indexes (read as s16)
    int32_t count[2];
};
static_assert(sizeof(VoteBlock) == 0x18, "vote block");
VoteBlock &vote_block() { return G<VoteBlock>(kVoteBlock); }

// ---- the machine's own state (PS3 0xf22638.. flags) ---------------------------------------------
struct Machine {
    bool running = false;          // between start and leave / match creation
    bool leave_requested = false;  // PS3 0xf22638: "Leave Matchmaking?" answered yes
    bool ready_sent = false;       // member: kMmReady sent
    bool created = false;          // host: the match was created
    uint32_t gather_since = 0;
    uint32_t ready_slots = 0;      // host: transport slots whose page is up (bit 0 = host)
    uint32_t next_tip = 0;
    int32_t logged_state = -1;
    int32_t last_phase = 0;
};
Machine mm;
bool hooks_installed = false;

int32_t net_state() { return G<int32_t>(kNetState); }
void set_net_state(int32_t s) { G<int32_t>(kNetState) = s; }
uint32_t now() { return session::net_ticks(); }
uint32_t ticks_left() {
    const int32_t left = int32_t(G<uint32_t>(kNetInfo + NI_DEADLINE) - now());
    return left > 0 ? uint32_t(left) : 0u;
}
void set_deadline(uint32_t ticks) { G<uint32_t>(kNetInfo + NI_DEADLINE) = now() + ticks; }

uint32_t top_page() {
    const uint64_t menu = addr::menu_manager;
    const int32_t top = G<int32_t>(menu + MENU_TOP);
    if (top < 0 || top > 0x40) return 0;
    const uint64_t page = G<uint64_t>(menu + MENU_PAGES + 8 * uint64_t(top));
    return page ? *reinterpret_cast<const uint32_t *>(uintptr_t(page) + PAGE_ID) : 0;
}
void pop_page() { guest<void(U2MP_GUEST *)(void *)>(fPopPage)(image.at<void>(addr::menu_manager)); }
void set_net_game(int16_t index) {
    guest<void(U2MP_GUEST *)(void *, int16_t, const void *, int)>(fSetNetGame)(image.at<void>(kNgm), index, nullptr, 10);
}

struct DcTable { int32_t count; int32_t pad; uint8_t *records; };
DcTable *dc_lookup(uint32_t id) { return guest<DcTable *(U2MP_GUEST *)(uint32_t)>(fDcLookup)(id); }

void send(int slot, std::vector<uint8_t> body, int except = -1) { session::send_message(slot, body, except); }
void put16(std::vector<uint8_t> &b, int16_t v) { b.insert(b.end(), reinterpret_cast<uint8_t *>(&v), reinterpret_cast<uint8_t *>(&v) + 2); }
void put32(std::vector<uint8_t> &b, uint32_t v) { b.insert(b.end(), reinterpret_cast<uint8_t *>(&v), reinterpret_cast<uint8_t *>(&v) + 4); }
int16_t get16(const uint8_t *p) { int16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t get32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

// ---- vote (PS3 0x4b0868 host, 0x4b0700 members) --------------------------------------------------
// PS3 0x49bdb8 (PS4 0x64a440) picked two games of the playlist from its game list; our playlists
// (menus.cpp) are sets of game types, so: two different *net-games* records of the playlist's types,
// DLC maps only when imported.
bool pick_vote_games(int16_t &a, int16_t &b) {
    std::vector<int32_t> types;
    DcTable *games = dc_lookup(0x3b9a067d);
    if (!games || !games->records || games->count <= 0) return false;
    const bool any = !playlist_game_types(types);
    std::vector<int16_t> pool;
    for (int32_t i = 0; i < games->count; ++i) {
        const int32_t type = *reinterpret_cast<const int32_t *>(games->records + 0x90 * i + 0x0c);
        bool ok = any;
        for (int32_t t : types) ok = ok || t == type;
        if (ok && dlc_net_game_available(i)) pool.push_back(int16_t(i));
    }
    if (pool.empty()) return false;
    a = pool[size_t(std::rand()) % pool.size()];
    b = a;
    if (pool.size() > 1)
        while (b == a) b = pool[size_t(std::rand()) % pool.size()];
    return true;
}

void open_vote(int16_t a, int16_t b, uint32_t left) {
    VoteBlock &v = vote_block();
    v = VoteBlock{};
    v.active = 1;
    v.mine = -1;
    v.game[0] = a;
    v.game[1] = b;
    G<int32_t>(kNetInfo + NI_PHASE) = kPhaseVote;
    set_deadline(left);
    frontend6::preload_net_game(true, true, true);   // PS3 0x4b0840 (member) / 0x4b0a28 (host)
}

void start_vote() {   // host: SERVER_LOBBY -> SERVER_VOTE_SCREEN (PS3 0x4b35c0)
    int16_t a = 0, b = 0;
    if (!pick_vote_games(a, b)) {
        a = b = G<int16_t>(kNgm + NGM_GAME) >= 0 ? G<int16_t>(kNgm + NGM_GAME) : 0;
        log("matchmaking: no game of the playlist found, voting on net game %d", a);
    }
    open_vote(a, b, kVoteTicks);
    set_net_state(kStateServerVote);
    std::vector<uint8_t> m{kMmVoteOptions};
    put16(m, a);
    put16(m, b);
    put32(m, kVoteTicks);
    send(-1, m);
    log("matchmaking: vote between net games %d and %d (%u players)", a, b, unsigned(session::member_count()));
}

void apply_result(int16_t game, uint32_t left) {
    VoteBlock &v = vote_block();
    v.closed = 1;
    set_net_game(game);
    G<int32_t>(kNetInfo + NI_PHASE) = kPhaseStart;
    set_deadline(left);
    frontend6::preload_net_game(true, true, false);   // PS3 0x4affe0 (host) / 0x4b1fd4 (member): the winner's levels
}

void finish_vote() {   // host: the winner (PS3 mailbox 0x109)
    VoteBlock &v = vote_block();
    const int winner = v.count[0] > v.count[1] ? 0 : v.count[1] > v.count[0] ? 1 : std::rand() % 2;
    const int16_t game = int16_t(v.game[winner]);
    apply_result(game, kStartTicks);
    std::vector<uint8_t> m{kMmResult};
    put16(m, game);
    put32(m, kStartTicks);
    send(-1, m);
    log("matchmaking: votes %d / %d, net game %d wins", v.count[0], v.count[1], game);
}

// ---- leave ----------------------------------------------------------------------------------------
void leave(bool tell) {
    if (tell) send(session::is_host() ? -1 : 0, {kMmLeave});
    log("matchmaking: leaving%s", tell ? "" : " (the party left)");
    vote_block() = VoteBlock{};
    G<int32_t>(kNetInfo + NI_PHASE) = 0;
    G<uint32_t>(kNetInfo + NI_DEADLINE) = 0;
    set_net_state(kStateChooseHostJoin);
    if (top_page() == kMessageBoxPage) pop_page();   // a "Leave Matchmaking?" box still up
    if (top_page() == kPageMatchmaking) pop_page();
    mm = Machine{};
}

// The yes button of "Leave Matchmaking?" (PS3 24df6961 = 0x4af4fc: [0xf22638] = 1).
U2MP_GUEST void leave_yes(void *) { mm.leave_requested = true; }

void register_leave_callback() {
    const uint64_t menu = addr::menu_manager;
    uint32_t &count = G<uint32_t>(menu + MENU_CALLBACK_COUNT);
    for (uint32_t i = 0; i < count && i < kMaxCallbacks; ++i)
        if (G<uint32_t>(menu + MENU_CALLBACKS + i * MENU_CALLBACK_SIZE) == kCbLeaveYes) return;
    if (count >= kMaxCallbacks) {
        log("matchmaking: menu callback table full, Leave Matchmaking answers are not seen");
        return;
    }
    const uint64_t e = menu + MENU_CALLBACKS + count * MENU_CALLBACK_SIZE;
    G<uint32_t>(e) = kCbLeaveYes;
    G<uint64_t>(e + 8) = uint64_t(uintptr_t(&leave_yes));
    G<uint64_t>(e + 0x10) = 0;
    ++count;
}

// PS3 0x42a3e8(menu, cca090f2, 0260a263, 24df6961, 075bc82d, 2a2d147e): the two-button box
// (PS4 layout as 0x5e4270 writes it).
void ask_leave() {
    register_leave_callback();
    uint8_t *box = G<uint8_t *>(addr::menu_manager + MENU_BOX);
    if (!box) {
        mm.leave_requested = true;
        return;
    }
    const uint32_t words[] = {1, kTextYes, 0x11d8e78a, kTextNo, 0x1519fa3d, kCbLeaveYes, kCbLeaveNo};   // +0x40..+0x58
    std::memcpy(box + 0x08, &kTextLeaveMatchmaking, 4);
    std::memcpy(box + 0x40, words, sizeof words);
    show_page(kMessageBoxPage);
}

// ---- the page handler (PS3 0x4b0a58) ----------------------------------------------------------------
void begin(bool host) {
    mm = Machine{};
    mm.running = true;
    mm.gather_since = now();
    mm.ready_slots = 1;
    mm.next_tip = now() + kTipTicks;
    vote_block() = VoteBlock{};
    G<int32_t>(kNetInfo + NI_PHASE) = kPhaseSearch;
    G<uint8_t>(kNetInfo + NI_CREATED_BY_ME) = host ? 1 : 0;   // status: "Searching for players..." / tips
    mm.last_phase = kPhaseSearch;
    log("matchmaking: started (%s)", host ? "host" : "member");
}

void cycle_tip() {
    if (int32_t(now() - mm.next_tip) < 0) return;
    mm.next_tip = now() + kTipTicks;
    const DcTable *tips = dc_lookup(kTips);
    if (tips && tips->count > 0) G<int32_t>(kTipIndex) = (G<int32_t>(kTipIndex) + 1) % tips->count;
}

void update() {
    const int32_t st = net_state();
    const int32_t phase = G<int32_t>(kNetInfo + NI_PHASE);
    if (st != mm.logged_state) {
        log("matchmaking: NET_SM %d, phase %d", st, phase);
        mm.logged_state = st;
    }
    // Back from a matchmaking match: Leave::Enter (0x5f00d0) pushed this page again with phase 1 and
    // NET_SM 16 / 17. A new round (PS3: the same room votes on the next game).
    if (mm.running && mm.last_phase == kPhaseStart && phase == kPhaseSearch) {
        log("matchmaking: back from the match, next round");
        mm = Machine{};
    }
    mm.last_phase = phase;
    if (mm.created) return;
    if (G<uint8_t>(kNetInfo + NI_ERROR)) {   // host gone / kicked: the lobby shows the error box
        leave(false);
        return;
    }
    if (mm.leave_requested) {
        leave(true);
        return;
    }
    const bool host = session::is_host();
    if (!mm.running) begin(host);
    if (!host) {
        if (!mm.ready_sent && (st == kStateStartMatchmaking || st == kStateClientLobby)) {
            send(0, {kMmReady});
            mm.ready_sent = true;
        }
        if (G<int32_t>(kNetInfo + NI_PHASE) == kPhaseSearch) cycle_tip();
        return;
    }
    switch (st) {
    case kStateStartMatchmaking:   // PS3: search / create the room; here the party is the room
        set_net_state(kStateServerLobby);
        break;
    case kStateServerLobby: {      // PS3 "GATHER": wait for the players
        int ready = 0;
        for (uint32_t s = 0; s < 32; ++s) ready += (mm.ready_slots >> s) & 1;
        const uint32_t waited = now() - mm.gather_since;
        if (waited >= kGatherMax || (waited >= kGatherMin && ready >= session::member_count())) start_vote();
        break;
    }
    case kStateServerVote:
        if (G<int32_t>(kNetInfo + NI_PHASE) == kPhaseVote && ticks_left() == 0) finish_vote();
        else if (G<int32_t>(kNetInfo + NI_PHASE) == kPhaseStart && ticks_left() <= kStartAt) {
            // PS3 0x4b354c -> 0x4b1bf8: the host starts the game task. Here: the custom-game create
            // path (session.cpp), which the members follow; netInfo+0x34 stays set as on PS3 (the
            // ranked stats in Leave::Enter and the return to this page read it).
            mm.created = true;
            log("matchmaking: starting net game %d", G<int16_t>(kNgm + NGM_GAME));
            session::create_match();
            G<int32_t>(kNetInfo + NI_PHASE) = kPhaseStart;
        }
        break;
    default:
        break;
    }
}

U2MP_GUEST void matchmaking_page(uint8_t *ev) {
    if (!ev) return;
    switch (*reinterpret_cast<uint32_t *>(ev)) {
    case 0:   // closed (PS3: SetNetGame(-1), flags and vote block cleared, 0x4af240 publish)
        // Not while the match is being started from here (the voted net game must stay) or while
        // the "Leave Matchmaking?" box is above the page.
        if (mm.created || G<int32_t>(kNetInfo + NI_PHASE) == kPhaseStart || top_page() == kMessageBoxPage) break;
        mm = Machine{};
        vote_block() = VoteBlock{};
        set_net_game(-1);
        guest<void(U2MP_GUEST *)()>(fPlayerInfo)();
        break;
    case 1:   // shown
        register_leave_callback();
        break;
    case 2:
    case 3: {   // back (PS3 0x4b0d3c): ask first; once answered let it through in 18 / 19
        const int32_t st = net_state();
        if (!mm.leave_requested) {
            ask_leave();
            ev[0x10] = 1;
        } else {
            ev[0x10] = (st == kStateStartMatchmaking || st == kStateChooseHostJoin) ? 0 : 1;
        }
        break;
    }
    case 6:
        update();
        break;
    default:
        break;
    }
}

// LIST_MATCHMAKING_VOTE (PS4 0x64e8e0 kept the local count; PS3 0x4b0670 also sent mailbox 0x10a
// {room, vote} to the match members through 0x4af160).
U2MP_GUEST void vote(uint8_t *arg) {
    if (!arg) return;
    VoteBlock &v = vote_block();
    if (!v.active || v.closed || v.mine == 0 || v.mine == 1) return;   // the rows are disabled then
    const uint8_t choice = *reinterpret_cast<const uint32_t *>(arg + 4) != 0 ? 1 : 0;
    v.mine = choice;
    ++v.count[choice];
    send(session::is_host() ? -1 : 0, {kMmVote, choice});
    log("matchmaking: voted for net game %d", v.game[choice]);
}

// bbb649ef / bf6ce4d5 (PS3 0x439a88 / 0x439b04): the leader cancels the "Starting in" countdown.
// PS3: when the party host, mailbox 0x115 to the members (whose popup handler 0x439b80 pops it)
// and Matching2 room hidden; then PopPage. PS4 kept the PopPage.
U2MP_GUEST void leader_back(void *, void *, void *) {
    if (session::is_host()) send(-1, {kLeaderBack});
    pop_page();
}

void ensure_page() {
    if (top_page() == kPageMatchmaking) return;
    guest<void(U2MP_GUEST *)(void *, void *, void *)>(fStartMatchmaking)(nullptr, nullptr, nullptr);
}

// Checks `lea rdi,[rip+menu manager]; jmp PopPage` (the two PS4 leader-back callbacks).
bool is_pop_callback(uint64_t at) {
    static const unsigned char lea[] = {0x48, 0x8d, 0x3d};
    if (!image.matches(at, lea, sizeof lea) || image.base[at + 7] != 0xe9) return false;
    int32_t d1 = 0, d2 = 0;
    std::memcpy(&d1, image.base + at + 3, 4);
    std::memcpy(&d2, image.base + at + 8, 4);
    return at + 7 + int64_t(d1) == addr::menu_manager && at + 12 + int64_t(d2) == fPopPage;
}

}  // namespace

// ---- frontend3.h ----------------------------------------------------------------------------------
namespace matchmaking {

bool installed() { return hooks_installed; }

void leader_start() {
    if (!session::is_host()) return;
    log("matchmaking: the leader starts matchmaking (%d players)", session::member_count());
    send(-1, {kMmStart});
    mm = Machine{};
    guest<void(U2MP_GUEST *)(void *, void *, void *)>(fStartMatchmaking)(nullptr, nullptr, nullptr);
}

void on_message(uint8_t from, const uint8_t *d, size_t n) {
    if (!n) return;
    const bool host = session::is_host();
    switch (d[0]) {
    case kMmStart:   // PS3 0x43a444 on mailbox 0x107: menu reset, NET_SM 19, the matchmaking page
        if (!host) {
            mm = Machine{};
            ensure_page();
        }
        break;
    case kMmReady:
        if (host && from <= 31) mm.ready_slots |= 1u << from;
        break;
    case kMmVoteOptions:   // PS3 0x4b1e44: 0x4b0700, NET_SM 21
        if (!host && n >= 9) {
            ensure_page();
            if (!mm.running) begin(false);
            open_vote(get16(d + 1), get16(d + 3), get32(d + 5));
            set_net_state(kStateClientVote);
            log("matchmaking: vote between net games %d and %d", get16(d + 1), get16(d + 3));
        }
        break;
    case kMmVote:   // PS3 0x4b1df0: count[vote]++
        if (n >= 2 && d[1] <= 1) {
            VoteBlock &v = vote_block();
            if (v.active && !v.closed) ++v.count[d[1]];
            if (host) send(-1, {kMmVote, d[1]}, from);   // the other members see it too
        }
        break;
    case kMmResult:   // PS3 0x4b1f34: SetNetGame, phase 3, the countdown
        if (!host && n >= 7) {
            apply_result(get16(d + 1), get32(d + 3));
            log("matchmaking: net game %d chosen", get16(d + 1));
        }
        break;
    case kMmLeave:
        if (!mm.running && top_page() != kPageMatchmaking) break;
        if (host) leave(true);   // a member left: matchmaking ends for the party (PC: party = match)
        else if (from == 0) leave(false);
        break;
    case kLeaderBack:   // PS3 0x439b80: the member's "Starting in" popup closes
        if (!host && from == 0 && (frontend6::popup_up() || top_page() == kMessageBoxPage)) pop_page();
        break;
    case frontend6::kStartingMessage:   // PS3 0x43a444 on mailbox 0x114: the "Starting in" popup (frontend6.cpp)
        if (!host && from == 0) frontend6::member_starting();
        break;
    default:
        break;
    }
}

}  // namespace matchmaking

void install_frontend3(int &installed) {
    // PAGE_MULTIPLAYER_MATCHMAKING 0x64e910: ret.
    if (!is_ret_stub(pMatchmaking))
        log("PAGE_MULTIPLAYER_MATCHMAKING at 0x%llx: not the stripped handler this port expects, left alone",
            (unsigned long long)pMatchmaking);
    else if (replace_function(pMatchmaking, 5, reinterpret_cast<void *>(&matchmaking_page), "PAGE_MULTIPLAYER_MATCHMAKING")) {
        ++installed;
        hooks_installed = true;
    }
    // LIST_MATCHMAKING_VOTE 0x64e8e0: cmp dword [rdi+4],0; setne al; movzx eax,al; lea rcx,[vote block]
    static const unsigned char vote_bytes[] = {0x83, 0x7f, 0x04, 0x00, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x8d, 0x0d};
    int32_t disp = 0;
    if (image.matches(cbVote, vote_bytes, sizeof vote_bytes)) std::memcpy(&disp, image.base + cbVote + 13, 4);
    if (!image.matches(cbVote, vote_bytes, sizeof vote_bytes) || cbVote + 17 + int64_t(disp) != kVoteBlock)
        log("LIST_MATCHMAKING_VOTE at 0x%llx: not the function this port expects, left alone", (unsigned long long)cbVote);
    else if (replace_function(cbVote, 5, reinterpret_cast<void *>(&vote), "LIST_MATCHMAKING_VOTE"))
        ++installed;
    // The leader's back-out callbacks.
    for (const auto &c : {std::pair<uint64_t, const char *>{cbLeaderBack1, "leader back (bbb649ef)"},
                          std::pair<uint64_t, const char *>{cbLeaderBack2, "leader back (bf6ce4d5)"}}) {
        if (!is_pop_callback(c.first))
            log("%s at 0x%llx: not the function this port expects, left alone", c.second, (unsigned long long)c.first);
        else if (replace_function(c.first, 5, reinterpret_cast<void *>(&leader_back), c.second))
            ++installed;
    }
    // START_MATCHMAKING 0x5eba40 is called, not replaced: push rbp; mov rbp,rsp; push rbx; push rax;
    // lea rbx,[menu]; mov rdi,rbx; call 0x5cd1e0; mov [NET_SM],0x13
    static const unsigned char start_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8d, 0x1d};
    if (!image.matches(fStartMatchmaking, start_bytes, sizeof start_bytes) || G<uint32_t>(fStartMatchmaking + 0x1b) != 0x13 ||
        G<uint32_t>(fStartMatchmaking + 0x20) != kPageMatchmaking) {
        log("START_MATCHMAKING at 0x%llx differs: matchmaking off (Start Match plays the party's game)",
            (unsigned long long)fStartMatchmaking);
        hooks_installed = false;
    }
}

}  // namespace u2mp
