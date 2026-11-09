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
#include <cstdio>
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

// ---- 5. Character draw diagnostics (test runs 41-42: the player model is not seen) -------------------
// Three questions for the next test, logged as `render diag:`:
//   a. does MeshInstance::Init (0xc3b6f0) run for the player's prototypes (name contains "mp_hero")?
//      -> logged per instance: prototype, sub-element count, flags, bounds and transform.
//   b. does the binding walker (0xc25670, run for every instance the renderer submits) see one of their
//      cbuffer lists (instance +0x170 / +0x1b0, sub-element +0x340)? -> the first submit is logged per
//      instance, then counts every 0x40000 walker calls. No submit = culled or never queued.
//   c. at the first submit: the transform and the walker's override table (count, first entries), which
//      carries per-draw data such as the skinning palette. A zero / far transform = drawn off-screen.
// Run 43: the hero's 4 instances were made (actor path 0xbe48d0) but never submitted in 60 s. Run 44 adds
// references (other actors and level instances, whose submits prove the counter counts drawn objects),
// the actor's scope-6 list (+0x200), the teardown (0xc3f4b0 at 0xbe46c5) and the hero's transform and
// bounds again with every summary (identity / zero after 10 s = nothing positions or animates it).
// UNCHARTED_U2MP_RENDER_DIAG=0 turns it off; =all records every prototype (in the 64 hero slots).
constexpr uint64_t fMeshInit = 0xc3b6f0, fWalker = 0xc25670, fMeshFree = 0xc3f4b0;
constexpr uint64_t cInitA = 0xbe4bb3, cInitB = 0xc29a74, cInitC = 0xd256b7, cFreeActor = 0xbe46c5;
// Slots: the hero's prototypes, then references that prove the counter counts drawn objects: the last 8
// other actors (MeshInstance::Init from 0xbe48d0 at 0xbe4bb3, the actor path the hero uses: weapons,
// props) and the last 8 level / GeoGroup instances (from 0xc29a74).
constexpr int kHeroSlots = 64, kActorRefs = 8, kGeoRefs = 8;
constexpr int kActorBase = kHeroSlots, kGeoBase = kHeroSlots + kActorRefs, kTracked = kGeoBase + kGeoRefs;
constexpr int kListSlots = 8192;
const char *const kListNames[] = {"+0x170 scope 3", "+0x1b0 scope 4", "sub-element +0x340 scope 5",
                                  "+0x200 scope 6 (actor)"};

struct Tracked {
    uint64_t instance;     // 0 = free / destroyed
    uint32_t generation;
    char name[48];
    const char *kind;
    uint64_t born;         // walker calls at init
    uint64_t submits;
    uint64_t submits_reported;
    bool logged_first;
    uint64_t render_object;   // the AddMesh owner (0x1248600 render object), 0 = not known
    uint64_t collects;        // 0xbd6d00 calls for that render object
    uint64_t collects_reported;
    uint64_t draw_jobs;       // 0xc419d0 calls for this instance
    uint64_t draw_jobs_reported;
};
Tracked tracked[kTracked];

// ---- run 45: the render object gate ----------------------------------------------------------------
// DrawControl::AddMesh (0x4b6e20) gets each mesh's draw data from the manager 0x1248600 with
// 0xbd3bc0(manager, render object) (calls 0x4b6faf / 0x4b7163): draw data are 0x4b0-byte records in
// manager +0x400, the MeshInstance is draw data +0x1c0, flags at +0x1b8 (0xbe4ad3: +0x2000 on creation,
// +1 for an empty prototype; DrawControl Show/Hide clear/set +2). Per frame 0xbd6d00(render object,
// out list) collects a render object's draw data (+0xfc count, +0x100 index list), skipping those with
// flags & 7, and 0xbd4f80 skips render objects whose per-frame byte (+5 at the frame offset) lacks 4.
// Logged here: the render object of every tracked instance, the draw data flags, and how often 0xbd6d00
// ran for that render object. Hero instances never submitted with collects > 0 = the draw data gate
// (flags); collects == 0 = the render object is never visible (object-level gate: process / show).
constexpr uint64_t fDrawDataNew = 0xbd3bc0, cDrawDataA = 0x4b6faf, cDrawDataB = 0x4b7163;
constexpr uint64_t fCollect = 0xbd6d00;
constexpr int kPairs = 1024;
uint64_t pair_drawdata[kPairs], pair_object[kPairs];
uint32_t pair_next = 0;

using DrawDataNewFn = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t);
U2MP_GUEST uint64_t drawdata_new_logged(uint64_t manager, uint64_t object) {
    const uint64_t r = reinterpret_cast<DrawDataNewFn>(image.base + fDrawDataNew)(manager, object);
    const uint32_t i = __atomic_fetch_add(&pair_next, 1, __ATOMIC_RELAXED) % kPairs;
    pair_object[i] = object;
    __atomic_store_n(&pair_drawdata[i], r, __ATOMIC_RELEASE);
    return r;
}

uint64_t render_object_of(uint64_t drawdata) {
    for (int i = 0; i < kPairs; ++i)
        if (__atomic_load_n(&pair_drawdata[i], __ATOMIC_ACQUIRE) == drawdata) return pair_object[i];
    return 0;
}

