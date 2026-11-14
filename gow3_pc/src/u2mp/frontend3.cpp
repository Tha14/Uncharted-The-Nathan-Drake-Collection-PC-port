// u2mp frontend3: matchmaking on the PC session layer.
//
// The PS4 build lost the matchmaking page handler (it is a bare ret), the vote broadcast and the party
// mailboxes, but kept the page's widgets: the status line, the two vote rows and the player rows, all
// of which read game memory. This file drives that memory from a small state machine:
//   - the party leader starts matchmaking and tells the members (session messages),
//   - the host gathers players (on LAN a short wait; online it first finds or makes a room with
//     strangers through online.cpp),
//   - the host picks two net games of the chosen playlist, runs a vote, sends the winner,
//   - counts down and creates the match.
// Members follow the host's messages. It also replaces the vote row callback and the two "leader backs
// out" callbacks, and asks "Leave Matchmaking?" when the player presses back.
//
// Reimplemented from the behaviour description u2mp/docs/spec/frontend.md.

#include <bit>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "frontend3.h"
#include "frontend6.h"
#include "online.h"
#include "session_api.h"
#include "u2mp_internal.h"

namespace u2mp {
namespace {

// ---- where things are in the image ----
constexpr uint64_t kNetSm = 0x14ce97c;          // s32 network state machine state
constexpr uint64_t kNetInfo = 0x104f1c8;
constexpr uint64_t kInfoCreatedByMe = 0x08;     // u8: host text vs member tips on the status line
constexpr uint64_t kInfoError = 0x1c;           // u8: host gone / kicked
constexpr uint64_t kInfoDeadline = 0x20;        // u32 clock ticks, the status line countdown
constexpr uint64_t kInfoPhase = 0x34;           // s32: 0 none, 1 gathering, 2 voting, 3 starting
constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kNgmCurrentGame = 0x1ca0;    // s16, -1 none
constexpr uint64_t kVote = 0x113db50;           // 0x18-byte block the vote rows read
constexpr uint64_t kTipIndex = 0x113ddac;
constexpr uint64_t kNetPlayerData = 0x11474f8;
constexpr uint64_t kPlaylistId = 0x104;         // u32 in NetPlayerData

constexpr uint64_t kMenuTopIndex = 0x1010;
constexpr uint64_t kMenuPageStack = 0x9d0;
constexpr uint64_t kMenuMessageBox = 0x17d0;
constexpr uint64_t kMenuCallbackCount = 0x18ec;
constexpr uint64_t kMenuCallbacks = 0x18f0;
constexpr uint32_t kMenuCallbackMax = 128;

// Guest functions.
constexpr uint64_t kPopPage = 0x5cc6f0;
constexpr uint64_t kSetNetGame = 0x63dc30;
constexpr uint64_t kDcLookup = 0xa15e70;
constexpr uint64_t kStartMatchmaking = 0x5eba40;
constexpr uint64_t kPublishPlayerInfo = 0x64e810;

// Hooked entry points.
constexpr uint64_t kPageHandler = 0x64e910;
constexpr uint64_t kVoteRow = 0x64e8e0;
constexpr uint64_t kLeaderBackA = 0x5eba80;
constexpr uint64_t kLeaderBackB = 0x5eba30;

// Ids.
constexpr uint32_t kMatchmakingPage = 0xe35f0f1a;
constexpr uint32_t kMessageBoxPage = 0x719e9fec;
constexpr uint32_t kNetGamesTable = 0x3b9a067d;
constexpr uint32_t kTipsTable = 0xb25ab071;
constexpr uint32_t kLeaveYesId = sid("U2MP_MATCHMAKING_LEAVE");
static_assert(kLeaveYesId == 0x7d1ecad7u, "leave callback id");

// NET_SM values.
constexpr int32_t kSmServerLobby = 16;
constexpr int32_t kSmClientLobby = 17;
constexpr int32_t kSmChooseHostJoin = 18;
constexpr int32_t kSmStartMatchmaking = 19;
constexpr int32_t kSmServerVote = 20;
constexpr int32_t kSmClientVote = 21;

// Clock ticks (300 per second).
constexpr uint32_t kGatherMin = 900;
constexpr uint32_t kGatherMax = 3000;
constexpr uint32_t kVoteTime = 4500;
constexpr uint32_t kStartCountdown = 3000;
constexpr int32_t kCreateAt = 600;
constexpr uint32_t kTipPeriod = 2400;
constexpr uint32_t kOnlineGather = 6000;
constexpr uint32_t kOnlineGatherMax = 12000;
constexpr uint32_t kResearchMin = 13500;
constexpr uint32_t kRetryAfterFailure = 900;

// Session message types.
enum : uint8_t {
    kMsgStart = 0x10,
    kMsgReady = 0x11,
    kMsgVoteOptions = 0x12,
    kMsgVote = 0x13,
    kMsgResult = 0x14,
    kMsgLeave = 0x15,
    kMsgLeaderBack = 0x16,
};

// ---- game memory, little-endian and unaligned-safe ----
template <typename T> T get(uint64_t offset) {
    T v;
    std::memcpy(&v, image.base + offset, sizeof v);
    return v;
}
template <typename T> void put(uint64_t offset, T v) { std::memcpy(image.base + offset, &v, sizeof v); }
template <typename T> T get_at(uintptr_t address) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void *>(address), sizeof v);
    return v;
}
template <typename T> void put_at(uintptr_t address, T v) {
    std::memcpy(reinterpret_cast<void *>(address), &v, sizeof v);
}

