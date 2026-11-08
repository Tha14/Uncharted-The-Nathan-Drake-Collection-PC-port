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
//
// Second limit (run 32): with the commands surviving, the quads overflow the per-frame dynamic
// vertex / index streams of the 2D context (256 KiB vertex = 1,024 quads, 8 KiB index = 682
// quads) and the writer 0xbdc190 asserts at 0xbdc5c2 / 0xbdc65a. install_streams raises the stream
// sizes at their setup 0xc79d40 (guest frame-heap memory, as before) and guards 0xbdc190 so that a
// quad that still does not fit is dropped instead of written past the end.
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

// ---- the dynamic vertex / index streams the quads are written to --------------------------------
// Render-side execution of the 0x303a command, 0xbdc190(ctx, vertices, alpha in xmm0), writes 4
// vertices of 0x40 bytes at the 64-aligned current pointer of the vertex stream [ctx+0x10] and 6
// u16 indices (vertex number = offset / 0x40) to the index stream [ctx+0x18]. Only afterwards does
// it assert (dynamic-stream-mgr.h SetPointer, line 0x9a: 0xbdc5c2 / 0xbdc65a) that the stream did
// not pass its size. Stream object (0x28 bytes): +0 buffer object, +8 per-frame view, +0x10 start,
// +0x18 current, +0x20 u32 size. The flush 0xbdc670 (0x303c) draws (index count - batch start)
// indices and skips the draw when that is 0, so a dropped quad is safe.
constexpr uint64_t fQuadWrite = 0xbdc190;
constexpr uint64_t kQuadCall = 0xbdad3c;   // call 0xbdc190 (0x303a in the command list executor 0xbdaca0)
constexpr uint64_t kQuadJmp = 0xbdd16c;    // jmp 0xbdc190 (immediate path of 0xbdd130)
constexpr uint32_t kVertexOld = 0x40000, kVertexNew = 0x180000;   // 1,024 -> 6,144 quads
constexpr uint32_t kIndexOld = 0x2000, kIndexNew = 0x12000;       // 682 -> 6,144 quads
static_assert(kVertexNew / 0x40 <= 0x10000, "u16 indices address the whole vertex stream");

std::atomic<uint64_t> quads_dropped{0};

uint64_t rd64(const void *p, ptrdiff_t off) {
    uint64_t v;
    std::memcpy(&v, static_cast<const uint8_t *>(p) + off, sizeof v);
    return v;
}
uint32_t rd32(const void *p, ptrdiff_t off) {
    uint32_t v;
    std::memcpy(&v, static_cast<const uint8_t *>(p) + off, sizeof v);
    return v;
}

// True when `bytes` more fit in stream `s` at its current pointer aligned to `align` (0xbdc203).
bool stream_room(const uint8_t *s, uint64_t align, uint64_t bytes, uint64_t *offset) {
    const uint64_t start = rd64(s, 0x10), cur = rd64(s, 0x18), size = rd32(s, 0x20);
    const uint64_t at = (cur - start + align - 1) & ~(align - 1);
    if (offset) *offset = at;
    return at + bytes < size;
}

U2MP_GUEST void quad_write(uint8_t *ctx, void *vertices, float alpha) {
    const uint8_t *vs = ctx ? reinterpret_cast<const uint8_t *>(rd64(ctx, 0x10)) : nullptr;
    const uint8_t *is = ctx ? reinterpret_cast<const uint8_t *>(rd64(ctx, 0x18)) : nullptr;
    // Streams missing or unmapped: the original asserts as before.
    if (vs && is && rd64(vs, 0x10) && rd64(vs, 0x18) && rd64(is, 0x10) && rd64(is, 0x18)) {
        uint64_t voff = 0;
        const bool ok = stream_room(vs, 0x40, 0x100, &voff) && stream_room(is, 1, 0xc, nullptr) &&
                        voff / 0x40 + 3 <= 0xffff;   // u16 indices
        if (!ok) {
            const uint64_t n = quads_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 4 || (n & (n - 1)) == 0)
                log("menu draw: vertex/index stream full, quad dropped (%llu so far; streams %u / %u bytes)",
                    (unsigned long long)n, rd32(vs, 0x20), rd32(is, 0x20));
            return;
        }
    }
    guest<void(U2MP_GUEST *)(uint8_t *, void *, float)>(fQuadWrite)(ctx, vertices, alpha);
}

