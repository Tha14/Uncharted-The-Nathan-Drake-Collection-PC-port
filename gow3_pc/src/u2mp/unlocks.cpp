// u2mp unlocks: every multiplayer unlockable (hero / villain skins, boosters of both slots, co-op weapon
// upgrades, taunts) reported unlocked and owned, and the GOTY skin and booster tables the PS4 net.bin
// lacks, so the store ("Rewards") and the profile lists show every item with its name and picture.
// Static analysis only (the game was not run). Addresses are PS4 1.02 image offsets; notes and the PS3
// GOTY 1.10 counterparts: u2mp/docs/UNLOCKS.md.
//
// How the PS4 build gates the multiplayer items (same scheme as PS3 GOTY):
//  - The items: dc symbol 0x92c6db02 in net.bin (PS3 net-patch9 has the same 6 sections and items):
//    {s32 sections; +8 section*}, section 0x18 {+0 id, +4 title text, +8 s32 count, +0x10 item*},
//    item 0x50 {+0 bit index, +4 type (10 skin, 11 booster, 12 co-op weapon upgrade, 13 taunt),
//    +8 id, +0xc/+0x10 taunt texts, +0x14 cash, +0x18 condition list*, +0x2c weapon, +0x34 tier}.
//    Condition list {s32 count; +8 condition* (0x18 each: +0 type, +4/+8 values, +0x10 string);
//    +0x10 any (else all)}. The multiplayer items use type 7 (rank from cash >= value) and type 1
//    (the previous tier owned).
//  - IsLocked 0xa4bb40 (PS3 0x1642c0) evaluates an item's list with 0xa4bcb0. It is the only unlock
//    test of the multiplayer code: the hero / villain lists (0x6595a0 = PS3 0x4c0ea8 -> 0x4b88e0),
//    the booster lists (0x659bd0), the random skin pick (0x64b4c0), the store rows and their
//    LOCKED / UNLOCKED column (0xa4c390 item state, 0x5aef40, 0x5b0260) and the store module.
//    0xa4bb40 also serves the single-player Rewards (855a9c58), so it is ported whole and only the
//    multiplayer items (those inside the 92c6db02 arrays) always pass. The game's own debug switch
//    "unlock all" (byte 0x1114178, menu 0x3fed90) is left alone: it would unlock the single-player
//    Rewards too.
//  - Owned (the purchase bit, NetPlayerData +0xe0 bit = item +0 index, set by the purchase 0xa4d900)
//    is read only by the co-op weapon upgrade level 0xa4d280 (the other "test bit" helpers 0x661480 /
//    0x661540 have no callers): ported, every upgrade tier counts as owned.
//  - Nothing else checks rank or cash for an item: the store has no price column on PS4 (0x5b0260:
//    name, description, LOCKED / UNLOCKED); cash and rank stay the player's own.
//
// Why names and pictures were missing in the store: the PS4 net.bin defines the skin table
// 5a4731d2 with 2 records (Drake, Eddy Raja) and the booster table 916e03b0 with the 2 "None"
// records; PS3 GOTY took the full tables (107 skins, 32 boosters) from the server module net-patch9,
// which the import converts and netpatch.cpp loads, but netpatch binds only the symbols the PS4 game
// lacks. A store row looks its skin up in 5a4731d2 (0x5aef40 name +0x18, 0x5b0070 picture +0x1c) and
// its booster in the booster list 0x113a788 (name +0, picture +8) that netinit.cpp fills from
// 916e03b0: with the stubs every row but Drake / Eddy / the taunts / the weapon upgrades stayed blank,
// and only those two skins could be picked. This file binds net-patch9's two tables over the stubs
// (its own copy of the converted file): the booster table as converted (4-byte fields only, checked
// equal to PS3), the skin table re-laid out (the converter wrote 0x28-byte records; PS4 records are
// 0x30 with an 8-byte id: +0 index, +4 side, +8 package name*, +0x10 id, +0x18 name, +0x1c photo,
// +0x20 icon, +0x24, +0x28). The two PS4 records replace the first GOTY record of their id (GOTY's
// first Drake was the server's holiday "frosty" model). Then the booster list is filled again.
// 15 GOTY skins have a name id that no GOTY bank has (their texts came with the PS3 DLC packs):
// unlocks_text() answers those ids, only when every bank misses, with a label from the package name.
//
// UNCHARTED_U2MP_UNLOCKS=off: no spoof (the tables are still bound).
#include "unlocks.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "u2mp_internal.h"

extern "C" int runtime_file_translate(const char *guest, char *out, size_t size);

namespace u2mp {
namespace {

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }
template <typename T> T rd(const void *p, ptrdiff_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t *>(p) + off, sizeof v);
    return v;
}
template <typename T> void wr(void *p, ptrdiff_t off, T v) { std::memcpy(static_cast<uint8_t *>(p) + off, &v, sizeof v); }

// ---- guest addresses ----------------------------------------------------------------------------
constexpr uint64_t hIsLocked = 0xa4bb40;      // bool IsLocked(ctx, item, s32 *reason) (PS3 0x1642c0)
constexpr uint64_t fCondition = 0xa4bcb0;     // bool ConditionMet(ctx, condition, s32 *out)
constexpr uint64_t hUpgradeLevel = 0xa4d280;  // s32 CoopUpgradeLevel(?, weapon record) -> tier, cached at +0x20
constexpr uint64_t fRelocate = 0xa19a00;      // RelocateModule(buf) -> root slot (netpatch.cpp)
constexpr uint64_t fBindSymbol = 0xa16d70;    // BindSymbol(sid, const u64 *value)
constexpr uint64_t fDcFind = 0xa15300;        // DcFind(sid) -> value slot or NULL
constexpr uint64_t gHashBuckets = 0x15340d0;  // NULL before the script symbol table exists
constexpr uint64_t gUnlockAll = 0x1114178;    // debug "unlock all" byte (menu 0x3fed90)
constexpr uint64_t gNetState = 0x14ce97c;     // NET_SM
constexpr uint64_t kBoosterList = 0x113a788;  // {u32 count; pad; rec *recs[64]} (netinit.cpp)
constexpr uint32_t kStoreTable = 0x92c6db02, kSkinTable = 0x5a4731d2, kBoosterTable = 0x916e03b0;
constexpr uint32_t kRedId = 0x76474d48;   // sid("mp-hero-red"), hero store item 5
static_assert(sid("mp-hero-red") == kRedId, "skin id");
// The other net.bin stubs net-patch9 fills (UNLOCKS.md 3.1)
constexpr uint32_t kCharAttrs = 0x42d1ec02, kSpeakers = 0xbdf0cfd6, kMaterialParams = 0xef68c757,
                   kNpcInfo = 0xa977dad3, kDefaultSettings = 0x1ad223b4;
constexpr uint32_t kBoosterNone0 = 0x05454b23, kBoosterNone1 = 0x01845694;   // the two "None" boosters
constexpr uint32_t kRootType = 0x916aa605;
constexpr const char *kNetPatch = "/app0/u2data/build/main/dc1/ps4/net-patch9.bin";

constexpr uint64_t SEC_STRIDE = 0x18, ITEM_STRIDE = 0x50, COND_STRIDE = 0x18;
constexpr uint32_t ITEM_UPGRADE = 12;
constexpr uint64_t SKIN_STRIDE = 0x30, SKIN_STRIDE_CONVERTED = 0x28, BOOST_STRIDE = 0x20;
constexpr int kMaxBoosters = 63;

using CondFn = uint8_t(U2MP_GUEST *)(void *, const uint8_t *, int32_t *);
using RelocateFn = void **(U2MP_GUEST *)(void *);
using BindFn = void(U2MP_GUEST *)(uint32_t, const uint64_t *);
using FindFn = void **(U2MP_GUEST *)(uint32_t);

bool g_spoof = true;
std::atomic<int> g_spoof_log{1};

int32_t net_state() { return G<int32_t>(gNetState); }
std::atomic<bool> g_bound{false};   // unlocks_refresh() bound the GOTY tables

const uint8_t *dc_object(uint32_t sid) {
    if (!G<uint64_t>(gHashBuckets)) return nullptr;
    void **slot = guest<FindFn>(fDcFind)(sid);
    return slot ? static_cast<const uint8_t *>(*slot) : nullptr;
}

// ---- multiplayer items ---------------------------------------------------------------------------
// True when `item` is a record of one of the 92c6db02 item arrays.
bool mp_item(const uint8_t *item) {
    const uint8_t *store = dc_object(kStoreTable);
    if (!store) return false;
    const int32_t sections = rd<int32_t>(store, 0);
    const uint8_t *sec = rd<const uint8_t *>(store, 8);
    if (sections <= 0 || sections > 32 || !sec) return false;
    for (int32_t s = 0; s < sections; ++s) {
        const uint8_t *e = sec + SEC_STRIDE * uint64_t(s);
        const int32_t n = rd<int32_t>(e, 8);
        const uint8_t *items = rd<const uint8_t *>(e, 0x10);
        if (!items || n <= 0 || n > 1024) continue;
        if (item >= items && item < items + ITEM_STRIDE * uint64_t(n) && (item - items) % ITEM_STRIDE == 0) return true;
    }
    return false;
}

