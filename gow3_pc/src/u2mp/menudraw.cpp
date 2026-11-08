// u2mp menu draw: the per-frame 2D command arena the PS4 menus draw into is too small for the
// multiplayer pages. Notes: u2mp/docs/PORT-MENUDRAW.md.
//
// Every menu primitive is a deferred render command allocated from one linear arena, the global
// 0x124a000 (allocator object at +0x10: +0x10 top, +0x18 end, +0x20 base, +0x28 size << 2 | flags,
// +0x30 high-water mark, +0x40 spin lock, +0x70 command list):
//   * textured quad 0xbdce90 with a NULL draw list: material 0x3039 (0x50 bytes, 0xbdcf48),
//     vertices 0x303a (0x140, 0xbdd2c1), end 0x303c (0x48, 0xbdd518): 0x1d0 bytes per quad,
//   * text 0xbda9c0: 0xe0 + strlen bytes (the renderer's text at 0x5be615),
//   * the renderer 0x5bcd70 itself (0x5c1b25, 0x5c2941) and the item tail 0x5b4e90 (0x5b501d,
//     0x5b50d8), the frames / rows 0x5b3eb0..0x5b5de0, plus HUD and other 2D users.
// HeapArena::Allocate (0xb32170) returns NULL when the arena is full; with flag bit 0 set (Draw
// sets it, 0x3fc434) it does not even print "HeapArena::Allocate: out of memory". Callers skip the
// primitive on NULL. Draw (0x3fc0b0, Bluepoint's bp_ucctweaks.cpp, called each frame from
// 0x3fb596) gives the arena 0x30000 bytes (192 KiB) of memory context 0x33 every frame
// (0x3fc38c mov edi,0x30000; 0x3fc391 call 0xb33160; 0x3fc3f3 or rax,0xc0000; 0x3fc403 add
// r14,0x30000). That fits the single-player menus; the Profile page over the page behind it does
// not, and the arena is shared by worker threads (spin lock), so which primitives are lost changes
// frame to frame. PS3 1.10 has no such arena (its menus draw through GCM with the PS3 frame
// allocator, renderer 0x41a790): the limit is the port's.
//
// Fix: the arena gets kArenaSize bytes from a ring of host buffers (one per frame, kRing deep so a
// buffer is not reused while the render side may still read it). The size constants of Draw are
// patched to match, and the call 0x3fc391 goes to arena_memory below. A diagnostic wrapper on the
// call at 0x3fae9c (just before the frame reset of the arena, 0x3faea1) logs the previous frame's
// use and the peak.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
namespace {

constexpr uint64_t kArena = 0x124a000;          // the 2D command arena (lea at 0x3fc39e)
constexpr uint64_t fFrameAlloc = 0xb33160;      // Allocate(size, &memory context)
constexpr uint64_t fFrameBegin = 0x4b4f30;      // called at 0x3fae9c, right before the reset
constexpr uint64_t kMovSize = 0x3fc38c;         // mov edi,0x30000
constexpr uint64_t kCallAlloc = 0x3fc391;       // call 0xb33160
constexpr uint64_t kOrFlags = 0x3fc3f3;         // or rax,0xc0000   (size << 2)
constexpr uint64_t kAddEnd = 0x3fc403;          // add r14,0x30000
constexpr uint64_t kCallFrameBegin = 0x3fae9c;  // call 0x4b4f30
constexpr uint32_t kOldSize = 0x30000;
constexpr uint32_t kArenaSize = 0x200000;       // 2 MiB, about 4,500 quads
constexpr unsigned kRing = 4;
static_assert(kArenaSize % 0x40 == 0 && (uint64_t(kArenaSize) << 2) < 0x80000000u, "imm32 of or rax");

alignas(64) unsigned char ring[kRing][kArenaSize];
std::atomic<unsigned> ring_next{0};

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }

