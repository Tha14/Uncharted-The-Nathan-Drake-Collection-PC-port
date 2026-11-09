// u2mp menus: the multiplayer menu queries the PS4 1.02 build left empty, ported from PS3 1.10.
// Page-by-page audit and what each port shows: u2mp/docs/MENUS.md.
//
// Menu controls with action 22 (and page records with a panel query) ask a query function for their
// text, value or image. The function comes from the query table at 0x105e370 (PS3 0xe15d30): 132
// entries {u32 cond, u32 pad, fn at +8, u64 arg at +0x10}, ended by fn == 0, read on every call by the
// query helpers 0x65f4e0.. (`call [entry+8]`). The PS3 and PS4 tables list the same conditions in
// the same order, and the PS4 menu.bin multiplayer pages have the same controls and conditions as
// the PS3 menu.bin, so nothing has to be added to menu.bin: what made the pages look empty is code and
// data. Query object: +0 op (1 title text, 2 row text, 3 image, 4 get index, 5 last index, 7 enabled,
// 8 reset, 9 set index), +4 value / row, +0x40 text out, +0x48 table arg, +0x50 element id,
// +0x54 column, +0x78 the table entry.
//
// Two ways in, both checked first: a stub (ret) is replaced whole (replace_function), and a query that
// still has code but lacks data is wrapped by pointing its table entry at C++ that sets the data up and
// calls the original (the entry must still hold the original function).
#include <algorithm>
#include <array>
#include <utility>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "frontend3.h"
#include "online.h"
#include "progression.h"
#include "session_api.h"
#include "u2mp_internal.h"
#include "voice.h"

