// u2mp nullsites: places where the PS4 1.02 multiplayer code reads through a pointer that is NULL only
// because PS4 removed the PS3 code that produced it (a stripped accessor folded to 0, or a global that
// the PS3 network init set and nothing on PS4 writes). Found by a systematic scan of the PS4 code
// (u2mp/docs/PORT-NULLSITES.md lists every candidate, the PS3 reference and why it is or is not
// patched here). Sites already fixed elsewhere are not repeated: remaining.cpp (0x66c75f strlen/memcpy
// of the host name, 0x4bf44c empty tag table, 0x640598 tracker values, 0x644130 level record
// package), session.cpp (net clock 0x113a040, 0x14ce990 room-attribute list via 0x5eb830),
// cinema.cpp (replay stream 0x113a258 when the cinema port is installed, cinema lists 0x113a990+0x1b0).
//
// Addresses are Uncharted 2 PS4 1.02 image offsets, PS3 1.10 originals in the comments. Every patch
// checks the bytes it replaces first and logs what it did.
#include <cstdint>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
namespace {

// ---- 1. WaitReady::Exit (0x5f2830, PS3 0x443f00) writes the replay stream start -------------------
// The replay stream 0x113a258 {u32 *data; u32 bits; u32 max_bits; u32 bytes} (PS3 0xf119b4) got its
// 128 KB buffer from the PS3 network init (0x448d68). PS4 has no such init and its static initializer
// (0x5ef970) leaves data = NULL; WaitReady::Exit then does `mov r8,[r14]; mov dword [r8],5` (0x5f2aa3)
// = a write to address 0 on every match start, and Playing::Update's stream pump (0x608c40, called at
// 0x5f177b) reads the same pointer. cinema.cpp gives the stream its buffer from a WaitReady::Exit hook
// when the cinema port installs; this is the fallback for when it does not: a stub at 0x5f2aa3 that
// attaches a buffer of the PS3 size when data is still NULL, then continues as the original. When
// cinema.cpp's hook ran first, data is already set and the stub only repeats the two original
// instructions. (cinema.cpp's ensure_stream replaces a foreign buffer with its own, so the two agree.)
constexpr uint64_t kStream = 0x113a258;
constexpr uint64_t cStreamStart = 0x5f2aa3, cStreamStartNext = 0x5f2aad;
constexpr size_t kStreamBytes = 0x20000;   // PS3 NetInit: 128 KB
alignas(64) uint32_t stream_buffer[(kStreamBytes + 64) / 4];   // spare words: readers look one word ahead

bool install_stream_start() {
    // 0x5f2a8c: lea r14,[rip+0xb477c5] (= 0x113a258); mov dword [r14+8],0; mov dword [r14+0x10],0
    static const unsigned char lea[] = {0x4c, 0x8d, 0x35};
    static const unsigned char clears[] = {0x41, 0xc7, 0x46, 0x08, 0, 0, 0, 0, 0x41, 0xc7, 0x46, 0x10, 0, 0, 0, 0};
    // 0x5f2aa3: mov r8,[r14]; mov dword [r8],5
    static const unsigned char site[] = {0x4d, 0x8b, 0x06, 0x41, 0xc7, 0x00, 0x05, 0x00, 0x00, 0x00};
    int32_t rel = 0;
    if (image.matches(0x5f2a8c, lea, sizeof lea)) std::memcpy(&rel, image.base + 0x5f2a8f, 4);
    if (!image.matches(0x5f2a8c, lea, sizeof lea) || 0x5f2a93 + int64_t(rel) != int64_t(kStream) ||
        !image.matches(0x5f2a93, clears, sizeof clears) || !image.matches(cStreamStart, site, sizeof site) ||
        !image.executable(cStreamStart, sizeof site)) {
        log("replay stream start (5f2aa3): bytes differ, left alone");
        return false;
    }
    unsigned char *stub = near_stub(64);
    const int64_t delta = stub ? int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + cStreamStart + 5)) : 0;
    if (!stub || delta < INT32_MIN || delta > INT32_MAX) {
        log("replay stream start (5f2aa3): no stub memory in range");
        return false;
    }
    unsigned char code[] = {
        0x4d, 0x8b, 0x06,                         // mov r8,[r14]          (original)
        0x4d, 0x85, 0xc0,                         // test r8,r8
        0x75, 0x15,                               // jnz have
        0x49, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,       // mov r8, stream_buffer
        0x4d, 0x89, 0x06,                         // mov [r14],r8          data
        0x41, 0xc7, 0x46, 0x0c, 0, 0, 0, 0,       // mov dword [r14+0xc], max bits
        // have:
        0x41, 0xc7, 0x00, 0x05, 0x00, 0x00, 0x00, // mov dword [r8],5      (original)
        0xff, 0x25, 0, 0, 0, 0,                   // jmp [rip] -> 0x5f2aad
        0, 0, 0, 0, 0, 0, 0, 0};
    const uint64_t buffer = uint64_t(uintptr_t(stream_buffer));
    const uint32_t max_bits = uint32_t(kStreamBytes * 8);
    const uint64_t back = uint64_t(uintptr_t(image.base + cStreamStartNext));
    std::memcpy(code + 10, &buffer, 8);
    std::memcpy(code + 25, &max_bits, 4);
    std::memcpy(code + 42, &back, 8);
    static_assert(sizeof code == 50, "stub layout");
    std::memcpy(stub, code, sizeof code);
    unsigned char *c = image.base + cStreamStart;
    const int32_t jump = int32_t(delta);
    c[0] = 0xe9;
    std::memcpy(c + 1, &jump, 4);
    std::memset(c + 5, 0x90, sizeof site - 5);
    log("replay stream start (5f2aa3): buffer attached when the stream has none (PS3 NetInit 0x448d68)");
    return true;
}

