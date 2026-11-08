// u2mp render: per-instance constant-buffer lists (u2mp/docs/PORT-RENDER.md).
//
// Every mesh instance owns lists of the constant buffers its shader programs declare, one list per
// cbuffer scope. MeshInstance::Init (0xc3b6f0) creates them with InitCbList (0xc24780), for the
// instance scope (3) with the capacity the prototype states at +0x30, and then adds the cbuffers of
// every material program with AddCb (0xc24b30, reached only through 0xc31c10 at 0xc31cc0 / 0xc31d60).
// A list is {u32 count, u32 capacity, u64 *keys, Entry *entries, u32 scope, ..., u64 rw-lock @+0x20,
// u8 changed @+0x30}; InitCbList makes ONE allocation of capacity * 0x38 bytes: `capacity` keys
// (cbuffer name << 32 | id) followed by `capacity` 48-byte entries. AddCb appends without checking the
// capacity, so a prototype that states too few cbuffers writes key[capacity] over entry[0].res,
// key[capacity + 1] over entry[0].buffer, and the surplus entries past the allocation.
//
// That is the first-frame crash of test runs 35-38: the converted PS3 skins (mp-hero ...) and props
// state capacity 2 (the U3 actor converter's fixed value) while their materials declare 3-4 instance
// cbuffers (retail states 3 or 4, the number its materials declare). Gompa run 36: entry[0] = the
// transform cbuffer 0x8704e118/0x2b444d15 with res = 0x33d5c036'f1b17bc2 and buffer =
// 0x08d9a770'6f1c0396 = the keys of the 3rd and 4th cbuffers of mp-hero.pak. The converter now states
// the right capacity (u2conv/u2actor.py fix_cbuffer_capacity); this file makes the engine safe for any
// asset that still states too few:
//
//  1. AddCb guard: the two AddCb calls go through add_cbuffer_checked, which grows a full list (new
//     arrays, old ones left in place) before the original AddCb appends. Nothing overflows any more.
//  2. Entry repair (was nullsites.cpp section 2): the binding walker 0xc25670 checks, at 0xc2574f,
//     that a live entry's resource is a user-space pointer; one that is not gets a real cbuffer
//     resource made the way AddCb makes it (0xc16820), so every reader (the walker's 0xc12150 copy and
//     its res->type read at 0xc25ddb, the draw lookup 0xc30420, 0xc04340) sees a valid resource.
//  3. Draw lookup fallback: 0xc30420 stores NULL for a cbuffer whose key is not in the list (0xc30790)
//     and 0xc04340 then reads through it (run 38: fault at 0xc04403). NULL is replaced by a zeroed
//     4 KiB cbuffer resource.
//  4. Bind guard at the walker's 0xc12150 call (0xc258b2), kept from run 35 as the last check.
//
// Addresses are Uncharted 2 PS4 1.02 image offsets. Every patch checks the bytes it replaces first.
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
namespace {

constexpr uint64_t fListInit = 0xc24780;     // InitCbList(list, scope, capacity)
constexpr uint64_t fListAdd = 0xc24b30;      // AddCb(list, -, def, flags, data)
constexpr uint64_t cAddFirst = 0xc31cc0, cAddSecond = 0xc31d60;   // in 0xc31c10: program +8 / +0x10
constexpr uint64_t fMakeBuffer = 0xc16820;   // MakeBuffer(&tag, desc{size | bit 34, u16 flags}, data, format, stat)
constexpr uint64_t fBindCopy = 0xc12150;     // per-frame copy of a cbuffer resource
constexpr uint64_t cBindCall = 0xc258b2;     // the walker's call of it
constexpr uint64_t cEntryCheck = 0xc2574f, cEntryLive = 0xc2575b, cEntryNext = 0xc25e38;
constexpr uint64_t cLookupStore = 0xc30790, cLookupNext = 0xc30798;

// A cbuffer definition of a shader program (made by 0xc26100 from the program's 0x30-byte records).
struct CbDef {
    uint32_t key;      // name sid
    uint32_t id;
    uint32_t scope;    // 3 = instance
    uint32_t size;
};

struct CbEntry {
    void *res;            // cbuffer resource (0x38-byte object, 32-byte descriptor + u16 type @+0x18)
    void *buffer;         // per-frame copy (types 7/8)
    const CbDef *def;
    const void *data;
    uint32_t used;        // non-zero = live (the def id)
    uint32_t refs;
    uint8_t dirty;
    uint8_t pad[3];
    uint32_t flags;       // the AddCb flags = the resource type
};
static_assert(sizeof(CbEntry) == 0x30, "cbuffer entry");

struct CbList {
    uint32_t count;
    uint32_t capacity;
    uint64_t *keys;
    CbEntry *entries;
    uint32_t scope;
    uint32_t pad;
    uint64_t lock;        // readers count up, a writer sets bit 63
};

bool bad_pointer(uint64_t p) { return p < 0x10000 || p >= 0x0000800000000000ull; }

void lock_write(CbList *l) {
    for (;;) {
        uint64_t expected = 0;
        if (__atomic_compare_exchange_n(&l->lock, &expected, 1ull << 63, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
        __builtin_ia32_pause();
    }
}
void unlock_write(CbList *l) { __atomic_store_n(&l->lock, 0, __ATOMIC_RELEASE); }

// ---- cbuffer resources made the way AddCb makes them -------------------------------------------------
using MakeBuffer = void *(U2MP_GUEST *)(const uint32_t *, const uint64_t *, const void *, uint32_t, uint32_t);

void *make_cbuffer(uint32_t size, uint16_t flags, const void *data) {
    static const uint32_t tag[4] = {0x31, 0, 0, 0};
    const uint64_t desc[2] = {(uint64_t((size + 15u) & ~15u)) | (1ull << 34), flags};
    return reinterpret_cast<MakeBuffer>(image.base + fMakeBuffer)(tag, desc, data, 0, 1);
}

constexpr uint32_t kNullBytes = 0x1000;
alignas(64) unsigned char null_bytes[kNullBytes];
void *null_cbuffer = nullptr;          // read by the lookup stub
uint64_t null_uses = 0;                // counted by the lookup stub

void ensure_null_cbuffer() {
    if (__atomic_load_n(&null_cbuffer, __ATOMIC_ACQUIRE)) return;
    void *r = make_cbuffer(kNullBytes, 2, null_bytes);
    void *expected = nullptr;
    if (r && __atomic_compare_exchange_n(&null_cbuffer, &expected, r, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        log("render: zeroed 4 KiB cbuffer for lookups that find nothing: %p", r);
}

void report_null_uses() {
    static uint64_t reported = 0;
    const uint64_t n = __atomic_load_n(&null_uses, __ATOMIC_RELAXED);
    if (n != reported && (reported < 8 || (n & 1023) == 0)) {
        reported = n;
        log("render: %llu cbuffer lookups found no entry and got the zeroed cbuffer", (unsigned long long)n);
    }
}

// ---- 1. AddCb guard ----------------------------------------------------------------------------------
using AddCb = void (U2MP_GUEST *)(CbList *, void *, const CbDef *, uint32_t, const void *);

uint64_t grown_lists = 0, grown_bytes = 0;

// True when AddCb would write a new entry at index `count` (the key is new and no entry is free) and
// that index is past the arrays. Same search as 0xc24b82..0xc24c28.
bool needs_room(const CbList *l, uint64_t key) {
    const uint32_t n = l->count;
    if (n) {
        for (uint32_t i = 0; i < n; ++i)
            if (l->keys[i] == key) return false;
        for (uint32_t i = 0; i < n; ++i)
            if (l->entries[i].used == 0) return false;
    }
    return n >= l->capacity || !l->keys || !l->entries;
}

void grow(CbList *l, const CbDef *def) {
    const uint32_t n = l->count, cap = l->capacity;
    uint32_t next = cap * 2 > cap + 4 ? cap * 2 : cap + 4;
    if (next < n + 1) next = n + 1;
    // Host memory, never freed: the lists live inside the instance's allocation block (0xc249a0 moves
    // pointers that point into it), which the game releases whole; nothing frees the arrays on their own.
    const size_t bytes = size_t(next) * (sizeof(uint64_t) + sizeof(CbEntry));
    unsigned char *mem = static_cast<unsigned char *>(std::calloc(1, bytes + 16));
    if (!mem) return;
    uint64_t *keys = reinterpret_cast<uint64_t *>(mem);
    CbEntry *entries = reinterpret_cast<CbEntry *>(mem + size_t(next) * sizeof(uint64_t));
    if (n && l->keys) std::memcpy(keys, l->keys, size_t(n) * sizeof(uint64_t));
    if (n && l->entries) std::memcpy(entries, l->entries, size_t(n) * sizeof(CbEntry));
    l->keys = keys;
    l->entries = entries;
    l->capacity = next;
    ++grown_lists;
    grown_bytes += bytes;
    if (grown_lists <= 16 || (grown_lists & 255) == 0)
        log("render: cbuffer list %p (scope %u) full at capacity %u (%u used) adding %08x/%08x: grown to %u "
            "(%llu lists, %llu bytes so far; the asset states too few cbuffers - re-run its import)",
            static_cast<void *>(l), l->scope, cap, n, def->key, def->id, next, (unsigned long long)grown_lists,
            (unsigned long long)grown_bytes);
}

U2MP_GUEST void add_cbuffer_checked(CbList *l, void *unused, const CbDef *def, uint32_t flags, const void *data) {
    ensure_null_cbuffer();
    report_null_uses();
    if (l && !bad_pointer(uint64_t(uintptr_t(def)))) {
        const uint64_t key = (uint64_t(def->key) << 32) | def->id;   // as 0xc24b88..0xc24b9b
        lock_write(l);
        if (needs_room(l, key)) grow(l, def);
        unlock_write(l);
    }
    reinterpret_cast<AddCb>(image.base + fListAdd)(l, unused, def, flags, data);
}

// ---- 2. Entry repair in the binding walker -----------------------------------------------------------
uint64_t repaired_count = 0, repaired_last = 0;

// Called from the stub at 0xc2574f with a live entry whose resource is not a user-space pointer.
// Returns non-zero when the walker may bind the entry, 0 to skip it.
U2MP_GUEST uint64_t repair_entry(CbEntry *e) {
    const uint64_t bad = uint64_t(uintptr_t(e->res));
    const uint64_t n = __atomic_add_fetch(&repaired_count, 1, __ATOMIC_RELAXED);
    repaired_last = bad;
    const uint64_t def = uint64_t(uintptr_t(e->def));
    void *res = nullptr;
    if (!bad_pointer(def) && e->def->size && e->def->size <= 0x10000)
        // type 9 wraps the data pointer instead of copying it (0xc168af..0xc169c2): AddCb passes NULL there
        res = make_cbuffer(e->def->size, uint16_t(e->flags),
                           (e->flags & 0xffff) != 9 && e->def->size <= kNullBytes ? null_bytes : nullptr);
    if (n <= 12 || (n & 1023) == 0) {
        log("render: entry %p resource %016llx is not a pointer (buffer %016llx, def %016llx, id %08x, flags %u); "
            "%s (%llu repaired)", static_cast<void *>(e), (unsigned long long)bad,
            (unsigned long long)uintptr_t(e->buffer), (unsigned long long)def, e->used, e->flags,
            res ? "new cbuffer resource" : bad_pointer(def) ? "no def: entry skipped" : "no resource: entry skipped",
            (unsigned long long)n);
        if (!bad_pointer(def)) {
            const uint32_t *d = reinterpret_cast<const uint32_t *>(uintptr_t(def));
            log("render: def %08x %08x %08x %08x %08x %08x %08x %08x", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
        }
    }
    if (!res) {
        // The draw lookup 0xc30420 matches by key without testing `used` and returns entry.res, so the bad
        // value must not stay there: point it at the shared zeroed cbuffer as well (review, 2026-11-08).
        ensure_null_cbuffer();
        void *expected = reinterpret_cast<void *>(uintptr_t(bad));
        __atomic_compare_exchange_n(&e->res, &expected, null_cbuffer, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
        e->buffer = nullptr;
        e->used = 0;       // the walker skips it
        return 0;
    }
    void *expected = reinterpret_cast<void *>(uintptr_t(bad));
    if (__atomic_compare_exchange_n(&e->res, &expected, res, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        e->buffer = nullptr;   // made again from res by the walker (dirty)
        e->dirty = 1;
    }
    return 1;
}

// cmp dword [rax+r13+0x20],0 ; je 0xc25e38 (the walker's live-entry test)
const unsigned char kEntrySite[] = {0x42, 0x83, 0x7c, 0x28, 0x20, 0x00, 0x0f, 0x84, 0xdd, 0x06, 0x00, 0x00};

// Assembled from (Intel syntax):
//     cmp dword [rax+r13+0x20],0 ; je next          (original test)
//     mov rdx,[rax+r13] ; mov rcx,rdx ; shr rcx,47 ; jnz bad ; cmp rdx,0x10000 ; jb bad
//   live: jmp [rip+live_addr]          next: jmp [rip+next_addr]
//   bad:  push rbp ; mov rbp,rsp ; push rax,rsi,rdi,r8,r9,r10,r11 ; and rsp,-16 ; sub rsp,256
//         movdqu [rsp+16*i],xmm<i> (0..15) ; lea rdi,[rax+r13] ; mov rax,repair_entry ; call rax
//         mov rcx,rax ; movdqu xmm<i>,[rsp+16*i] ; lea rsp,[rbp-56] ; pop r11..rax ; pop rbp
//         test ecx,ecx ; jz next ; jmp live
// rcx and rdx are free at this point: the live path writes both before reading them (0xc2576a /
// 0xc2576e or the setne at 0xc257a6 / 0xc257af, then 0xc257b7), the next path reloads them.
const unsigned char kRepairStub[] = {
    0x42, 0x83, 0x7c, 0x28, 0x20, 0x00, 0x74, 0x1c, 0x4a, 0x8b, 0x14, 0x28,
    0x48, 0x89, 0xd1, 0x48, 0xc1, 0xe9, 0x2f, 0x75, 0x15, 0x48, 0x81, 0xfa,
    0x00, 0x00, 0x01, 0x00, 0x72, 0x0c, 0xff, 0x25, 0x4e, 0x01, 0x00, 0x00,
    0xff, 0x25, 0x50, 0x01, 0x00, 0x00, 0x55, 0x48, 0x89, 0xe5, 0x50, 0x56,
    0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x48, 0x83, 0xe4,
    0xf0, 0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00, 0xf3, 0x0f, 0x7f, 0x04,
    0x24, 0xf3, 0x0f, 0x7f, 0x4c, 0x24, 0x10, 0xf3, 0x0f, 0x7f, 0x54, 0x24,
    0x20, 0xf3, 0x0f, 0x7f, 0x5c, 0x24, 0x30, 0xf3, 0x0f, 0x7f, 0x64, 0x24,
    0x40, 0xf3, 0x0f, 0x7f, 0x6c, 0x24, 0x50, 0xf3, 0x0f, 0x7f, 0x74, 0x24,
    0x60, 0xf3, 0x0f, 0x7f, 0x7c, 0x24, 0x70, 0xf3, 0x44, 0x0f, 0x7f, 0x84,
    0x24, 0x80, 0x00, 0x00, 0x00, 0xf3, 0x44, 0x0f, 0x7f, 0x8c, 0x24, 0x90,
    0x00, 0x00, 0x00, 0xf3, 0x44, 0x0f, 0x7f, 0x94, 0x24, 0xa0, 0x00, 0x00,
    0x00, 0xf3, 0x44, 0x0f, 0x7f, 0x9c, 0x24, 0xb0, 0x00, 0x00, 0x00, 0xf3,
    0x44, 0x0f, 0x7f, 0xa4, 0x24, 0xc0, 0x00, 0x00, 0x00, 0xf3, 0x44, 0x0f,
    0x7f, 0xac, 0x24, 0xd0, 0x00, 0x00, 0x00, 0xf3, 0x44, 0x0f, 0x7f, 0xb4,
    0x24, 0xe0, 0x00, 0x00, 0x00, 0xf3, 0x44, 0x0f, 0x7f, 0xbc, 0x24, 0xf0,
    0x00, 0x00, 0x00, 0x4a, 0x8d, 0x3c, 0x28, 0x48, 0xb8, 0x11, 0x11, 0x11,
    0x11, 0x11, 0x11, 0x11, 0x11, 0xff, 0xd0, 0x48, 0x89, 0xc1, 0xf3, 0x0f,
    0x6f, 0x04, 0x24, 0xf3, 0x0f, 0x6f, 0x4c, 0x24, 0x10, 0xf3, 0x0f, 0x6f,
    0x54, 0x24, 0x20, 0xf3, 0x0f, 0x6f, 0x5c, 0x24, 0x30, 0xf3, 0x0f, 0x6f,
    0x64, 0x24, 0x40, 0xf3, 0x0f, 0x6f, 0x6c, 0x24, 0x50, 0xf3, 0x0f, 0x6f,
    0x74, 0x24, 0x60, 0xf3, 0x0f, 0x6f, 0x7c, 0x24, 0x70, 0xf3, 0x44, 0x0f,
    0x6f, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00, 0xf3, 0x44, 0x0f, 0x6f, 0x8c,
    0x24, 0x90, 0x00, 0x00, 0x00, 0xf3, 0x44, 0x0f, 0x6f, 0x94, 0x24, 0xa0,
    0x00, 0x00, 0x00, 0xf3, 0x44, 0x0f, 0x6f, 0x9c, 0x24, 0xb0, 0x00, 0x00,
    0x00, 0xf3, 0x44, 0x0f, 0x6f, 0xa4, 0x24, 0xc0, 0x00, 0x00, 0x00, 0xf3,
    0x44, 0x0f, 0x6f, 0xac, 0x24, 0xd0, 0x00, 0x00, 0x00, 0xf3, 0x44, 0x0f,
    0x6f, 0xb4, 0x24, 0xe0, 0x00, 0x00, 0x00, 0xf3, 0x44, 0x0f, 0x6f, 0xbc,
    0x24, 0xf0, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x65, 0xc8, 0x41, 0x5b, 0x41,
    0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x58, 0x5d, 0x85, 0xc9, 0x0f,
    0x84, 0xb7, 0xfe, 0xff, 0xff, 0xe9, 0xac, 0xfe, 0xff, 0xff, 0x22, 0x22,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33,
    0x33, 0x33,
};
constexpr size_t kRepairHost = 0xc9, kRepairLive = 0x172, kRepairNext = 0x17a;
static_assert(sizeof kRepairStub == 0x182, "repair stub layout");

// Writes `code` to a near stub and a jmp to it over `site_len` bytes at `site`.
unsigned char *install_stub(uint64_t site, size_t site_len, const unsigned char *code, size_t len, const char *name) {
    unsigned char *stub = near_stub(len);
    const int64_t delta = stub ? int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + site + 5)) : 0;
    if (!stub || delta < INT32_MIN || delta > INT32_MAX) {
        log("%s: no stub memory in range", name);
        return nullptr;
    }
    std::memcpy(stub, code, len);
    unsigned char *c = image.base + site;
    const int32_t jump = int32_t(delta);
    c[0] = 0xe9;
    std::memcpy(c + 1, &jump, 4);
    std::memset(c + 5, 0x90, site_len - 5);
    return stub;
}

bool install_entry_repair() {
    if (!image.matches(cEntryCheck, kEntrySite, sizeof kEntrySite) || !image.executable(cEntryCheck, sizeof kEntrySite)) {
        log("render entry repair (c2574f): bytes differ, left alone");
        return false;
    }
    unsigned char code[sizeof kRepairStub];
    std::memcpy(code, kRepairStub, sizeof code);
    const uint64_t host = uint64_t(uintptr_t(&repair_entry));
    const uint64_t live = uint64_t(uintptr_t(image.base + cEntryLive));
    const uint64_t next = uint64_t(uintptr_t(image.base + cEntryNext));
    std::memcpy(code + kRepairHost, &host, 8);
    std::memcpy(code + kRepairLive, &live, 8);
    std::memcpy(code + kRepairNext, &next, 8);
    if (!install_stub(cEntryCheck, sizeof kEntrySite, code, sizeof code, "render entry repair (c2574f)")) return false;
    log("render entry repair (c2574f): an entry whose resource is not a pointer gets a new cbuffer resource");
    return true;
}

// ---- 3. Draw lookup fallback -------------------------------------------------------------------------
// 0xc30790: mov [r9+r10*8],rdx ; mov rax,[rdi+0x28]   (rdx = the found resource, or 0)
const unsigned char kLookupSite[] = {0x4b, 0x89, 0x14, 0xd1, 0x48, 0x8b, 0x47, 0x28};
// test rdx,rdx ; jnz store ; mov rdx,&null_uses ; lock inc qword [rdx] ; mov rdx,&null_cbuffer ;
// mov rdx,[rdx] ; store: mov [r9+r10*8],rdx ; mov rax,[rdi+0x28] ; jmp [rip] -> 0xc30798
const unsigned char kLookupStub[] = {
    0x48, 0x85, 0xd2, 0x75, 0x1b, 0x48, 0xba, 0x44, 0x44, 0x44, 0x44, 0x44,
    0x44, 0x44, 0x44, 0xf0, 0x48, 0xff, 0x02, 0x48, 0xba, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x48, 0x8b, 0x12, 0x4b, 0x89, 0x14, 0xd1,
    0x48, 0x8b, 0x47, 0x28, 0xff, 0x25, 0x00, 0x00, 0x00, 0x00, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
};
constexpr size_t kLookupCount = 0x7, kLookupSlot = 0x15, kLookupBack = 0x2e;
static_assert(sizeof kLookupStub == 0x36, "lookup stub layout");

bool install_lookup_fallback() {
    if (!image.matches(cLookupStore, kLookupSite, sizeof kLookupSite) ||
        !image.executable(cLookupStore, sizeof kLookupSite)) {
        log("render lookup fallback (c30790): bytes differ, left alone");
        return false;
    }
    unsigned char code[sizeof kLookupStub];
    std::memcpy(code, kLookupStub, sizeof code);
    const uint64_t count = uint64_t(uintptr_t(&null_uses));
    const uint64_t slot = uint64_t(uintptr_t(&null_cbuffer));
    const uint64_t back = uint64_t(uintptr_t(image.base + cLookupNext));
    std::memcpy(code + kLookupCount, &count, 8);
    std::memcpy(code + kLookupSlot, &slot, 8);
    std::memcpy(code + kLookupBack, &back, 8);
    if (!install_stub(cLookupStore, sizeof kLookupSite, code, sizeof code, "render lookup fallback (c30790)"))
        return false;
    log("render lookup fallback (c30790): a cbuffer the instance lists do not have binds a zeroed cbuffer");
    return true;
}

// ---- 4. Bind guard at 0xc258b2 -----------------------------------------------------------------------
alignas(64) uint64_t zero_descriptor[8];
using BindCopy = void *(U2MP_GUEST *)(const void **, uint64_t, void *);

U2MP_GUEST void *bind_copy_checked(const void **res, uint64_t size, void *out) {
    const uint64_t r = uint64_t(uintptr_t(*res));
    if (bad_pointer(r)) {
        static int logged = 0;
        if (logged < 12) {
            ++logged;
            const uint64_t *entry = reinterpret_cast<const uint64_t *>(static_cast<uint8_t *>(out) - 8);
            log("render bind guard (c258b2): resource %016llx not a pointer after the repair; entry %016llx %016llx "
                "%016llx %016llx %016llx %016llx, size 0x%llx", (unsigned long long)r,
                (unsigned long long)entry[0], (unsigned long long)entry[1], (unsigned long long)entry[2],
                (unsigned long long)entry[3], (unsigned long long)entry[4], (unsigned long long)entry[5],
                (unsigned long long)size);
        }
        *res = zero_descriptor;   // the caller's local copy only (type 0: 0xc12150 makes no descriptor)
    }
    return reinterpret_cast<BindCopy>(image.base + fBindCopy)(res, size, out);
}

}  // namespace

// Called from install_nullsites (nullsites.cpp), so install.cpp needs no line of its own.
void install_render(int &installed) {
    static const unsigned char add_prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55};
    static const unsigned char init_prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53};
    static const unsigned char make_prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55};
    if (!image.matches(fListAdd, add_prologue, sizeof add_prologue) ||
        !image.matches(fListInit, init_prologue, sizeof init_prologue) ||
        !image.matches(fMakeBuffer, make_prologue, sizeof make_prologue)) {
        log("render: cbuffer list functions differ, left alone");
        return;
    }
    int both = 0;
    both += retarget_call(cAddFirst, fListAdd, reinterpret_cast<void *>(&add_cbuffer_checked),
                          "render AddCb guard (first program)");
    both += retarget_call(cAddSecond, fListAdd, reinterpret_cast<void *>(&add_cbuffer_checked),
                          "render AddCb guard (second program)");
    installed += both;
    if (install_entry_repair()) ++installed;
    if (install_lookup_fallback()) ++installed;
    if (retarget_call(cBindCall, fBindCopy, reinterpret_cast<void *>(&bind_copy_checked),
                      "render bind guard (unresolved cbuffer resource)"))
        ++installed;
}

}  // namespace u2mp