// 0xa4bb40 ported. Returns true while the item's condition list is not met. Multiplayer items: never.
U2MP_GUEST bool is_locked(void *ctx, const uint8_t *item, int32_t *reason) {
    if (!item) return false;
    if (reason) *reason = 0;
    if (!g_bound.load(std::memory_order_relaxed)) unlocks_refresh();   // the store can open before 0x64e810 ran
    if (g_spoof && mp_item(item)) {
        if (g_spoof_log.exchange(0)) log("unlocks: multiplayer items reported unlocked (IsLocked 0xa4bb40)");
        return false;
    }
    const uint8_t *list = rd<const uint8_t *>(item, 0x18);
    if (!list) return false;
    auto met = guest<CondFn>(fCondition);
    bool locked = false;
    if (!G<uint8_t>(gUnlockAll)) {
        locked = rd<int32_t>(list, 0x10) != 0 && rd<int32_t>(list, 0) > 0;   // "any": locked until one is met
        for (int32_t i = 0; i < rd<int32_t>(list, 0); ++i) {
            const bool ok = met(ctx, rd<const uint8_t *>(list, 8) + COND_STRIDE * uint64_t(i), nullptr) != 0;
            list = rd<const uint8_t *>(item, 0x18);
            const bool any = rd<int32_t>(list, 0x10) != 0;
            if (ok && any) { locked = false; break; }
            if (!ok && !any) { locked = true; break; }
        }
    }
    // The reason: the out value of the first condition that is met (as 0xa4bc2f..0xa4bc74).
    if (reason) {
        for (int32_t i = 0; i < rd<int32_t>(list, 0); ++i) {
            int32_t out = 0;
            if (met(ctx, rd<const uint8_t *>(list, 8) + COND_STRIDE * uint64_t(i), &out)) {
                *reason = out;
                break;
            }
            list = rd<const uint8_t *>(item, 0x18);
        }
    }
    return locked;
}

// 0xa4d280 ported: the co-op upgrade tier of a weapon record (829f99c6 table: +0 weapon id, +0x20 the
// cached tier, -1 until computed) = the highest +0x34 of the type-12 store items of that weapon that
// are owned (debug switch, or the NetPlayerData bit). With the spoof every tier counts as owned.
U2MP_GUEST int32_t upgrade_level(void *, uint8_t *weapon) {
    if (!weapon || net_state() < 2) return 0;
    const int32_t cached = rd<int32_t>(weapon, 0x20);
    if (cached >= 0) return cached;
    const uint8_t *store = dc_object(kStoreTable);
    int32_t best = 0;
    if (store && rd<int32_t>(store, 0) > 0) {
        const uint8_t *npd = image.at<uint8_t>(0x11474f8);
        const bool all = g_spoof || G<uint8_t>(gUnlockAll);
        const uint32_t id = rd<uint32_t>(weapon, 0);
        const uint8_t *sec = rd<const uint8_t *>(store, 8);
        for (int32_t s = 0; sec && s < rd<int32_t>(store, 0); ++s) {
            const uint8_t *e = sec + SEC_STRIDE * uint64_t(s);
            const uint8_t *items = rd<const uint8_t *>(e, 0x10);
            for (int32_t j = 0; items && j < rd<int32_t>(e, 8); ++j) {
                const uint8_t *it = items + ITEM_STRIDE * uint64_t(j);
                if (rd<uint32_t>(it, 4) != ITEM_UPGRADE || rd<uint32_t>(it, 0x2c) != id) continue;
                const uint32_t bit = rd<uint32_t>(it, 0);
                // NetPlayerData +0x4c + 4 * (0x25 + bit / 32), as 0xa4d371 (connected-but-offline guard
                // not repeated: progression.cpp keeps that state consistent)
                const bool owned = all || (bit < 288 && (rd<uint32_t>(npd, 0x4c + 4 * (0x25 + bit / 32)) >> (bit % 32) & 1));
                if (owned) best = std::max(best, rd<int32_t>(it, 0x34));
            }
        }
    }
    wr<int32_t>(weapon, 0x20, best);
    return best;
}

// ---- the GOTY tables ------------------------------------------------------------------------------
const char *actor_source(const char *name);   // spawn diagnostics below
std::mutex g_tables_lock;
bool g_ready = false;        // install checked the loader functions
bool g_given_up = false;     // the file is missing or bad: not retried
uint8_t *g_np9 = nullptr;    // our relocated copy of net-patch9 (kept for the whole game)
size_t g_np9_size = 0;
uint8_t *g_skins = nullptr;  // {s32 count; pad; rec *} + records, PS4 layout
const uint8_t *g_boosters = nullptr;   // net-patch9's booster table object
int32_t g_booster_count = 0;

std::vector<std::pair<uint32_t, std::string>> g_names;   // skin name id -> label (built once)
std::atomic<bool> g_names_ready{false};

bool in_np9(const void *p, size_t bytes = 1) {
    const uint8_t *q = static_cast<const uint8_t *>(p);
    return g_np9 && q >= g_np9 && q + bytes <= g_np9 + g_np9_size;
}

// An actor package name ("mp-hero..."), NUL-terminated within 64 bytes.
bool package_name(const char *s, bool must_be_ours) {
    if (!s || (must_be_ours && !in_np9(s, 4))) return false;
    if (std::strncmp(s, "mp-", 3) != 0) return false;
    for (int i = 0; i < 64; ++i) {
        if (!s[i]) return true;
        if (must_be_ours && !in_np9(s + i + 1)) return false;
    }
    return false;
}

bool load_np9() {
    char host[1024];
    if (runtime_file_translate(kNetPatch, host, sizeof host) != 0) return false;
    std::FILE *f = std::fopen(host, "rb");
    if (!f) return false;
    std::string data;
    char buffer[65536];
    for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;) data.append(buffer, n);
    std::fclose(f);
    const auto *d = reinterpret_cast<const unsigned char *>(data.data());
    if (data.size() < 0x40 || std::memcmp(d, "00CD", 4) != 0 || rd<uint64_t>(d, 0x10) != 0x20 ||
        rd<uint32_t>(d, 0x1c) != kRootType) {
        log("unlocks: %s is not a PS4 DC module", kNetPatch);
        return false;
    }
    const uint64_t reloc = rd<uint64_t>(d, 8);
    if (reloc < 0x28 || reloc % 8 || reloc + 4 > data.size() || reloc + 4 + rd<uint32_t>(d, reloc) > data.size()) {
        log("unlocks: %s has a bad relocation table", kNetPatch);
        return false;
    }
    auto *buf = static_cast<uint8_t *>(::operator new(data.size(), std::align_val_t(16), std::nothrow));
    if (!buf) return false;
    std::memcpy(buf, data.data(), data.size());
    void **root = guest<RelocateFn>(fRelocate)(buf);
    if (root != reinterpret_cast<void **>(buf + 0x10) || *root != buf + 0x20) {
        log("unlocks: relocating %s failed", kNetPatch);
        ::operator delete(buf, std::align_val_t(16));
        return false;
    }
    g_np9 = buf;
    g_np9_size = data.size();
    return true;
}

const uint8_t *np9_symbol(uint32_t sid) {
    const uint8_t *root = g_np9 + 0x20;
    const uint32_t count = rd<uint32_t>(root, 4);
    for (uint32_t i = 0; i < count && in_np9(root + 8 + 16 * uint64_t(i), 16); ++i)
        if (rd<uint32_t>(root, 8 + 16 * ptrdiff_t(i)) == sid) {
            const uint8_t *v = rd<const uint8_t *>(root, 8 + 16 * ptrdiff_t(i) + 8);
            return in_np9(v, 16) ? v : nullptr;
        }
    return nullptr;
}

// Every record of a {count, recs} table at `stride` has a package name at +8 (and, for the PS4 layout,
// the high half of the 8-byte id at +0x14 is 0).
bool skin_layout(const uint8_t *recs, int32_t n, uint64_t stride, bool ps4, bool ours) {
    if (!recs || n <= 0 || n > 512 || (ours && !in_np9(recs, stride * uint64_t(n)))) return false;
    for (int32_t i = 0; i < n; ++i) {
        const uint8_t *r = recs + stride * uint64_t(i);
        if (ps4 && rd<uint32_t>(r, 0x14) != 0) return false;
        if (!package_name(rd<const char *>(r, 8), ours)) return false;
    }
    return true;
}