using CollectFn = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
unsigned char *collect_original = nullptr;
U2MP_GUEST uint64_t collect_logged(uint64_t object, uint64_t out, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    for (int i = 0; i < kTracked; ++i)
        if (tracked[i].render_object == object && __atomic_load_n(&tracked[i].instance, __ATOMIC_RELAXED))
            __atomic_add_fetch(&tracked[i].collects, 1, __ATOMIC_RELAXED);
    return reinterpret_cast<CollectFn>(collect_original)(object, out, a2, a3, a4, a5);
}

// 0xc419d0(MeshInstance, ...) is the per-instance draw job (reached through the thunk 0xc42250 of the
// renderable interface at MeshInstance +0x150); the binding walker runs inside it. Counted per tracked
// instance: a hero with collects but no draw jobs is dropped between 0xbd6d00 and the job list.
constexpr uint64_t fDrawJob = 0xc419d0;
using DrawJobFn = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
unsigned char *drawjob_original = nullptr;
U2MP_GUEST uint64_t drawjob_logged(uint64_t inst, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    for (int i = 0; i < kTracked; ++i)
        if (__atomic_load_n(&tracked[i].instance, __ATOMIC_RELAXED) == inst)
            __atomic_add_fetch(&tracked[i].draw_jobs, 1, __ATOMIC_RELAXED);
    return reinterpret_cast<DrawJobFn>(drawjob_original)(inst, a1, a2, a3, a4, a5);
}

void log_gate(const Tracked &t, uint64_t inst) {
    const uint8_t *dd = reinterpret_cast<const uint8_t *>(uintptr_t(inst - 0x1c0));
    const uint64_t ro = t.render_object;
    if (!ro || bad_pointer(ro)) {
        log("render diag: %s %p draw data flags +1b8 %08x, render object unknown", t.kind,
            reinterpret_cast<const void *>(uintptr_t(inst)), *reinterpret_cast<const uint32_t *>(dd + 0x1b8));
        return;
    }
    const uint8_t *o = reinterpret_cast<const uint8_t *>(uintptr_t(ro));
    log("render diag: %s %p draw data flags +1b8 %08x; render object %p: %llu collects (+%llu), +0 %016llx "
        "%016llx +f8 %u/%u", t.kind, reinterpret_cast<const void *>(uintptr_t(inst)),
        *reinterpret_cast<const uint32_t *>(dd + 0x1b8), static_cast<const void *>(o),
        (unsigned long long)t.collects, (unsigned long long)(t.collects - t.collects_reported),
        (unsigned long long)*reinterpret_cast<const uint64_t *>(o), (unsigned long long)*reinterpret_cast<const uint64_t *>(o + 8),
        *reinterpret_cast<const uint32_t *>(o + 0xfc), *reinterpret_cast<const uint32_t *>(o + 0xf8));
}
uint32_t hero_next = 0, actor_next = 0, geo_next = 0, generation_next = 0;
uint64_t list_key[kListSlots];      // list address
int16_t list_owner[kListSlots];     // index into tracked
uint8_t list_code[kListSlots];      // kListNames index
uint32_t list_gen[kListSlots];      // the owner's generation when inserted
int diag_mode = 1;                  // 0 off, 1 mp_hero + references, 2 every prototype (hero slots)
uint64_t walker_calls = 0;
uint32_t lists_used = 0, lists_dropped = 0;   // list table fill (never emptied)

bool readable_string(const char *s, char *out, size_t n) {
    if (bad_pointer(uint64_t(uintptr_t(s)))) return false;
    size_t i = 0;
    for (; i + 1 < n && s[i]; ++i) {
        if (static_cast<unsigned char>(s[i]) < 0x20 || static_cast<unsigned char>(s[i]) > 0x7e) return false;
        out[i] = s[i];
    }
    out[i] = 0;
    return i > 0;
}

uint32_t list_hash(uint64_t list) { return uint32_t((list >> 4) * 0x9e3779b1u) & (kListSlots - 1); }

void list_insert(uint64_t list, int owner, int code, uint32_t gen) {
    uint32_t h = list_hash(list);
    for (int probe = 0; probe < kListSlots; ++probe, h = (h + 1) & (kListSlots - 1)) {
        const uint64_t k = __atomic_load_n(&list_key[h], __ATOMIC_ACQUIRE);
        if (k == list || k == 0) {
            if (k == 0) __atomic_add_fetch(&lists_used, 1, __ATOMIC_RELAXED);
            list_owner[h] = int16_t(owner);
            list_code[h] = uint8_t(code);
            list_gen[h] = gen;
            __atomic_store_n(&list_key[h], list, __ATOMIC_RELEASE);
            return;
        }
    }
    __atomic_add_fetch(&lists_dropped, 1, __ATOMIC_RELAXED);   // table full (=all mode): not watched
}
// The live tracked owner of a list (-1: none, or its instance was destroyed / the slot reused).
int list_find(uint64_t list, int *code) {
    uint32_t h = list_hash(list);
    for (int probe = 0; probe < 64; ++probe, h = (h + 1) & (kListSlots - 1)) {
        const uint64_t k = __atomic_load_n(&list_key[h], __ATOMIC_ACQUIRE);
        if (k == list) {
            const int o = list_owner[h];
            if (!tracked[o].instance || tracked[o].generation != list_gen[h]) return -1;
            *code = list_code[h];
            return o;
        }
        if (k == 0) return -1;
    }
    return -1;
}

