// u2mp progression: the multiplayer progression back end the PS4 1.02 build lacks, on a local file.
//
// What the PS4 build still does (static analysis; addresses are PS4 1.02 image offsets):
//  - NetPlayerData lives at 0x11474f8 with the PS3 layout (PS3 0xf2da60): +0x44 Data (0x228 bytes,
//    version 10), +0x26c TUS copy, +0x494 HDD copy, +0x6bc publish state, +0x6c0 state
//    (3 = connected). Data fields used here: +0xd8 cash, +0x184/+0x188 kills/deaths, +0x18c/+0x190
//    wins/losses, +0x198 time, +0x19c co-op points, +0x1a0 co-op time, +0x1a4 publish count, and
//    the trophy counters at +0x4c + 4*name (kNpdTrophyStatCount*, checked by 661a50).
//  - Match tallies: NetStatGame 0x1147bd0 (66c600 start + CaptureTallies copy at its end; 66cb70 =
//    PS3 Stop 0x4dfc08 with AddMoneyToTally 0x4dcf88 inlined, then the skill update 0x4df898;
//    66d8c0 NPC kill money). Cash is committed into Data +0xd8 by 5de280 (PS3 0x0d0720, called by
//    648c30 = PS3 0x4a46b0). Leave::Enter 5f00d0 (PS3 0x445608) adds kills, deaths, wins, losses,
//    time and the trophy counters. So the PS3 log-only entry points are present in merged form and
//    nothing on PS4 calls a missing copy of them.
//  - Gone: everything that set state 3 and loaded the Data (connect thread 0x4c6ab8, FetchTusData
//    0x4c65f8, FetchHddData 0x4c699c), NetPlayerData::Publish 0x4c6298 (called by Leave at
//    0x445b9c and at match start), and the NP Score code (init 0x4ab854, "Page" fetch 0x4ab3e8 ->
//    0x4aa170 rankings, RecordScore 0x4ab6d0 from Leave). With state never 3 the Data block starts
//    at zero each run and every match's progress is lost on exit; PAGE_MENU_LEADERBOARD stays
//    empty because the list update (5b3000 = PS3 0x411fd8) no longer calls the "Page" fetch.
//
// What this file restores:
//  1. Connect (FetchHdd/FetchTus/RunConnectThread): loads the Data block from the store and sets
//     state 3. Done from 64e810, the PS4 remnant of PS3 0x4af240 (its body is now only the
//     GetVolatileData guard), which PAGE_MULTIPLAYER, PAGE_LOBBY, POSTGAME and match start call.
//  2. Publish: on Leave (the call at 5f0952 that follows the stat updates) and whenever 64e810
//     runs with changed data, as PS3 published on Leave, match start and menu changes.
//  3. Leaderboards (mode 4): 5b3000 ported to C++ with the PS3 "Page" fetch put back; the fetch
//     fills the leaderboard object 0x113d960 (PS3 0xf1fd20) from the local player's Data and the
//     players this PC saw in recent matches. Net trophies (mode 5, LIST_STATS, vtable 0x1009100)
//     read the Data block directly and need only the connect.
#include "progression.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "progression_store.h"
#include "u2mp_internal.h"

extern "C" const char *runtime_file_user_dir(void);

