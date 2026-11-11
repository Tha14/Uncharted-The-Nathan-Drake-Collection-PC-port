// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include "../gpu/shim/gow3_toggles.h" // sigjmp_buf plus the fault/toggle declarations defined below
// Renderer tests have no guest process. Clock/host-thread services work; guest accesses abort.
extern "C" {
__thread sigjmp_buf* runtime_fault_recover{};     // no fault recovery point is ever armed
uint64_t runtime_disabled_optimizations{};        // every optimization toggle left on
uint64_t runtime_tsc_frequency() { return 1000000000; }
int runtime_file_translate(const char*, char*, size_t) { std::abort(); }
uint64_t runtime_memory_clamp(uintptr_t, uint64_t) { std::abort(); }
uint64_t runtime_process_time_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int runtime_memory_region(uintptr_t, uintptr_t*, uintptr_t*, int*) { std::abort(); }
void runtime_memory_set_gpu_hooks(void (*)(uintptr_t, uint64_t),
    void (*)(uintptr_t, uint64_t), void (*)(uintptr_t, uint64_t)) { std::abort(); }
void runtime_thread_attach_host(const char*) {}
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) { std::abort(); }
void runtime_restart() { std::abort(); }
uint64_t runtime_process_time_counter() { return runtime_process_time_us() * 1000; }
uint64_t runtime_read_tsc() { return runtime_process_time_counter(); }
int32_t* runtime_errno() { std::abort(); }
int runtime_memory_write_backing(uintptr_t, const void*, uint64_t) { std::abort(); }
// Backing-store copies, guest mappings, exec watches and pad remaps all need a live game.
void runtime_memory_copy_backing(uintptr_t /*guest*/, void* /*host*/, uint64_t /*bytes*/) { std::abort(); }
int runtime_memory_read_backing(uintptr_t /*guest*/, void* /*host*/, uint64_t /*bytes*/) { std::abort(); }
void* runtime_map_guest_memory(uint64_t /*guest*/) { std::abort(); }
void runtime_exec_watch_retarget(uint64_t /*target*/) { std::abort(); }
int runtime_gamesettings_active() { return 0; /* the in-game settings menu is never open */ }
void runtime_pad_effective_maps(const char* /*pad*/, const char* /*keys*/, const char* /*mouse*/,
                                char* /*pad_out*/, char* /*keys_out*/, char* /*mouse_out*/, size_t /*cap*/) { std::abort(); }
// The texture cache peeks at the last decoded movie frame range; no movie runs in these tests.
volatile unsigned long long runtime_video_frame_lo{};
volatile unsigned long long runtime_video_frame_hi{};
} // extern "C"