void log_instance(const char *what, const uint8_t *inst) {
    const float *m = reinterpret_cast<const float *>(inst + 0x10);
    const float *bb = reinterpret_cast<const float *>(inst + 0xa0);
    log("render diag: %s %p transform [%.3g %.3g %.3g | %.3g %.3g %.3g | %.3g %.3g %.3g | pos %.3f %.3f %.3f] "
        "bounds %.3f %.3f %.3f .. %.3f %.3f %.3f, flags +c0 %04x +d8 %llx", what, static_cast<const void *>(inst),
        m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14], bb[0], bb[1], bb[2], bb[4],
        bb[5], bb[6], *reinterpret_cast<const uint16_t *>(inst + 0xc0),
        (unsigned long long)*reinterpret_cast<const uint64_t *>(inst + 0xd8));
}

using MeshInitFn = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                          uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// MeshInstance::Init(instance, -, -, prototype, desc, -, 7 stack words): forwarded unchanged.
U2MP_GUEST uint64_t mesh_init_logged(uint64_t inst, uint64_t a1, uint64_t a2, uint64_t proto, uint64_t desc,
                                     uint64_t a5, uint64_t s0, uint64_t s1, uint64_t s2, uint64_t s3, uint64_t s4,
                                     uint64_t s5, uint64_t s6) {
    const uint64_t r = reinterpret_cast<MeshInitFn>(image.base + fMeshInit)(inst, a1, a2, proto, desc, a5, s0, s1,
                                                                             s2, s3, s4, s5, s6);
    if (!diag_mode || bad_pointer(inst) || bad_pointer(proto)) return r;
    const uint64_t from = uint64_t(uintptr_t(__builtin_return_address(0))) - uint64_t(uintptr_t(image.base));
    const bool actor = from == cInitA + 5, geo = from == cInitB + 5;
    char name[48];
    if (!readable_string(*reinterpret_cast<const char *const *>(uintptr_t(proto) + 0x10), name, sizeof name))
        std::strcpy(name, "?");
    const bool hero = std::strstr(name, "mp_hero") != nullptr;
    int slot;
    const char *kind;
    if (hero || diag_mode == 2) {
        slot = int(__atomic_fetch_add(&hero_next, 1, __ATOMIC_RELAXED) % kHeroSlots);
        kind = hero ? "hero" : "any";
    } else if (actor) {
        slot = kActorBase + int(__atomic_fetch_add(&actor_next, 1, __ATOMIC_RELAXED) % kActorRefs);
        kind = "reference actor";
    } else if (geo) {
        slot = kGeoBase + int(__atomic_fetch_add(&geo_next, 1, __ATOMIC_RELAXED) % kGeoRefs);
        kind = "reference level";
    } else {
        return r;
    }
    Tracked &t = tracked[slot];
    const uint32_t gen = __atomic_add_fetch(&generation_next, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&t.instance, uint64_t(0), __ATOMIC_RELEASE);
    t.generation = gen;
    std::memcpy(t.name, name, sizeof t.name);
    t.kind = kind;
    t.born = __atomic_load_n(&walker_calls, __ATOMIC_RELAXED);
    t.submits = t.submits_reported = 0;
    t.logged_first = false;
    t.render_object = actor ? render_object_of(inst - 0x1c0) : 0;
    t.collects = t.collects_reported = 0;
    t.draw_jobs = t.draw_jobs_reported = 0;
    __atomic_store_n(&t.instance, inst, __ATOMIC_RELEASE);
    const uint8_t *in = reinterpret_cast<const uint8_t *>(uintptr_t(inst));
    list_insert(inst + 0x170, slot, 0, gen);
    list_insert(inst + 0x1b0, slot, 1, gen);
    if (actor) list_insert(inst + 0x200, slot, 3, gen);   // 0xbe4bb8: lea r12,[r14+0x3c0] = MeshInstance +0x200
    const int32_t nsub = *reinterpret_cast<const int32_t *>(in + 0x164);
    const uint64_t subs = *reinterpret_cast<const uint64_t *>(in + 0x168);
    for (int32_t i = 0; i < nsub && i < 64 && !bad_pointer(subs); ++i)
        list_insert(subs + 0x3a0 * uint64_t(i) + 0x340, slot, 2, gen);
    const uint32_t *pw = reinterpret_cast<const uint32_t *>(uintptr_t(proto));
    log("render diag: init %s %p prototype '%s' (%u submeshes, cbuffer capacity %u), %d sub-elements, %u instance "
        "cbuffers, called from %llx", kind, static_cast<const void *>(in), name, pw[1], pw[12], nsub,
        *reinterpret_cast<const uint32_t *>(in + 0x170), (unsigned long long)from);
    log_instance("init", in);
    return r;
}

// 0xc3f4b0 (the instance teardown), called for actors at 0xbe46c5: logs a tracked instance's end.
using MeshFreeFn = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t, uint64_t);
U2MP_GUEST uint64_t mesh_free_logged(uint64_t inst, uint64_t a1, uint64_t a2, uint64_t a3) {
    for (int i = 0; diag_mode && i < kTracked; ++i) {
        Tracked &t = tracked[i];
        if (__atomic_load_n(&t.instance, __ATOMIC_ACQUIRE) != inst) continue;
        log("render diag: destroy %s %p '%s' after %llu walker calls, %llu submits", t.kind,
            reinterpret_cast<const void *>(uintptr_t(inst)), t.name,
            (unsigned long long)(__atomic_load_n(&walker_calls, __ATOMIC_RELAXED) - t.born),
            (unsigned long long)__atomic_load_n(&t.submits, __ATOMIC_RELAXED));
        __atomic_store_n(&t.instance, uint64_t(0), __ATOMIC_RELEASE);
    }
    return reinterpret_cast<MeshFreeFn>(image.base + fMeshFree)(inst, a1, a2, a3);
}