namespace u2mp {
namespace {

using namespace progression;

// ---- guest data ---------------------------------------------------------------------------------
constexpr uint64_t kNetState = 0x14ce97c;   // PS3 NET_SM 0xf11764; IsOnline = state > 1
constexpr uint64_t kNpd = 0x11474f8;        // NetPlayerData (PS3 0xf2da60)
constexpr uint64_t NPD_DATA = 0x44, NPD_STATE = 0x6c0;
constexpr uint64_t D_MONEY = 0xd8, D_KILLS = 0x184, D_DEATHS = 0x188, D_WINS = 0x18c, D_LOSSES = 0x190,
                   D_SECONDS = 0x198, D_COOP = 0x19c, D_COOP_SECONDS = 0x1a0, D_PUBLISHES = 0x1a4;
constexpr int32_t kConnected = 3, kOffline = 2;
constexpr uint64_t kLeaderboard = 0x113d960;   // PS3 0xf1fd20
constexpr uint64_t LB_TOTAL = 0x24, LB_ROWS = 0x28, LB_INFO = 0x30, LB_STATUS = 0x50, LB_BUSY = 0x58,
                   LB_COUNT = 0x70, LB_PAGE = 0x74, LB_RESET = 0x78, LB_BOARD = 0x80, LB_FRIENDS = 0x84,
                   LB_REFRESH = 0x85;
constexpr uint64_t L_COLUMNS = 0x40, L_DELTA = 0x44, L_PAGE = 0x4c, L_BOARD = 0x50, L_FRIENDS = 0x54;
constexpr uint64_t kBoardTable = 0xdef0a0;     // 4 x {NP Score board id 300..303, column set, ?}
constexpr uint64_t kStatGame = 0x1147bd0;      // NetStatGame
constexpr uint64_t SG_COUNT = 0x8, SG_RECORDS = 0x38ec, SG_RECORD = 0x80, SG_TALLIES = 0x3e54, SG_TALLY = 0x34,
                   SG_TEAMS = 0x3dec;
// functions
constexpr uint64_t fDcLookup = 0xa15e70;       // DC symbol lookup by string id
constexpr uint32_t kRankTable = 0xc85e199d;    // cash thresholds: +0x50 count, +0x58 s32[]
// hooks
constexpr uint64_t hPlayerInfo = 0x64e810;     // PS3 0x4af240 remnant (guard only)
constexpr uint64_t hListUpdate = 0x5b3000;     // leaderboard list update (PS3 0x411fd8)
constexpr uint64_t hLeaveCall = 0x5f0952;      // Leave::Enter: call DcLookup(c85e199d) after the stats

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
template <typename T> T &F(uint8_t *p, uint64_t offset) { return *reinterpret_cast<T *>(p + offset); }

int32_t net_state() { return G<int32_t>(kNetState); }
int32_t &npd_state() { return G<int32_t>(kNpd + NPD_STATE); }
uint8_t *npd_data() { return image.at<uint8_t>(kNpd + NPD_DATA); }
void *dc_lookup(uint32_t id) {
    return reinterpret_cast<void *(U2MP_GUEST *)(uint32_t)>(image.base + fDcLookup)(id);
}

// ---- store --------------------------------------------------------------------------------------
Store store;
bool store_loaded = false;
std::array<uint8_t, kBlobSize> last_published{};
uint32_t last_match_signature = 0;

std::string store_path() {
    if (const char *p = std::getenv("UNCHARTED_U2MP_PROGRESSION"); p && *p) return p;
    const char *user = runtime_file_user_dir();
    return std::string(user && *user ? user : ".") + "/u2mp/progression.bin";
}

uint64_t now() { return uint64_t(std::time(nullptr)); }

// PS3 4c4be0: version 10, the rest zero.
void reset_data() {
    uint8_t *d = npd_data();
    std::memset(d, 0, kBlobSize);
    const uint32_t version = kBlobVersion;
    std::memcpy(d, &version, 4);
}

void load_store() {
    if (store_loaded) return;
    store_loaded = true;
    const std::string path = store_path();
    if (load(path, store)) return;
    store = Store{};
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        // Unreadable (and so is .bak): keep it aside rather than overwrite it with a fresh profile.
        std::filesystem::rename(path, path + ".unreadable", ec);
        log("progression: %s unreadable, kept as .unreadable; starting a new profile", path.c_str());
    }
}

// PS3 RunConnectThread 0x4c6ab8: fetch, copy the newest Data into place, state 3.
void ensure_connected() {
    if (npd_state() == kConnected || net_state() <= 1) return;
    load_store();
    uint32_t version = 0;
    std::memcpy(&version, store.blob.data(), 4);
    if (store.has_blob && version == kBlobVersion) {
        std::memcpy(npd_data(), store.blob.data(), kBlobSize);
        log("progression: profile loaded (cash %u, publish %u)", F<uint32_t>(image.at<uint8_t>(kNpd), D_MONEY),
            store.publish_count);
    } else {
        reset_data();   // PS3: "New user (data not found)" / "Wrong version"
        log("progression: new profile");
    }
    std::memcpy(last_published.data(), npd_data(), kBlobSize);
    npd_state() = kConnected;
}

// PS3 NetPlayerData::Publish 0x4c6298 (only while connected; bumps Data +0x160 first).
void publish() {
    if (npd_state() != kConnected) return;
    load_store();
    uint8_t *npd = image.at<uint8_t>(kNpd);
    F<uint32_t>(npd, D_PUBLISHES) += 1;
    std::memcpy(store.blob.data(), npd_data(), kBlobSize);
    store.has_blob = true;
    store.publish_count = F<uint32_t>(npd, D_PUBLISHES);
    std::memcpy(last_published.data(), npd_data(), kBlobSize);
    if (!save(store_path(), store, now())) log("progression: could not write %s", store_path().c_str());
}

void publish_if_changed() {
    if (npd_state() != kConnected) return;
    if (std::memcmp(last_published.data(), npd_data(), kBlobSize) != 0) publish();
}

// The inlined GetVolatileData guard (PS3 0x4c4f74): connected but offline -> clear, state 2.
void guard() {
    if (npd_state() == kConnected && net_state() <= 1) {
        publish_if_changed();
        reset_data();
        npd_state() = kOffline;
    }
}

std::string local_name() {
    if (const char *n = std::getenv("UNCHARTED_U2MP_NAME"); n && *n) return std::string(n).substr(0, kNameMax);
    return store.local_name.empty() ? std::string("Drake") : store.local_name;
}

// PS3 0x0cfa58: rank index from cash (the badge PS3 stored in the leaderboard game info).
uint32_t rank_of(uint32_t money) {
    const uint8_t *t = static_cast<const uint8_t *>(dc_lookup(kRankTable));
    if (!t) return 0;
    const int32_t last = *reinterpret_cast<const int32_t *>(t + 0x50) - 1;
    const int32_t *thresholds = *reinterpret_cast<int32_t *const *>(t + 0x58);
    if (last <= 0 || !thresholds) return last > 0 ? uint32_t(last) : 0;
    for (int32_t i = 0; i < last; ++i)
        if (thresholds[i] <= int32_t(money) && thresholds[i + 1] > int32_t(money)) return uint32_t(i);
    return uint32_t(last);
}

LocalStats local_stats() {
    uint8_t *npd = image.at<uint8_t>(kNpd);
    LocalStats s;
    s.name = local_name();
    s.money = F<uint32_t>(npd, D_MONEY);
    s.kills = F<uint32_t>(npd, D_KILLS);
    s.deaths = F<uint32_t>(npd, D_DEATHS);
    s.wins = F<uint32_t>(npd, D_WINS);
    s.losses = F<uint32_t>(npd, D_LOSSES);
    s.seconds = F<uint32_t>(npd, D_SECONDS);
    s.coop_points = F<uint32_t>(npd, D_COOP);
    s.coop_seconds = F<uint32_t>(npd, D_COOP_SECONDS);
    s.rank = rank_of(s.money);
    return s;
}

// The players of the match being left, from NetStatGame (as Leave::Enter reads them).
void record_match() {
    const uint32_t count = G<uint32_t>(kStatGame + SG_COUNT);
    if (count == 0 || count > 10) return;
    int team_players[2] = {0, 0};
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t team = G<uint8_t>(kStatGame + SG_RECORDS + i * SG_RECORD + 7);
        if (team < 2) ++team_players[team];
    }
    const uint16_t score0 = G<uint16_t>(kStatGame + SG_TEAMS + 2), score1 = G<uint16_t>(kStatGame + SG_TEAMS + SG_TALLY + 2);
    const bool decided = team_players[0] && team_players[1] && score0 != score1;
    std::vector<MatchPlayer> players;
    uint32_t signature = crc32(reinterpret_cast<const uint8_t *>(&count), 4);
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t rec = kStatGame + SG_RECORDS + i * SG_RECORD, tally = kStatGame + SG_TALLIES + i * SG_TALLY;
        MatchPlayer m;
        const char *name = image.at<char>(rec + 0x14);
        m.name.assign(name, strnlen(name, kNameMax));
        m.local = G<uint8_t>(rec + 0xa) != 0;
        m.rank = G<uint8_t>(rec + 0xb);
        m.seconds = G<uint32_t>(rec + 0xc);
        m.kills = G<uint16_t>(tally + 4);
        m.deaths = G<uint16_t>(tally + 6);
        m.coop_points = G<uint32_t>(tally + 0x14);
        m.money = G<uint32_t>(tally + 0x18);
        const uint8_t team = G<uint8_t>(rec + 7);
        if (decided && team < 2) m.result = G<uint16_t>(kStatGame + SG_TEAMS + team * SG_TALLY) == 1 ? 1 : -1;
        signature = crc32(reinterpret_cast<const uint8_t *>(m.name.data()), m.name.size(), signature);
        const uint32_t values[] = {m.kills, m.deaths, m.seconds, m.money};
        signature = crc32(reinterpret_cast<const uint8_t *>(values), sizeof values, signature);
        players.push_back(std::move(m));
    }
    if (signature == last_match_signature) return;   // the same match seen twice
    last_match_signature = signature;
    load_store();
    note_match(store, players, now());
}