// Replaces the call at 0x3fc391: the arena's memory for this frame.
U2MP_GUEST void *arena_memory(uint64_t size, void *context) {
    if (uint32_t(size) == kArenaSize) {
        const unsigned i = ring_next.fetch_add(1, std::memory_order_relaxed) % kRing;
        return ring[i];
    }
    return guest<void *(U2MP_GUEST *)(uint64_t, void *)>(fFrameAlloc)(size, context);
}

uint64_t peak = 0;
unsigned frames = 0;

// Wraps the call at 0x3fae9c: the arena still holds the previous frame's commands here.
U2MP_GUEST uint64_t frame_begin(void *arg) {
    const uint64_t top = G<uint64_t>(kArena + 0x10), base = G<uint64_t>(kArena + 0x20);
    const uint64_t high = G<uint64_t>(kArena + 0x30);
    const uint64_t used = (base && top >= base) ? top - base : 0;
    const uint64_t most = used > high ? used : high;
    ++frames;
    if (most > peak) {
        // Log each new peak once it grew by 1/16 of the arena (or crossed the old 192 KiB limit).
        const bool crossed = peak < kOldSize && most >= kOldSize;
        if (crossed || most >= peak + kArenaSize / 16 || peak == 0)
            log("menu draw: 2D arena peak %llu of %u bytes (old limit %u) at frame %u%s",
                (unsigned long long)most, kArenaSize, kOldSize, frames,
                most >= kOldSize ? " - the PS4 arena would have dropped primitives" : "");
        peak = most;
    }
    return guest<uint64_t(U2MP_GUEST *)(void *)>(fFrameBegin)(arg);
}

}  // namespace

void install_menudraw(int &installed) {
    static const unsigned char mov_size[] = {0xbf, 0x00, 0x00, 0x03, 0x00, 0xe8};
    static const unsigned char or_flags[] = {0x48, 0x0d, 0x00, 0x00, 0x0c, 0x00};
    static const unsigned char add_end[] = {0x49, 0x81, 0xc6, 0x00, 0x00, 0x03, 0x00};
    // lea r12,[rip+..] = 0x124a000 right after the call (0x3fc39e), mov [r12+0x20],r14 (0x3fc3e5)
    static const unsigned char lea_arena[] = {0x4c, 0x8d, 0x25};
    static const unsigned char set_base[] = {0x4d, 0x89, 0x74, 0x24, 0x20};
    int32_t rel = 0;
    std::memcpy(&rel, image.base + 0x3fc3a1, 4);
    if (!image.matches(kMovSize, mov_size, sizeof mov_size) || !image.matches(kOrFlags, or_flags, sizeof or_flags) ||
        !image.matches(kAddEnd, add_end, sizeof add_end) || !image.matches(0x3fc39e, lea_arena, sizeof lea_arena) ||
        0x3fc3a5 + int64_t(rel) != int64_t(kArena) || !image.matches(0x3fc3e5, set_base, sizeof set_base)) {
        log("menu draw: Draw 0x3fc0b0 differs, 2D arena left at 0x%x bytes", kOldSize);
        return;
    }
    if (!retarget_call(kCallAlloc, fFrameAlloc, reinterpret_cast<void *>(&arena_memory), "menu draw: 2D arena memory"))
        return;
    std::memcpy(image.base + kMovSize + 1, &kArenaSize, 4);
    const uint32_t flags = kArenaSize << 2;
    std::memcpy(image.base + kOrFlags + 2, &flags, 4);
    std::memcpy(image.base + kAddEnd + 3, &kArenaSize, 4);
    log("menu draw: 2D command arena 0x124a000 0x%x -> 0x%x bytes (%u host buffers)", kOldSize, kArenaSize, kRing);
    ++installed;

    // Diagnostics only: the previous frame's use, read just before the reset at 0x3faea1.
    static const unsigned char frame_call[] = {0x48, 0x8d, 0x3d};   // lea rdi,[rip+..] at 0x3fae95
    if (image.matches(0x3fae95, frame_call, sizeof frame_call) &&
        retarget_call(kCallFrameBegin, fFrameBegin, reinterpret_cast<void *>(&frame_begin), "menu draw: arena use log"))
        ++installed;
}

}  // namespace u2mp
