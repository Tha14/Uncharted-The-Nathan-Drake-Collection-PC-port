// Uncharted 3 1.02, 12 Nov 2026: the network-process memory pool is too small for multiplayer.
//
// The game creates its memory pools from a table in its data (image 0x16f8d90, read by the memory setup at 0x5ac2f1):
// 32-byte entries {u32 pool id, u32 type, u64 size, u64 alignment, u64 0}. Pool 0x14 (network) has 16 MiB, pool 0x15
// (network process, a "Wrapped Malloc" type-5 pool) only 343,040 bytes: the PS4 game has no multiplayer. With the
// restored multiplayer, players joining and leaving and round after round fill it: "Allocation failed: 576 bytes ...
// Wrapped Malloc [F:336 T:343040]", then a write through the null result (0x6ab7fb / 0x6ae308), and the game closes
// (four logs of 2026-11-12: a co-op host after two rounds, a host after players left, and a host that kept failing
// 279 times after a late joiner left). The pool's budget is raised to 16 MiB, the size of pool 0x14, before the game
// starts; a type-5 pool takes memory from the C library heap as it needs it, so only what is used is used.
#include "u3fix.h"
#include "u3fix_hook.h"

#include <cstdio>
#include <cstring>

namespace u3fix {

namespace {

struct PoolEntry {   // the game's pool table entry
    uint32_t id, type;
    uint64_t size, align, zero;
};
static_assert(sizeof(PoolEntry) == 32);

constexpr uint64_t kOldSize = 0x53c00;    // 343,040 bytes
constexpr uint64_t kNewSize = 0x1000000;  // 16 MiB, as pool 0x14

}  // namespace

bool raise_net_process_pool(unsigned char *image, uint64_t size) {
    // pool 0x14 (type 5, 16 MiB, align 0x80) followed by pool 0x15 (type 5, 343,040 bytes, align 0x80)
    const PoolEntry want14{0x14, 5, 0x1000000, 0x80, 0}, want15{0x15, 5, kOldSize, 0x80, 0};
    unsigned char pattern[64];
    std::memcpy(pattern, &want14, 32);
    std::memcpy(pattern + 32, &want15, 32);
    for (uint64_t at = 0; at + sizeof(pattern) <= size; at += 8) {
        if (std::memcmp(image + at, pattern, sizeof(pattern)) != 0) continue;
        PoolEntry e;
        std::memcpy(&e, image + at + 32, sizeof(e));
        e.size = kNewSize;
        std::memcpy(image + at + 32, &e, sizeof(e));
        std::printf("Uncharted 3: network-process memory pool (0x15) raised from %llu bytes to %llu (pool table at %#llx)\n",
                    static_cast<unsigned long long>(kOldSize), static_cast<unsigned long long>(kNewSize),
                    static_cast<unsigned long long>(at));
        std::fflush(stdout);
        return true;
    }
    return false;
}

}  // namespace u3fix