// ---- 2. Shader-resource binding with an unresolved resource (test run 35) ---------------------------
// The binding walker 0xc25670 (a render job, reached from 0xc419d0's list at +0x170) goes over 48-byte
// entries {const desc *res; void *buffer; const param *def; ...; u32 used @+0x20; u8 dirty @+0x28} and
// for each live one calls 0xc12150(&res, def->size, &entry.buffer), which reads the 32-byte resource
// descriptor at `res` (type u16 at +0x18) and copies it into a new GPU buffer. Run 35 reached Playing on
// Shambhala and crashed at 0xc12173 on the first frame: res = 0x33d5c036f1b17bc2, a non-canonical
// value built at run time (the pair is in no file), i.e. a binding whose resource was never linked.
// This guard at the one call that crashed (0xc258b2) logs the entry, so the asset can be found, and
// lets that frame bind an all-zero descriptor (a null resource) instead of crashing.
constexpr uint64_t cBindCall = 0xc258b2, fBindCopy = 0xc12150;
alignas(64) uint64_t zero_descriptor[8];
using BindCopy = void *(U2MP_GUEST *)(const void **, uint64_t, void *);

bool bad_pointer(uint64_t p) { return p < 0x10000 || p >= 0x0000800000000000ull; }

extern uint64_t repaired_count, repaired_last;