std::string label_of(const char *package) {
    std::string s(package);
    if (s.rfind("mp-hero-", 0) == 0) s = s.substr(8);
    std::string out;
    bool start = true;
    for (char c : s) {
        if (c == '-' || c == '_') {
            if (!out.empty() && out.back() != ' ') out += ' ';
            start = true;
            continue;
        }
        out += start ? char(std::toupper(static_cast<unsigned char>(c))) : c;
        start = false;
    }
    return out;
}

// Skins the hero / villain lists (0x6595a0, cond 40/41) lack. The lists walk only the MP store's sections
// 681022f5 (heroes) / 4252add5 (villains) and show an item when a skin record has its id, so each needs a store
// item too (build_store). The first two have a GOTY record but no store item (net-patch9 swapped their ids with
// the "ND" / "Doughnut" twins); the next three are campaign costumes with no MP skin; the last two are GOTY MP
// skins without a store item. Photos / icons of a close twin where the costume has none.
struct CampaignSkin { const char *package; uint32_t id, name, photo, icon; bool villain; };
const CampaignSkin kCampaignSkins[] = {
    {"mp-hero-lazarevic", 0x345329d1, 0xe2ce50df, 0xeabed4cf, 0xffa8b107, true},              // Zoran Lazarevic
    {"mp-hero-pirate", 0x5a2e4839, 0xc0f12cfd, 0x9f8d3fe9, 0xb980d32e, true},                 // Prakoso
    {"costume-hero-lazarevic-healed", 0x1c14ad7b, 0x899e21dd, 0xeabed4cf, 0xffa8b107, true},  // Lazarevic - Healed
    {"hero-dirty-beatup", 0x1b3af509, 0x1d8dbe6a, 0x05a8ba5f, 0x14c66775, false},           // Drake - Dirty and Beat-up
    {"costume-hero-skeleton-sir-francis", 0x7a66b9bb, 0xba9eb65d, 0x200eed64, 0x3160304e, false},   // Sir Francis Drake
    {"mp-hero-nd", 0x258034ff, 0x9b7568c5, 0xaa7dcc3a, 0x2534f068, false},                  // ND Drake
    {"mp-hero-frosty", 0xf0d8079a, 0xaacd3194, 0x01bf230e, 0xb58cceaa, false},              // Santa Drake
};
constexpr size_t kCampaignSkinCount = sizeof kCampaignSkins / sizeof *kCampaignSkins;
constexpr uint32_t kSecHeroSkins = 0x681022f5, kSecVillainSkins = 0x4252add5;
uint8_t *g_store = nullptr;   // our copy of 92c6db02 with the added skin items

// The MP store (92c6db02) with a store item for every kCampaignSkins entry that has none: the sections array
// copied, the hero / villain item arrays copied and extended (a sibling item as the template: no condition).
void build_store() {
    const uint8_t *store = dc_object(kStoreTable);
    if (!store || store == g_store) return;
    const int32_t sections = rd<int32_t>(store, 0);
    const uint8_t *sec = rd<const uint8_t *>(store, 8);
    if (sections <= 0 || sections > 32 || !sec) return;
    auto *mem = static_cast<uint8_t *>(netheap_alloc(0x10 + SEC_STRIDE * uint64_t(sections), 16));
    if (!mem) return;
    std::memset(mem, 0, 0x10);
    uint8_t *secs = mem + 0x10;
    std::memcpy(secs, sec, SEC_STRIDE * uint64_t(sections));
    int added = 0;
    for (int32_t s = 0; s < sections; ++s) {
        uint8_t *e = secs + SEC_STRIDE * uint64_t(s);
        const uint32_t id = rd<uint32_t>(e, 0);
        if (id != kSecHeroSkins && id != kSecVillainSkins) continue;
        const bool villain = id == kSecVillainSkins;
        const int32_t n = rd<int32_t>(e, 8);
        const uint8_t *items = rd<const uint8_t *>(e, 0x10);
        if (n <= 0 || n > 200 || !items) continue;
        std::vector<const CampaignSkin *> add;
        for (const CampaignSkin &c : kCampaignSkins) {
            if (c.villain != villain) continue;
            bool have = false;
            for (int32_t i = 0; i < n && !have; ++i) have = rd<uint32_t>(items + ITEM_STRIDE * uint64_t(i), 8) == c.id;
            if (!have) add.push_back(&c);
        }
        if (add.empty()) continue;
        auto *arr = static_cast<uint8_t *>(netheap_alloc(ITEM_STRIDE * uint64_t(n + int32_t(add.size())), 16));
        if (!arr) continue;
        std::memcpy(arr, items, ITEM_STRIDE * uint64_t(n));
        const uint8_t *tmpl = items + ITEM_STRIDE * uint64_t(n - 1);
        for (size_t k = 0; k < add.size(); ++k) {
            uint8_t *it = arr + ITEM_STRIDE * uint64_t(n + int32_t(k));
            std::memcpy(it, tmpl, ITEM_STRIDE);   // owned bit, flags +0x28 / +0x30 / +0x38 of a sibling
            wr<uint32_t>(it, 4, 10);
            wr<uint32_t>(it, 8, add[k]->id);
            wr<uint64_t>(it, 0x18, 0);             // no unlock condition
            wr<uint64_t>(it, 0x40, uint64_t(uintptr_t(add[k]->package)));
            wr<uint64_t>(it, 0x48, add[k]->id);
            ++added;
        }
        wr<int32_t>(e, 8, n + int32_t(add.size()));
        wr<uint64_t>(e, 0x10, uint64_t(uintptr_t(arr)));
    }
    wr<int32_t>(mem, 0, sections);
    wr<uint64_t>(mem, 8, uint64_t(uintptr_t(secs)));
    g_store = mem;
    log("unlocks: MP store copied with %d added skin item(s) (hero / villain lists)", added);
}

