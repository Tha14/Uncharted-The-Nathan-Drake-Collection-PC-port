// u2mp netinit: runtime state that the PS3 multiplayer initialisation set up and that the PS4 1.02 build
// never sets up, because the code that did it (NetInit 0x447a5c, the PAGE_MULTIPLAYER_CONNECTING update
// 0x445020 and NetGameManager::Init 0x49c740, all PS3 1.10) is gone. Static analysis only; the
// findings, readers and PS3 sources are in u2mp/docs/NETINIT.md.
//
//   netinit_prepare()  once when Multiplayer is entered (idempotent):
//     1. NetGameManager hash tables ngm+0x1ba0 (16 entries, node 0x60) and ngm+0x1c10 (256 entries,
//        node 0x38): PS3 NetGameManager::Init 0x49c740 -> HashTable::Init 0x4950f0 / 0x494ee0.
//        Without them the game reset at match start (0x640fc0, PS3 0x4a1be8) reads a null bucket array.
//     2. ngm+0x1ca2..0x1ca9 = 0xff (recent-game history, PS3 memset(ngm+6382, -1, 8) in 0x49c740).
//     3. Session leave status +0xc8 = 1 on the match (0x1065b08) and party (0x105efb8) sessions
//        (PS3 Session::Init 0x4d1e2c, run by NetInit); the leaving pages wait while it is 0.
//     4. The booster list 0x113a788 (PS3 NetBoosterManager init 0x4554c0 on 0xf1b2c0): every
//        GetAmount (0x5f6410) returns the default while it is empty. Needs the "net" dc data, so it is
//        retried by netinit_match() when the data was not loaded yet.
//   netinit_match()    at match start (idempotent): the same, for anything not done yet.
#include <cstddef>
#include <cstdint>

#include "u2mp_internal.h"

namespace u2mp {
namespace {

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }

// ---- guest addresses (PS4 1.02 image offsets) -------------------------------------------------
constexpr uint64_t kNgm = 0x113b760;          // g_netGameManager (PS3 0xf1dc80)
constexpr uint64_t kMatch = 0x1065b08;        // g_matchSession (PS3 0xf20c48)
constexpr uint64_t kParty = 0x105efb8;        // party session (PS3 0xf2c8a0)
constexpr uint64_t kBoosters = 0x113a788;     // booster list {u32 count; pad; rec* recs[64]} (PS3 0xf1b2c0)
constexpr uint64_t kAllocTablePtr = 0x104ae00;   // allocator context table: [ptr] + ctx*8 -> heap

constexpr uint64_t fAlloc = 0xb332a0;         // void *Alloc(u64 size, const int *ctx, u32 align)
constexpr uint64_t fDcFind = 0xa15300;        // void **DcFind(u32 sid): slot or NULL, no assert (0xa15e70 asserts)

constexpr int kCtxNetwork = 0x14, kCtxDefault = 0x46;

// NetGameManager fields.
constexpr uint64_t NGM_TABLE_A = 0x1ba0, NGM_TABLE_B = 0x1c10;   // PS3 ngm+6260 / ngm+6304
constexpr uint64_t NGM_RECENT_GAMES = 0x1ca2;                     // 4 x s16, PS3 ngm+6382
constexpr uint64_t S_LEFT = 0xc8;                                 // PS3 session +188

// Embedded hash table (ndlib HashTable, 64-bit layout, read by 0x640fc0, 0x645065, 0x64d910 and the
// inlined instance at 0x5d9030):
constexpr uint64_t HT_BUCKETS = 0x00;    // node *buckets[num_buckets + 1]; the extra slot holds the sentinel
constexpr uint64_t HT_FREE = 0x08;       // free list head
constexpr uint64_t HT_NODE_SIZE = 0x10;  // u32
constexpr uint64_t HT_COUNT = 0x14;      // u32 pool nodes
constexpr uint64_t HT_POOL = 0x18;       // first pool node
constexpr uint64_t HT_SENTINEL = 0x20;   // pool + count * node_size (end node)
constexpr uint64_t HT_MEMORY = 0x28;     // the allocation
constexpr uint64_t HT_BUCKETS_END = 0x30;   // &buckets[num_buckets]
constexpr uint64_t HT_NUM_BUCKETS = 0x3c;   // s32
constexpr uint64_t HT_READY = 0x40;      // u8
// node: +0 next, +8 prev, +0x10 u32 key, +0x18 value

constexpr uint32_t kBoosterTable = 0x916e03b0;   // *boosters*: {s32 count; rec* at +8}, stride 0x20
constexpr uint64_t BOOST_STRIDE = 0x20, BOOST_MASK = 0x18;
constexpr int kMaxBoosters = 63;                  // PS3 0x4554c0 stopped at 63

// ---- checks -------------------------------------------------------------------------------------
bool code_checked = false, code_ok = false;

bool check_code() {
    if (code_checked) return code_ok;
    code_checked = true;
    struct Want { uint64_t at; unsigned char bytes[16]; size_t n; const char *what; };
    static const Want wants[] = {
        // the game reset that faulted: mov rdi,[r15+0x1ba0] / movsxd r8,[r15+0x1bdc] / node 0x60 /
        // mov rdi,[r15+0x1c10] / movsxd rcx,[r15+0x1c4c] / node 0x38
        {0x640fe4, {0x49, 0x8b, 0xbf, 0xa0, 0x1b, 0x00, 0x00}, 7, "reset table A"},
        {0x641012, {0x4d, 0x63, 0x87, 0xdc, 0x1b, 0x00, 0x00}, 7, "reset table A buckets"},
        {0x6410e0, {0x4c, 0x8d, 0x60, 0x60}, 4, "table A node size"},
        {0x64119e, {0x49, 0x8b, 0xbf, 0x10, 0x1c, 0x00, 0x00}, 7, "reset table B"},
        {0x6411d2, {0x49, 0x63, 0x8f, 0x4c, 0x1c, 0x00, 0x00}, 7, "reset table B buckets"},
        {0x641290, {0x4c, 0x8d, 0x68, 0x38}, 4, "table B node size"},
        // the inlined HashTable::Init at 0x5d9030: mov dword [r15+0x10], 0x58 (node size field)
        {0x5d90bb, {0x41, 0xc7, 0x47, 0x10, 0x58, 0x00, 0x00}, 7, "hash table layout"},
        // match session leave-status reader (LEAVING_GAME area): lea rcx,[0x1065b08]; xor eax,eax;
        // cmp byte [rcx+0xc8],0
        {0x5ec590, {0x48, 0x8d, 0x0d, 0x71, 0x95, 0xa7, 0x00, 0x31, 0xc0, 0x80, 0xb9, 0xc8, 0x00, 0x00, 0x00, 0x00},
         16, "session leave status"},
        // booster GetAmount scan: mov rax,[rdi+rdx*8+8]; cmp [rax+0xc],esi; je
        {0x5f6750, {0x48, 0x8b, 0x44, 0xd7, 0x08, 0x39, 0x70, 0x0c, 0x74}, 9, "booster list"},
        // lea rax,[0x113a788] in a booster count reader
        {0x5fd8ef, {0x48, 0x8d, 0x05, 0x92, 0xce, 0xb3, 0x00}, 7, "booster object"},
        // function prologues
        {fAlloc, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x41, 0x89}, 16, "Alloc"},
        {fDcFind, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x18, 0x4c, 0x8b, 0x05, 0xf0, 0x58, 0x63, 0x00}, 16, "DcFind"},
    };
    code_ok = true;
    for (const Want &w : wants) {
        if (!image.matches(w.at, w.bytes, w.n)) {
            log("netinit: bytes at %#llx (%s) differ; nothing done", static_cast<unsigned long long>(w.at), w.what);
            code_ok = false;
        }
    }
    return code_ok;
}