using WalkerFn = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
unsigned char *walker_original = nullptr;
void log_hero_state(const char *when);   // section 6

// The walker (list, -, -, -, flag, override table, override count): forwarded unchanged.
U2MP_GUEST uint64_t walker_logged(uint64_t list, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t table,
                                  uint64_t count) {
    const uint64_t calls = __atomic_add_fetch(&walker_calls, 1, __ATOMIC_RELAXED);
    int code = 0;
    const int owner = list_find(list, &code);
    if (owner >= 0) {
        Tracked &t = tracked[owner];
        __atomic_add_fetch(&t.submits, 1, __ATOMIC_RELAXED);
        if (!t.logged_first) {
            t.logged_first = true;
            const int32_t n = int32_t(uint32_t(count));
            char ids[200] = "";
            size_t at = 0;
            for (int32_t i = 0; i < n && i < 8 && !bad_pointer(table); ++i) {
                const uint32_t *e = reinterpret_cast<const uint32_t *>(uintptr_t(table) + 16 * uint64_t(i));
                const int w = std::snprintf(ids + at, sizeof ids - at, " %u/%08x->%llx", e[0], e[1],
                                            (unsigned long long)*reinterpret_cast<const uint64_t *>(e + 2));
                if (w < 0 || size_t(w) >= sizeof ids - at) break;
                at += size_t(w);
            }
            log("render diag: first submit of %s %p '%s' through list %s (%llu walker calls after init), override "
                "table %d entries:%s", t.kind, reinterpret_cast<const void *>(uintptr_t(t.instance)), t.name,
                kListNames[code & 3], (unsigned long long)(calls - t.born), n, ids);
            log_instance("submit", reinterpret_cast<const uint8_t *>(uintptr_t(t.instance)));
        }
    }
    if ((calls & 0x3ffff) == 0) {
        log("render diag: summary at walker call %llu; list table %u of %d used, %u lists not watched (table full)",
            (unsigned long long)calls, __atomic_load_n(&lists_used, __ATOMIC_RELAXED), kListSlots,
            __atomic_load_n(&lists_dropped, __ATOMIC_RELAXED));
        for (int i = 0; i < kTracked; ++i) {
            Tracked &t = tracked[i];
            const uint64_t inst = __atomic_load_n(&t.instance, __ATOMIC_ACQUIRE);
            if (!inst) continue;
            const uint64_t s = __atomic_load_n(&t.submits, __ATOMIC_RELAXED);
            log("render diag: %s %p '%s': %llu walker submits (+%llu)", t.kind,
                reinterpret_cast<const void *>(uintptr_t(inst)), t.name, (unsigned long long)s,
                (unsigned long long)(s - t.submits_reported));
            t.submits_reported = s;
            if (i == 0) log_hero_state("summary");
            if (i < kHeroSlots) log_instance("now", reinterpret_cast<const uint8_t *>(uintptr_t(inst)));
            if (t.render_object || i < kActorBase + kActorRefs) log_gate(t, inst);
            t.collects_reported = t.collects;
            if (i < kHeroSlots && t.draw_jobs != t.draw_jobs_reported) {
                log("render diag: %s %p: %llu draw jobs (0xc419d0) (+%llu)", t.kind,
                    reinterpret_cast<const void *>(uintptr_t(inst)), (unsigned long long)t.draw_jobs,
                    (unsigned long long)(t.draw_jobs - t.draw_jobs_reported));
            }
            t.draw_jobs_reported = t.draw_jobs;
        }
    }
    return reinterpret_cast<WalkerFn>(walker_original)(list, a1, a2, a3, a4, table, count);
}