// The merged skin table: net-patch9's records in PS4 layout, the PS4 records in place of the first
// GOTY record of their id (or appended).
bool build_skins(const uint8_t *ps4_table) {
    const uint8_t *t = np9_symbol(kSkinTable);
    if (!t) {
        log("unlocks: net-patch9 has no skin table 0x%08x", kSkinTable);
        return false;
    }
    const int32_t n = rd<int32_t>(t, 0);
    const uint8_t *recs = rd<const uint8_t *>(t, 8);
    uint64_t stride = 0;
    if (skin_layout(recs, n, SKIN_STRIDE, true, true)) stride = SKIN_STRIDE;
    else if (skin_layout(recs, n, SKIN_STRIDE_CONVERTED, false, true)) stride = SKIN_STRIDE_CONVERTED;
    if (!stride) {
        log("unlocks: net-patch9 skin table (%d records) has an unknown layout; PS4 skins kept", n);
        return false;
    }
    const int32_t ps4_n = ps4_table ? rd<int32_t>(ps4_table, 0) : 0;
    const uint8_t *ps4_recs = ps4_table ? rd<const uint8_t *>(ps4_table, 8) : nullptr;
    // the PS4 stub has 2 records; anything larger is not the stub (e.g. a table bound by
    // UNCHARTED_U2MP_NETPATCH=all), and its pointers are not followed
    const bool ps4_ok = ps4_n > 0 && ps4_n <= 8 && skin_layout(ps4_recs, ps4_n, SKIN_STRIDE, true, false);
    const int32_t total = n + (ps4_ok ? ps4_n : 0) + 1 + int32_t(kCampaignSkinCount);   // + Baseball Shirt Drake, campaign skins
    auto *mem = static_cast<uint8_t *>(netheap_alloc(0x10 + SKIN_STRIDE * uint64_t(total), 16));
    if (!mem) return false;
    std::memset(mem, 0, 0x10 + SKIN_STRIDE * uint64_t(total));
    uint8_t *out = mem + 0x10;
    for (int32_t i = 0; i < n; ++i) {
        const uint8_t *r = recs + stride * uint64_t(i);
        uint8_t *o = out + SKIN_STRIDE * uint64_t(i);
        if (stride == SKIN_STRIDE) {
            std::memcpy(o, r, SKIN_STRIDE);
            continue;
        }
        // converted: +0 index, +4 side, +8 name*, +0x10 id, +0x14 name, photo, icon, +0x20, +0x24
        wr<uint32_t>(o, 0, rd<uint32_t>(r, 0));
        wr<uint32_t>(o, 4, rd<uint32_t>(r, 4));
        wr<uint64_t>(o, 8, rd<uint64_t>(r, 8));
        wr<uint32_t>(o, 0x10, rd<uint32_t>(r, 0x10));
        for (int k = 0; k < 5; ++k) wr<uint32_t>(o, 0x18 + 4 * k, rd<uint32_t>(r, 0x14 + 4 * k));
    }
    int32_t count = n, replaced = 0, appended = 0;
    for (int32_t j = 0; ps4_ok && j < ps4_n; ++j) {
        const uint8_t *p = ps4_recs + SKIN_STRIDE * uint64_t(j);
        const uint32_t id = rd<uint32_t>(p, 0x10);
        int32_t at = -1;
        for (int32_t i = 0; i < n && at < 0; ++i)
            if (rd<uint32_t>(out + SKIN_STRIDE * uint64_t(i), 0x10) == id) at = i;
        if (at < 0) {
            at = count++;
            ++appended;
        } else {
            ++replaced;
        }
        std::memcpy(out + SKIN_STRIDE * uint64_t(at), p, SKIN_STRIDE);
        // +8 points into the PS4 net.bin module, which may be reloaded: keep our own copy (review 2026-11-09)
        if (const char *pkg = reinterpret_cast<const char *>(uintptr_t(rd<uint64_t>(p, 8)))) {
            const size_t len = std::strlen(pkg);
            if (auto *copy = static_cast<char *>(netheap_alloc(len + 1, 16))) {
                std::memcpy(copy, pkg, len + 1);
                wr<uint64_t>(out + SKIN_STRIDE * uint64_t(at), 8, uint64_t(uintptr_t(copy)));
            }
        }
    }
    // Hero store item 5 (PS4 store, id 76474d48 = sid("mp-hero-red")) has no record in the PS4 stub or in
    // net-patch9 (GOTY's patch put mp-hero-nd at that index). The GOTY base net.bin (dc1.psarc) has it:
    // {index 1, hero, "mp-hero-red", 76474d48, name 9a88ecdc "Baseball Shirt Drake", photo dafcd51b
    // general/net/player-photos/hero-red, icon c283da48 general/net/player-icons/hero-red, 0}. The PS4 game
    // has every piece (mp-hero-red.pak, both net.pak images, the text in its networking bank). Appended
    // at the end, so the indices of every other record stay those of net-patch9's order.
    bool have_red = false;
    for (int32_t i = 0; i < count && !have_red; ++i) have_red = rd<uint32_t>(out + SKIN_STRIDE * uint64_t(i), 0x10) == kRedId;
    if (!have_red) {
        uint8_t *o = out + SKIN_STRIDE * uint64_t(count++);
        static const char kRedPackage[] = "mp-hero-red";
        wr<uint32_t>(o, 0, 1);
        wr<uint32_t>(o, 4, 0);
        wr<uint64_t>(o, 8, uint64_t(uintptr_t(kRedPackage)));
        wr<uint64_t>(o, 0x10, kRedId);
        wr<uint32_t>(o, 0x18, 0x9a88ecdcu);
        wr<uint32_t>(o, 0x1c, 0xdafcd51bu);
        wr<uint32_t>(o, 0x20, 0xc283da48u);
        ++appended;
    }
    // Campaign costumes of the PS4 game with no multiplayer skin (store.bin 855a9c58 "Character Skins"): their
    // actor27 packages share the MP skeleton, so the package name goes straight into +8. Appended after the
    // above, so every earlier position (the crossplay byte index) stays.
    for (const CampaignSkin &c : kCampaignSkins) {
        bool have = false;
        for (int32_t i = 0; i < count && !have; ++i) have = rd<uint32_t>(out + SKIN_STRIDE * uint64_t(i), 0x10) == c.id;
        if (have) continue;
        uint8_t *o = out + SKIN_STRIDE * uint64_t(count++);
        wr<uint32_t>(o, 0, uint32_t(count - 1));
        wr<uint32_t>(o, 4, c.villain ? 1u : 0u);
        wr<uint64_t>(o, 8, uint64_t(uintptr_t(c.package)));
        wr<uint64_t>(o, 0x10, c.id);
        wr<uint32_t>(o, 0x18, c.name);
        wr<uint32_t>(o, 0x1c, c.photo);
        wr<uint32_t>(o, 0x20, c.icon);
        ++appended;
    }
    wr<int32_t>(mem, 0, count);
    wr<uint64_t>(mem, 8, uint64_t(uintptr_t(out)));
    g_skins = mem;
    // Crossplay: the player data message carries a skin as the record's position in this table (a byte,
    // 0x6145a7 / 0x614632; the receiver reads the same position, 0x641aa0). The order is net-patch9's,
    // then the PS4 replacements in place and the appended records, so it is the same on every PC with
    // the same import. The fingerprint (CRC of the ids in order) must match between the PCs of a match.
    uint32_t fp = 0;
    for (int32_t i = 0; i < count; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08x", rd<uint32_t>(out + SKIN_STRIDE * uint64_t(i), 0x10));
        fp = sid(buf, fp);
    }
    log("unlocks: skin table order fingerprint %08x (%d records; every PC of a match must log the same)", fp, count);
    // labels for the name ids (served only when no bank has the id)
    std::vector<std::pair<uint32_t, std::string>> names;
    for (int32_t i = 0; i < count; ++i) {
        const uint8_t *o = out + SKIN_STRIDE * uint64_t(i);
        const uint32_t name = rd<uint32_t>(o, 0x18);
        if (!name) continue;
        bool have = false;
        for (const auto &e : names) have = have || e.first == name;
        if (!have) names.emplace_back(name, label_of(rd<const char *>(o, 8)));
    }
    // where each skin's actor comes from (the PS4 game, the import overlay, nowhere)
    int from_ps4 = 0, from_overlay = 0;
    std::string missing;
    for (int32_t i = 0; i < count; ++i) {
        const char *pk = rd<const char *>(out + SKIN_STRIDE * uint64_t(i), 8);
        const char *src = actor_source(pk);
        if (std::strcmp(src, "PS4 game") == 0) ++from_ps4;
        else if (std::strcmp(src, "import overlay") == 0) ++from_overlay;
        else if (missing.size() < 400 && missing.find(pk) == std::string::npos) missing += std::string(" ") + pk;
    }
    log("unlocks: skin actors: %d records from the PS4 game, %d from the import overlay, missing:%s", from_ps4,
        from_overlay, missing.empty() ? " none" : missing.c_str());
    g_names = std::move(names);
    g_names_ready.store(true, std::memory_order_release);
    log("unlocks: skin table: %d GOTY records (%s layout) + PS4 %d (%d replaced, %d appended) = %d", n,
        stride == SKIN_STRIDE ? "PS4" : "0x28-byte converted", ps4_ok ? ps4_n : 0, replaced, appended, count);
    return true;
}

bool build_boosters() {
    const uint8_t *t = np9_symbol(kBoosterTable);
    if (!t) {
        log("unlocks: net-patch9 has no booster table 0x%08x", kBoosterTable);
        return false;
    }
    const int32_t n = rd<int32_t>(t, 0);
    const uint8_t *recs = rd<const uint8_t *>(t, 8);
    if (n <= 0 || n > kMaxBoosters || !in_np9(recs, BOOST_STRIDE * uint64_t(n))) {
        log("unlocks: net-patch9 booster table (%d records) out of range; PS4 boosters kept", n);
        return false;
    }
    bool none0 = false, none1 = false;
    for (int32_t i = 0; i < n; ++i) {
        const uint8_t *r = recs + BOOST_STRIDE * uint64_t(i);
        const uint32_t id = rd<uint32_t>(r, 0xc), slot = rd<uint32_t>(r, 0x10);
        if (!id || slot > 1) {
            log("unlocks: net-patch9 booster %d (id %08x, slot %u) is not a booster record; PS4 boosters kept", i, id, slot);
            return false;
        }
        none0 = none0 || id == kBoosterNone0;
        none1 = none1 || id == kBoosterNone1;
    }
    if (!none0 || !none1) {
        log("unlocks: net-patch9 booster table lacks the two None boosters; PS4 boosters kept");
        return false;
    }
    g_boosters = t;
    g_booster_count = n;
    log("unlocks: booster table: %d GOTY records", n);
    return true;
}

// netinit.cpp's list (PS3 NetBoosterManager init 0x4554c0): every record with a non-zero +0x18.
void fill_booster_list() {
    const uint8_t *recs = rd<const uint8_t *>(g_boosters, 8);
    uint32_t kept = 0;
    for (int32_t i = 0; i < g_booster_count && kept < uint32_t(kMaxBoosters); ++i) {
        const uint8_t *r = recs + BOOST_STRIDE * uint64_t(i);
        if (rd<uint32_t>(r, 0x18) == 0) continue;
        G<uint64_t>(kBoosterList + 8 + 8 * uint64_t(kept)) = uint64_t(uintptr_t(r));
        ++kept;
    }
    G<uint32_t>(kBoosterList) = kept;
    log("unlocks: booster list 0x113a788: %u boosters", kept);
}

bool booster_list_ours() {
    const uint32_t n = G<uint32_t>(kBoosterList);
    if (n == 0 || n > uint32_t(kMaxBoosters)) return false;
    return in_np9(reinterpret_cast<const void *>(uintptr_t(G<uint64_t>(kBoosterList + 8))), BOOST_STRIDE);
}

