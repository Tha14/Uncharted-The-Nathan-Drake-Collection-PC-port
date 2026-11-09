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
    const int32_t total = n + (ps4_ok ? ps4_n : 0);
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
    wr<int32_t>(mem, 0, count);
    wr<uint64_t>(mem, 8, uint64_t(uintptr_t(out)));
    g_skins = mem;
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

}  // namespace

void unlocks_refresh() {
    if (!g_ready || !image.base || net_state() <= 1 || !G<uint64_t>(gHashBuckets)) return;
    if (g_bound.load(std::memory_order_acquire)) {
        // cheap re-check: still ours (a later bind of all net-patch9 symbols would replace them)
        const uint8_t *skins = dc_object(kSkinTable), *boosters = dc_object(kBoosterTable);
        if ((!g_skins || skins == g_skins) && (!g_boosters || boosters == g_boosters) &&
            (!g_boosters || booster_list_ours()))
            return;
    }
    std::lock_guard<std::mutex> lock(g_tables_lock);
    if (g_given_up) return;
    auto bind = guest<BindFn>(fBindSymbol);
    if (!g_np9) {
        static const uint8_t *ps4_skins = nullptr;
        ps4_skins = dc_object(kSkinTable);   // the PS4 stub (before anything of ours is bound)
        if (!load_np9()) {
            log("unlocks: %s not imported: the PS4 skin / booster tables stay (2 skins, no boosters)", kNetPatch);
            g_given_up = true;
            g_bound.store(true, std::memory_order_release);   // nothing to bind: stop retrying
            return;
        }
        build_skins(ps4_skins);
        build_boosters();
        if (!g_skins && !g_boosters) {
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
    if (g_boosters && dc_object(kBoosterTable) != g_boosters && binds_left > 0) {
        const uint64_t v = uint64_t(uintptr_t(g_boosters));
        bind(kBoosterTable, &v);
        if (dc_object(kBoosterTable) != g_boosters) --binds_left;
        log("unlocks: booster table 0x%08x bound (%d records)%s", kBoosterTable, g_booster_count,
            dc_object(kBoosterTable) == g_boosters ? "" : " - NOT effective");
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

    log("unlocks: %s", g_spoof ? "every multiplayer skin, booster, co-op weapon upgrade and taunt unlocked"
                                : "spoof off (UNCHARTED_U2MP_UNLOCKS=off); GOTY tables only");
}

}  // namespace u2mp
