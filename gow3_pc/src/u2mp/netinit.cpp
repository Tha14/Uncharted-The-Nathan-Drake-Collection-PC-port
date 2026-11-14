// u2mp netinit: the state the PS3 multiplayer start-up used to create, set up again for PS4 code that still
// reads it.
//
// PS4 1.02 lost NetInit, the connecting page update and NetGameManager::Init, but the remaining code still
// expects what they built: two hash tables inside the NetGameManager (the match-start reset walks their
// bucket arrays and crashes on NULL), an empty recent-game history, the leave status of the match and party
// sessions, and the list of boosters. session.cpp calls in here when Multiplayer is entered and when a match
// starts; no game code is patched.
//
// Reimplemented from the behaviour description u2mp/docs/spec/progression.md.
#include <cstdint>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {

namespace {

// ---------------------------------------------------------------------------------------------
// Game image offsets (PS4 1.02).

constexpr uint64_t kTableA = 0x113d300;          // NetGameManager +0x1ba0
constexpr uint64_t kTableB = 0x113d370;          // NetGameManager +0x1c10
constexpr uint64_t kGameHistory = 0x113d402;     // 4 x s16
constexpr uint64_t kMatchLeaveStatus = 0x1065bd0;
constexpr uint64_t kPartyLeaveStatus = 0x105f080;
constexpr uint64_t kBoosterList = 0x113a788;     // u32 count, then pointers from +8
constexpr uint32_t kMaxBoosters = 63;
constexpr uint64_t kDcFind = 0xa15300;
constexpr uint32_t kBoostersSid = 0x916e03b0;

// Hash table fields.
constexpr uint64_t kHtBuckets = 0x00;
constexpr uint64_t kHtFree = 0x08;
constexpr uint64_t kHtNodeSize = 0x10;
constexpr uint64_t kHtNodeCount = 0x14;
constexpr uint64_t kHtPool = 0x18;
constexpr uint64_t kHtSentinel = 0x20;
constexpr uint64_t kHtAlloc = 0x28;
constexpr uint64_t kHtBucketsEnd = 0x30;
constexpr uint64_t kHtBucketCount = 0x3c;
constexpr uint64_t kHtReady = 0x40;

// The PS4 code this file stands in for: if any of it differs, the layout assumptions here do not hold.
struct CodeCheck {
    uint64_t offset;
    const char *label;
    unsigned char bytes[16];
    size_t length;
};

const CodeCheck kChecks[] = {
    {0x640fe4, "reset table A", {0x49, 0x8b, 0xbf, 0xa0, 0x1b, 0x00, 0x00}, 7},
    {0x641012, "reset table A buckets", {0x4d, 0x63, 0x87, 0xdc, 0x1b, 0x00, 0x00}, 7},
    {0x6410e0, "table A node size", {0x4c, 0x8d, 0x60, 0x60}, 4},
    {0x64119e, "reset table B", {0x49, 0x8b, 0xbf, 0x10, 0x1c, 0x00, 0x00}, 7},
    {0x6411d2, "reset table B buckets", {0x49, 0x63, 0x8f, 0x4c, 0x1c, 0x00, 0x00}, 7},
    {0x641290, "table B node size", {0x4c, 0x8d, 0x68, 0x38}, 4},
    {0x5d90bb, "hash table layout", {0x41, 0xc7, 0x47, 0x10, 0x58, 0x00, 0x00}, 7},
    {0x5ec590, "session leave status",
     {0x48, 0x8d, 0x0d, 0x71, 0x95, 0xa7, 0x00, 0x31, 0xc0, 0x80, 0xb9, 0xc8, 0x00, 0x00, 0x00, 0x00}, 16},
    {0x5f6750, "booster list", {0x48, 0x8b, 0x44, 0xd7, 0x08, 0x39, 0x70, 0x0c, 0x74}, 9},
    {0x5fd8ef, "booster object", {0x48, 0x8d, 0x05, 0x92, 0xce, 0xb3, 0x00}, 7},
    {0xb332a0, "Alloc", {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x41, 0x89}, 16},
    {0xa15300, "DcFind", {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x18, 0x4c, 0x8b, 0x05, 0xf0, 0x58, 0x63, 0x00}, 16},
};

// ---------------------------------------------------------------------------------------------
// Memory access (little-endian, possibly unaligned).

template <typename T> T get(const uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <typename T> void put(uint8_t *p, T v) { std::memcpy(p, &v, sizeof v); }

uint64_t address_of(const uint8_t *p) { return uint64_t(reinterpret_cast<uintptr_t>(p)); }
uint8_t *pointer_at(const uint8_t *p) { return reinterpret_cast<uint8_t *>(uintptr_t(get<uint64_t>(p))); }

// ---------------------------------------------------------------------------------------------
// Module state.

bool g_checked = false;
bool g_checks_ok = false;
bool g_history_done = false;
bool g_boosters_done = false;

bool code_matches() {
    if (!g_checked) {
        g_checked = true;
        g_checks_ok = true;
        for (const CodeCheck &c : kChecks) {
            if (image.matches(c.offset, c.bytes, c.length)) continue;
            log("netinit: bytes at %#llx (%s) differ; nothing done", (unsigned long long)c.offset, c.label);
            g_checks_ok = false;
        }
    }
    return g_checks_ok;
}

// Builds a hash table with an empty bucket array and every node on the free list, in one allocation:
// bucket slots (plus one that points at the sentinel), then the node pool, then the sentinel node.
void build_table(uint64_t offset, uint32_t entries, uint32_t node_size, const char *name) {
    uint8_t *table = image.at<uint8_t>(offset);
    if (get<uint64_t>(table + kHtBuckets) != 0 && table[kHtReady] != 0) return;

    uint32_t buckets = entries - 1;
    buckets |= buckets >> 1;
    buckets |= buckets >> 2;
    buckets |= buckets >> 4;
    buckets |= buckets >> 8;
    buckets |= buckets >> 16;
    buckets += 1;
    const uint64_t pool_offset = (uint64_t(buckets) * 8 + 8 + 15) & ~uint64_t(15);
    const uint64_t size = pool_offset + (uint64_t(entries) + 1) * node_size;

    uint8_t *mem = static_cast<uint8_t *>(netheap_alloc(size, 16));
    if (!mem) {
        log("netinit: %s: allocation of %llu bytes failed", name, (unsigned long long)size);
        return;
    }

    uint8_t *pool = mem + pool_offset;
    uint8_t *sentinel = pool + uint64_t(entries) * node_size;
    for (uint32_t i = 0; i < buckets; ++i) put<uint64_t>(mem + 8 * uint64_t(i), 0);
    put<uint64_t>(mem + 8 * uint64_t(buckets), address_of(sentinel));
    put<uint64_t>(sentinel + 0, address_of(sentinel));
    put<uint64_t>(sentinel + 8, address_of(sentinel));
    for (uint32_t i = 0; i < entries; ++i) {
        uint8_t *node = pool + uint64_t(i) * node_size;
        put<uint64_t>(node + 0, i + 1 < entries ? address_of(node + node_size) : 0);
        put<uint64_t>(node + 8, i > 0 ? address_of(node - node_size) : 0);
    }

    put<uint64_t>(table + kHtFree, address_of(pool));
    put<uint32_t>(table + kHtNodeSize, node_size);
    put<uint32_t>(table + kHtNodeCount, entries);
    put<uint64_t>(table + kHtPool, address_of(pool));
    put<uint64_t>(table + kHtSentinel, address_of(sentinel));
    put<uint64_t>(table + kHtAlloc, address_of(mem));
    put<uint64_t>(table + kHtBucketsEnd, address_of(mem + 8 * uint64_t(buckets)));
    put<int32_t>(table + kHtBucketCount, int32_t(buckets));
    put<uint64_t>(table + kHtBuckets, address_of(mem));
    table[kHtReady] = 1;
    log("netinit: %s: %d entries of %#x, %d buckets", name, int(entries), node_size, int(buckets));
}

// Lists the boosters of the *boosters* DC table. When some boosters carry a DLC mask, only those count
// (the PS4 table also holds placeholders without one).
void list_boosters() {
    if (g_boosters_done) return;
    uint8_t *list = image.at<uint8_t>(kBoosterList);
    if (get<uint32_t>(list) != 0) {
        g_boosters_done = true;
        return;
    }
    const uint8_t *slot =
        static_cast<const uint8_t *>(reinterpret_cast<void *(U2MP_GUEST *)(uint32_t)>(image.at<uint8_t>(kDcFind))(kBoostersSid));
    const uint8_t *table = slot ? pointer_at(slot) : nullptr;
    if (!table) {
        log("netinit: *boosters* not loaded yet");
        return;
    }
    const int32_t n = get<int32_t>(table);
    uint8_t *records = pointer_at(table + 8);
    if (n <= 0 || !records) return;

    auto mask = [&](int32_t i) { return get<uint32_t>(records + 0x20 * uint64_t(i) + 0x18); };
    bool any_mask = false;
    for (int32_t i = 0; i < n; ++i)
        if (mask(i) != 0) any_mask = true;

    uint32_t listed = 0;
    for (int32_t i = 0; i < n && listed < kMaxBoosters; ++i) {
        if (any_mask && mask(i) == 0) continue;
        put<uint64_t>(list + 8 + 8 * uint64_t(listed), address_of(records + 0x20 * uint64_t(i)));
        ++listed;
    }
    put<uint32_t>(list, listed);
    g_boosters_done = true;
    log("netinit: %u of %d boosters listed", listed, n);
}

void prepare() {
    if (!code_matches()) return;
    if (!g_history_done) {
        g_history_done = true;
        std::memset(image.at<uint8_t>(kGameHistory), 0xff, 8);   // four s16 of -1: no recent games
    }
    *image.at<uint8_t>(kMatchLeaveStatus) = 1;
    *image.at<uint8_t>(kPartyLeaveStatus) = 1;
    build_table(kTableA, 16, 0x60, "ngm table 0x1ba0");
    build_table(kTableB, 256, 0x38, "ngm table 0x1c10");
    list_boosters();
}

void dump_table(const char *when, const char *label, uint64_t offset) {
    const uint8_t *table = image.at<uint8_t>(offset);
    const uint64_t buckets = get<uint64_t>(table + kHtBuckets);
    const uint64_t end = get<uint64_t>(table + kHtBucketsEnd);
    const int32_t n = get<int32_t>(table + kHtBucketCount);
    int used = 0, minus = 0;
    if (buckets != 0 && n > 0 && n <= 4096) {
        const uint8_t *slots = reinterpret_cast<const uint8_t *>(uintptr_t(buckets));
        for (int32_t i = 0; i <= n; ++i) {
            const uint64_t v = get<uint64_t>(slots + 8 * uint64_t(i));
            if (v != 0) ++used;
            if (v == ~uint64_t(0)) ++minus;
        }
    }
    const bool ok = buckets != 0 && end == buckets + uint64_t(int64_t(n)) * 8;
    log("netinit[%s] table %s: buckets %#llx end %#llx (%s) n %d free %#llx pool %#llx sentinel %#llx ready %u; "
        "non-null slots %d, -1 slots %d",
        when, label, (unsigned long long)buckets, (unsigned long long)end, ok ? "ok" : "MISMATCH", n,
        (unsigned long long)get<uint64_t>(table + kHtFree), (unsigned long long)get<uint64_t>(table + kHtPool),
        (unsigned long long)get<uint64_t>(table + kHtSentinel), unsigned(table[kHtReady]), used, minus);
}

}  // namespace

void netinit_prepare() { prepare(); }

void netinit_match() { prepare(); }

void netinit_dump(const char *when) {
    dump_table(when, "0x1ba0", kTableA);
    dump_table(when, "0x1c10", kTableB);
}

}  // namespace u2mp