// ---- the other net.bin stubs (UNLOCKS.md 3.1) ----------------------------------------------------
// The PS4 net.bin keeps these five symbols as stubs; GOTY's net-patch9 has the full data (the readers
// are intact PS4 code; netpatch.cpp binds only the symbols the PS4 game lacks, so the stubs stayed):
//   a977dad3 net-npc-info, {s32 n; pad; rec *} of 12 bytes {+0 archetype sid, +4 class sid, +8 co-op
//     points}: GetCoopPointsForKill 0x5decf0 (moneymanager.cpp) and 0x645d5e / 0x645e9b / 0x64ab78. An
//     archetype that is not listed hits an assert (int 0x41 at 0x5dedac / 0x5dedd8): with the 1-record
//     stub every co-op kill of another NPC type. GOTY 56 records (= the GOTY base net.bin's 56).
//   42d1ec02 character attributes by actor name, 0x20 {+0 actor name*, +8 key = sid(actor name),
//     +0x10, +0x14 rig variant (2 / 3), +0x18 class 1..5 (tables 0xdef540 / 0xdef560), +0x1c base sid of
//     "<x>-phys-fx"}; searched after 74bfa7d0 by 0x5fdbc0 (phys-fx, default 0x474e00), 0x725480,
//     0x725820, 0x94f61f, 0x94fbc0, 0x9579e1, 0x964fc1, 0x965b05, 0x97863f, 0x9786ff. Stub: mp-hero and
//     mp-hero-eddie only; GOTY 98. net-patch9 as converted packs the 4-byte fields after the id
//     ({ptr, id, a, b, c, sid, pad}); re-laid out here.
//   bdf0cfd6 speakers, sorted 0x10 {u32 key, pad, value*} (binary search, unsigned), value {name*, line
//     prefix*, *, *}; multiplayer supplement of 1b405df2 at 0x474c08 / 0x725564. Stub 1, GOTY 52 (PS3
//     records have a fifth u32 the PS4 reader never reads; the converter keeps it after the four pointers).
//   ef68c757 material parameter set, 0x30 {material path*, parameter*, type (1), from*, to*, cache*},
//     applied by 0x9f3b70 (from 0x64b926, value from the map record +0x34) through 0xbe78d0, which
//     follows +0x28 only when set. Stub 1 (Drake's outfit), GOTY 98 (the converted layout is the PS4 one).
//   1ad223b4 default game settings (type 2209c8ff, *net-games* +0x64 when a game names none): +0 player
//     health (0x5fd853, scaled by the health booster 9a925faf), floats +4..+0x30. Stub = the GOTY base
//     value 80; net-patch9 90. The two per-game settings (63e35446, ac1dedb5) are equal in both.
struct Extra {
    uint32_t sid;
    const char *what;
    const uint8_t *ps4 = nullptr;   // the PS4 stub, captured before anything of ours is bound
    uint8_t *ours = nullptr;
};
Extra g_extra[] = {{kNpcInfo, "net-npc-info"}, {kCharAttrs, "character attributes"}, {kSpeakers, "speakers"},
                   {kMaterialParams, "material parameter set"}, {kDefaultSettings, "default game settings"}};
bool g_extra_on = true;   // UNCHARTED_U2MP_GOTY_TABLES=off

bool np9_string(const void *p) {
    const char *s = static_cast<const char *>(p);
    for (int i = 0; i < 256; ++i) {
        if (!in_np9(s + i)) return false;
        if (!s[i]) return i > 0;
    }
    return false;
}

uint8_t *new_table(int32_t count, uint64_t stride) {
    const uint64_t bytes = 0x10 + stride * uint64_t(count);
    auto *mem = static_cast<uint8_t *>(netheap_alloc(bytes, 16));
    if (!mem) return nullptr;
    std::memset(mem, 0, bytes);
    wr<int32_t>(mem, 0, count);
    wr<uint64_t>(mem, 8, uint64_t(uintptr_t(mem + 0x10)));
    return mem;
}

// {s32 count; pad; rec *} of net-patch9 with `stride`-byte records inside our copy; NULL when not.
const uint8_t *np9_table(uint32_t s, uint64_t stride, int32_t &n, const char *what) {
    const uint8_t *t = np9_symbol(s);
    n = t ? rd<int32_t>(t, 0) : 0;
    const uint8_t *recs = t ? rd<const uint8_t *>(t, 8) : nullptr;
    if (!t || n <= 0 || n > 1024 || !in_np9(recs, stride * uint64_t(n))) {
        log("unlocks: net-patch9 %s 0x%08x missing or out of range (%d records); PS4 stub kept", what, s, n);
        return nullptr;
    }
    return recs;
}

// a977dad3: as converted (all 4-byte fields), PS4 records whose archetype GOTY lacks appended.
uint8_t *build_npc_info(const uint8_t *ps4) {
    int32_t n = 0;
    const uint8_t *recs = np9_table(kNpcInfo, 12, n, "net-npc-info");
    if (!recs) return nullptr;
    for (int32_t i = 0; i < n; ++i)
        if (!rd<uint32_t>(recs, 12 * i)) {
            log("unlocks: net-npc-info record %d has no archetype; PS4 stub kept", i);
            return nullptr;
        }
    const int32_t pn = ps4 ? rd<int32_t>(ps4, 0) : 0;
    const uint8_t *pr = ps4 ? rd<const uint8_t *>(ps4, 8) : nullptr;
    const bool ps4_ok = pn > 0 && pn <= 8 && pr;
    uint8_t *t = new_table(n + (ps4_ok ? pn : 0), 12);
    if (!t) return nullptr;
    uint8_t *out = t + 0x10;
    std::memcpy(out, recs, 12 * size_t(n));
    int32_t count = n;
    for (int32_t j = 0; ps4_ok && j < pn; ++j) {
        bool have = false;
        for (int32_t i = 0; i < n && !have; ++i) have = rd<uint32_t>(out, 12 * i) == rd<uint32_t>(pr, 12 * j);
        if (!have) std::memcpy(out + 12 * count++, pr + 12 * j, 12);
    }
    wr<int32_t>(t, 0, count);
    log("unlocks: net-npc-info: %d GOTY records + %d PS4", n, count - n);
    return t;
}

// 42d1ec02: re-laid out to the PS4 record; the PS4 records replace the first GOTY record of their key
// (the PS4 asset rule, as the skins), others appended; their name strings copied (net.bin may reload).
uint8_t *build_char_attrs(const uint8_t *ps4) {
    int32_t n = 0;
    const uint8_t *recs = np9_table(kCharAttrs, 0x20, n, "character attributes");
    if (!recs) return nullptr;
    bool converted = true, ps4_layout = true;
    for (int32_t i = 0; i < n; ++i) {
        const uint8_t *r = recs + 0x20 * uint64_t(i);
        if (!np9_string(rd<const void *>(r, 0)) || !rd<uint32_t>(r, 8)) converted = ps4_layout = false;
        if (rd<uint32_t>(r, 0x1c) != 0 || rd<uint32_t>(r, 0x18) == 0) converted = false;   // sid at +0x18
        if (rd<uint32_t>(r, 0xc) != 0 || rd<uint32_t>(r, 0x1c) == 0) ps4_layout = false;  // sid at +0x1c
    }
    if (!converted && !ps4_layout) {
        log("unlocks: net-patch9 character attributes (%d records): unknown layout; PS4 stub kept", n);
        return nullptr;
    }
    const int32_t pn = ps4 ? rd<int32_t>(ps4, 0) : 0;
    const uint8_t *pr = ps4 ? rd<const uint8_t *>(ps4, 8) : nullptr;
    const bool ps4_ok = pn > 0 && pn <= 8 && pr;
    uint8_t *t = new_table(n + (ps4_ok ? pn : 0), 0x20);
    if (!t) return nullptr;
    uint8_t *out = t + 0x10;
    for (int32_t i = 0; i < n; ++i) {
        const uint8_t *r = recs + 0x20 * uint64_t(i);
        uint8_t *o = out + 0x20 * uint64_t(i);
        if (ps4_layout) {
            std::memcpy(o, r, 0x20);
            continue;
        }
        wr<uint64_t>(o, 0, rd<uint64_t>(r, 0));
        wr<uint32_t>(o, 8, rd<uint32_t>(r, 8));
        for (int k = 0; k < 4; ++k) wr<uint32_t>(o, 0x10 + 4 * k, rd<uint32_t>(r, 0xc + 4 * k));
    }
    int32_t count = n, replaced = 0;
    for (int32_t j = 0; ps4_ok && j < pn; ++j) {
        const uint8_t *p = pr + 0x20 * uint64_t(j);
        int32_t at = -1;
        for (int32_t i = 0; i < n && at < 0; ++i)
            if (rd<uint32_t>(out + 0x20 * uint64_t(i), 8) == rd<uint32_t>(p, 8)) at = i;
        if (at < 0) at = count++;
        else ++replaced;
        uint8_t *o = out + 0x20 * uint64_t(at);
        std::memcpy(o, p, 0x20);
        if (const char *nm = rd<const char *>(p, 0)) {
            const size_t len = std::strlen(nm);
            if (auto *copy = static_cast<char *>(netheap_alloc(len + 1, 16))) {
                std::memcpy(copy, nm, len + 1);
                wr<uint64_t>(o, 0, uint64_t(uintptr_t(copy)));
            }
        }
    }
    wr<int32_t>(t, 0, count);
    log("unlocks: character attributes: %d GOTY records (%s layout), PS4 %d replaced, %d appended", n,
        ps4_layout ? "PS4" : "converted", replaced, count - n);
    return t;
}