bool install_draw_diag(int &installed) {
    if (const char *v = std::getenv("UNCHARTED_U2MP_RENDER_DIAG")) {
        if (std::strcmp(v, "0") == 0) diag_mode = 0;
        else if (std::strcmp(v, "all") == 0) diag_mode = 2;
    }
    if (!diag_mode) {
        log("render diag: off (UNCHARTED_U2MP_RENDER_DIAG=0)");
        return false;
    }
    static const unsigned char init_prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                                  0x41, 0x54, 0x53, 0x48, 0x81, 0xe4};
    // push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx (13 bytes, no rip use); sub rsp,0x78
    static const unsigned char walker_prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                                    0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x78};
    if (!image.matches(fMeshInit, init_prologue, sizeof init_prologue) ||
        !image.matches(fWalker, walker_prologue, sizeof walker_prologue) || !image.executable(fWalker, 13)) {
        log("render diag: MeshInstance::Init / walker bytes differ, left alone");
        return false;
    }
    int n = 0;
    n += retarget_call(cInitA, fMeshInit, reinterpret_cast<void *>(&mesh_init_logged), "render diag: MeshInstance::Init A");
    n += retarget_call(cInitB, fMeshInit, reinterpret_cast<void *>(&mesh_init_logged), "render diag: MeshInstance::Init B");
    n += retarget_call(cInitC, fMeshInit, reinterpret_cast<void *>(&mesh_init_logged), "render diag: MeshInstance::Init C");
    n += retarget_call(cFreeActor, fMeshFree, reinterpret_cast<void *>(&mesh_free_logged), "render diag: actor mesh teardown");
    n += retarget_call(cDrawDataA, fDrawDataNew, reinterpret_cast<void *>(&drawdata_new_logged), "render diag: AddMesh draw data A");
    n += retarget_call(cDrawDataB, fDrawDataNew, reinterpret_cast<void *>(&drawdata_new_logged), "render diag: AddMesh draw data B");
    // 0xbd6d00: mov r8d,[rsi]; mov eax,0x800 (8 bytes, no rip use) -> trampoline, entry jumps to the host
    static const unsigned char collect_prologue[] = {0x44, 0x8b, 0x06, 0xb8, 0x00, 0x08, 0x00, 0x00, 0x44, 0x29, 0xc0};
    if (image.matches(fCollect, collect_prologue, sizeof collect_prologue) && image.executable(fCollect, 8)) {
        unsigned char *ctramp = near_stub(32);
        unsigned char *centry = near_stub(16);
        const int64_t cdelta = centry ? int64_t(uintptr_t(centry)) - int64_t(uintptr_t(image.base + fCollect + 5)) : 0;
        if (ctramp && centry && cdelta >= INT32_MIN && cdelta <= INT32_MAX) {
            std::memcpy(ctramp, collect_prologue, 8);
            ctramp[8] = 0xff;
            ctramp[9] = 0x25;
            std::memset(ctramp + 10, 0, 4);
            const uint64_t cback = uint64_t(uintptr_t(image.base + fCollect + 8));
            std::memcpy(ctramp + 14, &cback, 8);
            collect_original = ctramp;
            const uint64_t chost = uint64_t(uintptr_t(&collect_logged));
            centry[0] = 0x48;
            centry[1] = 0xb8;
            std::memcpy(centry + 2, &chost, 8);
            centry[10] = 0xff;
            centry[11] = 0xe0;
            const int32_t crel = int32_t(cdelta);
            unsigned char *cc = image.base + fCollect;
            cc[0] = 0xe9;
            std::memcpy(cc + 1, &crel, 4);
            std::memset(cc + 5, 0x90, 3);
            ++n;
            log("render diag: render object collect 0xbd6d00 counted");
        }
    } else {
        log("render diag: 0xbd6d00 differs, render object collects not counted");
    }
    // 0xc419d0: the same 13-byte prologue as the walker, then and rsp,-32 (48 81 e4 e0 ff ff ff)
    static const unsigned char job_prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                                 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xe4, 0xe0};
    if (image.matches(fDrawJob, job_prologue, sizeof job_prologue) && image.executable(fDrawJob, 13)) {
        unsigned char *jtramp = near_stub(32);
        unsigned char *jentry = near_stub(16);
        const int64_t jdelta = jentry ? int64_t(uintptr_t(jentry)) - int64_t(uintptr_t(image.base + fDrawJob + 5)) : 0;
        if (jtramp && jentry && jdelta >= INT32_MIN && jdelta <= INT32_MAX) {
            std::memcpy(jtramp, job_prologue, 13);
            jtramp[13] = 0xff;
            jtramp[14] = 0x25;
            std::memset(jtramp + 15, 0, 4);
            const uint64_t jback = uint64_t(uintptr_t(image.base + fDrawJob + 13));
            std::memcpy(jtramp + 19, &jback, 8);
            drawjob_original = jtramp;
            const uint64_t jhost = uint64_t(uintptr_t(&drawjob_logged));
            jentry[0] = 0x48;
            jentry[1] = 0xb8;
            std::memcpy(jentry + 2, &jhost, 8);
            jentry[10] = 0xff;
            jentry[11] = 0xe0;
            const int32_t jrel = int32_t(jdelta);
            unsigned char *jc = image.base + fDrawJob;
            jc[0] = 0xe9;
            std::memcpy(jc + 1, &jrel, 4);
            std::memset(jc + 5, 0x90, 8);
            ++n;
            log("render diag: per-instance draw job 0xc419d0 counted");
        }
    } else {
        log("render diag: 0xc419d0 differs, draw jobs not counted");
    }
    installed += n;
    // walker: trampoline = its 13-byte prologue + jmp back to 0xc2567d; the entry jumps to the host
    unsigned char *tramp = near_stub(32);
    unsigned char *entry = near_stub(16);
    const int64_t delta = entry ? int64_t(uintptr_t(entry)) - int64_t(uintptr_t(image.base + fWalker + 5)) : 0;
    if (!tramp || !entry || delta < INT32_MIN || delta > INT32_MAX) {
        log("render diag: no stub memory for the walker hook");
        return n > 0;
    }
    std::memcpy(tramp, walker_prologue, 13);
    tramp[13] = 0xff;   // jmp [rip+0]
    tramp[14] = 0x25;
    std::memset(tramp + 15, 0, 4);
    const uint64_t back = uint64_t(uintptr_t(image.base + fWalker + 13));
    std::memcpy(tramp + 19, &back, 8);
    walker_original = tramp;
    const uint64_t host = uint64_t(uintptr_t(&walker_logged));
    entry[0] = 0x48;    // mov rax, host ; jmp rax   (rax carries no argument)
    entry[1] = 0xb8;
    std::memcpy(entry + 2, &host, 8);
    entry[10] = 0xff;
    entry[11] = 0xe0;
    const int32_t rel = int32_t(delta);
    unsigned char *c = image.base + fWalker;
    c[0] = 0xe9;
    std::memcpy(c + 1, &rel, 4);
    std::memset(c + 5, 0x90, 8);
    ++installed;
    log("render diag: MeshInstance::Init, teardown and the binding walker logged for %s",
        diag_mode == 2 ? "every prototype" : "mp_hero prototypes + 8 actor / 8 level references");
    return true;
}