namespace u2mp {
namespace {

// ---- guest data -----------------------------------------------------------------------------
constexpr uint64_t kQueryTable = 0x105e370;   // first entry (cond 50); PS3 0xe15d30 area
constexpr int kQueryEntries = 132;
constexpr uint64_t kNetInfo = 0x104f1c8;      // PS3 0xf1fa48
constexpr uint64_t NI_LIVE_COUNT = 0x10;      // PS3 live-data value read by 0x4a9d04 (status line)
constexpr uint64_t NI_ONLINE_ID = 0x298;      // PS3 +0x288: the SceNpId (online id, 16 chars + NUL)
constexpr uint64_t NI_MODE = 0x2c4;
constexpr uint64_t kNgm = 0x113b760;          // g_netGameManager (PS3 0xf1dc80)
constexpr uint64_t NGM_GAME = 0x1ca0, NGM_PLAYLISTS = 0x20a0, NGM_PLAYLIST_COUNT = 0x21a0;
constexpr int kMaxPlaylists = 32;
constexpr uint64_t kNpd = 0x11474f8;          // NetPlayerData (PS3 0xf2da60)
constexpr uint64_t NPD_PLAYLIST = 0x104;      // the chosen playlist id (saved)
constexpr uint64_t kTextDb = 0x1066660;
// functions
constexpr uint64_t fTextFind = 0xcddc00;      // TextDb::Find(db, id) -> text or NULL
constexpr uint64_t fDcLookup = 0xa15e70;      // DC symbol by string id
constexpr uint64_t fSetNetGame = 0x63dc30;    // SetNetGame(ngm, s16 index, overrides, 10) (PS3 0x49c458)
constexpr uint32_t kNetGames = 0x3b9a067d;    // *net-games*: {s32 count; rec*} at +8, stride 0x90, +0xc type
constexpr uint32_t kGameTypes = 0xf985c7b6;   // game types: {s32 count; rec*} at +8, stride 0x44
// texts
constexpr uint32_t kTextPlayersInParty = 0xba8db526;   // "[A] players in party" (PS3 0x4c2098)
constexpr uint32_t kTextPartyLeader = 0xac182308;      // "Party Leader"

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }
template <typename T> T &Q(uint8_t *q, uint64_t off) { return *reinterpret_cast<T *>(q + off); }

using QueryFn = void(U2MP_GUEST *)(uint8_t *query);

enum Op : uint32_t { kTitle = 1, kRowText = 2, kImage = 3, kGetIndex = 4, kLastIndex = 5, kEnabled = 7,
                     kReset = 8, kSetIndex = 9 };

const char kEmpty[] = "";

const char *game_text(uint32_t id) {
    void *db = image.at<void>(kTextDb);
    const char *t = guest<const char *(U2MP_GUEST *)(void *, uint32_t)>(fTextFind)(db, id);
    return t ? t : kEmpty;
}

void *dc_lookup(uint32_t id) { return guest<void *(U2MP_GUEST *)(uint32_t)>(fDcLookup)(id); }

// "[A]" -> value, as the game's formatter (0xcdde40) does for one number.
void format_count(char *out, size_t size, uint32_t text, int value) {
    std::string s = game_text(text);
    const size_t at = s.find("[A]");
    if (at != std::string::npos) s.replace(at, 3, std::to_string(value));
    std::snprintf(out, size, "%s", s.c_str());
}

void set_text(uint8_t *q, const char *text) { Q<uint64_t>(q, 0x40) = uint64_t(uintptr_t(text)); }

// A plain list (LIST_FRIENDS, LIST_PLAYERSMET, the join list): rows of text, a current index.
template <typename RowFn>
void list_query(uint8_t *q, int count, int &current, RowFn row) {
    switch (Q<uint32_t>(q, 0)) {
    case kRowText: {
        const int r = Q<int32_t>(q, 4);
        set_text(q, Q<uint32_t>(q, 0x54) == 0 && r >= 0 && r < count ? row(r) : kEmpty);
        break;
    }
    case kGetIndex: Q<int32_t>(q, 4) = count ? (current < count ? current : count - 1) : -1; break;
    case kLastIndex: Q<int32_t>(q, 4) = count - 1; break;
    case kEnabled: Q<int32_t>(q, 4) = 1; break;
    case kReset: current = 0; break;
    case kSetIndex: current = Q<int32_t>(q, 4); break;
    default: break;
    }
}

// ---- cond 133: the profile panel of PAGE_MULTIPLAYER (0x65e890 = PS3 0x4bddd4) ------------------
// Intact: name, rank badge (cash -> rank table c85e199d), character, kills, wins, money, co-op points,
// all from NetPlayerData (progression.cpp loads it). Its name line reads the online id at
// netInfo+0x298, which PS3 NetInit filled (GetNpId -> 0xf1fa48+0x288) and nothing on PS4 does.
QueryFn orig_panel = nullptr;
void set_online_id() {
    char *id = image.at<char>(kNetInfo + NI_ONLINE_ID);
    const std::string name = session::local_name();
    std::memset(id, 0, 17);
    std::memcpy(id, name.data(), std::min<size_t>(name.size(), 16));
}
U2MP_GUEST void panel_query(uint8_t *q) {
    set_online_id();
    orig_panel(q);
}

// ---- cond 49: the status line under the party list (0x659a90 = PS3 0x4bae1c) ----------------------
// PS3 formatted a live-data count from Naughty Dog's server (0x4a9d04 = [netInfo+0x10]) with text
// 69cbee70 "[A] daily players" (PS3 dlc-patch7 bank, served from the import by remaining.cpp). There
// is no server: the count is the number of players in the party.
QueryFn orig_status = nullptr;
U2MP_GUEST void status_query(uint8_t *q) {
    int32_t &live = G<int32_t>(kNetInfo + NI_LIVE_COUNT);
    const int32_t saved = live;
    live = session::member_count();
    orig_status(q);
    live = saved;
}

// ---- cond 55: PLAYLIST on PAGE_LOBBY in matchmaking (0x6562d0 = PS3 0x4bd9c8) ----------------------
// The query is intact, but the playlists (ngm+0x20a0[], count +0x21a0) came from the PS3 live-data
// download, which PS4 dropped: the row stayed empty. A record is {+0 id (also its image id), +4 name
// text, +8 description text, +0x28 game list (NULL = any), +0x30 ranked, +0x34 skill board (-1 none)}
// (readers 0x6562d0, 0x6400d0, 0x640f00, 0x64a460, 0x64aac0). Here one playlist per game type of
// *net-games*, named and described by the game type's own texts; choosing one sets the lobby's net
// game to a game of that type, and Start Match (session.cpp) picks a random game of that type.
struct Playlist {
    uint32_t id, name, desc;
    uint32_t pad0[7];
    uint64_t games;        // +0x28
    uint32_t ranked;       // +0x30
    int32_t skill;         // +0x34
    uint32_t pad1[2];
    int32_t type;          // ours: the game type the lobby shows (the first available of `types`)
    uint32_t image;        // ours: the playlist picture (net package image table)
    int32_t types[8];      // ours: the game types a match of this playlist is picked from
    int32_t type_count;
};
static_assert(offsetof(Playlist, games) == 0x28 && offsetof(Playlist, skill) == 0x34, "playlist layout");
Playlist playlists[kMaxPlaylists];
int playlist_count = 0;
uint32_t applied_playlist = 0;
QueryFn orig_playlist = nullptr;

struct DcTable { int32_t count; int32_t pad; uint8_t *records; };

// The playlist picture. PS3 drew the row's image as type 0x25 (playlist id -> picture); the PS4 menu
// renderer has no case for 0x25 (dispatch 0x5bd855 sends 0x1e, 0x22..0x25 and 0x27 to its default),
// so the box stayed empty. The pictures are in the PS4 net package as images of the net image table
// (0x1130008+0x70; materials general/net/playlists:<name>, id = sid("general/net/playlists/<name>")),
// which a type 3 image reads (as the game type and map rows do, 0x656abb). Which picture: by the
// game type's name text (the ids the PS4 text bank uses), as the PS3 server playlists grouped them.
uint32_t playlist_image(uint32_t name_text) {
    switch (name_text) {
    case 0xa30341e7u: case 0x8dfdede9u: case 0x3b8de400u: return sid("general/net/playlists/deathmatch");
    case 0x3d9470a5u: case 0x0544e6f6u: case 0x13f76fccu: case 0x8f86cb5fu: case 0x4bf30e73u:
        return sid("general/net/playlists/objective");
    case 0x2ee192abu: case 0x5136a578u: case 0x00248bb5u: case 0xe9995f4cu: return sid("general/net/playlists/coop");
    case 0x2959eca4u: case 0xb81a7c18u: return sid("general/net/playlists/survival");
    default: return sid("general/net/playlists/default");
    }
}
constexpr uint32_t kTextMachinima = 0x5614c769;   // the cinema game type: not a matchmaking playlist

// The PS3 server playlists (live data of PS3 1.10, from the PS3 reference screenshots): name and
// description texts (the PS4 networking bank still has most; the four only the server sent are
// served in every language by texts.cpp), picture (general/net/playlists/<name>, in the PS4 net.pak or, for
// the-lab, net-u2mp.pak) and the game types (by their name texts; two ids where the bank has two).
namespace text {
constexpr uint32_t Deathmatch = 0xa30341e7, Deathmatch2 = 0x8dfdede9, Elimination = 0x3b8de400, Plunder = 0x3d9470a5,
                   TurfWar = 0x0544e6f6, KingOfTheHill = 0x13f76fcc, ChainReaction = 0x71ede2dc,
                   ChainReaction2 = 0xf625a024, CoopObjective = 0x5136a578, CoopArena = 0x2ee192ab,
                   GoldRush = 0x00248bb5, GoldRush2 = 0xe9995f4c, Survival = 0x2959eca4, Survival2 = 0xb81a7c18,
                   Objectives = 0x4bf30e73, Ranked = 0x47b24464;
}
constexpr uint32_t kTextAllCompetitive = sid("u2mp/playlist/all-competitive"),
                   kTextAllCompetitiveDesc = sid("u2mp/playlist/all-competitive/desc"),
                   kTextTheLab = sid("u2mp/playlist/the-lab"), kTextTheLabDesc = sid("u2mp/playlist/the-lab/desc"),
                   kTextEliminationDesc = sid("u2mp/playlist/elimination/desc");
struct PlaylistDef {
    const char *key;
    uint32_t name, desc;   // desc 0: the first game type's description
    const char *picture;   // preferred pictures, ','-separated: the first one no earlier playlist took
    uint32_t types[8];     // game type name texts, 0-terminated
};
// The PS3 playlist pictures (general/net/playlists/<name>): the PS4 net.pak has deathmatch, objective,
// coop, survival, mlg and default; elimination, grab-bag, plunder and the-lab come from net-u2mp.pak
// (the GOTY net.pak, the only place PS4 lacks them; menuimages.cpp looks the PS4 one up first).
// Ten pictures for eleven playlists: each playlist takes the first of its list that no earlier
// playlist took, so no two playlists built here share one (only the eleventh, when every one is
// playable, can repeat one).
const PlaylistDef kPlaylistDefs[] = {
    {"all-competitive", kTextAllCompetitive, kTextAllCompetitiveDesc, "grab-bag",
     {text::Deathmatch, text::Deathmatch2, text::Plunder, text::TurfWar, text::KingOfTheHill, text::ChainReaction,
      text::ChainReaction2, text::Elimination}},
    {"ranked", text::Ranked, 0x57fc82aa, "mlg",
     {text::Deathmatch, text::Deathmatch2, text::Plunder, text::TurfWar, text::KingOfTheHill, text::ChainReaction,
      text::ChainReaction2, text::Elimination}},
    {"deathmatch", text::Deathmatch, 0xfc8dd900, "deathmatch", {text::Deathmatch, text::Deathmatch2, text::Elimination}},
    {"elimination", text::Elimination, kTextEliminationDesc, "elimination", {text::Elimination}},
    {"objectives", text::Objectives, 0x675ef552, "objective",
     {text::Plunder, text::TurfWar, text::KingOfTheHill, text::ChainReaction, text::ChainReaction2}},
    {"plunder", text::Plunder, 0x32cc1acb, "plunder", {text::Plunder}},
    {"coop-objective", text::CoopObjective, 0xc35e16e8, "coop", {text::CoopObjective}},
    {"coop-arena", text::CoopArena, 0, "coop,survival,default", {text::CoopArena}},
    {"gold-rush", text::GoldRush, 0, "default,survival,coop", {text::GoldRush, text::GoldRush2}},
    {"survival", text::Survival, 0, "survival,default,coop", {text::Survival, text::Survival2}},
    {"the-lab", kTextTheLab, kTextTheLabDesc, "the-lab,default", {text::Deathmatch, text::Deathmatch2}},
};

// The first picture of `list` no playlist built so far uses (else the first of the list).
uint32_t pick_picture(const char *list) {
    uint32_t first = 0;
    for (const char *p = list; *p;) {
        const char *e = std::strchr(p, ',');
        const std::string name(p, e ? size_t(e - p) : std::strlen(p));
        const uint32_t id = sid(("general/net/playlists/" + name).c_str());
        if (!first) first = id;
        bool used = false;
        for (int i = 0; i < playlist_count; ++i) used = used || playlists[i].image == id;
        if (!used) return id;
        p = e ? e + 1 : p + name.size();
    }
    return first;
}

void build_playlists() {
    uint32_t &count = G<uint32_t>(kNgm + NGM_PLAYLIST_COUNT);
    if (count != 0) return;   // filled (by us, or by anything else that has playlists)
    if (playlist_count != 0) {   // ours, cleared since: put them back
        for (int p = 0; p < playlist_count; ++p)
            G<uint64_t>(kNgm + NGM_PLAYLISTS + 8 * uint64_t(p)) = uint64_t(uintptr_t(&playlists[p]));
        count = uint32_t(playlist_count);
        return;
    }
    auto *games = static_cast<DcTable *>(dc_lookup(kNetGames));
    auto *types = static_cast<DcTable *>(dc_lookup(kGameTypes));
    if (!games || !types || games->count <= 0 || types->count <= 0 || !games->records || !types->records) return;
    // game types that have a playable net game here (DLC maps only when imported)
    std::vector<bool> playable(size_t(types->count), false);
    for (int32_t i = 0; i < games->count; ++i) {
        const int32_t type = *reinterpret_cast<int32_t *>(games->records + 0x90 * i + 0x0c);
        if (type >= 0 && type < types->count && dlc_net_game_available(i)) playable[size_t(type)] = true;
    }
    for (const PlaylistDef &d : kPlaylistDefs) {
        if (playlist_count >= kMaxPlaylists) break;
        Playlist pl{};
        for (const uint32_t want : d.types) {
            if (!want) break;
            for (int32_t t = 0; t < types->count; ++t) {
                if (!playable[size_t(t)] || *reinterpret_cast<const uint32_t *>(types->records + 0x44 * t) != want) continue;
                bool dup = false;
                for (int k = 0; k < pl.type_count; ++k) dup = dup || pl.types[k] == t;
                if (!dup && pl.type_count < 8) pl.types[pl.type_count++] = t;
            }
        }
        if (pl.type_count == 0) continue;
        pl.type = pl.types[0];
        pl.name = d.name;
        pl.desc = d.desc ? d.desc : *reinterpret_cast<const uint32_t *>(types->records + 0x44 * pl.type + 4);
        pl.id = sid((std::string("u2mp/playlist/") + d.key).c_str());
        pl.image = pick_picture(d.picture);
        pl.pad0[1] = pl.image;   // +0x10: the picture the renderer's type 0x24/0x25 reads (menuimages.cpp)
        pl.skill = -1;
        playlists[playlist_count++] = pl;
        log("menus: playlist %s -> picture %08x", d.key, pl.image);
    }
    if (playlist_count > 0) {
        for (int p = 0; p < playlist_count; ++p)
            G<uint64_t>(kNgm + NGM_PLAYLISTS + 8 * uint64_t(p)) = uint64_t(uintptr_t(&playlists[p]));
        count = uint32_t(playlist_count);
        log("menus: %d PS3 playlists", playlist_count);
        return;
    }
    // Fallback (no game type text matched): one playlist per game type, as before.
    for (int32_t i = 0; i < games->count && playlist_count < kMaxPlaylists; ++i) {
        const int32_t type = *reinterpret_cast<int32_t *>(games->records + 0x90 * i + 0x0c);
        if (type < 0 || type >= types->count || !dlc_net_game_available(i)) continue;
        bool seen = false;
        for (int p = 0; p < playlist_count; ++p) seen = seen || playlists[p].type == type;
        if (seen) continue;
        const uint8_t *t = types->records + 0x44 * type;
        if (*reinterpret_cast<const uint32_t *>(t + 0) == kTextMachinima) continue;
        Playlist &pl = playlists[playlist_count];
        pl = Playlist{};
        pl.name = *reinterpret_cast<const uint32_t *>(t + 0);
        pl.desc = *reinterpret_cast<const uint32_t *>(t + 4);
        pl.id = *reinterpret_cast<const uint32_t *>(t + 8);   // the game type's image id
        bool taken = pl.id == 0;
        for (int p = 0; p < playlist_count; ++p) taken = taken || playlists[p].id == pl.id;
        if (taken) pl.id = 0x55320000u + uint32_t(type);   // keep ids unique and non-zero
        pl.skill = -1;
        pl.type = type;
        pl.types[0] = type;
        pl.type_count = 1;
        pl.image = playlist_image(pl.name);
        pl.pad0[1] = pl.image;   // +0x10: the picture the renderer's type 0x24/0x25 reads (menuimages.cpp)
        ++playlist_count;
    }
    // Deathmatch first, as PS3 listed it (stable: the others keep the *net-games* order).
    std::stable_sort(playlists, playlists + playlist_count, [](const Playlist &a, const Playlist &b) {
        const uint32_t dm = sid("general/net/playlists/deathmatch");
        return (a.image == dm) > (b.image == dm);
    });
    for (int p = 0; p < playlist_count; ++p)
        G<uint64_t>(kNgm + NGM_PLAYLISTS + 8 * uint64_t(p)) = uint64_t(uintptr_t(&playlists[p]));
    count = uint32_t(playlist_count);
    log("menus: %d playlists from %d net games", playlist_count, games->count);
}

// The leader's choice reaches the lobby's net game (a game of the playlist's type).
void apply_playlist() {
    if (!session::is_host() || G<int32_t>(kNetInfo + NI_MODE) != 1) return;
    const uint32_t id = G<uint32_t>(kNpd + NPD_PLAYLIST);
    if (id == applied_playlist) return;
    const Playlist *pl = nullptr;
    for (int p = 0; p < playlist_count; ++p)
        if (playlists[p].id == id) pl = &playlists[p];
    if (!pl) return;
    auto *games = static_cast<DcTable *>(dc_lookup(kNetGames));
    if (!games || !games->records) return;
    const int16_t current = G<int16_t>(kNgm + NGM_GAME);
    if (current >= 0 && current < games->count &&
        *reinterpret_cast<int32_t *>(games->records + 0x90 * current + 0x0c) == pl->type && dlc_net_game_available(current)) {
        applied_playlist = id;
        return;
    }
    for (int32_t i = 0; i < games->count; ++i)
        if (*reinterpret_cast<int32_t *>(games->records + 0x90 * i + 0x0c) == pl->type && dlc_net_game_available(i)) {
            guest<void(U2MP_GUEST *)(void *, int16_t, const void *, int)>(fSetNetGame)(image.at<void>(kNgm), int16_t(i),
                                                                                    nullptr, 10);
            applied_playlist = id;
            log("menus: playlist %08x -> net game %d", id, i);
            return;
        }
}

// A type 3 image of the net image table (0x1130008+0x70), index -1 = look the id up (0x5558c0).
void net_image(uint8_t *q, uint32_t id) {
    Q<uint32_t>(q, 0x08) = 3;
    Q<uint64_t>(q, 0x10) = 0;
    Q<uint32_t>(q, 0x18) = 0;
    Q<uint64_t>(q, 0x1c) = uint64_t(id) | (uint64_t(1) << 32);
    Q<int32_t>(q, 0x24) = -1;
    Q<uint32_t>(q, 0x28) = 0;
    Q<int32_t>(q, 0x2c) = -1;
    Q<float>(q, 0x30) = 1.0f;
}

U2MP_GUEST void playlist_query(uint8_t *q) {
    build_playlists();
    if (G<int16_t>(kNgm + NGM_GAME) < 0) applied_playlist = 0;   // left Multiplayer: apply again
    orig_playlist(q);
    if (Q<uint32_t>(q, 0) == kImage && Q<uint32_t>(q, 0x08) == 0x25) {   // see playlist_image
        const uint32_t row = Q<uint32_t>(q, 4);
        if (row < uint32_t(playlist_count)) net_image(q, playlists[row].image);
        return;
    }
    if (Q<uint32_t>(q, 0) != kGetIndex || playlist_count == 0) return;
    if (Q<int32_t>(q, 4) < 0) {   // the saved id is not one of ours: take the first
        G<uint32_t>(kNpd + NPD_PLAYLIST) = playlists[0].id;
        Q<int32_t>(q, 4) = 0;
    }
    apply_playlist();
}

// ---- cond 36: LIST_FRIENDS on PAGE_MULTIPLAYER_FRIENDS_LIST (0x65bbb0 = ret; PS3 0x4c1fe0) --------
// PS3: the PSN friends list, titled "[A] players in party", each row the friend's name and status.
// PC has no friends list: the rows are the party (selecting one opens the player popup,
// remaining.cpp friends_row, which takes the same row order), the title the party size.
int friends_current = 0;
char friends_title[96];
char friends_rows[16][96];
U2MP_GUEST void friends_query(uint8_t *q) {
    if (online::active()) {   // online: the RPCN friends (PS3 0x4c1fe0: the PSN friends, label "Friends")
        const int n = online::friend_count();
        if (Q<uint32_t>(q, 0) == kTitle) {
            int on = 0;
            for (int i = 0; i < n; ++i) on += online::friend_online(i) ? 1 : 0;
            std::snprintf(friends_title, sizeof friends_title, "Friends (%d of %d online)", on, n);
            set_text(q, friends_title);
            return;
        }
        // row 0: the online game list (PS3 reached it from BUTTON_MULTIPLAYER_JOIN, which PS4's pages lost;
        // the Invites row only shows a label while invites wait), then the friends
        list_query(q, 1 + (n ? n : 1), friends_current, [n](int r) -> const char * {
            if (r == 0) return "Browse Online Games";
            if (!n) return "No RPCN friends yet. Add friends in the launcher.";
            return online::friend_row(r - 1);
        });
        return;
    }
    const int count = session::member_count();
    if (Q<uint32_t>(q, 0) == kTitle) {
        format_count(friends_title, sizeof friends_title, kTextPlayersInParty, count);
        set_text(q, friends_title);
        return;
    }
    list_query(q, count, friends_current, [](int r) -> const char * {
        uint32_t id = 0;
        std::string name;
        if (r >= 16 || !session::member_at(r, id, name)) return kEmpty;
        if (id == 1) name += std::string(" - ") + game_text(kTextPartyLeader);   // the host leads (net id 1)
        std::snprintf(friends_rows[r], sizeof friends_rows[r], "%s", name.c_str());
        return friends_rows[r];
    });
}

// ---- cond 39: LIST_PLAYERSMET (0x65bcd0 = PS3 0x4b8638) ---------------------------------------------
// Intact, but its rows (0x113b668: +0x10 count, +0x18 rows of 0x28) came from the PS3 NP "players
// met" fetch (0x48f79c, absent). Here: the players of recent matches from the local progression
// store (progression.cpp records them on Leave). Selecting a row: remaining.cpp replaces 0x5ebdf0
// (Add Friend online / View Profile).
QueryFn orig_players_met = nullptr;
int met_current = 0;
std::vector<std::string> met_names;
U2MP_GUEST void players_met_query(uint8_t *q) {
    if (Q<uint32_t>(q, 0) == kReset || met_names.empty()) met_names = progression_recent_players(50);
    list_query(q, int(met_names.size()), met_current,
               [](int r) -> const char * { return met_names[size_t(r)].c_str(); });
}

// ---- cond 3: BUTTON_MULTIPLAYER_SELECT_JOIN_GAME list (0x659590 = ret; PS3 0x4b8ebc) ---------------
// PS3 listed the Matching2 rooms found by the search. PC joins by address (session.cpp), so the list
// is empty; answering keeps the page from reading stale values.
int join_current = 0;
U2MP_GUEST void join_list_query(uint8_t *q) {
    if (const int rows = online::join_rows(); rows >= 0) {   // online: the rooms of the search (online.cpp)
        if (Q<uint32_t>(q, 0) == kTitle) set_text(q, online::join_title());
        else list_query(q, rows, join_current, [](int r) -> const char * { return online::join_row(r); });
        return;
    }
    if (Q<uint32_t>(q, 0) == kTitle) set_text(q, kEmpty);
    else list_query(q, 0, join_current, [](int) -> const char * { return kEmpty; });
}

// ---- cond 104: voice volume slider of PAGE_AUDIO_VOLUMES (0x65f3f0 = PS3 0x4b77ac) ----------------------
// PS4 answers 0 / max 20 and drops changes (cellVoice is gone). PS3: op 4 value = volume x 20, op 5 max 20,
// op 9 volume = value x 0.05. Here the volume is voice.cpp's playback volume; outside multiplayer the
// original answers (PS3 put a blank label there).
QueryFn orig_voice_volume = nullptr;
U2MP_GUEST void voice_volume_query(uint8_t *q) {
    const uint32_t op = Q<uint32_t>(q, 0);
    if (op == 4 && G<int32_t>(0x14ce97c) > 1) {
        const float v = voice::volume() * 20.0f + 0.5f;
        *reinterpret_cast<int32_t *>(q + 4) = std::min(20, std::max(0, int32_t(v)));
        return;
    }
    if (op == 9 && G<int32_t>(0x14ce97c) > 1) {
        voice::set_volume(float(Q<int32_t>(q, 4)) * 0.05f);
        return;
    }
    orig_voice_volume(q);
}

// ---- install ------------------------------------------------------------------------------------
struct QueryEntry {
    uint32_t cond, pad;
    uint64_t fn, arg;
};
static_assert(sizeof(QueryEntry) == 0x18, "query table entry");

// Profile page query trace (diagnostics, see install_menus).
constexpr uint32_t kTraceConds[] = {105, 77, 78, 134, 40, 41, 80, 82, 135, 108, 109, 110};
constexpr size_t kTraceCount = sizeof(kTraceConds) / sizeof(*kTraceConds);
QueryFn traced_fn[kTraceCount] = {};
int trace_budget[kTraceCount] = {};
template <size_t I> U2MP_GUEST void trace_query(uint8_t *q) {
    const uint32_t op = Q<uint32_t>(q, 0), row = Q<uint32_t>(q, 4);
    traced_fn[I](q);
    if (trace_budget[I] < 12) {
        ++trace_budget[I];
        log("profile query cond %u: op %u row %u -> +4 %d +8 %u +0x40 %p +0x54 %u", kTraceConds[I], op, row,
            Q<int32_t>(q, 4), Q<uint32_t>(q, 8), reinterpret_cast<void *>(uintptr_t(Q<uint64_t>(q, 0x40))),
            Q<uint32_t>(q, 0x54));
    }
}
template <size_t... I> constexpr std::array<QueryFn, sizeof...(I)> make_traces(std::index_sequence<I...>) {
    return {&trace_query<I>...};
}
const std::array<QueryFn, kTraceCount> trace_fns = make_traces(std::make_index_sequence<kTraceCount>{});

QueryEntry *find_entry(uint32_t cond, uint64_t expected_fn) {
    const uint64_t base = uint64_t(uintptr_t(image.base));
    for (int i = 0; i < kQueryEntries; ++i) {
        auto *e = image.at<QueryEntry>(kQueryTable + uint64_t(i) * sizeof(QueryEntry));
        if (e->fn == 0) break;
        if (e->cond == cond) return e->fn == base + expected_fn ? e : nullptr;
    }
    return nullptr;
}

// Points the table entry of `cond` at `host`, keeping the original in `*original`.
bool wrap_query(uint32_t cond, uint64_t expected_fn, QueryFn host, QueryFn *original, const char *name) {
    QueryEntry *e = find_entry(cond, expected_fn);
    if (!e || !image.executable(expected_fn, 1)) {
        log("%s (cond %u): table entry is not 0x%llx, left alone", name, cond, (unsigned long long)expected_fn);
        return false;
    }
    *original = reinterpret_cast<QueryFn>(uintptr_t(e->fn));
    e->fn = uint64_t(uintptr_t(host));
    log("%s (cond %u): 0x%llx wrapped", name, cond, (unsigned long long)expected_fn);
    return true;
}

}  // namespace

