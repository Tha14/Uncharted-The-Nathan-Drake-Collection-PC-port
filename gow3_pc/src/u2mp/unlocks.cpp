// u2mp unlocks: multiplayer items count as unlocked, co-op weapon upgrades count as owned, the GOTY
// data tables (skins, boosters, store skins and five more) from the player's imported net-patch9 are
// bound over the PS4 net.bin stubs, the equipped skins are checked before the player data message is
// built, and the skin picks / spawn model lookups are logged.
//
// Reimplemented from the behaviour description u2mp/docs/spec/unlocks.md.

#include "unlocks.h"
#include "u2mp_internal.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

extern "C" int runtime_file_translate(const char *guest, char *out, size_t size);

namespace u2mp {
namespace {

// ---------------------------------------------------------------------------------------------
// Game facts (image offsets of Uncharted 2 PS4 1.02)

constexpr uint64_t kFnConditionMet = 0xa4bcb0;
constexpr uint64_t kFnRelocate = 0xa19a00;
constexpr uint64_t kFnBindSymbol = 0xa16d70;
constexpr uint64_t kFnDcFind = 0xa15300;
constexpr uint64_t kFnArtLookup = 0x56a980;
constexpr uint64_t kFnPlayerData = 0x6144b0;
constexpr uint64_t kFnRandomSkin = 0x64b430;

constexpr uint64_t kSymbolBuckets = 0x15340d0;
constexpr uint64_t kDebugUnlockAll = 0x1114178;
constexpr uint64_t kNetState = 0x14ce97c;
constexpr uint64_t kPlayerData = 0x11474f8;
constexpr uint64_t kBoosterList = 0x113a788;
constexpr uint64_t kArtNames = 0x14705f0;
constexpr uint64_t kArtNameCount = 0x14775f0;

constexpr uint32_t kStoreSym = 0x92c6db02;
constexpr uint32_t kSkinSym = 0x5a4731d2;
constexpr uint32_t kBoosterSym = sid("*net-boosters*");
static_assert(kBoosterSym == 0x916e03b0u, "booster table id");
constexpr uint32_t kCharAttrSym = sid("*net-skins*");
static_assert(kCharAttrSym == 0x42d1ec02u, "character attributes id");
constexpr uint32_t kHeroSection = 0x681022f5;
constexpr uint32_t kVillainSection = 0x4252add5;
constexpr uint32_t kRootType = 0x916aa605;

constexpr uint32_t kDrake = sid("mp-hero");
static_assert(kDrake == 0xb095d270u, "default Drake id");
constexpr uint32_t kBaseballDrake = sid("mp-hero-red");
static_assert(kBaseballDrake == 0x76474d48u, "Baseball Shirt Drake id");
constexpr uint32_t kNoBoosterA = 0x05454b23;
constexpr uint32_t kNoBoosterB = 0x01845694;

const char kPatchPath[] = "/app0/u2data/build/main/dc1/ps4/net-patch9.bin";

// Skins the hero / villain lists could not show otherwise (spec 9.1). The order fixes the positions
// of appended records and so the fingerprint every PC of a match must agree on.
struct ExtraSkin {
    const char *package;
    uint32_t id, name, photo, icon;
    bool villain;
};
constexpr ExtraSkin kExtraSkins[7] = {
    {"mp-hero-lazarevic", sid("mp-hero-lazarevic-fat"), 0xe2ce50df, sid("general/net/player-photos/lazarevic"),
     sid("general/net/player-icons/lazarevic"), true},
    {"mp-hero-pirate", sid("mp-hero-pirate-nd"), 0xc0f12cfd, sid("general/net/player-photos/pirate"),
     sid("general/net/player-icons/pirate"), true},
    {"costume-hero-lazarevic-healed", sid("costume-hero-lazarevic-healed"), 0x899e21dd,
     sid("general/net/player-photos/lazarevic"), sid("general/net/player-icons/lazarevic"), true},
    {"hero-dirty-beatup", sid("hero-dirty-beatup"), 0x1d8dbe6a, 0x05a8ba5f, 0x14c66775, false},
    {"costume-hero-skeleton-sir-francis", sid("costume-hero-skeleton-sir-francis"), 0xba9eb65d, 0x200eed64,
     0x3160304e, false},
    {"mp-hero-nd", sid("mp-hero-nd"), 0x9b7568c5, sid("general/net/player-photos/hero-nd"),
     sid("general/net/player-icons/hero-nd"), false},
    {"mp-hero-frosty", sid("mp-hero-frosty"), 0xaacd3194, 0x01bf230e, 0xb58cceaa, false},
};
static_assert(kExtraSkins[0].id == 0x345329d1u && kExtraSkins[0].photo == 0xeabed4cfu &&
                  kExtraSkins[0].icon == 0xffa8b107u, "campaign skin 1");
static_assert(kExtraSkins[1].id == 0x5a2e4839u && kExtraSkins[1].photo == 0x9f8d3fe9u &&
                  kExtraSkins[1].icon == 0xb980d32eu, "campaign skin 2");
static_assert(kExtraSkins[2].id == 0x1c14ad7bu, "campaign skin 3");
static_assert(kExtraSkins[3].id == 0x1b3af509u, "campaign skin 4");
static_assert(kExtraSkins[4].id == 0x7a66b9bbu, "campaign skin 5");
static_assert(kExtraSkins[5].id == 0x258034ffu && kExtraSkins[5].photo == 0xaa7dcc3au &&
                  kExtraSkins[5].icon == 0x2534f068u, "campaign skin 6");
static_assert(kExtraSkins[6].id == 0xf0d8079au, "campaign skin 7");

// Baseball Shirt Drake record values (spec 9.2).
constexpr uint32_t kBaseballName = 0x9a88ecdc;
constexpr uint32_t kBaseballPhoto = sid("general/net/player-photos/hero-red");
constexpr uint32_t kBaseballIcon = sid("general/net/player-icons/hero-red");
static_assert(kBaseballPhoto == 0xdafcd51bu && kBaseballIcon == 0xc283da48u, "Baseball Shirt pictures");

// The five other net.bin symbols, in the order they are built, checked and bound.
enum Extra { kNpcInfo, kCharAttr, kSpeakers, kMaterials, kSettings, kExtraCount };
constexpr uint32_t kExtraSym[kExtraCount] = {0xa977dad3, kCharAttrSym, 0xbdf0cfd6, 0xef68c757, 0x1ad223b4};
const char *const kExtraLabel[kExtraCount] = {"net-npc-info", "character attributes", "speakers",
                                               "material parameter set", "default game settings"};

// ---------------------------------------------------------------------------------------------
// Process-wide state (never reset)

bool g_spoof = true;
bool g_extras_on = true;
bool g_ready = false;
bool g_given_up = false;
std::atomic<bool> g_bound{false};
std::mutex g_mutex;

uint8_t *g_patch = nullptr;  // our relocated net-patch9 copy (host heap, kept forever)
uint64_t g_patch_size = 0;
uint8_t *g_skins = nullptr;  // merged skin table
int32_t g_skin_count = 0;
uint8_t *g_store = nullptr;  // store copy with the added skin items
uint8_t *g_boosters = nullptr;  // net-patch9's own booster table
int32_t g_booster_count = 0;
uint8_t *g_extra[kExtraCount] = {};
uint8_t *g_skin_stub = nullptr;
uint8_t *g_extra_stub[kExtraCount] = {};

std::atomic<uint32_t> g_fingerprint{0};

struct Label {
    uint32_t id;
    std::string text;
};
std::vector<Label> g_labels;  // filled once, then only read after g_labels_ready
std::atomic<bool> g_labels_ready{false};

std::atomic<int> g_islocked_note{1};
std::atomic<int> g_skin_log_budget{32};
std::atomic<int> g_spawn_log_budget{32};
int g_bind_budget = 4;
int g_store_rebuilds = 4;
std::atomic<bool> g_choice_at_bind{false};

bool g_skin_hooks = false;
std::string g_forced_skin;

// ---------------------------------------------------------------------------------------------
// Memory access helpers (unaligned-safe)

uint8_t *game(uint64_t offset) { return image.base + offset; }

uint32_t rd32(const void *p, size_t off = 0) {
    uint32_t v;
    std::memcpy(&v, static_cast<const uint8_t *>(p) + off, sizeof v);
    return v;
}
int32_t rds32(const void *p, size_t off = 0) { return int32_t(rd32(p, off)); }
uint64_t rd64(const void *p, size_t off = 0) {
    uint64_t v;
    std::memcpy(&v, static_cast<const uint8_t *>(p) + off, sizeof v);
    return v;
}
uint8_t *rdptr(const void *p, size_t off = 0) { return reinterpret_cast<uint8_t *>(uintptr_t(rd64(p, off))); }
void wr32(void *p, size_t off, uint32_t v) { std::memcpy(static_cast<uint8_t *>(p) + off, &v, sizeof v); }
void wr64(void *p, size_t off, uint64_t v) { std::memcpy(static_cast<uint8_t *>(p) + off, &v, sizeof v); }
void wrptr(void *p, size_t off, const void *v) { wr64(p, off, uint64_t(uintptr_t(v))); }

// ---------------------------------------------------------------------------------------------
// Guest calls

using DcFindFn = void *(U2MP_GUEST *)(uint32_t);
using BindSymbolFn = void(U2MP_GUEST *)(uint32_t, const uint64_t *);
using RelocateFn = void *(U2MP_GUEST *)(void *);
using ConditionFn = uint8_t(U2MP_GUEST *)(void *, const uint8_t *, int32_t *);
using ArtLookupFn = void *(U2MP_GUEST *)(void *, const char *);
using PlayerDataFn = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
using RandomSkinFn = uint64_t(U2MP_GUEST *)(void *, int32_t);

template <typename Fn> Fn guest(uint64_t offset) { return reinterpret_cast<Fn>(game(offset)); }

// The value of a script data symbol. Before the symbol table exists the game's finder must not run.
uint8_t *dc_find(uint32_t symbol) {
    if (rd64(game(kSymbolBuckets)) == 0) return nullptr;
    void *slot = guest<DcFindFn>(kFnDcFind)(symbol);
    return slot ? rdptr(slot) : nullptr;
}

void bind_symbol(uint32_t symbol, uint8_t *table) {
    const uint64_t value = uint64_t(uintptr_t(table));
    guest<BindSymbolFn>(kFnBindSymbol)(symbol, &value);
}

// ---------------------------------------------------------------------------------------------
// Checks on our net-patch9 copy

bool in_copy(const void *p, uint64_t n = 1) {
    if (!g_patch) return false;
    const uintptr_t a = uintptr_t(p), lo = uintptr_t(g_patch), hi = lo + g_patch_size;
    return a >= lo && a + n <= hi;
}

// A data symbol of the net-patch9 root object, or NULL.
uint8_t *patch_symbol(uint32_t symbol) {
    const uint8_t *root = g_patch + 0x20;
    const uint32_t count = rd32(root, 4);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *entry = root + 8 + 16 * uint64_t(i);
        if (!in_copy(entry, 16)) break;
        if (rd32(entry) != symbol) continue;
        uint8_t *value = rdptr(entry, 8);
        return in_copy(value, 16) ? value : nullptr;
    }
    return nullptr;
}

// An actor package name the game can use as a skin: "mp-..." and NUL-terminated within 64 bytes.
// `ours` also keeps every byte read inside our copy.
bool package_name_ok(const char *name, bool ours) {
    if (!name) return false;
    if (ours && !in_copy(name, 4)) return false;
    if (std::strncmp(name, "mp-", 3) != 0) return false;
    for (int i = 0; i < 64; ++i) {
        if (name[i] == 0) return true;
        if (ours && !in_copy(name + i + 1)) return false;
    }
    return false;
}

bool skin_records_ok(const uint8_t *recs, int32_t n, uint64_t stride, bool ps4, bool ours) {
    if (!recs || n <= 0 || n > 512) return false;
    if (ours && !in_copy(recs, stride * uint64_t(n))) return false;
    for (int32_t i = 0; i < n; ++i) {
        const uint8_t *rec = recs + stride * uint64_t(i);
        if (ps4 && rd32(rec, 0x14) != 0) return false;
        if (!package_name_ok(reinterpret_cast<const char *>(rdptr(rec, 8)), ours)) return false;
    }
    return true;
}

// A non-empty string of at most 255 characters lying wholly in our copy.
bool patch_string_ok(const uint8_t *p) {
    for (int i = 0; i < 256; ++i) {
        if (!in_copy(p + i)) return false;
        if (p[i] == 0) return i > 0;
    }
    return false;
}

struct Records {
    uint8_t *recs = nullptr;
    int32_t n = 0;
};

// A {count, records} table of net-patch9 whose records all lie in our copy.
bool patch_table(uint32_t symbol, uint64_t stride, const char *what, Records &out) {
    uint8_t *t = patch_symbol(symbol);
    const int32_t n = t ? rds32(t) : 0;
    uint8_t *recs = t ? rdptr(t, 8) : nullptr;
    if (!t || n <= 0 || n > 1024 || !in_copy(recs, stride * uint64_t(n))) {
        log("unlocks: net-patch9 %s 0x%08x missing or out of range (%d records); PS4 stub kept", what, symbol, n);
        return false;
    }
    out.recs = recs;
    out.n = n;
    return true;
}

// A zeroed {count, pad, records} table on the net heap.
uint8_t *new_table(int32_t count, uint64_t stride) {
    const size_t bytes = 0x10 + stride * uint64_t(count);
    auto *t = static_cast<uint8_t *>(netheap_alloc(bytes, 16));
    if (!t) return nullptr;
    std::memset(t, 0, bytes);
    wr32(t, 0, uint32_t(count));
    wrptr(t, 8, t + 0x10);
    return t;
}

// A copy of a C string on the net heap, or NULL.
char *heap_string(const char *s) {
    const size_t len = std::strlen(s);
    auto *copy = static_cast<char *>(netheap_alloc(len + 1, 16));
    if (copy) std::memcpy(copy, s, len + 1);
    return copy;
}

// ---------------------------------------------------------------------------------------------
// Skin helpers

// A readable name for a skin without a text: its package name in words ("mp-hero-u1-merc" -> "U1 Merc").
std::string package_label(const char *name) {
    if (std::strncmp(name, "mp-hero-", 8) == 0) name += 8;
    std::string text;
    bool word_start = true;
    for (; *name; ++name) {
        const char c = *name;
        if (c == '-' || c == '_') {
            if (!text.empty() && text.back() != ' ') text += ' ';
            word_start = true;
            continue;
        }
        text += (word_start && c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
        word_start = false;
    }
    return text;
}

// Where the game would load a skin's actor package from (tells a missing import apart from the PS4 game).
const char *actor_source(const char *name) {
    const std::string guest_path = std::string("/app0/u2data/build/main/actor27/ps4/") + name + ".pak";
    char host[1024];
    if (runtime_file_translate(guest_path.c_str(), host, sizeof host) != 0) return "no file";
    FILE *f = std::fopen(host, "rb");
    if (!f) return "no file";
    std::fclose(f);
    return std::strstr(host, "CUSA02320") ? "PS4 game" : "import overlay";
}

// Position of a skin id in the bound skin table (-1 when absent) and its package name.
std::pair<int32_t, const char *> skin_index(uint32_t id) {
    const uint8_t *t = dc_find(kSkinSym);
    if (!t) return {-1, nullptr};
    const uint8_t *recs = rdptr(t, 8);
    for (int32_t i = 0; recs && i < rds32(t) && i < 512; ++i) {
        const uint8_t *rec = recs + 0x30 * uint64_t(i);
        if (rd32(rec, 0x10) == id) return {i, reinterpret_cast<const char *>(rdptr(rec, 8))};
    }
    return {-1, nullptr};
}

// The id of the bound skin record with this package name, 0 when none.
uint32_t skin_id_of(const char *name) {
    const uint8_t *t = dc_find(kSkinSym);
    if (!t || !name || !*name) return 0;
    const uint8_t *recs = rdptr(t, 8);
    for (int32_t i = 0; recs && i < rds32(t) && i < 512; ++i) {
        const uint8_t *rec = recs + 0x30 * uint64_t(i);
        const char *pkg = reinterpret_cast<const char *>(rdptr(rec, 8));
        if (pkg && std::strcmp(pkg, name) == 0) return rd32(rec, 0x10);
    }
    return 0;
}

// One line of the shared "skins:" budget, so a long match cannot flood the log.
bool skins_log_slot() {
    if (g_skin_log_budget.load() <= 0) return false;
    g_skin_log_budget.fetch_sub(1);
    return true;
}

const char *or_dash(const char *s) { return s ? s : "-"; }

// Makes sure both equipped skins exist in the bound table (a skin id the table lacks would be sent by
// a position the other PCs read differently), or forces the skin the tester asked for.
void apply_skin_choice(const char *when) {
    if (!g_skin_hooks) return;
    if (!dc_find(kSkinSym)) return;
    uint32_t forced = 0;
    if (!g_forced_skin.empty()) {
        forced = skin_id_of(g_forced_skin.c_str());
        if (forced == 0 && skins_log_slot())
            log("skins: UNCHARTED_U2MP_SKIN=%s is not a package of the skin table; not forced", g_forced_skin.c_str());
    }
    if (skins_log_slot()) log("skins: choice applied (%s)", when);

    static const struct {
        const char *side;
        uint64_t field;
    } kSides[2] = {{"hero", 0x10c}, {"villain", 0x110}};
    for (const auto &s : kSides) {
        uint8_t *field = game(kPlayerData + s.field);
        const uint32_t before = rd32(field);
        const auto [at, pk] = skin_index(before);
        const char *reason = "kept";
        if (forced != 0) {
            wr32(field, 0, forced);
            reason = "UNCHARTED_U2MP_SKIN";
        } else if (at < 0) {
            wr32(field, 0, kDrake);
            reason = "not in the table: Drake";
        }
        if (skins_log_slot()) {
            const uint32_t now_id = rd32(field);
            const auto [now_at, now] = skin_index(now_id);
            log("skins: %s id %08x (index %d '%s') -> %08x (index %d '%s'): %s", s.side, before, at, or_dash(pk), now_id,
                now_at, or_dash(now), reason);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Hooks the game reaches

// True when `item` is one of the records of the multiplayer store table.
bool is_store_item(const uint8_t *item) {
    const uint8_t *store = dc_find(kStoreSym);
    if (!store) return false;
    const int32_t sections = rds32(store);
    const uint8_t *array = rdptr(store, 8);
    if (sections <= 0 || sections > 32 || !array) return false;
    for (int32_t i = 0; i < sections; ++i) {
        const uint8_t *section = array + 0x18 * uint64_t(i);
        const int32_t n = rds32(section, 8);
        const uint8_t *items = rdptr(section, 0x10);
        if (!items || n <= 0 || n > 1024) continue;
        if (item >= items && item < items + 0x50 * uint64_t(n) && (item - items) % 0x50 == 0) return true;
    }
    return false;
}

U2MP_GUEST bool is_locked(void *ctx, const uint8_t *item, int32_t *reason) {
    if (!item) return false;
    if (reason) *reason = 0;
    if (!g_bound.load(std::memory_order_acquire)) unlocks_refresh();
    if (g_spoof && is_store_item(item)) {
        if (g_islocked_note.load() > 0) {
            g_islocked_note.fetch_sub(1);
            log("unlocks: multiplayer items reported unlocked (IsLocked 0xa4bb40)");
        }
        return false;
    }
    const uint8_t *list = rdptr(item, 0x18);
    if (!list) return false;

    auto condition_met = guest<ConditionFn>(kFnConditionMet);
    bool locked = false;
    if (*game(kDebugUnlockAll) == 0) {
        // "any" lists unlock on the first met condition; "all" lists lock on the first unmet one.
        locked = rds32(list, 0x10) != 0 && rds32(list) > 0;
        for (int32_t i = 0; i < rds32(list); ++i) {
            const uint8_t met = condition_met(ctx, rdptr(list, 8) + 0x18 * uint64_t(i), nullptr);
            list = rdptr(item, 0x18);  // a condition may change the item's list
            const bool any = rds32(list, 0x10) != 0;
            if (met && any) {
                locked = false;
                break;
            }
            if (!met && !any) {
                locked = true;
                break;
            }
        }
    }
    if (reason) {
        // The reason is the out value of the first met condition (also with the debug switch on).
        for (int32_t i = 0; i < rds32(list); ++i) {
            int32_t out = 0;
            if (condition_met(ctx, rdptr(list, 8) + 0x18 * uint64_t(i), &out)) {
                *reason = out;
                break;
            }
            list = rdptr(item, 0x18);
        }
    }
    return locked;
}

U2MP_GUEST int32_t coop_upgrade_level(void *, uint8_t *weapon) {
    if (!weapon || rds32(game(kNetState)) < 2) return 0;
    const int32_t cached = rds32(weapon, 0x20);
    if (cached >= 0) return cached;

    const uint8_t *store = dc_find(kStoreSym);
    int32_t best = 0;
    if (store && rds32(store) > 0) {
        const bool all = g_spoof || *game(kDebugUnlockAll) != 0;
        const uint32_t weapon_id = rd32(weapon);
        const uint8_t *player = game(kPlayerData);
        for (int32_t s = 0; rdptr(store, 8) && s < rds32(store); ++s) {
            const uint8_t *section = rdptr(store, 8) + 0x18 * uint64_t(s);
            for (int32_t k = 0; rdptr(section, 0x10) && k < rds32(section, 8); ++k) {
                const uint8_t *item = rdptr(section, 0x10) + 0x50 * uint64_t(k);
                if (rd32(item, 4) != 12 || rd32(item, 0x2c) != weapon_id) continue;
                const uint32_t bit = rd32(item);
                const bool owned = all || (bit < 288 && ((rd32(player, 0xe0 + 4 * (bit / 32)) >> (bit % 32)) & 1));
                if (owned && rds32(item, 0x34) > best) best = rds32(item, 0x34);
            }
        }
    }
    wr32(weapon, 0x20, uint32_t(best));
    return best;
}

U2MP_GUEST uint64_t player_data_message(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    apply_skin_choice("player data message 0x6144b0");
    return guest<PlayerDataFn>(kFnPlayerData)(a0, a1, a2, a3, a4, a5);
}

U2MP_GUEST uint64_t random_skin_pick(void *list, int32_t side) {
    const uint64_t r = guest<RandomSkinFn>(kFnRandomSkin)(list, side);
    if (skins_log_slot()) {
        const int32_t i = int32_t(uint32_t(r));
        const uint8_t *t = dc_find(kSkinSym);
        const char *pk = nullptr;
        if (t && i >= 0 && i < rds32(t) && rdptr(t, 8))
            pk = reinterpret_cast<const char *>(rdptr(rdptr(t, 8) + 0x30 * uint64_t(i), 8));
        log("skins: random pick (0x64b430), side %d -> index %d '%s'", side, i, or_dash(pk));
    }
    return r;
}

// Logs what a spawn model lookup found and why a skin model may be missing.
void *spawn_lookup(const char *site, void *table, const char *name) {
    void *r = guest<ArtLookupFn>(kFnArtLookup)(table, name);
    if (g_spawn_log_budget.load() <= 0) return r;
    g_spawn_log_budget.fetch_sub(1);
    const uint32_t id = name ? sid(name) : 0;
    const uint8_t *pkg = id ? package_by_id(id) : nullptr;
    const int32_t art_count = rds32(game(kArtNameCount));
    int32_t entries = 0, alias = 0;
    for (int32_t i = 0; i < art_count && i < 512; ++i) {
        ++entries;
        if (rd32(game(kArtNames) + 0x38 * uint64_t(i), 0x20) == id) alias = i + 1;
    }
    log("skins: spawn %s: model '%s' -> %p; package %08x %s (state %d), actor file from %s; name table %d entries, %s",
        site, name ? name : "(null)", r, id, pkg ? "loaded" : "not loaded", pkg ? rds32(pkg, 0x30) : -1,
        name ? actor_source(name) : "-", entries, alias ? "name listed" : "name NOT listed");
    return r;
}

U2MP_GUEST void *spawn_lookup_a(void *table, const char *name) { return spawn_lookup("0x663280", table, name); }
U2MP_GUEST void *spawn_lookup_b(void *table, const char *name) { return spawn_lookup("0x6636b0", table, name); }

// ---------------------------------------------------------------------------------------------
// net-patch9 loading and table building

void free_host(uint8_t *p) { ::operator delete(p, std::align_val_t(16)); }

bool load_patch() {
    char host[1024];
    if (runtime_file_translate(kPatchPath, host, sizeof host) != 0) return false;
    FILE *f = std::fopen(host, "rb");
    if (!f) return false;
    std::vector<uint8_t> file;
    uint8_t chunk[65536];
    for (size_t got; (got = std::fread(chunk, 1, sizeof chunk, f)) > 0;) file.insert(file.end(), chunk, chunk + got);
    std::fclose(f);
    const uint64_t size = file.size();

    if (size < 0x40 || std::memcmp(file.data(), "00CD", 4) != 0 || rd64(file.data(), 0x10) != 0x20 ||
        rd32(file.data(), 0x1c) != kRootType) {
        log("unlocks: %s is not a PS4 DC module", kPatchPath);
        return false;
    }
    const uint64_t reloc = rd64(file.data(), 8);
    if (reloc < 0x28 || reloc % 8 != 0 || reloc + 4 > size || reloc + 4 + rd32(file.data(), reloc) > size) {
        log("unlocks: %s has a bad relocation table", kPatchPath);
        return false;
    }
    auto *buffer = static_cast<uint8_t *>(::operator new(size, std::align_val_t(16), std::nothrow));
    if (!buffer) return false;
    std::memcpy(buffer, file.data(), size);
    void *root_slot = guest<RelocateFn>(kFnRelocate)(buffer);
    if (root_slot != buffer + 0x10 || rdptr(buffer, 0x10) != buffer + 0x20) {
        log("unlocks: relocating %s failed", kPatchPath);
        free_host(buffer);
        return false;
    }
    g_patch = buffer;
    g_patch_size = size;
    return true;
}

// Writes one appended skin record (Baseball Shirt Drake or a campaign skin).
void put_skin(uint8_t *rec, uint32_t index, bool villain, const char *package, uint32_t id, uint32_t name,
              uint32_t photo, uint32_t icon) {
    wr32(rec, 0, index);
    wr32(rec, 4, villain ? 1 : 0);
    wrptr(rec, 8, package);
    wr64(rec, 0x10, id);
    wr32(rec, 0x18, name);
    wr32(rec, 0x1c, photo);
    wr32(rec, 0x20, icon);
}

void build_skin_table() {
    const uint8_t *t = patch_symbol(kSkinSym);
    if (!t) {
        log("unlocks: net-patch9 has no skin table 0x%08x", kSkinSym);
        return;
    }
    const int32_t n = rds32(t);
    const uint8_t *recs = rdptr(t, 8);
    bool ps4 = skin_records_ok(recs, n, 0x30, true, true);
    if (!ps4 && !skin_records_ok(recs, n, 0x28, false, true)) {
        log("unlocks: net-patch9 skin table (%d records) has an unknown layout; PS4 skins kept", n);
        return;
    }
    int32_t stub_n = 0;
    const uint8_t *stub_recs = nullptr;
    if (g_skin_stub) {
        const int32_t c = rds32(g_skin_stub);
        if (c >= 1 && c <= 8 && skin_records_ok(rdptr(g_skin_stub, 8), c, 0x30, true, false)) {
            stub_n = c;
            stub_recs = rdptr(g_skin_stub, 8);
        }
    }
    const uint64_t capacity = uint64_t(n) + stub_n + 1 + 7;
    const size_t bytes = 0x10 + 0x30 * capacity;
    auto *block = static_cast<uint8_t *>(netheap_alloc(bytes, 16));
    if (!block) return;
    std::memset(block, 0, bytes);
    uint8_t *out = block + 0x10;
    auto rec_at = [&](int32_t i) { return out + 0x30 * uint64_t(i); };

    for (int32_t i = 0; i < n; ++i) {
        if (ps4) {
            std::memcpy(rec_at(i), recs + 0x30 * uint64_t(i), 0x30);
        } else {
            const uint8_t *src = recs + 0x28 * uint64_t(i);
            uint8_t *dst = rec_at(i);
            std::memcpy(dst, src, 0x10);         // index, side, package ptr
            std::memcpy(dst + 0x10, src + 0x10, 4);  // id (high half stays 0)
            std::memcpy(dst + 0x18, src + 0x14, 0x14);  // name, photo, icon and the two values after
        }
    }
    int32_t count = n, replaced = 0, appended = 0;
    for (int32_t s = 0; s < stub_n; ++s) {
        const uint8_t *src = stub_recs + 0x30 * uint64_t(s);
        const uint32_t id = rd32(src, 0x10);
        uint8_t *dst = nullptr;
        for (int32_t i = 0; i < n && !dst; ++i)
            if (rd32(rec_at(i), 0x10) == id) dst = rec_at(i);
        if (dst) {
            ++replaced;
        } else {
            dst = rec_at(count++);
            ++appended;
        }
        std::memcpy(dst, src, 0x30);
        if (char *copy = heap_string(reinterpret_cast<const char *>(rdptr(src, 8)))) wrptr(dst, 8, copy);
    }
    auto has_id = [&](uint32_t id) {
        for (int32_t i = 0; i < count; ++i)
            if (rd32(rec_at(i), 0x10) == id) return true;
        return false;
    };
    if (!has_id(kBaseballDrake)) {
        put_skin(rec_at(count++), 1, false, "mp-hero-red", kBaseballDrake, kBaseballName, kBaseballPhoto, kBaseballIcon);
        ++appended;
    }
    for (const ExtraSkin &e : kExtraSkins) {
        if (has_id(e.id)) continue;
        put_skin(rec_at(count), uint32_t(count), e.villain, e.package, e.id, e.name, e.photo, e.icon);
        ++count;
        ++appended;
    }
    wr32(block, 0, uint32_t(count));
    wrptr(block, 8, out);
    g_skins = block;
    g_skin_count = count;

    // The player data message sends skins by table position, so every PC must have the same order.
    uint32_t fp = 0;
    for (int32_t i = 0; i < count; ++i) {
        char text[16];
        std::snprintf(text, sizeof text, "%08x", rd32(rec_at(i), 0x10));
        fp = sid(text, fp);
    }
    log("unlocks: skin table order fingerprint %08x (%d records; every PC of a match must log the same)", fp, count);
    g_fingerprint.store(fp);

    for (int32_t i = 0; i < count; ++i) {
        const uint32_t name_id = rd32(rec_at(i), 0x18);
        if (name_id == 0) continue;
        bool known = false;
        for (const Label &l : g_labels) known = known || l.id == name_id;
        if (known) continue;
        const char *pkg = reinterpret_cast<const char *>(rdptr(rec_at(i), 8));
        g_labels.push_back({name_id, package_label(pkg ? pkg : "")});
    }

    int32_t from_game = 0, from_import = 0;
    std::string missing;
    for (int32_t i = 0; i < count; ++i) {
        const char *pkg = reinterpret_cast<const char *>(rdptr(rec_at(i), 8));
        const char *src = actor_source(pkg ? pkg : "");
        if (std::strcmp(src, "PS4 game") == 0) {
            ++from_game;
        } else if (std::strcmp(src, "import overlay") == 0) {
            ++from_import;
        } else if (missing.size() < 400 && missing.find(pkg ? pkg : "") == std::string::npos) {
            missing += ' ';
            missing += pkg ? pkg : "";
        }
    }
    log("unlocks: skin actors: %d records from the PS4 game, %d from the import overlay, missing:%s", from_game,
        from_import, missing.empty() ? " none" : missing.c_str());
    g_labels_ready.store(true, std::memory_order_release);
    log("unlocks: skin table: %d GOTY records (%s layout) + PS4 %d (%d replaced, %d appended) = %d", n,
        ps4 ? "PS4" : "0x28-byte converted", stub_n, replaced, appended, count);
}

// A copy of the game's store table whose hero / villain lists also offer the skins of kExtraSkins.
void build_store_copy() {
    const uint8_t *s = dc_find(kStoreSym);
    if (!s || s == g_store) return;
    const int32_t sections = rds32(s);
    const uint8_t *array = rdptr(s, 8);
    if (sections <= 0 || sections > 32 || !array) return;
    auto *block = static_cast<uint8_t *>(netheap_alloc(0x10 + 0x18 * uint64_t(sections), 16));
    if (!block) return;
    std::memset(block, 0, 0x10);
    std::memcpy(block + 0x10, array, 0x18 * uint64_t(sections));

    int32_t added = 0;
    for (int32_t j = 0; j < sections; ++j) {
        uint8_t *section = block + 0x10 + 0x18 * uint64_t(j);
        const uint32_t section_id = rd32(section);
        if (section_id != kHeroSection && section_id != kVillainSection) continue;
        const bool villain = section_id == kVillainSection;
        const int32_t n = rds32(section, 8);
        const uint8_t *items = rdptr(section, 0x10);
        if (n <= 0 || n > 200 || !items) continue;

        std::vector<const ExtraSkin *> wanted;
        for (const ExtraSkin &e : kExtraSkins) {
            if (e.villain != villain) continue;
            bool present = false;
            for (int32_t k = 0; k < n && !present; ++k) present = rd32(items + 0x50 * uint64_t(k), 8) == e.id;
            if (!present) wanted.push_back(&e);
        }
        if (wanted.empty()) continue;
        const int32_t total = n + int32_t(wanted.size());
        auto *list = static_cast<uint8_t *>(netheap_alloc(0x50 * uint64_t(total), 16));
        if (!list) continue;
        std::memcpy(list, items, 0x50 * uint64_t(n));
        for (size_t w = 0; w < wanted.size(); ++w) {
            // The new item borrows everything (price, rank, texts) from the list's last item.
            uint8_t *item = list + 0x50 * (uint64_t(n) + w);
            std::memcpy(item, items + 0x50 * uint64_t(n - 1), 0x50);
            wr32(item, 4, 10);
            wr32(item, 8, wanted[w]->id);
            wr64(item, 0x18, 0);
            wrptr(item, 0x40, wanted[w]->package);
            wr64(item, 0x48, wanted[w]->id);
        }
        wr32(section, 8, uint32_t(total));
        wrptr(section, 0x10, list);
        added += int32_t(wanted.size());
    }
    wr32(block, 0, uint32_t(sections));
    wrptr(block, 8, block + 0x10);
    g_store = block;
    log("unlocks: MP store copied with %d added skin item(s) (hero / villain lists)", added);
}

void build_booster_table() {
    uint8_t *t = patch_symbol(kBoosterSym);
    if (!t) {
        log("unlocks: net-patch9 has no booster table 0x%08x", kBoosterSym);
        return;
    }
    const int32_t n = rds32(t);
    const uint8_t *recs = rdptr(t, 8);
    if (n <= 0 || n > 63 || !in_copy(recs, 0x20 * uint64_t(n))) {
        log("unlocks: net-patch9 booster table (%d records) out of range; PS4 boosters kept", n);
        return;
    }
    bool none_a = false, none_b = false;
    for (int32_t i = 0; i < n; ++i) {
        const uint8_t *rec = recs + 0x20 * uint64_t(i);
        const uint32_t id = rd32(rec, 0xc), slot = rd32(rec, 0x10);
        if (id == 0 || slot > 1) {
            log("unlocks: net-patch9 booster %d (id %08x, slot %u) is not a booster record; PS4 boosters kept", i, id,
                slot);
            return;
        }
        none_a = none_a || id == kNoBoosterA;
        none_b = none_b || id == kNoBoosterB;
    }
    if (!none_a || !none_b) {
        log("unlocks: net-patch9 booster table lacks the two None boosters; PS4 boosters kept");
        return;
    }
    g_boosters = t;
    g_booster_count = n;
    log("unlocks: booster table: %d GOTY records", n);
}

// The PS4 stub of an extra table, if it has 1..8 records.
Records usable_stub(Extra which) {
    Records r;
    const uint8_t *stub = g_extra_stub[which];
    const int32_t n = stub ? rds32(stub) : 0;
    uint8_t *recs = stub ? rdptr(stub, 8) : nullptr;
    if (n >= 1 && n <= 8 && recs) {
        r.recs = recs;
        r.n = n;
    }
    return r;
}

uint8_t *build_npc_info() {
    Records p;
    if (!patch_table(kExtraSym[kNpcInfo], 12, kExtraLabel[kNpcInfo], p)) return nullptr;
    for (int32_t i = 0; i < p.n; ++i) {
        if (rd32(p.recs + 12 * uint64_t(i)) == 0) {
            log("unlocks: net-npc-info record %d has no archetype; PS4 stub kept", i);
            return nullptr;
        }
    }
    const Records stub = usable_stub(kNpcInfo);
    uint8_t *t = new_table(p.n + stub.n, 12);
    if (!t) return nullptr;
    uint8_t *out = t + 0x10;
    std::memcpy(out, p.recs, 12 * uint64_t(p.n));
    int32_t appended = 0;
    for (int32_t s = 0; s < stub.n; ++s) {
        const uint8_t *src = stub.recs + 12 * uint64_t(s);
        bool known = false;
        for (int32_t i = 0; i < p.n && !known; ++i) known = rd32(p.recs + 12 * uint64_t(i)) == rd32(src);
        if (known) continue;
        std::memcpy(out + 12 * uint64_t(p.n + appended), src, 12);
        ++appended;
    }
    wr32(t, 0, uint32_t(p.n + appended));
    log("unlocks: net-npc-info: %d GOTY records + %d PS4", p.n, appended);
    return t;
}

uint8_t *build_char_attributes() {
    Records p;
    if (!patch_table(kExtraSym[kCharAttr], 0x20, kExtraLabel[kCharAttr], p)) return nullptr;
    bool ps4_ok = true, converted_ok = true;
    for (int32_t i = 0; i < p.n; ++i) {
        const uint8_t *rec = p.recs + 0x20 * uint64_t(i);
        if (!patch_string_ok(rdptr(rec)) || rd32(rec, 8) == 0) ps4_ok = converted_ok = false;
        if (rd32(rec, 0x1c) != 0 || rd32(rec, 0x18) == 0) converted_ok = false;
        if (rd32(rec, 0xc) != 0 || rd32(rec, 0x1c) == 0) ps4_ok = false;
    }
    if (!ps4_ok && !converted_ok) {
        log("unlocks: net-patch9 character attributes (%d records): unknown layout; PS4 stub kept", p.n);
        return nullptr;
    }
    const Records stub = usable_stub(kCharAttr);
    uint8_t *t = new_table(p.n + stub.n, 0x20);
    if (!t) return nullptr;
    uint8_t *out = t + 0x10;
    for (int32_t i = 0; i < p.n; ++i) {
        const uint8_t *src = p.recs + 0x20 * uint64_t(i);
        uint8_t *dst = out + 0x20 * uint64_t(i);
        if (ps4_ok) {
            std::memcpy(dst, src, 0x20);
        } else {
            std::memcpy(dst, src, 0xc);            // name ptr and key
            std::memcpy(dst + 0x10, src + 0xc, 0x10);  // the four values, one slot later
        }
    }
    int32_t count = p.n, replaced = 0;
    for (int32_t s = 0; s < stub.n; ++s) {
        const uint8_t *src = stub.recs + 0x20 * uint64_t(s);
        const uint32_t key = rd32(src, 8);
        uint8_t *dst = nullptr;
        for (int32_t i = 0; i < p.n && !dst; ++i)
            if (rd32(out + 0x20 * uint64_t(i), 8) == key) dst = out + 0x20 * uint64_t(i);
        if (dst) {
            ++replaced;
        } else {
            dst = out + 0x20 * uint64_t(count++);
        }
        std::memcpy(dst, src, 0x20);
        if (const char *name = reinterpret_cast<const char *>(rdptr(src)))
            if (char *copy = heap_string(name)) wrptr(dst, 0, copy);
    }
    wr32(t, 0, uint32_t(count));
    log("unlocks: character attributes: %d GOTY records (%s layout), PS4 %d replaced, %d appended", p.n,
        ps4_ok ? "PS4" : "converted", replaced, count - p.n);
    return t;
}

uint8_t *build_speakers() {
    Records p;
    if (!patch_table(kExtraSym[kSpeakers], 0x10, kExtraLabel[kSpeakers], p)) return nullptr;
    for (int32_t i = 0; i < p.n; ++i) {
        const uint8_t *rec = p.recs + 0x10 * uint64_t(i);
        const uint8_t *value = rdptr(rec, 8);
        bool ok = rd32(rec, 4) == 0 && in_copy(value, 0x20) && (i == 0 || rd32(rec) > rd32(rec - 0x10));
        for (int k = 0; ok && k < 4; ++k) ok = in_copy(rdptr(value, 8 * k));
        if (!ok) {
            log("unlocks: net-patch9 speakers record %d is not a sorted {key, value} record; PS4 stub kept", i);
            return nullptr;
        }
    }
    uint8_t *t = new_table(p.n, 0x10);
    if (!t) return nullptr;
    std::memcpy(t + 0x10, p.recs, 0x10 * uint64_t(p.n));
    const Records stub = usable_stub(kSpeakers);
    int32_t stub_only = 0;
    for (int32_t s = 0; s < stub.n; ++s) {
        const uint32_t key = rd32(stub.recs + 0x10 * uint64_t(s));
        bool known = false;
        for (int32_t i = 0; i < p.n && !known; ++i) known = rd32(p.recs + 0x10 * uint64_t(i)) == key;
        if (!known) ++stub_only;
    }
    log("unlocks: speakers: %d GOTY records%s", p.n, stub_only ? " (a PS4 stub key is not among them)" : "");
    return t;
}

uint8_t *build_materials() {
    Records p;
    if (!patch_table(kExtraSym[kMaterials], 0x30, kExtraLabel[kMaterials], p)) return nullptr;
    for (int32_t i = 0; i < p.n; ++i) {
        const uint8_t *rec = p.recs + 0x30 * uint64_t(i);
        const uint32_t type = rd32(rec, 0x10);
        const uint64_t need = type == 3 ? 12 : 4;
        if (!patch_string_ok(rdptr(rec)) || !patch_string_ok(rdptr(rec, 8)) || (type != 1 && type != 3) ||
            !in_copy(rdptr(rec, 0x18), need) || !in_copy(rdptr(rec, 0x20), need) || rd64(rec, 0x28) != 0) {
            log("unlocks: net-patch9 material parameter record %d has an unknown layout; PS4 stub kept", i);
            return nullptr;
        }
    }
    // The records stay in our copy; only the header is new.
    auto *t = static_cast<uint8_t *>(netheap_alloc(0x10, 16));
    if (!t) return nullptr;
    wr32(t, 0, uint32_t(p.n));
    wr32(t, 4, 0);
    wrptr(t, 8, p.recs);
    log("unlocks: material parameter set: %d GOTY records", p.n);
    return t;
}

uint8_t *build_settings() {
    const uint8_t *g = patch_symbol(kExtraSym[kSettings]);
    const uint8_t *stub = g_extra_stub[kSettings];
    if (!g || !stub || !in_copy(g, 0x34)) {
        log("unlocks: default game settings: %s; PS4 values kept", g ? "no PS4 object" : "not in net-patch9");
        return nullptr;
    }
    const int32_t health = rds32(g);
    if (health <= 0 || health > 1000) {
        log("unlocks: net-patch9 default game settings: health %d out of range; PS4 values kept", health);
        return nullptr;
    }
    // PS4 has a few more values after net-patch9's object; they keep the PS4 stub's values.
    auto *t = static_cast<uint8_t *>(netheap_alloc(0x40, 16));
    if (!t) return nullptr;
    std::memset(t, 0, 0x40);
    std::memcpy(t, stub, 0x38);
    std::memcpy(t, g, 0x34);
    log("unlocks: default game settings: player health %d (PS4 %d)", health, rds32(stub));
    return t;
}

void build_extras() {
    g_extra[kNpcInfo] = build_npc_info();
    g_extra[kCharAttr] = build_char_attributes();
    g_extra[kSpeakers] = build_speakers();
    g_extra[kMaterials] = build_materials();
    g_extra[kSettings] = build_settings();
}

// The game keeps the boosters it offers in a fixed list; the PS4 one was filled from the stub table.
bool booster_list_ours() {
    const uint32_t n = rd32(game(kBoosterList));
    return n >= 1 && n <= 63 && in_copy(rdptr(game(kBoosterList), 8), 0x20);
}

void fill_booster_list() {
    const uint8_t *recs = rdptr(g_boosters, 8);
    uint32_t kept = 0;
    for (int32_t i = 0; i < g_booster_count && kept < 63; ++i) {
        const uint8_t *rec = recs + 0x20 * uint64_t(i);
        if (rd32(rec, 0x18) == 0) continue;
        wrptr(game(kBoosterList), 8 + 8 * uint64_t(kept), rec);
        ++kept;
    }
    wr32(game(kBoosterList), 0, kept);
    log("unlocks: booster list 0x113a788: %u boosters", kept);
}

// Binds `table` to `symbol`. Only a bind that does not stick uses up the shared budget.
void bind_table(uint32_t symbol, uint8_t *table) {
    bind_symbol(symbol, table);
    if (dc_find(symbol) != table) --g_bind_budget;
}

const char *bind_suffix(uint32_t symbol, uint8_t *table) {
    return dc_find(symbol) == table ? "" : " - NOT effective";
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Exported functions

void unlocks_refresh() {
    if (!g_ready || !image.base || rds32(game(kNetState)) <= 1 || rd64(game(kSymbolBuckets)) == 0) return;

    if (!g_choice_at_bind.load() && g_skins && dc_find(kSkinSym) == g_skins) {
        g_choice_at_bind.store(true);
        apply_skin_choice("skin table bound");
    }

    if (g_bound.load(std::memory_order_acquire)) {
        const uint8_t *skins_now = dc_find(kSkinSym);
        const uint8_t *boosters_now = dc_find(kBoosterSym);
        bool extras_ours = true;
        for (int k = 0; k < kExtraCount && extras_ours; ++k)
            if (g_extra[k] && dc_find(kExtraSym[k]) != g_extra[k]) extras_ours = false;
        if ((!g_skins || skins_now == g_skins) && (!g_store || dc_find(kStoreSym) == g_store) &&
            (!g_boosters || boosters_now == g_boosters) && (!g_boosters || booster_list_ours()) && extras_ours)
            return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_given_up) return;

    if (!g_patch) {
        g_skin_stub = dc_find(kSkinSym);
        for (int k = 0; k < kExtraCount; ++k) g_extra_stub[k] = dc_find(kExtraSym[k]);
        if (!load_patch()) {
            log("unlocks: %s not imported: the PS4 skin / booster tables stay (2 skins, no boosters)", kPatchPath);
            g_given_up = true;
            g_bound.store(true, std::memory_order_release);
            return;
        }
        build_skin_table();
        build_store_copy();
        build_booster_table();
        if (g_extras_on)
            build_extras();
        else
            log("unlocks: UNCHARTED_U2MP_GOTY_TABLES=off: the other net.bin stubs stay");
        bool any_extra = false;
        for (uint8_t *e : g_extra) any_extra = any_extra || e;
        if (!g_skins && !g_boosters && !g_store && !any_extra) {
            g_given_up = true;
            g_bound.store(true, std::memory_order_release);
            return;
        }
    }

    // The DcFind that tests "not ours yet" is made before the budget is looked at.
    if (g_skins && dc_find(kSkinSym) != g_skins && g_bind_budget > 0) {
        bind_table(kSkinSym, g_skins);
        log("unlocks: skin table 0x%08x bound (%d records)%s", kSkinSym, g_skin_count, bind_suffix(kSkinSym, g_skins));
    }
    if (g_store && dc_find(kStoreSym) != g_store && g_bind_budget > 0) {
        // The game put a new store in place: copy that one (its items may differ) before binding.
        if (g_store_rebuilds > 0) {
            --g_store_rebuilds;
            build_store_copy();
        }
        bind_table(kStoreSym, g_store);
        log("unlocks: MP store 0x%08x bound%s", kStoreSym, bind_suffix(kStoreSym, g_store));
    }
    if (g_boosters && dc_find(kBoosterSym) != g_boosters && g_bind_budget > 0) {
        bind_table(kBoosterSym, g_boosters);
        log("unlocks: booster table 0x%08x bound (%d records)%s", kBoosterSym, g_booster_count,
            bind_suffix(kBoosterSym, g_boosters));
    }
    for (int k = 0; k < kExtraCount; ++k) {
        if (!g_extra[k]) continue;
        if (dc_find(kExtraSym[k]) == g_extra[k] || g_bind_budget <= 0) continue;
        bind_table(kExtraSym[k], g_extra[k]);
        log("unlocks: %s 0x%08x bound%s", kExtraLabel[k], kExtraSym[k], bind_suffix(kExtraSym[k], g_extra[k]));
    }
    if (g_boosters && !booster_list_ours()) fill_booster_list();

    g_bound.store(true, std::memory_order_release);
}

const char *unlocks_text(uint32_t id) {
    if (id == 0 || !g_labels_ready.load(std::memory_order_acquire)) return nullptr;
    for (const Label &l : g_labels)
        if (l.id == id) return l.text.c_str();
    return nullptr;
}

uint32_t unlocks_skin_fingerprint() { return g_fingerprint.load(); }

void install_unlocks(int &installed) {
    const char *env = std::getenv("UNCHARTED_U2MP_UNLOCKS");
    if (env && std::strcmp(env, "off") == 0) g_spoof = false;
    env = std::getenv("UNCHARTED_U2MP_GOTY_TABLES");
    if (env && std::strcmp(env, "off") == 0) g_extras_on = false;

    // The DC loader functions the table binding calls.
    static const unsigned char kRelocate[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                              0x53, 0x48, 0x83, 0xEC, 0x18, 0x48, 0x89, 0x7D, 0xD0, 0x40, 0xF6, 0xC7};
    static const unsigned char kBind[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                          0x53, 0x48, 0x83, 0xEC, 0x18, 0x4C, 0x8B, 0x0D, 0x78, 0x3E, 0x63, 0x00};
    static const unsigned char kFind[] = {0x55, 0x48, 0x89, 0xE5, 0x53, 0x48, 0x83, 0xEC,
                                          0x18, 0x4C, 0x8B, 0x05, 0xF0, 0x58, 0x63, 0x00};
    g_ready = image.executable(kFnRelocate, sizeof kRelocate) && image.matches(kFnRelocate, kRelocate, sizeof kRelocate) &&
              image.executable(kFnBindSymbol, sizeof kBind) && image.matches(kFnBindSymbol, kBind, sizeof kBind) &&
              image.executable(kFnDcFind, sizeof kFind) && image.matches(kFnDcFind, kFind, sizeof kFind);
    if (!g_ready) log("unlocks: DC loader functions differ: GOTY skin / booster tables not bound");

    static const unsigned char kIsLocked[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                              0x53, 0x48, 0x83, 0xEC, 0x28, 0x49, 0x89, 0xF6, 0x49, 0x89, 0xFF};
    static const unsigned char kDebugLea[] = {0x48, 0x8D, 0x05, 0xEF, 0x85, 0x6C, 0x00};
    static const unsigned char kCondCall[] = {0xE8, 0xCF, 0x00, 0x00, 0x00};
    static const unsigned char kCondPrologue[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
                                                  0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC};
    if (image.executable(0xa4bb40, 0x100) && image.matches(0xa4bb40, kIsLocked, sizeof kIsLocked) &&
        image.matches(0xa4bb82, kDebugLea, sizeof kDebugLea) && image.matches(0xa4bbdc, kCondCall, sizeof kCondCall) &&
        image.matches(kFnConditionMet, kCondPrologue, sizeof kCondPrologue) &&
        replace_function(0xa4bb40, 16, reinterpret_cast<void *>(&is_locked), "item IsLocked (0xa4bb40)"))
        installed += 1;
    else
        log("unlocks: IsLocked 0xa4bb40 differs, multiplayer items keep their rank conditions");

    static const unsigned char kUpgrade[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
                                             0x54, 0x53, 0x48, 0x83, 0xEC, 0x48, 0x48, 0x89, 0x75, 0xB8, 0x31,
                                             0xC0, 0x48, 0x85, 0xF6, 0x0F, 0x84, 0x78, 0x01, 0x00, 0x00, 0x8B,
                                             0x0D, 0xD6, 0x16, 0xA8, 0x00, 0x83, 0xF9, 0x02};
    static const unsigned char kUpgradeLea[] = {0x48, 0x8D, 0x05, 0x7D, 0x6E, 0x6C, 0x00};
    if (image.executable(0xa4d280, 0x80) && image.matches(0xa4d280, kUpgrade, sizeof kUpgrade) &&
        image.matches(0xa4d2f4, kUpgradeLea, sizeof kUpgradeLea) &&
        replace_function(0xa4d280, 16, reinterpret_cast<void *>(&coop_upgrade_level), "co-op upgrade level (0xa4d280)"))
        installed += 1;
    else
        log("unlocks: co-op upgrade level 0xa4d280 differs, upgrades stay as bought");

    if (retarget_call(0x663339, kFnArtLookup, reinterpret_cast<void *>(&spawn_lookup_a), "spawn model lookup A") &&
        retarget_call(0x663782, kFnArtLookup, reinterpret_cast<void *>(&spawn_lookup_b), "spawn model lookup B"))
        installed += 1;
    else
        log("unlocks: spawn model lookups (0x663339 / 0x663782) differ, no spawn log");

    env = std::getenv("UNCHARTED_U2MP_SKIN");
    if (env && *env) g_forced_skin = env;

    static const unsigned char kPlayerDataPrologue[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                                        0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x18};
    static const unsigned char kHeroRead[] = {0x41, 0x8B, 0x9F, 0x0C, 0x01, 0x00, 0x00, 0xBF, 0xD2, 0x31, 0x47, 0x5A};
    static const unsigned char kVillainRead[] = {0x41, 0x8B, 0x9F, 0x10, 0x01, 0x00, 0x00, 0xBF, 0xD2, 0x31, 0x47, 0x5A};
    if (image.matches(kFnPlayerData, kPlayerDataPrologue, sizeof kPlayerDataPrologue) &&
        image.matches(0x6145a7, kHeroRead, sizeof kHeroRead) && image.matches(0x614632, kVillainRead, sizeof kVillainRead)) {
        void *message = reinterpret_cast<void *>(&player_data_message);
        void *pick = reinterpret_cast<void *>(&random_skin_pick);
        bool any = false;
        any |= retarget_call(0x5f2f49, kFnPlayerData, message, "skins: player data skin choice");
        any |= retarget_call(0x609551, kFnPlayerData, message, "skins: player data skin choice");
        any |= retarget_call(0x609c2b, kFnPlayerData, message, "skins: player data skin choice");
        any |= retarget_call(0x6145f5, kFnRandomSkin, pick, "skins: random hero pick");
        any |= retarget_call(0x614688, kFnRandomSkin, pick, "skins: random villain pick");
        if (any) installed += 1;
        g_skin_hooks = true;
        const bool forced = !g_forced_skin.empty();
        log("skins: default skin Drake when none is equipped%s%s", forced ? "; forced: " : "",
            forced ? g_forced_skin.c_str() : "");
    } else {
        log("skins: player data 0x6144b0 differs, skin choice left alone");
    }

    if (g_spoof)
        log("unlocks: every multiplayer skin, booster, co-op weapon upgrade and taunt unlocked");
    else
        log("unlocks: spoof off (UNCHARTED_U2MP_UNLOCKS=off); GOTY tables only");
}

}  // namespace u2mp