// Message bodies: little-endian fields appended after the type byte.
void append_s16(std::vector<uint8_t> &b, int16_t v) {
    b.push_back(uint8_t(uint16_t(v)));
    b.push_back(uint8_t(uint16_t(v) >> 8));
}
void append_u32(std::vector<uint8_t> &b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
int16_t read_s16(const uint8_t *p) { return int16_t(uint16_t(p[0] | (p[1] << 8))); }
uint32_t read_u32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// ---- guest calls ----
void pop_page() {
    using Fn = void(U2MP_GUEST *)(void *);
    reinterpret_cast<Fn>(image.base + kPopPage)(image.base + addr::menu_manager);
}
void set_net_game(int16_t index) {
    using Fn = void(U2MP_GUEST *)(void *, int16_t, const void *, int32_t);
    reinterpret_cast<Fn>(image.base + kSetNetGame)(image.base + kNgm, index, nullptr, 10);
}
// A DC table: {s32 count, s32 pad, u8 *records}.
const uint8_t *dc_lookup(uint32_t id) {
    using Fn = void *(U2MP_GUEST *)(uint32_t);
    return static_cast<const uint8_t *>(reinterpret_cast<Fn>(image.base + kDcLookup)(id));
}
void start_matchmaking_page(void *call) {
    using Fn = void(U2MP_GUEST *)(void *, void *, void *);
    reinterpret_cast<Fn>(image.base + kStartMatchmaking)(call, nullptr, nullptr);
}
void publish_player_info() {
    using Fn = void(U2MP_GUEST *)();
    reinterpret_cast<Fn>(image.base + kPublishPlayerInfo)();
}

// ---- the machine (host memory) ----
struct Machine {
    bool running = false;
    bool leave_requested = false;
    bool ready_sent = false;
    bool created = false;
    uint32_t gather_start = 0;
    uint32_t ready_bits = 0;     // bit n: transport slot n said it is ready
    uint32_t next_tip = 0;
    int32_t logged_sm = -1;      // NET_SM the last log line showed
    int32_t seen_phase = 0;
    bool online_room = false;    // online: we host a room and gather strangers in it
    uint32_t research_at = 0;
    uint32_t retry_at = 0;
};
Machine m;
bool page_installed = false;

void reset_machine() { m = Machine{}; }

uint32_t clock_now() { return session::net_ticks(); }

int32_t net_sm() { return get<int32_t>(kNetSm); }
void set_net_sm(int32_t v) { put<int32_t>(kNetSm, v); }
int32_t phase() { return get<int32_t>(kNetInfo + kInfoPhase); }
void set_phase(int32_t v) { put<int32_t>(kNetInfo + kInfoPhase, v); }
int16_t current_net_game() { return get<int16_t>(kNgm + kNgmCurrentGame); }

uint32_t top_page() {
    const uintptr_t menu = uintptr_t(image.base + addr::menu_manager);
    const int32_t top = get_at<int32_t>(menu + kMenuTopIndex);
    if (top < 0 || top > 0x40) return 0;
    const uint64_t page = get_at<uint64_t>(menu + kMenuPageStack + 8 * uint64_t(top));
    if (!page) return 0;
    return get_at<uint32_t>(uintptr_t(page) + 0x10);
}

uint32_t ticks_left() {
    const int32_t left = int32_t(get<uint32_t>(kNetInfo + kInfoDeadline) - clock_now());
    return left > 0 ? uint32_t(left) : 0;
}
void set_deadline(uint32_t ticks) { put<uint32_t>(kNetInfo + kInfoDeadline, clock_now() + ticks); }

void clear_vote() { std::memset(image.base + kVote, 0, 0x18); }

// Vote block fields.
uint8_t vote_active() { return get<uint8_t>(kVote + 0x00); }
uint8_t vote_closed() { return get<uint8_t>(kVote + 0x01); }
int32_t vote_mine() { return get<int32_t>(kVote + 0x04); }
int32_t vote_game(int option) { return get<int32_t>(kVote + 0x08 + 4 * option); }
int32_t vote_count(int option) { return get<int32_t>(kVote + 0x10 + 4 * option); }
void add_vote(int option) { put<int32_t>(kVote + 0x10 + 4 * option, vote_count(option) + 1); }

// Members only see the matchmaking page through START_MATCHMAKING; it is shown once.
void ensure_page() {
    if (top_page() != kMatchmakingPage) start_matchmaking_page(nullptr);
}

int reply_target() { return session::is_host() ? -1 : 0; }

void begin(bool host) {
    reset_machine();
    const uint32_t now = clock_now();
    m.running = true;
    m.gather_start = now;
    m.ready_bits = 1;   // slot 0, the host itself, counts as ready from the start
    m.next_tip = now + kTipPeriod;
    clear_vote();
    set_phase(1);
    put<uint8_t>(kNetInfo + kInfoCreatedByMe, host ? 1 : 0);
    m.seen_phase = 1;
    log("matchmaking: started (%s)", host ? "host" : "member");
}

void open_vote(int16_t a, int16_t b, uint32_t ticks) {
    put<uint8_t>(kVote + 0x00, 1);
    put<uint8_t>(kVote + 0x01, 0);
    put<uint8_t>(kVote + 0x02, 0);
    put<uint8_t>(kVote + 0x03, 0);
    put<int32_t>(kVote + 0x04, -1);
    put<int32_t>(kVote + 0x08, int32_t(a));
    put<int32_t>(kVote + 0x0c, int32_t(b));
    put<int32_t>(kVote + 0x10, 0);
    put<int32_t>(kVote + 0x14, 0);
    set_phase(2);
    set_deadline(ticks);
    // Both options start loading so the winner is ready sooner.
    frontend6::preload_net_game(true, true, true);
}

void apply_result(int16_t game, uint32_t ticks) {
    put<uint8_t>(kVote + 0x01, 1);
    set_net_game(game);
    set_phase(3);
    set_deadline(ticks);
    frontend6::preload_net_game(true, true, false);
}

// ---- host: choosing and running the vote ----

// Two net games of the chosen playlist whose maps this PC has. False when none qualifies.
bool pick_two_games(int16_t &a, int16_t &b) {
    const uint8_t *table = dc_lookup(kNetGamesTable);
    if (!table) return false;
    const int32_t count = get_at<int32_t>(uintptr_t(table));
    const uint64_t records = get_at<uint64_t>(uintptr_t(table) + 8);
    if (!records || count <= 0) return false;

    std::vector<int32_t> types;
    const bool any_type = !playlist_game_types(types);
    std::vector<int32_t> pool;
    for (int32_t i = 0; i < count; ++i) {
        const int32_t type = get_at<int32_t>(uintptr_t(records) + 0x90 * uint64_t(i) + 0x0c);
        bool accepted = any_type;
        for (int32_t t : types)
            if (t == type) accepted = true;
        if (accepted && dlc_net_game_available(i)) pool.push_back(i);
    }
    if (pool.empty()) return false;

    const size_t n = pool.size();
    a = int16_t(pool[size_t(std::rand()) % n]);
    b = a;
    if (n > 1) {
        do b = int16_t(pool[size_t(std::rand()) % n]);
        while (b == a);
    }
    return true;
}

void start_vote() {
    int16_t a = 0, b = 0;
    if (!pick_two_games(a, b)) {
        const int16_t current = current_net_game();
        a = b = current >= 0 ? current : 0;
        log("matchmaking: no game of the playlist found, voting on net game %d", a);
    }
    open_vote(a, b, kVoteTime);
    set_net_sm(kSmServerVote);
    std::vector<uint8_t> body{kMsgVoteOptions};
    append_s16(body, a);
    append_s16(body, b);
    append_u32(body, kVoteTime);
    session::send_message(-1, body);
    log("matchmaking: vote between net games %d and %d (%u players)", a, b, unsigned(session::member_count()));
}

void end_vote() {
    const int32_t c0 = vote_count(0), c1 = vote_count(1);
    int winner;
    if (c0 > c1) winner = 0;
    else if (c1 > c0) winner = 1;
    else winner = std::rand() % 2;
    const int16_t game = int16_t(vote_game(winner));
    apply_result(game, kStartCountdown);
    std::vector<uint8_t> body{kMsgResult};
    append_s16(body, game);
    append_u32(body, kStartCountdown);
    session::send_message(-1, body);
    log("matchmaking: votes %d / %d, net game %d wins", c0, c1, game);
}

// ---- leaving ----

void leave(bool tell) {
    if (tell) session::send_message(reply_target(), {kMsgLeave});
    log("matchmaking: leaving%s", tell ? "" : " (the party left)");
    online::mm_leave();
    clear_vote();
    set_phase(0);
    put<uint32_t>(kNetInfo + kInfoDeadline, 0);
    set_net_sm(kSmChooseHostJoin);
    // Close the question box if it is still up, then the matchmaking page under it.
    if (top_page() == kMessageBoxPage) pop_page();
    if (top_page() == kMatchmakingPage) pop_page();
    reset_machine();
}

// The box's "Yes": only flags the request; the next update does the leaving.
U2MP_GUEST void leave_yes(void *) { m.leave_requested = true; }

// Adds the Yes callback to the menu's callback table once, so the box's answer reaches us.
void register_leave_yes() {
    const uintptr_t menu = uintptr_t(image.base + addr::menu_manager);
    const uint32_t count = get_at<uint32_t>(menu + kMenuCallbackCount);
    const uint32_t seen = count < kMenuCallbackMax ? count : kMenuCallbackMax;
    for (uint32_t i = 0; i < seen; ++i)
        if (get_at<uint32_t>(menu + kMenuCallbacks + 0x18 * uint64_t(i)) == kLeaveYesId) return;
    if (count >= kMenuCallbackMax) {
        log("matchmaking: menu callback table full, Leave Matchmaking answers are not seen");
        return;
    }
    const uintptr_t entry = menu + kMenuCallbacks + 0x18 * uint64_t(count);
    put_at<uint32_t>(entry + 0x00, kLeaveYesId);
    put_at<uint64_t>(entry + 0x08, uint64_t(reinterpret_cast<uintptr_t>(&leave_yes)));
    put_at<uint64_t>(entry + 0x10, 0);
    put_at<uint32_t>(menu + kMenuCallbackCount, count + 1);
}

void ask_leave() {
    register_leave_yes();
    const uintptr_t menu = uintptr_t(image.base + addr::menu_manager);
    const uint64_t box = get_at<uint64_t>(menu + kMenuMessageBox);
    if (!box) {
        // No box to ask with: take the back press as the answer.
        m.leave_requested = true;
        return;
    }
    const uintptr_t d = uintptr_t(box);
    put_at<uint32_t>(d + 0x08, 0xcca090f2);   // "Leave Matchmaking?"
    put_at<uint32_t>(d + 0x40, 1);
    put_at<uint32_t>(d + 0x44, 0x0260a263);   // "Yes"
    put_at<uint32_t>(d + 0x48, 0x11d8e78a);
    put_at<uint32_t>(d + 0x4c, 0x075bc82d);   // "No"
    put_at<uint32_t>(d + 0x50, 0x1519fa3d);
    put_at<uint32_t>(d + 0x54, kLeaveYesId);
    put_at<uint32_t>(d + 0x58, 0x2a2d147e);   // "No": an id nobody registers, so it does nothing
    show_page(kMessageBoxPage);
}

// ---- per-frame update (page event 6) ----

int matchmaking_min_players() {
    // Read once; a test setting for small online rooms.
    static const int value = [] {
        const char *s = std::getenv("UNCHARTED_U2MP_MM_MIN");
        if (!s) return 2;
        const int v = std::atoi(s);
        return v >= 1 && v <= 10 ? v : 2;
    }();
    return value;
}

void host_start_matchmaking_step() {
    if (!online::mm_online()) {
        set_net_sm(kSmServerLobby);
        return;
    }
    switch (online::mm_step()) {
    case online::kMmIdle:
        if (int32_t(clock_now() - m.retry_at) >= 0) {
            const uint32_t playlist = get<uint32_t>(kNetPlayerData + kPlaylistId);
            log("matchmaking: looking for players online (playlist 0x%x)", playlist);
            online::mm_begin(playlist);
        }
        break;
    case online::kMmHosting: {
        m.online_room = true;
        const uint32_t now = clock_now();
        m.gather_start = now;
        m.research_at = now + kResearchMin + uint32_t(std::rand() % 15) * 300;
        set_net_sm(kSmServerLobby);
        break;
    }
    case online::kMmJoined:
        break;
    case online::kMmFailed:
        online::mm_reset();
        m.retry_at = clock_now() + kRetryAfterFailure;
        break;
    default:
        break;
    }
}

void host_gather_step() {
    const uint32_t now = clock_now();
    const int ready = std::popcount(m.ready_bits);
    const uint32_t waited = now - m.gather_start;
    if (m.online_room) {
        const int players = session::member_count();
        const int minimum = matchmaking_min_players();
        if (players >= 10 ||
            (players >= minimum && ((waited >= kOnlineGather && ready >= players) || waited >= kOnlineGatherMax))) {
            start_vote();
        } else if (players < minimum && int32_t(now - m.research_at) >= 0) {
            log("matchmaking: %d player(s) after %u s: looking for another room", players, waited / 300);
            online::mm_research();
            m.online_room = false;
            set_net_sm(kSmStartMatchmaking);
        }
        return;
    }
    if (waited >= kGatherMax || (waited >= kGatherMin && ready >= session::member_count())) start_vote();
}

void host_vote_step() {
    const int32_t p = phase();
    if (p == 2 && ticks_left() == 0) {
        end_vote();
    } else if (p == 3 && int32_t(ticks_left()) <= kCreateAt) {
        m.created = true;
        log("matchmaking: starting net game %d", current_net_game());
        session::create_match();
        set_phase(3);   // create_match may have cleared it; the status line keeps "starting"
    }
}

void member_tip_step() {
    const uint32_t now = clock_now();
    if (int32_t(now - m.next_tip) < 0) return;
    m.next_tip = now + kTipPeriod;
    const uint8_t *tips = dc_lookup(kTipsTable);
    if (!tips) return;
    const int32_t count = get_at<int32_t>(uintptr_t(tips));
    if (count <= 0) return;
    const int32_t next = int32_t(uint32_t(get<int32_t>(kTipIndex)) + 1u);
    put<int32_t>(kTipIndex, next % count);
}

void update() {
    const int32_t st = net_sm();
    const int32_t ph = phase();
    if (st != m.logged_sm) {
        log("matchmaking: NET_SM %d, phase %d", st, ph);
        m.logged_sm = st;
    }
    if (m.running && m.seen_phase == 3 && ph == 1) {
        log("matchmaking: back from the match, next round");
        reset_machine();
    }
    m.seen_phase = ph;
    if (m.created) return;
    if (get<uint8_t>(kNetInfo + kInfoError)) {
        leave(false);
        return;
    }
    if (m.leave_requested) {
        leave(true);
        return;
    }
    const bool host = session::is_host();
    if (!m.running) begin(host);

    if (!host) {
        if (!m.ready_sent && (st == kSmStartMatchmaking || st == kSmClientLobby)) {
            session::send_message(0, {kMsgReady});
            m.ready_sent = true;
        }
        if (phase() == 1) member_tip_step();
        return;
    }
    switch (st) {
    case kSmStartMatchmaking: host_start_matchmaking_step(); break;
    case kSmServerLobby: host_gather_step(); break;
    case kSmServerVote: host_vote_step(); break;
    default: break;
    }
}

// ---- hooked entry points ----

U2MP_GUEST void page_handler(uint8_t *ev) {
    if (!ev) return;
    switch (get_at<uint32_t>(uintptr_t(ev))) {
    case 0:   // closed
        if (m.created || phase() == 3 || top_page() == kMessageBoxPage) return;
        reset_machine();
        clear_vote();
        set_net_game(-1);
        publish_player_info();
        break;
    case 1:   // shown
        register_leave_yes();
        break;
    case 2:
    case 3: {   // back: hold it while we ask, let it through once leaving is under way
        const int32_t st = net_sm();
        if (!m.leave_requested) {
            ask_leave();
            ev[0x10] = 1;
        } else {
            ev[0x10] = (st == kSmStartMatchmaking || st == kSmChooseHostJoin) ? 0 : 1;
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

U2MP_GUEST void vote_row(uint8_t *arg) {
    if (!arg) return;
    if (!vote_active() || vote_closed()) return;
    const int32_t mine = vote_mine();
    if (mine == 0 || mine == 1) return;
    const int choice = get_at<uint32_t>(uintptr_t(arg) + 4) ? 1 : 0;
    put<int32_t>(kVote + 0x04, choice);
    add_vote(choice);
    session::send_message(reply_target(), {kMsgVote, uint8_t(choice)});
    log("matchmaking: voted for net game %d", vote_game(choice));
}

U2MP_GUEST void leader_back(void *, void *, void *) {
    if (session::is_host()) session::send_message(-1, {kMsgLeaderBack});
    pop_page();
}

// ---- install checks ----

// s32 from the image, false when it lies outside the image.
bool image_s32(uint64_t offset, int32_t &v) {
    if (offset > image.size || 4 > image.size - offset) return false;
    v = get<int32_t>(offset);
    return true;
}
bool image_u32(uint64_t offset, uint32_t &v) {
    int32_t s;
    if (!image_s32(offset, s)) return false;
    v = uint32_t(s);
    return true;
}

bool vote_row_intact() {
    static const unsigned char head[] = {0x83, 0x7f, 0x04, 0x00, 0x0f, 0x95, 0xc0,
                                         0x0f, 0xb6, 0xc0, 0x48, 0x8d, 0x0d};
    int32_t d;
    return image.matches(kVoteRow, head, sizeof head) && image_s32(kVoteRow + 13, d) &&
           kVoteRow + 17 + uint64_t(int64_t(d)) == kVote;
}

// lea rdi,[menu]; jmp PopPage
bool leader_back_intact(uint64_t at) {
    static const unsigned char lea[] = {0x48, 0x8d, 0x3d};
    static const unsigned char jmp[] = {0xe9};
    int32_t lea_d, jmp_d;
    return image.matches(at, lea, sizeof lea) && image.matches(at + 7, jmp, sizeof jmp) &&
           image_s32(at + 3, lea_d) && at + 7 + uint64_t(int64_t(lea_d)) == addr::menu_manager &&
           image_s32(at + 8, jmp_d) && at + 12 + uint64_t(int64_t(jmp_d)) == kPopPage;
}

bool start_matchmaking_intact() {
    static const unsigned char head[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8d, 0x1d};
    uint32_t state, page;
    return image.matches(kStartMatchmaking, head, sizeof head) && image_u32(kStartMatchmaking + 0x1b, state) &&
           state == 0x13 && image_u32(kStartMatchmaking + 0x20, page) && page == kMatchmakingPage;
}

}  // namespace

namespace matchmaking {

bool installed() { return page_installed; }

void leader_start() {
    if (!session::is_host()) return;
    log("matchmaking: the leader starts matchmaking (%d players)", session::member_count());
    session::send_message(-1, {kMsgStart});
    reset_machine();
    start_matchmaking_page(nullptr);
}

void on_peer_joined(uint8_t slot) {
    if (!m.running || !session::is_host() || top_page() != kMatchmakingPage) return;
    // Bring the newcomer to where the room is: the page, then the vote or its result.
    session::send_message(slot, {kMsgStart});
    const int32_t p = phase();
    if (p == 2 && vote_active()) {
        std::vector<uint8_t> body{kMsgVoteOptions};
        append_s16(body, int16_t(vote_game(0)));
        append_s16(body, int16_t(vote_game(1)));
        append_u32(body, ticks_left());
        session::send_message(slot, body);
    } else if (p == 3) {
        std::vector<uint8_t> body{kMsgResult};
        append_s16(body, current_net_game());
        append_u32(body, ticks_left());
        session::send_message(slot, body);
    }
    log("matchmaking: slot %u joined the room (phase %d)", unsigned(slot), p);
}

void on_migrated(bool host) {
    log("matchmaking: the room has a new host (%s)", host ? "this PC" : "another player");
    reset_machine();
    clear_vote();
    set_phase(1);
    online::mm_reset();
    set_net_sm(host ? kSmStartMatchmaking : kSmClientLobby);
}

void on_message(uint8_t from, const uint8_t *data, size_t size) {
    if (size == 0) return;
    const bool host = session::is_host();
    switch (data[0]) {
    case kMsgStart:
        if (host) return;
        reset_machine();
        ensure_page();
        break;
    case kMsgReady:
        if (host && from <= 31) m.ready_bits |= 1u << from;
        break;
    case kMsgVoteOptions: {
        if (host || size < 9) return;
        const int16_t a = read_s16(data + 1), b = read_s16(data + 3);
        ensure_page();
        if (!m.running) begin(false);
        open_vote(a, b, read_u32(data + 5));
        set_net_sm(kSmClientVote);
        log("matchmaking: vote between net games %d and %d", a, b);
        break;
    }
    case kMsgVote: {
        if (size < 2 || data[1] > 1) return;
        const uint8_t choice = data[1];
        if (vote_active() && !vote_closed()) add_vote(choice);
        // The host passes every vote on, so each member's rows show the same counts.
        if (host) session::send_message(-1, {kMsgVote, choice}, from);
        break;
    }
    case kMsgResult: {
        if (host || size < 7) return;
        const int16_t game = read_s16(data + 1);
        apply_result(game, read_u32(data + 3));
        log("matchmaking: net game %d chosen", game);
        break;
    }
    case kMsgLeave:
        if (!m.running && top_page() != kMatchmakingPage) return;
        if (host) {
            if (m.online_room) log("matchmaking: a player left matchmaking");   // strangers come and go
            else leave(true);
        } else if (from == 0) {
            leave(false);
        }
        break;
    case kMsgLeaderBack:
        if (!host && from == 0 && (frontend6::popup_up() || top_page() == kMessageBoxPage)) pop_page();
        break;
    case frontend6::kStartingMessage:
        if (!host && from == 0) frontend6::member_starting();
        break;
    default:
        break;
    }
}

}  // namespace matchmaking

void install_frontend3(int &installed) {
    if (!is_ret_stub(kPageHandler)) {
        log("PAGE_MULTIPLAYER_MATCHMAKING at 0x%llx: not the stripped handler this port expects, left alone",
            static_cast<unsigned long long>(kPageHandler));
    } else if (replace_function(kPageHandler, 5, reinterpret_cast<void *>(&page_handler),
                                "PAGE_MULTIPLAYER_MATCHMAKING")) {
        installed += 1;
        page_installed = true;
    }

    if (!vote_row_intact()) {
        log("LIST_MATCHMAKING_VOTE at 0x%llx: not the function this port expects, left alone",
            static_cast<unsigned long long>(kVoteRow));
    } else if (replace_function(kVoteRow, 5, reinterpret_cast<void *>(&vote_row), "LIST_MATCHMAKING_VOTE")) {
        installed += 1;
    }

    const struct { uint64_t at; const char *name; } backs[] = {
        {kLeaderBackA, "leader back (bbb649ef)"},
        {kLeaderBackB, "leader back (bf6ce4d5)"},
    };
    for (const auto &b : backs) {
        if (!leader_back_intact(b.at)) {
            log("%s at 0x%llx: not the function this port expects, left alone", b.name,
                static_cast<unsigned long long>(b.at));
        } else if (replace_function(b.at, 5, reinterpret_cast<void *>(&leader_back), b.name)) {
            installed += 1;
        }
    }

    // We call START_MATCHMAKING but do not patch it: it must still set NET_SM 19 and show our page.
    if (!start_matchmaking_intact()) {
        log("START_MATCHMAKING at 0x%llx differs: matchmaking off (Start Match plays the party's game)",
            static_cast<unsigned long long>(kStartMatchmaking));
        page_installed = false;
    }
}

}  // namespace u2mp