// Matchmaking Start Match: a random game type of the chosen playlist (-1: none chosen / not ours).
int32_t playlist_pick_type() {
    const uint32_t id = G<uint32_t>(kNpd + NPD_PLAYLIST);
    for (int p = 0; p < playlist_count; ++p)
        if (playlists[p].id == id && playlists[p].type_count > 0)
            return playlists[p].types[size_t(std::rand()) % size_t(playlists[p].type_count)];
    return -1;
}

// The matchmaking vote (frontend3.cpp): every game type of the chosen playlist.
bool playlist_game_types(std::vector<int32_t> &types) {
    types.clear();
    const uint32_t id = G<uint32_t>(kNpd + NPD_PLAYLIST);
    for (int p = 0; p < playlist_count; ++p)
        if (playlists[p].id == id)
            for (int t = 0; t < playlists[p].type_count; ++t) types.push_back(playlists[p].types[t]);
    return !types.empty();
}

void install_menus(int &installed) {
    // The table must look like the 1.02 one: cond 50 first, its STARTMATCH query at 0x655c40.
    const uint64_t base = uint64_t(uintptr_t(image.base));
    if (kQueryTable + kQueryEntries * sizeof(QueryEntry) > image.size ||
        image.at<QueryEntry>(kQueryTable)->cond != 50 || image.at<QueryEntry>(kQueryTable)->fn != base + 0x655c40) {
        log("menus: query table 0x%llx differs, menu queries not installed", (unsigned long long)kQueryTable);
        return;
    }
    // TextDb::Find prologue (remaining.cpp hooks it; either way it is callable).
    if (!image.executable(fTextFind, 16) || !image.executable(fSetNetGame, 16) || !image.executable(fDcLookup, 16)) {
        log("menus: guest helpers not where expected, menu queries not installed");
        return;
    }

    // Wrapped queries (code intact, data missing).
    struct Wrap { uint32_t cond; uint64_t fn; QueryFn host; QueryFn *orig; const char *name; } wraps[] = {
        {133, 0x65e890, &panel_query, &orig_panel, "profile panel"},
        {49, 0x659a90, &status_query, &orig_status, "party status line"},
        {55, 0x6562d0, &playlist_query, &orig_playlist, "PLAYLIST"},
        {39, 0x65bcd0, &players_met_query, &orig_players_met, "LIST_PLAYERSMET"},
        {104, 0x65f3f0, &voice_volume_query, &orig_voice_volume, "voice volume slider"},
    };
    for (const Wrap &w : wraps)
        if (wrap_query(w.cond, w.fn, w.host, w.orig, w.name)) ++installed;

    // Diagnostics: the Profile page (PAGE_MULTIPLAYER_PLAYER_OPTIONS) draws only its Level widget.
    // Log the first calls of each of its queries (op, row, and what they answer).
    for (size_t i = 0; i < kTraceCount; ++i) {
        QueryEntry *e = nullptr;
        for (uint32_t k = 0; k < kQueryEntries && !e; ++k) {
            auto *x = image.at<QueryEntry>(kQueryTable + uint64_t(k) * sizeof(QueryEntry));
            if (x->cond == kTraceConds[i] && x->fn) e = x;
        }
        if (!e) continue;
        traced_fn[i] = reinterpret_cast<QueryFn>(uintptr_t(e->fn));
        e->fn = uint64_t(uintptr_t(trace_fns[i]));
    }

    // Stubbed queries (ret), replaced whole; their table entries must still point at them.
    struct Stub { uint32_t cond; uint64_t at; void *fn; const char *name; } stubs[] = {
        {36, 0x65bbb0, reinterpret_cast<void *>(&friends_query), "LIST_FRIENDS query"},
        {3, 0x659590, reinterpret_cast<void *>(&join_list_query), "join game list query"},
    };
    for (const Stub &s : stubs) {
        if (!find_entry(s.cond, s.at) || !is_ret_stub(s.at)) {
            log("%s at 0x%llx: not the stripped query this port expects, left alone", s.name,
                (unsigned long long)s.at);
            continue;
        }
        if (replace_function(s.at, 5, s.fn, s.name)) ++installed;
    }
}

}  // namespace u2mp