// bdf0cfd6: sorted by key; GOTY's records (values in our copy), PS4 keys GOTY lacks are logged.
uint8_t *build_speakers(const uint8_t *ps4) {
    int32_t n = 0;
    const uint8_t *recs = np9_table(kSpeakers, 0x10, n, "speakers");
    if (!recs) return nullptr;
    for (int32_t i = 0; i < n; ++i) {
        const uint8_t *r = recs + 0x10 * uint64_t(i);
        const uint8_t *v = rd<const uint8_t *>(r, 8);
        bool ok = rd<uint32_t>(r, 4) == 0 && in_np9(v, 0x20) && (i == 0 || rd<uint32_t>(r, 0) > rd<uint32_t>(r, -0x10));
        for (int k = 0; ok && k < 4; ++k) ok = in_np9(rd<const void *>(v, 8 * k));
        if (!ok) {
            log("unlocks: net-patch9 speakers record %d is not a sorted {key, value} record; PS4 stub kept", i);
            return nullptr;
        }
    }
    uint8_t *t = new_table(n, 0x10);
    if (!t) return nullptr;
    std::memcpy(t + 0x10, recs, 0x10 * size_t(n));
    int missing = 0;
    const int32_t pn = ps4 ? rd<int32_t>(ps4, 0) : 0;
    const uint8_t *pr = ps4 ? rd<const uint8_t *>(ps4, 8) : nullptr;
    for (int32_t j = 0; pr && pn > 0 && pn <= 8 && j < pn; ++j) {
        bool have = false;
        for (int32_t i = 0; i < n && !have; ++i) have = rd<uint32_t>(recs, 0x10 * i) == rd<uint32_t>(pr, 0x10 * j);
        missing += have ? 0 : 1;
    }
    log("unlocks: speakers: %d GOTY records%s", n, missing ? " (a PS4 stub key is not among them)" : "");
    return t;
}

// ef68c757: the converted records are the PS4 layout; a new header over our copy's records.
uint8_t *build_material_params(const uint8_t *) {
    int32_t n = 0;
    const uint8_t *recs = np9_table(kMaterialParams, 0x30, n, "material parameter set");
    if (!recs) return nullptr;
    for (int32_t i = 0; i < n; ++i) {
        const uint8_t *r = recs + 0x30 * uint64_t(i);
        const uint32_t type = rd<uint32_t>(r, 0x10);
        const uint64_t need = type == 3 ? 12 : 4;
        if (!np9_string(rd<const void *>(r, 0)) || !np9_string(rd<const void *>(r, 8)) || (type != 1 && type != 3) ||
            !in_np9(rd<const void *>(r, 0x18), need) || !in_np9(rd<const void *>(r, 0x20), need) ||
            rd<uint64_t>(r, 0x28) != 0) {
            log("unlocks: net-patch9 material parameter record %d has an unknown layout; PS4 stub kept", i);
            return nullptr;
        }
    }
    uint8_t *t = static_cast<uint8_t *>(netheap_alloc(0x10, 16));
    if (!t) return nullptr;
    wr<int32_t>(t, 0, n);
    wr<int32_t>(t, 4, 0);
    wr<uint64_t>(t, 8, uint64_t(uintptr_t(recs)));
    log("unlocks: material parameter set: %d GOTY records", n);
    return t;
}

// 1ad223b4: the PS4 object with net-patch9's fields +0..+0x34.
uint8_t *build_default_settings(const uint8_t *ps4) {
    const uint8_t *g = np9_symbol(kDefaultSettings);
    if (!g || !ps4 || !in_np9(g, 0x34)) {
        log("unlocks: default game settings: %s; PS4 values kept", g ? "no PS4 object" : "not in net-patch9");
        return nullptr;
    }
    const int32_t health = rd<int32_t>(g, 0);
    if (health <= 0 || health > 1000) {
        log("unlocks: net-patch9 default game settings: health %d out of range; PS4 values kept", health);
        return nullptr;
    }
    auto *t = static_cast<uint8_t *>(netheap_alloc(0x40, 16));
    if (!t) return nullptr;
    std::memset(t, 0, 0x40);
    std::memcpy(t, ps4, 0x38);
    std::memcpy(t, g, 0x34);
    log("unlocks: default game settings: player health %d (PS4 %d)", health, rd<int32_t>(ps4, 0));
    return t;
}

void build_extras() {
    for (Extra &e : g_extra) {
        switch (e.sid) {
        case kNpcInfo: e.ours = build_npc_info(e.ps4); break;
        case kCharAttrs: e.ours = build_char_attrs(e.ps4); break;
        case kSpeakers: e.ours = build_speakers(e.ps4); break;
        case kMaterialParams: e.ours = build_material_params(e.ps4); break;
        case kDefaultSettings: e.ours = build_default_settings(e.ps4); break;
        default: break;
        }
    }
}

// ---- spawn diagnostics (test run 41: no player model) ---------------------------------------------
// The net player's model: tracker +0x138 = skin index into 5a4731d2 (AssignTeams 0x640a08 copies it
// from the member bytes +0x145 hero / +0x146 villain; those come from 0x6145a7, the index of
// NetPlayerData +0x10c / +0x110 in the table). LoadLevels requests rec +8 (0x641adf -> 0x56d170) and the
// spawn (0x663280 / 0x6636b0) resolves it with 0x56a980(package table, name) into tracker +0x290: the
// name's sid is looked up in the table at 0x14705f0 (count 0x14775f0, entries 0x38: +0x20 sid, +0x28
// package name), then that package in the package table. A NULL result = no model. Both calls
// (0x663339, 0x663782) are retargeted to log the name, the result, the package state and where the
// actor file comes from (the PS4 game or the import overlay).
constexpr uint64_t fArtLookup = 0x56a980, kArtCallA = 0x663339, kArtCallB = 0x663782;
constexpr uint64_t kArtTable = 0x14705f0, kArtCount = 0x14775f0, ART_STRIDE = 0x38;
using ArtFn = void *(U2MP_GUEST *)(void *, const char *);
std::atomic<int> g_art_log{32};

const char *actor_source(const char *name) {
    char host[1024];
    const std::string guest_path = std::string("/app0/u2data/build/main/actor27/ps4/") + name + ".pak";
    if (runtime_file_translate(guest_path.c_str(), host, sizeof host) != 0) return "no file";
    std::FILE *f = std::fopen(host, "rb");
    if (!f) return "no file";
    std::fclose(f);
    return std::strstr(host, "CUSA02320") ? "PS4 game" : "import overlay";
}

void *art_lookup(void *table, const char *name, const char *site) {
    void *r = guest<ArtFn>(fArtLookup)(table, name);
    if (g_art_log.load(std::memory_order_relaxed) <= 0) return r;
    g_art_log.fetch_sub(1, std::memory_order_relaxed);
    const uint32_t id = name ? sid(name) : 0;
    const uint8_t *pkg = id ? package_by_id(id) : nullptr;
    int entries = 0, alias = 0;
    const int32_t n = G<int32_t>(kArtCount);
    for (int32_t i = 0; i < n && i < 512; ++i) {   // 0x7000 / 0x38 entries
        const uint8_t *e = image.at<uint8_t>(kArtTable + ART_STRIDE * uint64_t(i));
        ++entries;
        if (rd<uint32_t>(e, 0x20) == id) alias = i + 1;
    }
    log("skins: spawn %s: model '%s' -> %p; package %08x %s (state %d), actor file from %s; name table %d "
        "entries, %s", site, name ? name : "(null)", r, id, pkg ? "loaded" : "not loaded",
        pkg ? rd<int32_t>(pkg, 0x30) : -1, name ? actor_source(name) : "-", entries,
        alias ? "name listed" : "name NOT listed");
    return r;
}
U2MP_GUEST void *art_lookup_a(void *table, const char *name) { return art_lookup(table, name, "0x663280"); }
U2MP_GUEST void *art_lookup_b(void *table, const char *name) { return art_lookup(table, name, "0x6636b0"); }