// ---- 6. The local hero's draw data stay "not drawable" (test runs 41-45: the player is never drawn) --
// Run 45: the hero's render object is collected every frame (0xbd6d00), but its draw data flags (+0x1b8)
// are 0x6a00 where every drawn actor has 0x2000. The extra bits come from the spawn's init chain:
//   0x0200  Character::Init 0x473f80 (0x474003, with object +0x155 |= 2) on every mesh. The per-instance
//           draw preparation 0xc47790 skips a draw data with it (0xc47db1: test byte +0x1b9, 2). The
//           campaign clears it when the object goes active: the NdGameObject active-state enters
//           0x987840 / 0x988c00 (0x9880c3, 0x989023, via the states 0x9ad5f0 / 0x9adbb0) and the player's
//           active state 0x260920 (0x260c33, which also clears 0x4000 and object +0x154 bit 2).
//   0x0800  the player Init 0x724b70 (0x724cf3, with 0x200 at 0x724ca3); the player event handler
//           0x7edd80 sets / clears it with 0x20 ("hide" events, 0x7ee1f6), the weapon code clears 0x820.
//   0x4000  the spawning / dead hide (NetCharacter::Init 0x5fd443 with object +0x154 |= 2, cleared by
//           its state enter 0x5f9750; the player's 0x260920 / 0x7a1d80 clear it the same way).
// The local MP hero is spawned by 0x663280 with 0x976300 (its only caller, 0x6634de) and never runs
// one of those clearing enters, so it stays hidden. PS3 1.10's local spawn 0x4c9b58 has the same calls
// after the spawn (0x6b84f4); the clearing comes from the object's state machine, which is not ported
// here. What is done: right after 0x976300 returns the hero, the three bits are cleared on all its draw
// data and the matching object bits (+0x154 / +0x155 bit 2) are cleared too, i.e. the effect of the
// active-state enter. Logged with the flags before and after. UNCHARTED_U2MP_HERO_SHOW=0 turns it off.
constexpr uint64_t fSpawnProcess = 0x976300, cSpawnHero = 0x6634de;
constexpr uint32_t kHeroHideBits = 0x4000 | 0x0800 | 0x0200;
using SpawnProcessFn = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// Every draw data of a game object's DrawControl (object +0x180: +0x328 meshes, 0x50 apart from +8,
// each {+0x48 count, draw data list at +0 of 0x10-byte entries}). Returns the number visited.
template <typename F> int for_each_draw_data(uint64_t object, F &&f) {
    const uint64_t dc = *reinterpret_cast<const uint64_t *>(uintptr_t(object) + 0x180);
    if (bad_pointer(dc)) return -1;
    const uint32_t meshes = *reinterpret_cast<const uint32_t *>(uintptr_t(dc) + 0x328);
    if (meshes > 64) return -1;
    int n = 0;
    for (uint32_t m = 0; m < meshes; ++m) {
        const uint8_t *mesh = reinterpret_cast<const uint8_t *>(uintptr_t(dc) + 8 + 0x50 * uint64_t(m));
        const uint32_t lods = *reinterpret_cast<const uint32_t *>(mesh + 0x40);   // DC + i*0x50 + 0x48
        for (uint32_t l = 0; l < lods && l < 16; ++l) {
            const uint64_t dd = *reinterpret_cast<const uint64_t *>(mesh + 0x10 * uint64_t(l));
            if (bad_pointer(dd)) continue;
            f(reinterpret_cast<uint32_t *>(uintptr_t(dd) + 0x1b8));
            ++n;
        }
    }
    return n;
}

bool hero_show = true;
uint64_t hero_object = 0;

// NetCharacter / player state descriptors (the Go<State> functions register these statics)
struct StateName {
    uint64_t desc;
    const char *name;
};
const StateName kStates[] = {{0x14ced60, "NetCharacter Active"}, {0x14ced88, "NetCharacter Idle"},
                             {0x14cedb0, "NetCharacter NetMove"}, {0x14cedd8, "NetCharacter NetFight"},
                             {0x14cee00, "NetCharacter Script"}, {0x14cec18, "NetCharacter Dead"},
                             {0x14cec40, "NetCharacter Ragdoll"}};

const char *state_name(uint64_t p) {
    for (const auto &s : kStates)
        if (p == uint64_t(uintptr_t(image.base + s.desc))) return s.name;
    return nullptr;
}