// ---- hash tables --------------------------------------------------------------------------------
void *game_alloc(uint64_t size, uint32_t align) {
    using AllocFn = void *(U2MP_GUEST *)(uint64_t, const int *, uint32_t);
    const uint64_t table = G<uint64_t>(kAllocTablePtr);
    // the network context like the rest of the net code, once its heap exists (session.cpp points it
    // at the default heap); the default heap otherwise
    const bool net_heap = table && *reinterpret_cast<const uint64_t *>(uintptr_t(table) + kCtxNetwork * 8);
    const int ctx = net_heap ? kCtxNetwork : kCtxDefault;
    return guest<AllocFn>(fAlloc)(size, &ctx, align);
}

// HashTable::Init(entries, node size) (PS3 0x4950f0 / 0x494ee0; PS4 only has inlined instances, e.g.
// 0x5d9030 with 256 x 0x58). One block: buckets (+1 sentinel slot), 16-aligned pool of `entries`
// nodes, then the sentinel node.
bool init_table(uint64_t t, int32_t entries, uint32_t node_size, const char *name) {
    if (G<uint64_t>(t + HT_BUCKETS) != 0 && G<uint8_t>(t + HT_READY)) return true;
    int32_t mask = entries - 1;   // PS3: smear(entries - 1) + 1 buckets
    mask |= mask >> 1; mask |= mask >> 2; mask |= mask >> 4; mask |= mask >> 8; mask |= mask >> 16;
    const int32_t buckets = mask + 1;
    const uint64_t pool_offset = (uint64_t(buckets) * 8 + 8 + 15) & ~uint64_t(15);
    const uint64_t size = pool_offset + uint64_t(entries + 1) * node_size;
    auto *mem = static_cast<uint8_t *>(game_alloc(size, 16));
    if (!mem) {
        log("netinit: %s: allocation of %llu bytes failed", name, static_cast<unsigned long long>(size));
        return false;
    }
    uint8_t *pool = mem + pool_offset;
    uint8_t *sentinel = pool + uint64_t(entries) * node_size;
    auto slot = [](uint8_t *p, uint64_t off) -> uint64_t & { return *reinterpret_cast<uint64_t *>(p + off); };
    for (int32_t i = 0; i < buckets; ++i) slot(mem, uint64_t(i) * 8) = 0;
    slot(mem, uint64_t(buckets) * 8) = uint64_t(uintptr_t(sentinel));
    slot(sentinel, 0) = uint64_t(uintptr_t(sentinel));
    slot(sentinel, 8) = uint64_t(uintptr_t(sentinel));
    for (int32_t i = 0; i < entries; ++i) {
        uint8_t *n = pool + uint64_t(i) * node_size;
        slot(n, 0) = i + 1 < entries ? uint64_t(uintptr_t(n + node_size)) : 0;
        slot(n, 8) = i > 0 ? uint64_t(uintptr_t(n - node_size)) : 0;
    }
    G<uint64_t>(t + HT_FREE) = uint64_t(uintptr_t(pool));
    G<uint32_t>(t + HT_NODE_SIZE) = node_size;
    G<uint32_t>(t + HT_COUNT) = uint32_t(entries);
    G<uint64_t>(t + HT_POOL) = uint64_t(uintptr_t(pool));
    G<uint64_t>(t + HT_SENTINEL) = uint64_t(uintptr_t(sentinel));
    G<uint64_t>(t + HT_MEMORY) = uint64_t(uintptr_t(mem));
    G<uint64_t>(t + HT_BUCKETS_END) = uint64_t(uintptr_t(mem + uint64_t(buckets) * 8));
    G<int32_t>(t + HT_NUM_BUCKETS) = buckets;
    G<uint64_t>(t + HT_BUCKETS) = uint64_t(uintptr_t(mem));
    G<uint8_t>(t + HT_READY) = 1;
    log("netinit: %s: %d entries of %#x, %d buckets", name, entries, node_size, buckets);
    return true;
}