// ---- the player's skin choice (test runs 42-43: always mp-hero-year-1) ---------------------------
// 0x6144b0 (called at 0x5f2f49, 0x609551, 0x609c2b) builds the local player's data message: skin
// bytes +0x10 hero / +0x11 villain = the index in 5a4731d2 of the record whose id (+0x10) equals
// NetPlayerData 0x11474f8 +0x10c (hero) / +0x110 (villain). The equip code (0xa4d163 / 0xa4d1f8)
// writes those two from the store item, -1 when none is equipped. When the id is not in the table the
// index comes from 0x64b430(0x113b760, side) (calls 0x6145f5 / 0x614688): a random unlocked record of
// that side. With every skin unlocked that is any of the 107, and the PRNG state 0x1542f28 starts the
// same every time (year-1 in runs 42 and 43). The bytes then go to AssignTeams (member +0x145 / +0x146
// -> tracker +0x138), and LoadLevels requests that record's actor package.
// Here, just before 0x6144b0 runs: UNCHARTED_U2MP_SKIN=<package> (e.g. mp-hero-navarro) puts that
// record's id into both slots (the lookup ignores the side, so any skin can be forced); otherwise an
// id that is not in the table becomes Drake (b095d270: the PS4 record build_skins put over GOTY's
// first record of that id), so the default is Drake, not a random skin. Every decision is logged.
constexpr uint64_t fPlayerData = 0x6144b0, gNetPlayerData = 0x11474f8, fRandomSkin = 0x64b430;
constexpr uint64_t kPlayerDataCalls[] = {0x5f2f49, 0x609551, 0x609c2b};
constexpr uint64_t kRandomHero = 0x6145f5, kRandomVillain = 0x614688;
constexpr uint32_t kDrakeId = 0xb095d270;
using PlayerDataFn = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
using RandomSkinFn = uint64_t(U2MP_GUEST *)(void *, int32_t);
std::string g_forced_skin;
std::atomic<int> g_skin_log{32};

bool skin_log_slot() {
    if (g_skin_log.load(std::memory_order_relaxed) <= 0) return false;
    g_skin_log.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

// Index of the record with this id (the search of 0x6145c3), -1 when none; `package` gets its package.
int32_t skin_index(uint32_t id, const char **package) {
    const uint8_t *t = dc_object(kSkinTable);
    if (!t) return -1;
    const int32_t n = rd<int32_t>(t, 0);
    const uint8_t *recs = rd<const uint8_t *>(t, 8);
    for (int32_t i = 0; recs && i < n && i < 512; ++i)
        if (rd<uint32_t>(recs + SKIN_STRIDE * uint64_t(i), 0x10) == id) {
            if (package) *package = rd<const char *>(recs + SKIN_STRIDE * uint64_t(i), 8);
            return i;
        }
    return -1;
}

// The id of the first record whose package is `name`, 0 when none.
uint32_t skin_id_of(const char *name) {
    const uint8_t *t = dc_object(kSkinTable);
    if (!t || !name || !*name) return 0;
    const int32_t n = rd<int32_t>(t, 0);
    const uint8_t *recs = rd<const uint8_t *>(t, 8);
    for (int32_t i = 0; recs && i < n && i < 512; ++i) {
        const uint8_t *r = recs + SKIN_STRIDE * uint64_t(i);
        const char *pk = rd<const char *>(r, 8);
        if (pk && std::strcmp(pk, name) == 0) return rd<uint32_t>(r, 0x10);
    }
    return 0;
}

void choose_skin(const char *side_name, uint64_t offset, uint32_t forced) {
    uint32_t &slot = G<uint32_t>(gNetPlayerData + offset);
    const uint32_t before = slot;
    const char *pk = nullptr;
    const int32_t at = skin_index(before, &pk);
    const char *what = "kept";
    if (forced) {
        slot = forced;
        what = "UNCHARTED_U2MP_SKIN";
    } else if (at < 0) {
        slot = kDrakeId;
        what = "not in the table: Drake";
    }
    if (skin_log_slot()) {
        const char *now = nullptr;
        const int32_t now_at = skin_index(slot, &now);
        log("skins: %s id %08x (index %d '%s') -> %08x (index %d '%s'): %s", side_name, before, at, pk ? pk : "-",
            slot, now_at, now ? now : "-", what);
    }
}

bool g_skin_hooks = false;   // install found 0x6144b0 as expected

void apply_skin_choice(const char *when) {
    if (!g_skin_hooks || !dc_object(kSkinTable)) return;
    uint32_t forced = 0;
    if (!g_forced_skin.empty()) {
        forced = skin_id_of(g_forced_skin.c_str());
        if (!forced && skin_log_slot())
            log("skins: UNCHARTED_U2MP_SKIN=%s is not a package of the skin table; not forced", g_forced_skin.c_str());
    }
    if (skin_log_slot()) log("skins: choice applied (%s)", when);
    choose_skin("hero", 0x10c, forced);
    choose_skin("villain", 0x110, forced);
}

U2MP_GUEST uint64_t player_data_chosen(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    apply_skin_choice("player data message 0x6144b0");
    return guest<PlayerDataFn>(fPlayerData)(a0, a1, a2, a3, a4, a5);
}

U2MP_GUEST uint64_t random_skin_logged(void *list, int32_t side) {
    const uint64_t r = guest<RandomSkinFn>(fRandomSkin)(list, side);
    if (skin_log_slot()) {
        const uint8_t *t = dc_object(kSkinTable);
        const char *pk = nullptr;
        const int32_t i = int32_t(uint32_t(r));
        if (t && i >= 0 && i < rd<int32_t>(t, 0)) pk = rd<const char *>(rd<const uint8_t *>(t, 8) + SKIN_STRIDE * uint64_t(i), 8);
        log("skins: random pick (0x64b430), side %d -> index %d '%s'", side, i, pk ? pk : "-");
    }
    return r;
}

}  // namespace

void unlocks_refresh() {
    if (!g_ready || !image.base || net_state() <= 1 || !G<uint64_t>(gHashBuckets)) return;
    // the skin choice once our skin table is bound (in the lobby, before LoadLevels requests the
    // package), so it does not depend on when 0x6144b0 builds the player data message
    static bool skin_applied = false;
    if (!skin_applied && g_skins && dc_object(kSkinTable) == g_skins) {
        skin_applied = true;
        apply_skin_choice("skin table bound");
    }
    if (g_bound.load(std::memory_order_acquire)) {
        // cheap re-check: still ours (a later bind of all net-patch9 symbols would replace them)
        const uint8_t *skins = dc_object(kSkinTable), *boosters = dc_object(kBoosterTable);
        bool extras_ours = true;
        for (const Extra &e : g_extra) extras_ours = extras_ours && (!e.ours || dc_object(e.sid) == e.ours);
        if ((!g_skins || skins == g_skins) && (!g_store || dc_object(kStoreTable) == g_store) &&
            (!g_boosters || boosters == g_boosters) &&
            (!g_boosters || booster_list_ours()) && extras_ours)
            return;
    }
    std::lock_guard<std::mutex> lock(g_tables_lock);
    if (g_given_up) return;
    auto bind = guest<BindFn>(fBindSymbol);
    if (!g_np9) {
        static const uint8_t *ps4_skins = nullptr;
        ps4_skins = dc_object(kSkinTable);   // the PS4 stub (before anything of ours is bound)
        for (Extra &e : g_extra) e.ps4 = dc_object(e.sid);
        if (!load_np9()) {
            log("unlocks: %s not imported: the PS4 skin / booster tables stay (2 skins, no boosters)", kNetPatch);
            g_given_up = true;
            g_bound.store(true, std::memory_order_release);   // nothing to bind: stop retrying
            return;
        }
        build_skins(ps4_skins);
        build_store();
        build_boosters();
        if (g_extra_on) build_extras();
        else log("unlocks: UNCHARTED_U2MP_GOTY_TABLES=off: the other net.bin stubs stay");
        bool any_extra = false;
        for (const Extra &e : g_extra) any_extra = any_extra || e.ours;
        if (!g_skins && !g_boosters && !g_store && !any_extra) {
            g_given_up = true;
            g_bound.store(true, std::memory_order_release);
            return;
        }
    }
    static int binds_left = 4;   // a symbol DcFind answers from elsewhere would be rebound forever
    if (g_skins && dc_object(kSkinTable) != g_skins && binds_left > 0) {
        const uint64_t v = uint64_t(uintptr_t(g_skins));
        bind(kSkinTable, &v);
        if (dc_object(kSkinTable) != g_skins) --binds_left;   // only ineffective binds count (review)
        log("unlocks: skin table 0x%08x bound (%d records)%s", kSkinTable, rd<int32_t>(g_skins, 0),
            dc_object(kSkinTable) == g_skins ? "" : " - NOT effective");
    }
    if (g_store && dc_object(kStoreTable) != g_store && binds_left > 0) {
        // the PS4 net.bin may have been reloaded: our copy points into it, so copy the current one again
        static int rebuilds = 0;
        if (rebuilds < 4) {
            ++rebuilds;
            build_store();
        }
        const uint64_t v = uint64_t(uintptr_t(g_store));
        bind(kStoreTable, &v);
        if (dc_object(kStoreTable) != g_store) --binds_left;
        log("unlocks: MP store 0x%08x bound%s", kStoreTable, dc_object(kStoreTable) == g_store ? "" : " - NOT effective");
    }
    if (g_boosters && dc_object(kBoosterTable) != g_boosters && binds_left > 0) {
        const uint64_t v = uint64_t(uintptr_t(g_boosters));
        bind(kBoosterTable, &v);
        if (dc_object(kBoosterTable) != g_boosters) --binds_left;
        log("unlocks: booster table 0x%08x bound (%d records)%s", kBoosterTable, g_booster_count,
            dc_object(kBoosterTable) == g_boosters ? "" : " - NOT effective");
    }
    for (const Extra &e : g_extra) {
        if (!e.ours || dc_object(e.sid) == e.ours || binds_left <= 0) continue;
        const uint64_t v = uint64_t(uintptr_t(e.ours));
        bind(e.sid, &v);
        if (dc_object(e.sid) != e.ours) --binds_left;
        log("unlocks: %s 0x%08x bound%s", e.what, e.sid, dc_object(e.sid) == e.ours ? "" : " - NOT effective");
    }
    if (g_boosters && !booster_list_ours()) fill_booster_list();
    g_bound.store(true, std::memory_order_release);
}