// ---- hooks --------------------------------------------------------------------------------------

// 64e810: PS3 0x4af240 (publish my rank/info to the party) lost everything but its
// GetVolatileData guards. Its callers (MP page open, lobby, post-game, match start) are where PS3
// had NetPlayerData connected and published.
void U2MP_GUEST player_info_update() {
    if (net_state() > 1) {
        ensure_connected();
        publish_if_changed();
    }
    guard();
}

// Leave::Enter, after its stat updates: PS3 Leave recorded the scores and called Publish here.
// No connect here: loading now would replace the match just added (PS3 also published only when
// connected; 64e810 has connected at match start).
void *U2MP_GUEST leave_publish(uint32_t id) {
    record_match();
    publish();
    return dc_lookup(id);
}

// PS3 "Page" fetch (0x4ab3e8 -> thread 0x4aa170) on the local store, synchronous.
alignas(16) uint8_t lb_rows[kRowsPerPage][0x80];   // PS3 SceNpScoreRankData: +0 name, +0x5c rank, +0x70 score
alignas(16) uint8_t lb_info[kRowsPerPage][0x40];   // game info: +0 badge, +4 time, +8 deaths, +0xc losses

void fetch_page(uint8_t *lb, uint32_t board_index, int32_t request) {
    ensure_connected();
    load_store();
    const Page pg = page(board(store, local_stats(), int(board_index)), request);
    std::memset(lb_rows, 0, sizeof lb_rows);
    std::memset(lb_info, 0, sizeof lb_info);
    for (size_t i = 0; i < pg.rows.size(); ++i) {
        const Entry &e = pg.rows[i];
        std::memcpy(lb_rows[i], e.name.data(), std::min(e.name.size(), kNameMax));
        F<uint32_t>(lb_rows[i], 0x58) = e.rank;   // serial rank
        F<uint32_t>(lb_rows[i], 0x5c) = e.rank;   // rank (the column the list prints)
        F<uint32_t>(lb_rows[i], 0x60) = e.rank;   // highest rank
        F<int64_t>(lb_rows[i], 0x70) = e.score;
        F<uint32_t>(lb_info[i], 0x0) = e.badge;
        F<uint32_t>(lb_info[i], 0x4) = e.seconds;
        F<uint32_t>(lb_info[i], 0x8) = e.deaths;
        F<uint32_t>(lb_info[i], 0xc) = e.losses;
    }
    F<uint8_t *>(lb, LB_ROWS) = &lb_rows[0][0];
    F<uint8_t *>(lb, LB_INFO) = &lb_info[0][0];
    F<uint32_t>(lb, LB_TOTAL) = pg.total;
    F<uint32_t>(lb, LB_COUNT) = uint32_t(pg.rows.size());
    F<int32_t>(lb, LB_PAGE) = pg.page;
    F<uint32_t>(lb, LB_STATUS) = 0;
}

