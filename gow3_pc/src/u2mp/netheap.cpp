// u2mp network heap: memory context 0x14 (ALLOCATION_NETWORK_HEAP on PS3).
//
// Every network allocation of the game (159 call sites in 0x5e0000..0x680000: events, queue nodes,
// PlayerInfo...) goes through the allocator (0xb332a0) with context 0x14, which takes its heap from
// the table at [0x104ae00] + 0x14 * 8. PS4 never creates that heap (the slot is null). Sharing the
// thread's current heap (context 0x46) is wrong: that is a scratch heap whose memory is reused, which
// corrupted the NetGameManager tables in test run 8. This is a heap of our own, with the interface the
// game's allocator and free (0xb356d0) use:
//   vtable +0x48 Allocate(self, size, align, file, line, file2) -> pointer
//   vtable +0x58 Free(self, pointer, ...)
//   vtable +0x68 / +0x70 ownership tests (self, pointer) -> bool (free asks them before +0x58)
// Other slots are not expected; they log once and return 0.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <array>
#include <utility>

#include "u2mp_internal.h"

namespace u2mp {
namespace {

std::mutex heap_lock;
std::unordered_set<uintptr_t> owned;
size_t live_bytes = 0, live_blocks = 0;

struct Header { void *base; size_t size; };

U2MP_GUEST void *heap_alloc(void *, uint64_t size, uint64_t align, void *, void *, void *) {
    if (align < 16) align = 16;
    if (align & (align - 1)) align = 16;
    const size_t total = size_t(size) + size_t(align) + sizeof(Header);
    void *base = std::calloc(1, total);
    if (!base) return nullptr;
    uintptr_t p = (uintptr_t(base) + sizeof(Header) + align - 1) & ~uintptr_t(align - 1);
    reinterpret_cast<Header *>(p)[-1] = Header{base, size_t(size)};
    std::lock_guard<std::mutex> g(heap_lock);
    owned.insert(p);
    live_bytes += size_t(size);
    ++live_blocks;
    return reinterpret_cast<void *>(p);
}

U2MP_GUEST void heap_free(void *, void *pointer, void *, void *, void *, void *) {
    if (!pointer) return;
    const uintptr_t p = uintptr_t(pointer);
    {
        std::lock_guard<std::mutex> g(heap_lock);
        if (!owned.erase(p)) return;
        live_bytes -= reinterpret_cast<Header *>(p)[-1].size;
        --live_blocks;
    }
    std::free(reinterpret_cast<Header *>(p)[-1].base);
}

U2MP_GUEST uint64_t heap_owns(void *, void *pointer) {
    std::lock_guard<std::mutex> g(heap_lock);
    return owned.count(uintptr_t(pointer)) ? 1 : 0;
}

template <int Slot> U2MP_GUEST uint64_t heap_other(void *, void *, void *, void *) {
    static bool said = false;
    if (!said) {
        said = true;
        log("network heap: unexpected vtable slot +%#x called (returns 0)", Slot * 8);
    }
    return 0;
}

template <int... S> constexpr auto make_vtable(std::integer_sequence<int, S...>) {
    return std::array<void *, sizeof...(S)>{reinterpret_cast<void *>(&heap_other<S>)...};
}

struct Heap {
    void **vtable;
} heap{};
std::array<void *, 32> vtable = make_vtable(std::make_integer_sequence<int, 32>{});

}  // namespace

void netheap_install() {
    constexpr uint64_t kAllocTablePtr = 0x104ae00, kNetContext = 0x14;
    const uint64_t table = *image.at<uint64_t>(kAllocTablePtr);
    if (!table) {
        log("network heap: allocator table not set up yet");
        return;
    }
    auto *slot = reinterpret_cast<uint64_t *>(uintptr_t(table) + kNetContext * 8);
    if (*slot == uint64_t(uintptr_t(&heap))) return;
    if (*slot) {
        log("network heap: context 0x14 already has a heap (%#llx), left alone", (unsigned long long)*slot);
        return;
    }
    vtable[0x48 / 8] = reinterpret_cast<void *>(&heap_alloc);
    vtable[0x58 / 8] = reinterpret_cast<void *>(&heap_free);
    vtable[0x68 / 8] = reinterpret_cast<void *>(&heap_owns);
    vtable[0x70 / 8] = reinterpret_cast<void *>(&heap_owns);
    heap.vtable = vtable.data();
    *slot = uint64_t(uintptr_t(&heap));
    log("network heap: context 0x14 has its own heap");
}

void *netheap_alloc(size_t size, size_t align) { return heap_alloc(nullptr, size, align, nullptr, nullptr, nullptr); }

}  // namespace u2mp