const char *unlocks_text(uint32_t id) {
    if (!id || !g_names_ready.load(std::memory_order_acquire)) return nullptr;
    for (const auto &e : g_names)
        if (e.first == id) return e.second.c_str();
    return nullptr;
}

void install_unlocks(int &installed) {
    if (const char *v = std::getenv("UNCHARTED_U2MP_UNLOCKS"); v && std::strcmp(v, "off") == 0) g_spoof = false;
    if (const char *v = std::getenv("UNCHARTED_U2MP_GOTY_TABLES"); v && std::strcmp(v, "off") == 0) g_extra_on = false;
    // 0xa4bb40: push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x28; mov r14,rsi; mov r15,rdi
    static const unsigned char locked_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                                 0x53, 0x48, 0x83, 0xec, 0x28, 0x49, 0x89, 0xf6, 0x49, 0x89, 0xff};
    // 0xa4bb82: lea rax,[0x1114178] (the debug switch); 0xa4bbdc: call 0xa4bcb0
    static const unsigned char flag_bytes[] = {0x48, 0x8d, 0x05, 0xef, 0x85, 0x6c, 0x00};
    static const unsigned char cond_call[] = {0xe8, 0xcf, 0x00, 0x00, 0x00};
    // 0xa4bcb0: the condition evaluator's prologue
    static const unsigned char cond_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                               0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec};
    // 0xa4d280: prologue, mov [rbp-0x48],rsi; xor eax,eax; test rsi,rsi; je; mov ecx,[NET_SM]; cmp ecx,2
    static const unsigned char level_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
                                                0x54, 0x53, 0x48, 0x83, 0xec, 0x48, 0x48, 0x89, 0x75, 0xb8, 0x31,
                                                0xc0, 0x48, 0x85, 0xf6, 0x0f, 0x84, 0x78, 0x01, 0x00, 0x00, 0x8b,
                                                0x0d, 0xd6, 0x16, 0xa8, 0x00, 0x83, 0xf9, 0x02};
    // 0xa4d2f4: lea rax,[0x1114178]
    static const unsigned char level_flag[] = {0x48, 0x8d, 0x05, 0x7d, 0x6e, 0x6c, 0x00};
    // the loader functions netpatch.cpp checks the same way
    static const unsigned char relocate_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                                   0x53, 0x48, 0x83, 0xec, 0x18, 0x48, 0x89, 0x7d, 0xd0, 0x40, 0xf6, 0xc7};
    static const unsigned char bind_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                               0x53, 0x48, 0x83, 0xec, 0x18, 0x4c, 0x8b, 0x0d, 0x78, 0x3e, 0x63, 0x00};
    static const unsigned char find_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec,
                                               0x18, 0x4c, 0x8b, 0x05, 0xf0, 0x58, 0x63, 0x00};

    g_ready = image.executable(fRelocate, sizeof relocate_bytes) && image.matches(fRelocate, relocate_bytes, sizeof relocate_bytes) &&
              image.executable(fBindSymbol, sizeof bind_bytes) && image.matches(fBindSymbol, bind_bytes, sizeof bind_bytes) &&
              image.executable(fDcFind, sizeof find_bytes) && image.matches(fDcFind, find_bytes, sizeof find_bytes);
    if (!g_ready) log("unlocks: DC loader functions differ: GOTY skin / booster tables not bound");

    if (image.executable(hIsLocked, 0x100) && image.matches(hIsLocked, locked_bytes, sizeof locked_bytes) &&
        image.matches(hIsLocked + 0x42, flag_bytes, sizeof flag_bytes) &&
        image.matches(hIsLocked + 0x9c, cond_call, sizeof cond_call) &&
        image.matches(fCondition, cond_bytes, sizeof cond_bytes) &&
        replace_function(hIsLocked, 16, reinterpret_cast<void *>(&is_locked), "item IsLocked (0xa4bb40)"))
        ++installed;
    else
        log("unlocks: IsLocked 0xa4bb40 differs, multiplayer items keep their rank conditions");

    if (image.executable(hUpgradeLevel, 0x80) && image.matches(hUpgradeLevel, level_bytes, sizeof level_bytes) &&
        image.matches(hUpgradeLevel + 0x74, level_flag, sizeof level_flag) &&
        replace_function(hUpgradeLevel, 16, reinterpret_cast<void *>(&upgrade_level), "co-op upgrade level (0xa4d280)"))
        ++installed;
    else
        log("unlocks: co-op upgrade level 0xa4d280 differs, upgrades stay as bought");

    if (retarget_call(kArtCallA, fArtLookup, reinterpret_cast<void *>(&art_lookup_a), "spawn model lookup A") &&
        retarget_call(kArtCallB, fArtLookup, reinterpret_cast<void *>(&art_lookup_b), "spawn model lookup B"))
        ++installed;
    else
        log("unlocks: spawn model lookups (0x663339 / 0x663782) differ, no spawn log");

    // the player's skin choice: default Drake, UNCHARTED_U2MP_SKIN=<package> forces one
    if (const char *v = std::getenv("UNCHARTED_U2MP_SKIN"); v && *v) g_forced_skin = v;
    static const unsigned char data_prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                                  0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18};
    // 0x6145a7: mov ebx,[r15+0x10c]; mov edi,0x5a4731d2   0x614632: mov ebx,[r15+0x110]; mov edi,0x5a4731d2
    static const unsigned char hero_read[] = {0x41, 0x8b, 0x9f, 0x0c, 0x01, 0x00, 0x00, 0xbf, 0xd2, 0x31, 0x47, 0x5a};
    static const unsigned char villain_read[] = {0x41, 0x8b, 0x9f, 0x10, 0x01, 0x00, 0x00, 0xbf, 0xd2, 0x31, 0x47, 0x5a};
    if (image.matches(fPlayerData, data_prologue, sizeof data_prologue) &&
        image.matches(0x6145a7, hero_read, sizeof hero_read) && image.matches(0x614632, villain_read, sizeof villain_read)) {
        int n = 0;
        for (uint64_t c : kPlayerDataCalls)
            n += retarget_call(c, fPlayerData, reinterpret_cast<void *>(&player_data_chosen), "skins: player data skin choice");
        n += retarget_call(kRandomHero, fRandomSkin, reinterpret_cast<void *>(&random_skin_logged), "skins: random hero pick");
        n += retarget_call(kRandomVillain, fRandomSkin, reinterpret_cast<void *>(&random_skin_logged),
                           "skins: random villain pick");
        if (n) ++installed;
        g_skin_hooks = true;
        log("skins: default skin Drake when none is equipped%s%s", g_forced_skin.empty() ? "" : "; forced: ",
            g_forced_skin.c_str());
    } else {
        log("skins: player data 0x6144b0 differs, skin choice left alone");
    }

    log("unlocks: %s", g_spoof ? "every multiplayer skin, booster, co-op weapon upgrade and taunt unlocked"
                                : "spoof off (UNCHARTED_U2MP_UNLOCKS=off); GOTY tables only");
}

}  // namespace u2mp