// 5b3000, the LIST_LEADERBOARD update, in C++ with the fetch PS3 0x411fd8 made at 0x4120f4.
void U2MP_GUEST leaderboard_list_update(uint8_t *list) {
    uint8_t *lb = image.at<uint8_t>(kLeaderboard);
    if (F<uint64_t>(lb, LB_BUSY)) return;
    int32_t first = F<int32_t>(lb, LB_PAGE);
    F<int32_t>(list, L_PAGE) = first;
    int32_t delta, shown;
    if (F<uint8_t>(lb, LB_RESET)) {
        F<int32_t>(list, L_BOARD) = -1;
        std::memset(list + L_COLUMNS, 0, 16);
        F<uint8_t>(list, L_FRIENDS) = 0;
        F<uint8_t>(lb, LB_RESET) = 0;
        shown = -1;
        delta = 0;
        first = 0;
    } else {
        delta = F<int32_t>(list, L_DELTA);
        shown = F<int32_t>(list, L_BOARD);
    }
    uint32_t board_index = F<uint32_t>(lb, LB_BOARD);
    uint8_t refresh = F<uint8_t>(lb, LB_REFRESH);
    bool fetch = false;
    if (shown == int32_t(board_index) && delta == 0) {
        if (F<uint8_t>(list, L_FRIENDS) != F<uint8_t>(lb, LB_FRIENDS) || refresh) fetch = true;
        else return;
    } else if (shown != int32_t(board_index) && delta == 0) {
        fetch = true;
    }
    if (!fetch) {   // scroll by `delta` pages
        int32_t p = first + delta;
        F<int32_t>(list, L_PAGE) = p;
        F<int32_t>(list, L_DELTA) = 0;
        const int32_t last = int32_t((F<uint32_t>(lb, LB_TOTAL) - 1u) / 11u);
        if (p < 0) p = 0;
        else if (p > last) p = last;
        F<int32_t>(list, L_PAGE) = p;
        if (p == first) return;
        refresh = F<uint8_t>(lb, LB_REFRESH);
    }
    if (refresh) {
        F<int32_t>(list, L_PAGE) = 0;
        F<uint8_t>(lb, LB_REFRESH) = 0;
    }
    board_index = F<uint32_t>(lb, LB_BOARD);
    const uint8_t friends = F<uint8_t>(lb, LB_FRIENDS);
    if (shown != int32_t(board_index) || F<uint8_t>(list, L_FRIENDS) != friends) F<int32_t>(list, L_PAGE) = -1;
    F<uint8_t>(list, L_FRIENDS) = friends;
    F<int32_t>(list, L_BOARD) = int32_t(board_index);
    if (board_index < uint32_t(kBoards)) fetch_page(lb, board_index, F<int32_t>(list, L_PAGE));
    F<uint32_t>(list, L_COLUMNS) = G<uint32_t>(kBoardTable + uint64_t(int64_t(int32_t(board_index))) * 12 + 4);
}

}  // namespace