// Stream sizes. 0xc79d40 (render init, from 0xc8edb0) sets up 4 frame slots of 0x370 bytes at
// 0x13d2ee0; each frame 0xc7a3e0 (from 0xb6315b, frame params +0x98) takes the memory of every
// stream from the frame heap (0xc12150 -> 0xb333f0, context 0x46 / 0x35) with the size at +0x20,
// so the streams stay guest, GPU-visible memory. Each stream also has a buffer object created
// from {u32 bytes, u32 kind} (0xc16820) whose byte count must agree with +0x20. The 2D context
// (0xccf830 type 0, set up at 0x4bab4c for the frame's 2D pass that runs the command list at
// 0x4bad05) uses stream +0x00 (vertex) and +0xf0 (index); types 3 / 4 use +0x28 / +0x118. The
// immediates are shared between neighbouring streams, so those grow too (all small).
struct StreamImm { uint64_t at; unsigned char op[2]; uint64_t old_value, new_value; };
constexpr uint64_t kVSize = 0x800000000ull, kVBuf = 0x100000000ull, kIBuf = 0x200000000ull;
constexpr StreamImm kStreamImms[] = {
    {0xc79d70, {0x48, 0xb8}, kVSize | kVertexOld, kVSize | kVertexNew},  // +0x20, +0x48 sizes
    {0xc79d8b, {0x48, 0xb8}, kVBuf | kVertexOld, kVBuf | kVertexNew},    // +0x00, +0x28 buffers
    {0xc79ed3, {0x48, 0xb8}, kVSize | kIndexOld, kVSize | kIndexNew},    // +0xc0 (vertex +0xa0), +0x110 sizes
    {0xc79ee8, {0x48, 0xb8}, kVBuf | kIndexOld, kVBuf | kIndexNew},      // +0xa0 buffer
    {0xc79f33, {0x49, 0xbc}, kIBuf | kIndexOld, kIBuf | kIndexNew},      // +0xf0, +0x118 buffers (r12)
    {0xc79f68, {0x48, 0xb8}, kVSize | kIndexOld, kVSize | kIndexNew},    // +0x138 size
};

void install_streams(int &installed) {
    bool all = true;
    for (const StreamImm &s : kStreamImms) {
        unsigned char want[10];
        std::memcpy(want, s.op, 2);
        std::memcpy(want + 2, &s.old_value, 8);
        if (!image.matches(s.at, want, sizeof want)) all = false;
    }
    if (all) {
        for (const StreamImm &s : kStreamImms) std::memcpy(image.base + s.at + 2, &s.new_value, 8);
        log("menu draw: dynamic streams per frame: vertex 0x%x -> 0x%x, index 0x%x -> 0x%x bytes", kVertexOld,
            kVertexNew, kIndexOld, kIndexNew);
        ++installed;
    } else {
        log("menu draw: stream setup 0xc79d40 differs, stream sizes left alone");
    }

    // The overflow guard, also when the sizes could not be raised.
    static const unsigned char quad_head[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                              0x41, 0x55, 0x41, 0x54, 0x53, 0x50};
    if (!image.matches(fQuadWrite, quad_head, sizeof quad_head)) {
        log("menu draw: quad writer 0xbdc190 differs, no stream overflow guard");
        return;
    }
    if (retarget_call(kQuadCall, fQuadWrite, reinterpret_cast<void *>(&quad_write), "menu draw: quad stream guard"))
        ++installed;
    int32_t rel = 0;
    std::memcpy(&rel, image.base + kQuadJmp + 1, 4);
    unsigned char *stub = nullptr;
    if (image.base[kQuadJmp] == 0xe9 && kQuadJmp + 5 + int64_t(rel) == int64_t(fQuadWrite) && (stub = near_stub(16))) {
        // mov rax,quad_write; jmp rax   (a tail jump: rdi, rsi and xmm0 pass through)
        const uint64_t target = uint64_t(uintptr_t(&quad_write));
        stub[0] = 0x48;
        stub[1] = 0xb8;
        std::memcpy(stub + 2, &target, 8);
        stub[10] = 0xff;
        stub[11] = 0xe0;
        const int64_t delta = int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + kQuadJmp + 5));
        if (delta >= INT32_MIN && delta <= INT32_MAX) {
            const int32_t r = int32_t(delta);
            std::memcpy(image.base + kQuadJmp + 1, &r, 4);
            log("menu draw: quad stream guard: jmp at 0x%llx -> C++", (unsigned long long)kQuadJmp);
        }
    } else {
        log("menu draw: jmp at 0x%llx differs, immediate quad path left unguarded", (unsigned long long)kQuadJmp);
    }
}

}  // namespace

void install_menudraw(int &installed) {
    install_streams(installed);   // first: more surviving 2D commands need room in the streams

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