// ---- boosters -------------------------------------------------------------------------------------
bool boosters_done = false;

void init_boosters() {
    if (boosters_done) return;
    uint32_t &count = G<uint32_t>(kBoosters);
    if (count != 0) {   // filled by something else
        boosters_done = true;
        return;
    }
    using FindFn = void **(U2MP_GUEST *)(uint32_t);
    void **found = guest<FindFn>(fDcFind)(kBoosterTable);
    const uint8_t *table = found ? static_cast<const uint8_t *>(*found) : nullptr;
    if (!table) {
        log("netinit: *boosters* not loaded yet");
        return;
    }
    const int32_t n = *reinterpret_cast<const int32_t *>(table);
    const uint8_t *recs = *reinterpret_cast<uint8_t *const *>(table + 8);
    if (n <= 0 || !recs) return;
    // PS3 kept a record when its DLC mask passed 0x4e8798 (mask & owned != 0); every pack counts as
    // owned here (DLC.md), so a non-zero mask is kept. If no record has one, the mask guess is wrong:
    // keep them all.
    bool any_mask = false;
    for (int32_t i = 0; i < n; ++i)
        any_mask = any_mask || *reinterpret_cast<const uint32_t *>(recs + uint64_t(i) * BOOST_STRIDE + BOOST_MASK) != 0;
    uint32_t kept = 0;
    for (int32_t i = 0; i < n && kept < uint32_t(kMaxBoosters); ++i) {
        const uint8_t *r = recs + uint64_t(i) * BOOST_STRIDE;
        if (any_mask && *reinterpret_cast<const uint32_t *>(r + BOOST_MASK) == 0) continue;
        G<uint64_t>(kBoosters + 8 + 8 * uint64_t(kept)) = uint64_t(uintptr_t(r));
        ++kept;
    }
    count = kept;
    boosters_done = true;
    log("netinit: %u of %d boosters listed", kept, n);
}

// ---- the rest ------------------------------------------------------------------------------------
bool once_done = false;

void init_common() {
    if (!check_code()) return;
    if (!once_done) {
        // NetGameManager::Init's recent-game history (PS3 memset(ngm+6382, 0xff, 8)); only shifted on
        // PS4 (0x641619), set for parity.
        for (int i = 0; i < 4; ++i) G<int16_t>(kNgm + NGM_RECENT_GAMES + 2 * uint64_t(i)) = -1;
        once_done = true;
    }
    // Session::Init 0x4d1e2c: leave status "idle" (1) on both sessions. PS3 cleared it only while a
    // LeaveRoom request was pending; PS4 has no such request, so it stays 1.
    G<uint8_t>(kMatch + S_LEFT) = 1;
    G<uint8_t>(kParty + S_LEFT) = 1;
    init_table(kNgm + NGM_TABLE_A, 16, 0x60, "ngm table 0x1ba0");
    init_table(kNgm + NGM_TABLE_B, 256, 0x38, "ngm table 0x1c10");
    init_boosters();
}

}  // namespace

void netinit_prepare() { init_common(); }

void netinit_match() { init_common(); }

}  // namespace u2mp
