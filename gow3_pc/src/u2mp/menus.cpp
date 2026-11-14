// u2mp menus: the multiplayer menu queries the PS4 1.02 build left empty or fed with PS3-only data.
//
// The menu pages (menu.bin) are the PS3 ones, but behind several of their controls the query
// functions are bare `ret` stubs, and others read live data only the PS3 game had (server data,
// PSN). This file gives those controls content: the profile panel's name, the party status line,
// the PS3 playlists of the lobby, the friends list (RPCN online, the party on LAN), the players
// met, the join game list and the voice volume slider. It also traces a few Profile page queries
// for diagnostics.
//
// A query reaches us either through the query table (we swap the entry's function pointer and may
// still call the game's function) or through a jump written over a stripped `ret` stub.
//
// Reimplemented from the behaviour description u2mp/docs/spec/menus.md.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "frontend3.h"
#include "online.h"
#include "progression.h"
#include "session_api.h"
#include "u2mp_internal.h"
#include "voice.h"

// Every float operation is rounded on its own, as the description requires.
#pragma clang fp contract(off)

namespace u2mp {
namespace {

// ---- guest memory and calls ----------------------------------------------------------------------

template <typename T> T get(const uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <typename T> void put(uint8_t *p, T v) { std::memcpy(p, &v, sizeof v); }

uint8_t *abs_at(uint64_t offset) { return image.base + offset; }

// Game locations used here (image offsets of the 1.02 build).
constexpr uint64_t kQueryTable = 0x105e370;
constexpr int kQueryEntries = 132;
constexpr uint64_t kEntrySize = 0x18;
constexpr uint64_t kStartMatchQuery = 0x655c40;
constexpr uint64_t kNetInfo = 0x104f1c8;
constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kNpd = 0x11474f8;
constexpr uint64_t kTextDb = 0x1066660;
constexpr uint64_t kNetSm = 0x14ce97c;
constexpr uint64_t kTextFind = 0xcddc00;
constexpr uint64_t kDcLookup = 0xa15e70;
constexpr uint64_t kSetNetGame = 0x63dc30;

constexpr uint32_t kDcNetGames = 0x3b9a067d;
constexpr uint32_t kDcGameTypes = 0xf985c7b6;
constexpr uint32_t kTextPartyCount = 0xba8db526;
constexpr uint32_t kTextPartyLeader = 0xac182308;
constexpr uint32_t kMachinimaName = 0x5614c769;

using QueryFn = void(U2MP_GUEST *)(uint8_t *q);

const char *game_text(uint32_t id) {
    auto find = reinterpret_cast<const char *(U2MP_GUEST *)(void *, uint32_t)>(abs_at(kTextFind));
    const char *text = find(abs_at(kTextDb), id);
    return text ? text : "";
}

void *dc_lookup(uint32_t id) {
    return reinterpret_cast<void *(U2MP_GUEST *)(uint32_t)>(abs_at(kDcLookup))(id);
}

// A DC table as the game lays it out: {s32 count; s32 pad; u8 *records}.
struct DcTable {
    int32_t count = 0;
    uint8_t *records = nullptr;
};
DcTable read_table(const void *symbol) {
    const auto *p = static_cast<const uint8_t *>(symbol);
    return {get<int32_t>(p), get<uint8_t *>(p + 8)};
}

// ---- query object ----------------------------------------------------------------------------------

uint32_t op_of(const uint8_t *q) { return get<uint32_t>(q); }
void set_text(uint8_t *q, const char *text) { put<uint64_t>(q + 0x40, reinterpret_cast<uint64_t>(text)); }

const char kNothing[] = "";

// Copies `text` into a 96-byte buffer, cutting it to 95 bytes.
void fill96(char (&buffer)[96], const std::string &text) {
    const size_t n = text.size() < 95 ? text.size() : 95;
    std::memcpy(buffer, text.data(), n);
    buffer[n] = 0;
}

// The list behaviour most controls share: `count` rows, a remembered current index, and the row
// text produced only for a valid row of column 0.
template <typename RowText> void plain_list(uint8_t *q, int32_t count, int32_t &cur, RowText row_text) {
    switch (op_of(q)) {
    case 2: {
        const int32_t r = get<int32_t>(q + 4);
        if (get<uint32_t>(q + 0x54) == 0 && r >= 0 && r < count)
            set_text(q, row_text(r));
        else
            set_text(q, kNothing);
        break;
    }
    case 4: put<int32_t>(q + 4, count == 0 ? -1 : (cur < count ? cur : count - 1)); break;
    case 5: put<int32_t>(q + 4, count - 1); break;
    case 7: put<int32_t>(q + 4, 1); break;
    case 8: cur = 0; break;
    case 9: cur = get<int32_t>(q + 4); break;
    default: break;
    }
}

// Game text `id` with its first "[A]" replaced by `value`.
void count_text(char (&buffer)[96], uint32_t id, int value) {
    std::string text = game_text(id);
    const size_t at = text.find("[A]");
    if (at != std::string::npos) text.replace(at, 3, std::to_string(value));
    fill96(buffer, text);
}

// ---- wrapped table entries --------------------------------------------------------------------------

uint64_t original_profile = 0, original_status = 0, original_playlist = 0, original_met = 0, original_volume = 0;

void call(uint64_t fn, uint8_t *q) { reinterpret_cast<QueryFn>(fn)(q); }

// cond 133: the profile panel shows the online id the PS3 got from PSN; give it our player name.
U2MP_GUEST void query_profile(uint8_t *q) {
    uint8_t *id = abs_at(kNetInfo + 0x298);
    std::memset(id, 0, 17);
    const std::string name = session::local_name();
    std::memcpy(id, name.data(), name.size() < 16 ? name.size() : 16);
    call(original_profile, q);
}

// cond 49: the status line counts the PS3 live data; let it count the party for this one call.
U2MP_GUEST void query_status(uint8_t *q) {
    uint8_t *field = abs_at(kNetInfo + 0x10);
    const int32_t saved = get<int32_t>(field);
    put<int32_t>(field, session::member_count());
    call(original_status, q);
    put<int32_t>(field, saved);
}

// ---- playlists (cond 55) ------------------------------------------------------------------------------

// One playlist record as the game reads it through ngm+0x20a0 (0x6c bytes used).
struct alignas(16) Playlist {
    uint8_t bytes[0x70];
    uint32_t id() const { return get<uint32_t>(bytes); }
    uint32_t picture() const { return get<uint32_t>(bytes + 0x10); }
    int32_t first_type() const { return get<int32_t>(bytes + 0x40); }
    int32_t type_count() const { return get<int32_t>(bytes + 0x68); }
    int32_t type(int i) const { return get<int32_t>(bytes + 0x48 + 4 * i); }
};

constexpr int kMaxPlaylists = 32;
Playlist playlists[kMaxPlaylists];
int built = 0;           // records of the last successful build (0: none yet)
uint32_t applied = 0;    // the playlist id whose net game is already set

void make_record(Playlist &p, uint32_t id, uint32_t name, uint32_t desc, uint32_t picture, const int32_t *types,
                 int n) {
    std::memset(p.bytes, 0, sizeof p.bytes);
    put<uint32_t>(p.bytes + 0x00, id);
    put<uint32_t>(p.bytes + 0x04, name);
    put<uint32_t>(p.bytes + 0x08, desc);
    put<uint32_t>(p.bytes + 0x10, picture);
    put<int32_t>(p.bytes + 0x34, -1);
    put<int32_t>(p.bytes + 0x40, types[0]);
    put<uint32_t>(p.bytes + 0x44, picture);
    for (int i = 0; i < n; ++i) put<int32_t>(p.bytes + 0x48 + 4 * i, types[i]);
    put<int32_t>(p.bytes + 0x68, n);
}

// The PS3 playlists. Name and description are text ids, pictures are names under
// general/net/playlists/, types are the game types' name text ids.
struct PlaylistDef {
    const char *key;
    uint32_t name;
    uint32_t desc;   // 0: the first game type's description
    const char *pictures[3];
    uint32_t types[8];
};

constexpr uint32_t kDeathmatch = 0xa30341e7, kTeamDeathmatch = 0x8dfdede9, kPlunder = 0x3d9470a5,
                   kTurf = 0x0544e6f6, kKoh = 0x13f76fcc, kObjA = 0x71ede2dc, kObjB = 0xf625a024,
                   kElimination = 0x3b8de400;

const PlaylistDef kDefs[] = {
    {"all-competitive", sid("u2mp/playlist/all-competitive"), sid("u2mp/playlist/all-competitive/desc"),
     {"grab-bag"}, {kDeathmatch, kTeamDeathmatch, kPlunder, kTurf, kKoh, kObjA, kObjB, kElimination}},
    {"ranked", 0x47b24464, 0x57fc82aa, {"mlg"},
     {kDeathmatch, kTeamDeathmatch, kPlunder, kTurf, kKoh, kObjA, kObjB, kElimination}},
    {"deathmatch", kDeathmatch, 0xfc8dd900, {"deathmatch"}, {kDeathmatch, kTeamDeathmatch, kElimination}},
    {"elimination", kElimination, sid("u2mp/playlist/elimination/desc"), {"elimination"}, {kElimination}},
    {"objectives", 0x4bf30e73, 0x675ef552, {"objective"}, {kPlunder, kTurf, kKoh, kObjA, kObjB}},
    {"plunder", kPlunder, 0x32cc1acb, {"plunder"}, {kPlunder}},
    {"coop-objective", 0x5136a578, 0xc35e16e8, {"coop"}, {0x5136a578}},
    {"coop-arena", 0x2ee192ab, 0, {"coop", "survival", "default"}, {0x2ee192ab}},
    {"gold-rush", 0x00248bb5, 0, {"default", "survival", "coop"}, {0x00248bb5, 0xe9995f4c}},
    {"survival", 0x2959eca4, 0, {"survival", "default", "coop"}, {0x2959eca4, 0xb81a7c18}},
    {"the-lab", sid("u2mp/playlist/the-lab"), sid("u2mp/playlist/the-lab/desc"), {"the-lab", "default"},
     {kDeathmatch, kTeamDeathmatch}},
};

uint32_t picture_id(const char *name) { return sid(("general/net/playlists/" + std::string(name)).c_str()); }

void publish_playlists() {
    uint8_t *ngm = abs_at(kNgm);
    for (int i = 0; i < built; ++i) put<uint64_t>(ngm + 0x20a0 + 8 * i, reinterpret_cast<uint64_t>(&playlists[i]));
    put<uint32_t>(ngm + 0x21a0, uint32_t(built));
}

// The picture of a fallback playlist, chosen by its game type's name.
uint32_t fallback_picture(uint32_t name) {
    switch (name) {
    case kDeathmatch: case kTeamDeathmatch: case kElimination: return picture_id("deathmatch");
    case kPlunder: case kTurf: case kKoh: case 0x8f86cb5f: case 0x4bf30e73: return picture_id("objective");
    case 0x2ee192ab: case 0x5136a578: case 0x00248bb5: case 0xe9995f4c: return picture_id("coop");
    case 0x2959eca4: case 0xb81a7c18: return picture_id("survival");
    default: return picture_id("default");
    }
}

void build_playlists() {
    uint8_t *ngm = abs_at(kNgm);
    if (get<uint32_t>(ngm + 0x21a0) != 0) return;
    if (built > 0) {   // the game cleared the list: hand it the same records again
        publish_playlists();
        return;
    }
    void *games_sym = dc_lookup(kDcNetGames);
    void *types_sym = dc_lookup(kDcGameTypes);
    if (!games_sym || !types_sym) return;
    const DcTable games = read_table(games_sym), types = read_table(types_sym);
    if (games.count <= 0 || types.count <= 0 || !games.records || !types.records) return;

    auto game_type = [&](int i) { return get<int32_t>(games.records + 0x90 * i + 0x0c); };
    auto type_record = [&](int t) { return types.records + 0x44 * t; };

    // A game type counts only when one of its net games can be played (DLC maps imported).
    std::vector<bool> playable(size_t(types.count), false);
    for (int i = 0; i < games.count; ++i) {
        const int32_t t = game_type(i);
        if (t >= 0 && t < types.count && dlc_net_game_available(i)) playable[size_t(t)] = true;
    }

    for (const PlaylistDef &def : kDefs) {
        if (built >= kMaxPlaylists) break;
        int32_t found[8];
        int n = 0;
        for (int w = 0; w < 8 && def.types[w] != 0; ++w)
            for (int32_t t = 0; t < types.count; ++t) {
                if (!playable[size_t(t)] || get<uint32_t>(type_record(t)) != def.types[w]) continue;
                bool known = false;
                for (int k = 0; k < n; ++k) known = known || found[k] == t;
                if (!known && n < 8) found[n++] = t;
            }
        if (n == 0) continue;

        // Prefer a picture no earlier playlist of this pass shows.
        uint32_t picture = picture_id(def.pictures[0]);
        for (const char *name : def.pictures) {
            if (!name) break;
            const uint32_t candidate = picture_id(name);
            bool taken = false;
            for (int k = 0; k < built; ++k) taken = taken || playlists[k].picture() == candidate;
            if (!taken) {
                picture = candidate;
                break;
            }
        }
        const uint32_t desc = def.desc ? def.desc : get<uint32_t>(type_record(found[0]) + 4);
        make_record(playlists[built++], sid(("u2mp/playlist/" + std::string(def.key)).c_str()), def.name, desc,
                    picture, found, n);
        log("menus: playlist %s -> picture %08x", def.key, picture);
    }
    if (built > 0) {
        publish_playlists();
        log("menus: %d PS3 playlists", built);
        return;
    }

    // No PS3 playlist fits these game types: one playlist per playable game type instead.
    for (int i = 0; i < games.count && built < kMaxPlaylists; ++i) {
        const int32_t t = game_type(i);
        if (t < 0 || t >= types.count || !dlc_net_game_available(i)) continue;
        bool have = false;
        for (int k = 0; k < built; ++k) have = have || playlists[k].first_type() == t;
        if (have) continue;
        const uint8_t *rec = type_record(t);
        const uint32_t name = get<uint32_t>(rec);
        if (name == kMachinimaName) continue;
        uint32_t id = get<uint32_t>(rec + 8);
        bool clash = id == 0;
        for (int k = 0; k < built; ++k) clash = clash || playlists[k].id() == id;
        if (clash) id = 0x55320000u + uint32_t(t);
        make_record(playlists[built++], id, name, get<uint32_t>(rec + 4), fallback_picture(name), &t, 1);
    }
    // Deathmatch playlists first, each group keeping its order.
    const uint32_t dm = picture_id("deathmatch");
    Playlist sorted[kMaxPlaylists];
    int s = 0;
    for (int k = 0; k < built; ++k)
        if (playlists[k].picture() == dm) sorted[s++] = playlists[k];
    for (int k = 0; k < built; ++k)
        if (playlists[k].picture() != dm) sorted[s++] = playlists[k];
    for (int k = 0; k < built; ++k) playlists[k] = sorted[k];
    publish_playlists();
    log("menus: %d playlists from %d net games", built, games.count);
}

// Host in matchmaking: make the current net game one of the chosen playlist's game types.
void apply_playlist() {
    if (!session::is_host() || get<int32_t>(abs_at(kNetInfo + 0x2c4)) != 1) return;
    const uint32_t id = get<uint32_t>(abs_at(kNpd + 0x104));
    if (id == applied) return;
    const Playlist *chosen = nullptr;
    for (int k = 0; k < built; ++k)
        if (playlists[k].id() == id) chosen = &playlists[k];
    if (!chosen) return;
    void *games_sym = dc_lookup(kDcNetGames);
    if (!games_sym) return;
    const DcTable games = read_table(games_sym);
    if (!games.records) return;
    auto game_type = [&](int i) { return get<int32_t>(games.records + 0x90 * i + 0x0c); };
    const int32_t type = chosen->first_type();

    const int16_t current = get<int16_t>(abs_at(kNgm + 0x1ca0));
    if (current >= 0 && current < games.count && game_type(current) == type && dlc_net_game_available(current)) {
        applied = id;
        return;
    }
    for (int i = 0; i < games.count; ++i) {
        if (game_type(i) != type || !dlc_net_game_available(i)) continue;
        // The index travels as a sign-extended s16 in esi.
        reinterpret_cast<void(U2MP_GUEST *)(void *, int32_t, const void *, int32_t)>(abs_at(kSetNetGame))(
            abs_at(kNgm), int32_t(int16_t(i)), nullptr, 10);
        applied = id;
        log("menus: playlist %08x -> net game %d", id, i);
        return;
    }
}

// cond 55: the lobby's PLAYLIST control, fed with our playlist records.
U2MP_GUEST void query_playlist(uint8_t *q) {
    build_playlists();
    if (get<int16_t>(abs_at(kNgm + 0x1ca0)) < 0) applied = 0;   // left Multiplayer: apply again next time
    call(original_playlist, q);

    if (op_of(q) == 3 && get<uint32_t>(q + 8) == 0x25) {
        const uint32_t row = get<uint32_t>(q + 4);
        if (row < uint32_t(built)) {
            // An image from the net image table, looked up by id.
            put<uint32_t>(q + 0x08, 3);
            put<uint64_t>(q + 0x10, 0);
            put<uint32_t>(q + 0x18, 0);
            put<uint32_t>(q + 0x1c, playlists[row].picture());
            put<uint32_t>(q + 0x20, 1);
            put<int32_t>(q + 0x24, -1);
            put<uint32_t>(q + 0x28, 0);
            put<int32_t>(q + 0x2c, -1);
            put<float>(q + 0x30, 1.0f);
        }
        return;
    }
    if (op_of(q) != 4 || built == 0) return;
    if (get<int32_t>(q + 4) < 0) {   // the saved choice is not one of ours: take the first playlist
        put<uint32_t>(abs_at(kNpd + 0x104), playlists[0].id());
        put<int32_t>(q + 4, 0);
    }
    apply_playlist();
}

// ---- players met (cond 39) ----------------------------------------------------------------------------

std::vector<std::string> met;
int32_t met_cur = 0;

U2MP_GUEST void query_players_met(uint8_t *q) {
    if (op_of(q) == 8 || met.empty()) met = progression_recent_players(50);
    plain_list(q, int32_t(met.size()), met_cur, [](int32_t r) { return met[size_t(r)].c_str(); });
}

// ---- voice volume (cond 104) --------------------------------------------------------------------------

U2MP_GUEST void query_volume(uint8_t *q) {
    const uint32_t op = op_of(q);
    const bool in_mp = get<int32_t>(abs_at(kNetSm)) > 1;
    if (op == 4 && in_mp) {
        // Slider steps 0..20: the scaled volume truncated to s32 the way x86 does it (a value out of
        // s32 range or NaN becomes INT32_MIN), then clamped; so huge volumes and +inf give 0.
        const float v = voice::volume() * 20.0f + 0.5f;
        int32_t step = (v >= -2147483648.0f && v < 2147483648.0f) ? int32_t(v) : INT32_MIN;
        if (step < 0) step = 0;
        if (step > 20) step = 20;
        put<int32_t>(q + 4, step);
    } else if (op == 9 && in_mp) {
        voice::set_volume(float(get<int32_t>(q + 4)) * 0.05f);
    } else {
        call(original_volume, q);
    }
}

// ---- friends (cond 36) and join list (cond 3): stripped queries ------------------------------------------

char title_text[96];
char member_text[16][96];
int32_t friends_cur = 0;
int32_t join_cur = 0;

U2MP_GUEST void query_friends(uint8_t *q) {
    if (online::active()) {
        const int n = online::friend_count();
        if (op_of(q) == 1) {
            int on = 0;
            for (int i = 0; i < n; ++i)
                if (online::friend_online(i)) ++on;
            std::snprintf(title_text, sizeof title_text, "Friends (%d of %d online)", on, n);
            set_text(q, title_text);
            return;
        }
        plain_list(q, n == 0 ? 2 : 1 + n, friends_cur, [n](int32_t r) -> const char * {
            if (r == 0) return "Browse Online Games";
            if (n == 0) return "No RPCN friends yet. Add friends in the launcher.";
            return online::friend_row(r - 1);
        });
        return;
    }
    // LAN: the list shows the party.
    const int count = session::member_count();
    if (op_of(q) == 1) {
        count_text(title_text, kTextPartyCount, count);
        set_text(q, title_text);
        return;
    }
    plain_list(q, count, friends_cur, [](int32_t r) -> const char * {
        if (r >= 16) return kNothing;
        uint32_t id = 0;
        std::string name;
        if (!session::member_at(r, id, name)) return kNothing;
        if (id == 1) name += std::string(" - ") + game_text(kTextPartyLeader);
        fill96(member_text[r], name);
        return member_text[r];
    });
}

U2MP_GUEST void query_join_list(uint8_t *q) {
    const int rows = online::join_rows();
    if (rows >= 0) {
        if (op_of(q) == 1)
            set_text(q, online::join_title());
        else
            plain_list(q, rows, join_cur, [](int32_t r) { return online::join_row(r); });
        return;
    }
    if (op_of(q) == 1)
        set_text(q, kNothing);
    else
        plain_list(q, 0, join_cur, [](int32_t) { return kNothing; });
}

// ---- diagnostic trace of the Profile page queries ---------------------------------------------------------

constexpr uint32_t kTracedConds[] = {105, 77, 78, 134, 40, 41, 80, 82, 135, 108, 109, 110};
constexpr int kTraced = int(sizeof kTracedConds / sizeof kTracedConds[0]);
uint64_t traced_original[kTraced];
std::atomic<int> traced_calls[kTraced];

template <int N> U2MP_GUEST void tracer(uint8_t *q) {
    const uint32_t op = get<uint32_t>(q), row = get<uint32_t>(q + 4);
    call(traced_original[N], q);
    if (traced_calls[N].fetch_add(1) < 12)
        log("profile query cond %u: op %u row %u -> +4 %d +8 %u +0x40 %p +0x54 %u", kTracedConds[N], op, row,
            get<int32_t>(q + 4), get<uint32_t>(q + 8), reinterpret_cast<void *>(get<uint64_t>(q + 0x40)),
            get<uint32_t>(q + 0x54));
}

template <size_t... N> constexpr auto make_tracers(std::index_sequence<N...>) {
    return std::array<QueryFn, sizeof...(N)>{&tracer<int(N)>...};
}

// ---- install ----------------------------------------------------------------------------------------

uint8_t *entry(int i) { return abs_at(kQueryTable + kEntrySize * uint64_t(i)); }

// The first entry of `cond` before the table's end marker (fn 0), or NULL.
uint8_t *find_entry(uint32_t cond) {
    for (int i = 0; i < kQueryEntries; ++i) {
        uint8_t *e = entry(i);
        if (get<uint64_t>(e + 8) == 0) return nullptr;
        if (get<uint32_t>(e) == cond) return e;
    }
    return nullptr;
}

}  // namespace

int32_t playlist_pick_type() {
    const uint32_t id = get<uint32_t>(abs_at(kNpd + 0x104));
    for (int k = 0; k < built; ++k) {
        const Playlist &p = playlists[k];
        if (p.id() != id || p.type_count() <= 0) continue;
        const unsigned r = unsigned(std::rand());
        return p.type(int(r % unsigned(p.type_count())));
    }
    return -1;
}

bool playlist_game_types(std::vector<int32_t> &types) {
    types.clear();
    const uint32_t id = get<uint32_t>(abs_at(kNpd + 0x104));
    for (int k = 0; k < built; ++k) {
        const Playlist &p = playlists[k];
        if (p.id() != id) continue;
        for (int i = 0; i < p.type_count(); ++i) types.push_back(p.type(i));
    }
    return !types.empty();
}

void install_menus(int &installed) {
    if (kQueryTable + kQueryEntries * kEntrySize > image.size || get<uint32_t>(entry(0)) != 50 ||
        get<uint64_t>(entry(0) + 8) != reinterpret_cast<uint64_t>(abs_at(kStartMatchQuery))) {
        log("menus: query table 0x%llx differs, menu queries not installed", (unsigned long long)kQueryTable);
        return;
    }
    if (!image.executable(kTextFind, 16) || !image.executable(kSetNetGame, 16) || !image.executable(kDcLookup, 16)) {
        log("menus: guest helpers not where expected, menu queries not installed");
        return;
    }

    struct Wrap {
        uint32_t cond;
        uint64_t expected;
        const char *name;
        QueryFn host;
        uint64_t *original;
    };
    const Wrap wraps[] = {
        {133, 0x65e890, "profile panel", query_profile, &original_profile},
        {49, 0x659a90, "party status line", query_status, &original_status},
        {55, 0x6562d0, "PLAYLIST", query_playlist, &original_playlist},
        {39, 0x65bcd0, "LIST_PLAYERSMET", query_players_met, &original_met},
        {104, 0x65f3f0, "voice volume slider", query_volume, &original_volume},
    };
    for (const Wrap &w : wraps) {
        uint8_t *e = find_entry(w.cond);
        if (!e || get<uint64_t>(e + 8) != reinterpret_cast<uint64_t>(abs_at(w.expected)) ||
            !image.executable(w.expected, 1)) {
            log("%s (cond %u): table entry is not 0x%llx, left alone", w.name, w.cond,
                (unsigned long long)w.expected);
            continue;
        }
        *w.original = get<uint64_t>(e + 8);
        put<uint64_t>(e + 8, reinterpret_cast<uint64_t>(w.host));
        log("%s (cond %u): 0x%llx wrapped", w.name, w.cond, (unsigned long long)w.expected);
        installed += 1;
    }

    // Trace: every entry of the table counts here, the end marker does not stop the search.
    static constexpr auto tracers = make_tracers(std::make_index_sequence<kTraced>());
    for (int n = 0; n < kTraced; ++n)
        for (int i = 0; i < kQueryEntries; ++i) {
            uint8_t *e = entry(i);
            if (get<uint64_t>(e + 8) == 0 || get<uint32_t>(e) != kTracedConds[n]) continue;
            traced_original[n] = get<uint64_t>(e + 8);
            put<uint64_t>(e + 8, reinterpret_cast<uint64_t>(tracers[size_t(n)]));
            break;
        }

    struct Stripped {
        uint32_t cond;
        uint64_t address;
        const char *name;
        QueryFn host;
    };
    const Stripped stripped[] = {
        {36, 0x65bbb0, "LIST_FRIENDS query", query_friends},
        {3, 0x659590, "join game list query", query_join_list},
    };
    for (const Stripped &s : stripped) {
        uint8_t *e = find_entry(s.cond);
        if (!e || get<uint64_t>(e + 8) != reinterpret_cast<uint64_t>(abs_at(s.address)) || !is_ret_stub(s.address)) {
            log("%s at 0x%llx: not the stripped query this port expects, left alone", s.name,
                (unsigned long long)s.address);
            continue;
        }
        if (replace_function(s.address, 5, reinterpret_cast<void *>(s.host), s.name)) installed += 1;
    }
}

}  // namespace u2mp
