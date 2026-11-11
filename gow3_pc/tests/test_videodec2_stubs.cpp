// Link stubs for videodec2-test. The decoder writes into host-side frame buffers only, so guest
// memory and the GPU texture cache never come into play; reaching any of these is a test bug.
#include "core/memory.h"  // Core::MemoryManager, VAddr, u64
#include <cstdlib>        // std::abort
extern "C" {  // normally defined by the game runtime in src/
// Frame range normally published for the texture cache; nothing reads it in this test.
volatile unsigned long long runtime_video_frame_lo{};
volatile unsigned long long runtime_video_frame_hi{};
void* runtime_map_guest_memory(std::uint64_t /*guest_address*/) { std::abort(); }
} // extern "C"
namespace Core {
void MemoryManager::InvalidateMemory(VAddr /*addr*/, u64 /*size*/) { std::abort(); }
bool MemoryManager::TryWriteBacking(void* /*dst*/, const void* /*src*/, u64 /*size*/) { std::abort(); }
} // namespace Core