void progression_publish() {
    if (!image.base) return;
    publish_if_changed();
}

std::vector<std::string> progression_recent_players(size_t max) {
    load_store();
    std::vector<const PlayerStats *> by_time;
    for (const PlayerStats &p : store.recent) by_time.push_back(&p);
    std::stable_sort(by_time.begin(), by_time.end(),
                     [](const PlayerStats *a, const PlayerStats *b) { return a->last_seen > b->last_seen; });
    std::vector<std::string> names;
    for (const PlayerStats *p : by_time)
        if (names.size() < max) names.push_back(p->name);
    return names;
}

void install_progression(int &installed) {
    // 64e810: push rbp; mov rbp,rsp; push rbx; push rax; lea rbx,[NetPlayerData]; mov eax,[rbx+0x6c0]
    static const unsigned char player_info[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8d, 0x1d, 0xdb,
                                                0x8c, 0xaf, 0x00, 0x8b, 0x83, 0xc0, 0x06, 0x00, 0x00};
    // 5b3000: lea r9,[leaderboard]; cmp qword [r9+0x58],0; jne ...
    static const unsigned char list_update[] = {0x4c, 0x8d, 0x0d, 0x59, 0xa9, 0xb8, 0x00, 0x49, 0x83,
                                                0x79, 0x58, 0x00, 0x0f, 0x85, 0x03, 0x01, 0x00, 0x00};
    // 5f094d: mov edi, 0xc85e199d (then the call at 5f0952)
    static const unsigned char leave_call[] = {0xbf, 0x9d, 0x19, 0x5e, 0xc8};
    // 0xdef0a0: board ids 300..303 at stride 12
    bool boards = true;
    for (uint32_t i = 0; i < uint32_t(kBoards); ++i) {
        const uint32_t id = 300 + i;
        boards = boards && image.matches(kBoardTable + i * 12, reinterpret_cast<const unsigned char *>(&id), 4);
    }

    if (image.matches(hPlayerInfo, player_info, sizeof player_info) &&
        replace_function(hPlayerInfo, sizeof player_info, reinterpret_cast<void *>(&player_info_update),
                         "NetPlayerData connect/publish (64e810)"))
        ++installed;
    else
        log("progression: 64e810 bytes differ, NetPlayerData connect not installed");

    if (image.matches(hLeaveCall - 5, leave_call, sizeof leave_call) &&
        retarget_call(hLeaveCall, fDcLookup, reinterpret_cast<void *>(&leave_publish), "Leave publish"))
        ++installed;
    else
        log("progression: Leave::Enter call site differs, publish on leave not installed");

    if (boards && image.matches(hListUpdate, list_update, sizeof list_update) &&
        replace_function(hListUpdate, sizeof list_update, reinterpret_cast<void *>(&leaderboard_list_update),
                         "leaderboard list update (5b3000)"))
        ++installed;
    else
        log("progression: leaderboard list bytes differ, leaderboards not installed");
}

}  // namespace u2mp