// ---- run 48: is the hero's animation updated? -------------------------------------------------------
// Run 46 read anim data byte +0 bit 0x80 as "animation updated". It is not, for a character: the anim
// update 0xb53250 sets it (0xb53a2d) only when byte +5+phase lacks bit 2 and clears it (0xb53e4a) when
// that bit is set; update type 3 (characters, 0xb4f680: +3..+6 = 01 02 03 05) has +5 = 3, so phase 0
// always clears it and phase 1 (+4 = 2) takes the stage-2 branch (0xb53d20) that leaves it alone. A
// type-3 character never has 0x80, the drawn references (type 2, +5 = 5) always do. The hero's 0x38 is
// the expected value. Also, the per-frame collect 0xbd6d00 (run 45: the hero is collected every frame)
// is only reached through 0xbd6240, which skips anim data with skin matrices (+0x108) == 0, and the
// only writers of +0x108 are the anim update (0xb535d4 / 0xb53b5f) and AnimateDisabledObjectJob
// (0xb54517, for anim data in the manager's "disabled" set +0x600, which nothing in this build sets).
// So the hero's animation has been updated. This probe counts it per frame to settle it:
//   * the 5 calls of 0xb53250 (0xb5258a in the manager update 0xb50ff0; the jobs 0xb52d70 x2,
//     0xb531d0, 0xb54580) go through anim_update_counted, which counts calls for the hero's anim data
//     by phase (record +4) and second pass (record +0xa);
//   * log_hero_state adds the anim data's update bytes, m_pAnimControl (+0x10, 0 = the update only
//     writes identity matrices, 0xb53507), skin matrices (+0x108, a new frame allocation per update),
//     the world position (+0xe0) next to the object's locator (+0xf0), and its bits in the anim manager
//     0x123e980 (+0 new, +0x200 live, +0x400 freed, +0x600 disabled, +0x800 excluded, +0x1000 + 0x200 *
//     bucket; the frame updates buckets 0-5, 0x3fdb00).
// Reading run 48: phase-1 updates rising and +0x108 changing = animated and skinned every frame, so the
// drop is after the collect (draw jobs, run-47 counter); world position at 0,0,0 while the locator is
// elsewhere = drawn at the origin; no updates = not in a bucket 0-5 / excluded (the bits say which).
constexpr uint64_t fAnimUpdate = 0xb53250, kAnimMgr = 0x123e980;
constexpr uint64_t cAnimUpdate[] = {0xb5258a, 0xb52dc1, 0xb52df8, 0xb5321e, 0xb545d4};
uint64_t hero_anim = 0;
uint64_t hero_updates[4];            // [phase * 2 + second pass]
uint64_t anim_updates_total = 0;

using AnimUpdateFn = void(U2MP_GUEST *)(uint64_t);
// 0xb53250(record): record +4 phase (u32), +8 / +9 / +0xa flags (u8, +0xa = second pass), +0x10 job
// data, +0x18 the anim data. Forwarded unchanged.
U2MP_GUEST void anim_update_counted(uint64_t rec) {
    __atomic_add_fetch(&anim_updates_total, 1, __ATOMIC_RELAXED);
    const uint64_t hero = __atomic_load_n(&hero_anim, __ATOMIC_RELAXED);
    if (hero && *reinterpret_cast<const uint64_t *>(uintptr_t(rec) + 0x18) == hero) {
        const uint32_t phase = *reinterpret_cast<const uint32_t *>(uintptr_t(rec) + 4) & 1u;
        const uint32_t second = *reinterpret_cast<const uint8_t *>(uintptr_t(rec) + 0xa) ? 1u : 0u;
        __atomic_add_fetch(&hero_updates[phase * 2 + second], 1, __ATOMIC_RELAXED);
    }
    reinterpret_cast<AnimUpdateFn>(image.base + fAnimUpdate)(rec);
}

// The anim data's bits in the manager: "N" new, "L" live, "F" freed, "D" disabled, "X" excluded, then
// the bucket digits it is in.
void anim_manager_bits(uint64_t anim, char *out, size_t n) {
    const uint8_t *mgr = image.base + kAnimMgr;
    const uint64_t pool = *reinterpret_cast<const uint64_t *>(mgr + 0x2000);
    if (bad_pointer(pool) || anim < pool || ((anim - pool) & 0x1ff) || ((anim - pool) >> 9) >= 0x1000) {
        std::snprintf(out, n, "not in the pool %llx", (unsigned long long)pool);
        return;
    }
    const uint64_t idx = (anim - pool) >> 9;
    auto bit = [&](uint32_t off) {
        return (*reinterpret_cast<const uint64_t *>(mgr + off + 8 * (idx >> 6)) >> (idx & 63)) & 1;
    };
    char buckets[9] = "";
    int nb = 0;
    for (uint32_t b = 0; b < 8; ++b)
        if (bit(0x1000 + 0x200 * b)) buckets[nb++] = char('0' + b);
    buckets[nb] = 0;
    std::snprintf(out, n, "index %llu %s%s%s%s%s bucket %s", (unsigned long long)idx, bit(0) ? "N" : "-",
                  bit(0x200) ? "L" : "-", bit(0x400) ? "F" : "-", bit(0x600) ? "D" : "-", bit(0x800) ? "X" : "-",
                  nb ? buckets : "none");
}

