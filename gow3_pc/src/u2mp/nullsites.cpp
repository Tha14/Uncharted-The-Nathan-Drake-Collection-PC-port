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

// ---- 2. Constant-buffer entries of mesh instances (test runs 35-38) -> render.cpp ---------------------
// The unresolved "shader resource" of runs 35-38 (res = 0x33d5c036'f1b17bc2) was not a missing link but
// an overflow: AddCb (0xc24b30) appends to a per-instance cbuffer list without checking its capacity,
// and the converted skins stated capacity 2 for 4 cbuffers, so cbuffer keys landed on entry 0.
// render.cpp grows full lists, repairs a bad entry with a real cbuffer resource and gives lookups that
// find nothing a zeroed cbuffer (u2mp/docs/PORT-RENDER.md). The stopgaps that were here moved there.

}  // namespace

void install_render(int &installed);   // render.cpp: cbuffer list guard, entry repair, lookup fallback

void install_nullsites(int &installed) {
    if (install_stream_start()) ++installed;
    install_render(installed);
}

}  // namespace u2mp