U2MP_GUEST void *bind_copy_checked(const void **res, uint64_t size, void *out) {
    static uint64_t reported = 0;
    if (repaired_count != reported && (reported < 8 || (repaired_count & 1023) == 0)) {
        reported = repaired_count;
        log("bind entry repair: %llu entries repaired so far (last unresolved key %016llx)",
            (unsigned long long)reported, (unsigned long long)repaired_last);
    }
    const uint64_t r = uint64_t(uintptr_t(*res));
    if (bad_pointer(r)) {
        static int logged = 0;
        if (logged < 12) {
            ++logged;
            const uint64_t *entry = reinterpret_cast<const uint64_t *>(static_cast<uint8_t *>(out) - 8);
            log("bind guard (c258b2): resource %016llx not a pointer; entry %016llx %016llx %016llx %016llx "
                "%016llx %016llx, size 0x%llx",
                (unsigned long long)r, (unsigned long long)entry[0], (unsigned long long)entry[1],
                (unsigned long long)entry[2], (unsigned long long)entry[3], (unsigned long long)entry[4],
                (unsigned long long)entry[5], (unsigned long long)size);
            const uint64_t def = entry[2];
            if (!bad_pointer(def)) {
                const uint32_t *d = reinterpret_cast<const uint32_t *>(uintptr_t(def));
                log("bind guard: param def %016llx: %08x %08x %08x %08x %08x %08x %08x %08x",
                    (unsigned long long)def, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
            }
        }
        *res = zero_descriptor;   // the caller's local copy only; the entry itself is not changed
    }
    return reinterpret_cast<BindCopy>(image.base + fBindCopy)(res, size, out);
}

// Run 36: with that call guarded, the walker read the same entry again at 0xc25ddb (res->type), and
// the logged entry showed the whole entry is raw ids, not resolved pointers: res = 0x33d5c036f1b17bc2,
// buffer = 0x08d9a770'6f1c0396 (6f1c0396 = sid "general"), param def {name 0x8704e118, id 0x2b444d15,
// 3, size 0x40}. The list is a mesh instance's shader parameters (GeoGroup, ndlib/render/ngen/
// geo-group.cpp: UpdateTransforms 0xb90ba0 marks param 0x2b444d15, the 4x4 transform, dirty), and the
// unresolved value is the key of a parameter reference {name 0x33d5c036, 0x6bbcf890, 0xf1b17bc2, 1}
// that was never linked. Run 37 skipped the entry, and another renderer path (0xc04340, reading
// entry.res at 0xc04403) crashed on it instead. So the entry is repaired, not skipped: the loop's
// `cmp dword [entry+0x20],0; je next` (0xc2574f) becomes a stub that, when entry.res is not a
// user-space pointer, points res and buffer at a zeroed block (a null resource, zero transform) and
// counts it; every later reader then sees valid memory. The walker runs first on a frame (run 35).
constexpr uint64_t cEntryCheck = 0xc2574f, cEntryLive = 0xc2575b, cEntryNext = 0xc25e38;
alignas(64) unsigned char zero_block[0x1000];
uint64_t repaired_count = 0, repaired_last = 0;

bool install_entry_repair() {
    // cmp dword [rax+r13+0x20],0; je 0xc25e38
    static const unsigned char site[] = {0x42, 0x83, 0x7c, 0x28, 0x20, 0x00, 0x0f, 0x84, 0xdd, 0x06, 0x00, 0x00};
    if (!image.matches(cEntryCheck, site, sizeof site) || !image.executable(cEntryCheck, sizeof site)) {
        log("bind entry repair (c2574f): bytes differ, left alone");
        return false;
    }
    unsigned char *stub = near_stub(128);
    const int64_t delta = stub ? int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + cEntryCheck + 5)) : 0;
    if (!stub || delta < INT32_MIN || delta > INT32_MAX) {
        log("bind entry repair (c2574f): no stub memory in range");
        return false;
    }
    // rcx and rdx are free here: the live path writes both before reading them (0xc2576a/0xc257a6, 0xc257c9).
    unsigned char code[] = {
        0x42, 0x83, 0x7c, 0x28, 0x20, 0x00,       //  0 cmp dword [rax+r13+0x20],0  (original)
        0x74, 0x1c,                               //  6 je next
        0x4a, 0x8b, 0x14, 0x28,                   //  8 mov rdx,[rax+r13]           entry.res
        0x48, 0x89, 0xd1,                         // 12 mov rcx,rdx
        0x48, 0xc1, 0xe9, 0x2f,                   // 15 shr rcx,47
        0x75, 0x15,                               // 19 jnz bad                     not user space
        0x48, 0x81, 0xfa, 0x00, 0x00, 0x01, 0x00, // 21 cmp rdx,0x10000
        0x72, 0x0c,                               // 28 jb bad
        0xff, 0x25, 0x36, 0x00, 0x00, 0x00,       // 30 live: jmp [rip+0x36] -> 0xc2575b
        0xff, 0x25, 0x38, 0x00, 0x00, 0x00,       // 36 next: jmp [rip+0x38] -> 0xc25e38
        0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0,       // 42 bad: mov rcx,&repaired_last
        0x48, 0x89, 0x11,                         // 52 mov [rcx],rdx
        0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0,       // 55 mov rcx,&repaired_count
        0xf0, 0x48, 0xff, 0x01,                   // 65 lock inc qword [rcx]
        0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0,       // 69 mov rcx,&zero_block
        0x4a, 0x89, 0x0c, 0x28,                   // 79 mov [rax+r13],rcx           entry.res
        0x4a, 0x89, 0x4c, 0x28, 0x08,             // 83 mov [rax+r13+8],rcx         entry.buffer
        0xeb, 0xc4,                               // 88 jmp live
        0, 0, 0, 0, 0, 0, 0, 0,                   // 90 0xc2575b
        0, 0, 0, 0, 0, 0, 0, 0};                  // 98 0xc25e38
    static_assert(sizeof code == 106, "stub layout");
    const uint64_t last = uint64_t(uintptr_t(&repaired_last));
    const uint64_t count = uint64_t(uintptr_t(&repaired_count));
    const uint64_t zero = uint64_t(uintptr_t(zero_block));
    const uint64_t live = uint64_t(uintptr_t(image.base + cEntryLive));
    const uint64_t next = uint64_t(uintptr_t(image.base + cEntryNext));
    std::memcpy(code + 44, &last, 8);
    std::memcpy(code + 57, &count, 8);
    std::memcpy(code + 71, &zero, 8);
    std::memcpy(code + 90, &live, 8);
    std::memcpy(code + 98, &next, 8);
    std::memcpy(stub, code, sizeof code);
    unsigned char *c = image.base + cEntryCheck;
    const int32_t jump = int32_t(delta);
    c[0] = 0xe9;
    std::memcpy(c + 1, &jump, 4);
    std::memset(c + 5, 0x90, sizeof site - 5);
    log("bind entry repair (c2574f): entries whose resource is not a pointer get a zeroed block");
    return true;
}

}  // namespace

void install_nullsites(int &installed) {
    if (install_stream_start()) ++installed;
    if (install_entry_repair()) ++installed;
    if (retarget_call(cBindCall, fBindCopy, reinterpret_cast<void *>(&bind_copy_checked),
                      "bind guard (unresolved shader resource)"))
        ++installed;
}

}  // namespace u2mp