// The hero object: its class (vtable), state machine (+0x50: the Go<State> functions write the pending
// state at +8), and its anim data (+0x190 -> +8 = the render object). Bytes +2..+6 of the anim data are
// the update type set by 0xb4f680 (type 2: 02 01 00 05 00 = the pool default, drawn props; type 3:
// 03 01 02 03 05 = what character Inits ask for, 0x995ed0(obj, 3)); bit 0x80 of +0: see run 48 above.
void log_hero_state(const char *when) {
    const uint64_t obj = hero_object;
    if (!obj || bad_pointer(obj)) return;
    const uint8_t *o = reinterpret_cast<const uint8_t *>(uintptr_t(obj));
    const uint64_t vt = *reinterpret_cast<const uint64_t *>(o);
    const uint64_t sm = *reinterpret_cast<const uint64_t *>(o + 0x50);
    char smtext[256] = "-";
    if (!bad_pointer(sm)) {
        const uint64_t *w = reinterpret_cast<const uint64_t *>(uintptr_t(sm));
        size_t at = 0;
        for (int i = 0; i < 6; ++i) {
            const char *nm = state_name(w[i]);
            const int k = std::snprintf(smtext + at, sizeof smtext - at, " +%x=%llx%s%s%s", i * 8,
                                        (unsigned long long)w[i], nm ? " (" : "", nm ? nm : "", nm ? ")" : "");
            if (k < 0 || size_t(k) >= sizeof smtext - at) break;
            at += size_t(k);
        }
    }
    uint64_t anim = 0, ad0 = 0;
    const uint64_t ac = *reinterpret_cast<const uint64_t *>(o + 0x190);
    if (!bad_pointer(ac)) anim = *reinterpret_cast<const uint64_t *>(uintptr_t(ac) + 8);
    if (!bad_pointer(anim)) ad0 = *reinterpret_cast<const uint64_t *>(uintptr_t(anim));
    log("render: hero %s %p: vtable %llx, +34 %02x %02x %02x, state machine %llx:%s; anim data %llx +0 %016llx",
        when, static_cast<const void *>(o), (unsigned long long)(vt - uint64_t(uintptr_t(image.base))), o[0x34],
        o[0x35], o[0x36], (unsigned long long)sm, smtext, (unsigned long long)anim, (unsigned long long)ad0);
    if (bad_pointer(anim)) return;
    __atomic_store_n(&hero_anim, anim, __ATOMIC_RELAXED);
    const uint8_t *a = reinterpret_cast<const uint8_t *>(uintptr_t(anim));
    auto q = [&](uint32_t off) { return (unsigned long long)*reinterpret_cast<const uint64_t *>(a + off); };
    const float *wp = reinterpret_cast<const float *>(a + 0xe0);
    const float *lp = reinterpret_cast<const float *>(o + 0xf0);
    char bits[96];
    anim_manager_bits(anim, bits, sizeof bits);
    static uint64_t reported[4] = {0, 0, 0, 0};
    uint64_t u[4];
    for (int i = 0; i < 4; ++i) u[i] = __atomic_load_n(&hero_updates[i], __ATOMIC_RELAXED);
    log("render: hero %s anim data: type %02x %02x %02x %02x %02x, m_pAnimControl %llx, +18 %llx, +98 %llx, "
        "skin matrices +108 %llx (previous %llx), callback +160 %llx; world pos %.2f %.2f %.2f, object locator "
        "%.2f %.2f %.2f; manager %s; anim updates phase 0 %llu (+%llu) / second pass %llu, phase 1 %llu (+%llu) / "
        "second pass %llu, all objects %llu",
        when, a[2], a[3], a[4], a[5], a[6], q(0x10), q(0x18), q(0x98), q(0x108), q(0x110), q(0x160), wp[0], wp[1],
        wp[2], lp[0], lp[1], lp[2], bits, (unsigned long long)u[0], (unsigned long long)(u[0] - reported[0]),
        (unsigned long long)u[1], (unsigned long long)u[2], (unsigned long long)(u[2] - reported[2]),
        (unsigned long long)u[3], (unsigned long long)__atomic_load_n(&anim_updates_total, __ATOMIC_RELAXED));
    for (int i = 0; i < 4; ++i) reported[i] = u[i];
}

U2MP_GUEST uint64_t spawn_hero_shown(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    const uint64_t obj = reinterpret_cast<SpawnProcessFn>(image.base + fSpawnProcess)(a0, a1, a2, a3, a4, a5);
    if (bad_pointer(obj)) return obj;
    if (!hero_show) {   // only the run-48 probe
        hero_object = obj;
        log_hero_state("spawned");
        return obj;
    }
    uint32_t before_or = 0, after_or = 0;
    const int n = for_each_draw_data(obj, [&](uint32_t *flags) {
        before_or |= *flags;
        __atomic_and_fetch(flags, ~kHeroHideBits, __ATOMIC_RELAXED);
        after_or |= *flags;
    });
    uint8_t *o = reinterpret_cast<uint8_t *>(uintptr_t(obj));
    const uint8_t b154 = o[0x154], b155 = o[0x155];
    if (n > 0) {
        o[0x154] = uint8_t(b154 & ~2u);
        o[0x155] = uint8_t(b155 & ~2u);
    }
    log("render: hero spawned %p: %d draw data, flags (or of all) %08x -> %08x, object +154 %02x +155 %02x -> %02x %02x",
        reinterpret_cast<void *>(uintptr_t(obj)), n, before_or, after_or, b154, b155, o[0x154], o[0x155]);
    hero_object = obj;
    log_hero_state("spawned");
    return obj;
}

}  // namespace

// Called from install_nullsites (nullsites.cpp), so install.cpp needs no line of its own.
void install_render(int &installed) {
    const bool diag = install_draw_diag(installed);
    if (const char *v = std::getenv("UNCHARTED_U2MP_HERO_SHOW"); v && std::strcmp(v, "0") == 0) hero_show = false;
    if ((hero_show || diag) &&
        retarget_call(cSpawnHero, fSpawnProcess, reinterpret_cast<void *>(&spawn_hero_shown),
                      hero_show ? "render: local hero spawn (draw data shown)" : "render: local hero spawn (logged)"))
        ++installed;
    if (diag) {   // run 48: count the hero's anim updates (section 6)
        int n = 0;
        for (const uint64_t site : cAnimUpdate)
            n += retarget_call(site, fAnimUpdate, reinterpret_cast<void *>(&anim_update_counted),
                               "render diag: anim update 0xb53250 counted");
        installed += n;
    }
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
