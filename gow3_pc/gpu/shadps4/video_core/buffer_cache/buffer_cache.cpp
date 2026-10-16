#include <atomic>
#include <mutex>
#include <vector>
#include <cstdio>
#include <functional>
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <array>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <string>
#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>
#include "gow3_copy.h"
#include "gow3_toggles.h"
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace Vulkan {
extern std::function<void(u64)> g_describe_gpu_address;
void DumpGpuCheckpoints(const Instance& instance);
void DumpDeviceFault(const Instance& instance);
}

namespace VideoCore {

// GOW3_READ_SOURCES=1 (with GOW3_FAULT_STATS=1): CPU reads of GPU-written bytes and the shaders
// that wrote their windows. Off with the default diagnostics: a lock and map update per GPU write.
bool ReadbackStatsEnabled() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_READBACK_STATS");
        return env && env[0] == '1';
    }();
    return on;
}

static bool FaultStatsEnabled() {
    static const bool on = [] {
        const char* stats = std::getenv("GOW3_FAULT_STATS");
        const char* sources = std::getenv("GOW3_READ_SOURCES");
        return (stats && stats[0] == '1' && sources && sources[0] == '1') || ReadbackStatsEnabled();
    }();
    return on;
}

static std::mutex g_rb_stats_mutex;
static std::unordered_map<u64, std::array<u64, 2>> g_rb_window_bytes; ///< window -> bytes, pieces
static std::unordered_map<u64, u32> g_rb_window_kinds;                ///< window -> ReadbackKind bits

void NoteReadbackKind(VAddr address, u64 size, u32 kind) {
    if (!ReadbackStatsEnabled() || size == 0) {
        return;
    }
    std::scoped_lock lock{g_rb_stats_mutex};
    for (u64 window = address >> 20; window <= (address + size - 1) >> 20; ++window) {
        g_rb_window_kinds[window] |= kind;
    }
}
static std::mutex g_writers_mutex;
static std::unordered_map<u64, std::map<u64, u32>> g_window_writers; ///< window -> shader -> writes
/// CPU reads that hit GPU-written bytes (not only their page): window -> (reads, addresses).
static std::unordered_map<u64, std::pair<u32, std::map<VAddr, u32>>> g_gpu_byte_reads;

void NoteGpuWriter(VAddr address, u64 size, u64 shader_hash) {
    if (!FaultStatsEnabled() || size == 0) {
        return;
    }
    std::scoped_lock lock{g_writers_mutex};
    const u64 first = address >> 20;
    const u64 last = std::min<u64>((address + size - 1) >> 20, first + 63);
    for (u64 window = first; window <= last; ++window) {
        ++g_window_writers[window][shader_hash];
    }
}

/// "; written by <shader>x<writes> ..." for the statistics, empty unless GOW3_READ_SOURCES=1.
static std::string WindowWriters(u64 window) {
    if (!FaultStatsEnabled()) {
        return {};
    }
    std::scoped_lock lock{g_writers_mutex};
    const auto it = g_window_writers.find(window);
    if (it == g_window_writers.end()) {
        return "; written by no shader";
    }
    std::string out = "; written by";
    for (const auto& [hash, n] : it->second) {
        out += fmt::format(" {:08x}x{}", hash, n);
    }
    return out;
}

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
// gow3: 512 MiB (GOW3_STREAM_MB overrides; 64-2048): with two frames queued Uncharted 2's heavy
// scenes wrapped the 128 MiB ring in under two frames and waited for the GPU (1.3 s per 5 s).
static size_t StreamBufferSize() {
    const char* env = std::getenv("GOW3_STREAM_MB");
    const size_t mb = env ? std::clamp<size_t>(std::strtoull(env, nullptr, 10), 64, 2048) : 512;
    return mb * 1_MB;
}

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, StreamBufferSize()},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    page_manager = &tracker;
    while (arena_page_bits > MIN_ARENA_PAGE_BITS &&
           (u64{2} << arena_page_bits) > instance.GetMaxBufferSize()) {
        --arena_page_bits;
    }
    arena_page_size = u64{1} << arena_page_bits;
    num_arena_pages = u64{1} << (ADDRESS_SPACE_BITS - arena_page_bits);
    if (arena_page_bits != MAX_ARENA_PAGE_BITS) {
        LOG_INFO(Render_Vulkan, "Buffer arenas of {} MiB (driver buffer limit {} MiB)",
                 arena_page_size >> 20, instance.GetMaxBufferSize() >> 20);
    }
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * num_arena_pages) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * num_arena_pages);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
    // Diagnostics: name the arena and guest address of a GPU fault address.
    Vulkan::g_describe_gpu_address = [this](u64 gpu_address) {
        for (const Buffer& arena : arenas) {
            const u64 base = arena.BufferDeviceAddress();
            if (gpu_address < base || gpu_address >= base + arena.SizeBytes()) {
                continue;
            }
            const VAddr guest = arena.CpuAddr() + (gpu_address - base);
            const u64 block = guest >> block_shift;
            bool resident = true;
            resident_ranges.ForEachGap(block, block + 1, [&](u64, u64) { resident = false; });
            std::fprintf(stderr, "GPU fault address is arena %#llx+%#llx: guest %#llx, block %llu %s"
                         " (block size %#llx)\n",
                         (unsigned long long)arena.CpuAddr(), (unsigned long long)arena.SizeBytes(),
                         (unsigned long long)guest, (unsigned long long)block,
                         resident ? "resident" : "NOT resident", (unsigned long long)block_size);
        }
        const auto describe = [&](const char* name, const Buffer& buffer) {
            const u64 base = buffer.BufferDeviceAddress();
            if (gpu_address >= base && gpu_address < base + buffer.SizeBytes()) {
                std::fprintf(stderr, "GPU fault address is in %s at %#llx\n", name,
                             (unsigned long long)(gpu_address - base));
            }
        };
        describe("stream buffer", stream_buffer);
        describe("GDS buffer", gds_buffer);
        describe("BDA page table", *bda_pagetable_buffer);
    };
}

BufferCache::~BufferCache() = default;

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

// gow3: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in the test scene, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const Gow3Copy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
void BufferCache::SmallGuestCopy(const Gow3Copy::Item& item) {
    if (scheduler.IsRecordingDeferred() && !Gow3Toggle::Disabled(Gow3Toggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
        return;
    }
    Gow3Copy::QueueCopy(item);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static const u64 window = [] {
        const char* env = std::getenv("GOW3_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= TRACKER_BYTES_PER_PAGE || Gow3Toggle::Disabled(Gow3Toggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

static std::atomic<u32> g_write_fault_overlap, g_write_fault_same_page, g_write_fault_window;
static std::atomic<u64> g_write_fault_gpu_bytes;
static std::atomic<u32> g_lazy_writes, g_lazy_cpu_wins, g_readback_waits, g_async_readbacks;
static std::atomic<u32> g_readback_read_waits;
// gow3 statistics (GOW3_FAULT_STATS=1): read faults on pages with GPU data on its way back that
// hit those GPU bytes, or only other data on their page; time the faulting thread waited for the
// GPU thread to take the request, and for the readback itself (microseconds).
static std::atomic<u32> g_read_on_gpu_bytes, g_read_same_page;
static std::atomic<u32> g_pieces_recurring, g_pieces_protected;
static std::atomic<u64> g_readback_bytes;
// gow3 statistics: fences deferred to ray-cast readbacks (label, delay until written).
static std::atomic<u32> g_deferred_fences;
static std::atomic<u64> g_deferred_us, g_deferred_max_us;
static std::atomic<u32> g_swept_pages, g_swept_windows;
static constexpr auto RecurringWindow = std::chrono::milliseconds{500};
static std::atomic<u64> g_read_queue_us, g_read_wait_us;
static std::mutex g_read_pieces_mutex;
static std::unordered_map<u64, u32> g_read_pieces; ///< (piece address << 16 | size) -> reads
static std::atomic<u32> g_readback_yields;

// GOW3_ASYNC_READBACK=1: pages with a readback in flight are read-protected, so the CPU (the game,
// or the SRT walker) never reads their stale data: a read waits for that readback only. =2 is
// the earlier unprotected mode (stale reads: Uncharted 3 "dcb full", garbage sharps).
static bool SafeReadbacks() {
    // GOW3_ASYNC_READBACK=1: pages of pending readbacks are read-protected (a CPU read waits for
    // its readback). Default (unset, 2): not protected, the CPU reads the previous value.
    static const bool safe = [] {
        const char* env = std::getenv("GOW3_ASYNC_READBACK");
        return env && env[0] == '1';
    }();
    return safe;
}

// GOW3_LAZY_WRITES=1: a CPU write into a GPU-written page goes ahead without a download; the
// GPU bytes are merged in later where guest memory still matches a snapshot. Off by default
// since 15 Oct: a byte the CPU rewrites with its previous value is indistinguishable from an
// untouched one, so the merge put stale GPU bytes back into rewritten data (Uncharted 2's
// skinning tables in the per-frame ring: half-float V#s, a GPU hang and game asserts at the
// treasure screen).
static bool LazyWritesEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_LAZY_WRITES");
        return env && env[0] == '1';
    }();
    // Precise readbacks read-protect GPU-written pages: the records would be read through a
    // protected page.
    return enabled && EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Relaxed;
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    {
        // gow3: GOW3_FAULT_STATS=1 — every GPU-to-guest download request (CPU read of GPU data,
        // or CPU write into GPU-written memory), by 1 MiB window, every 5 s.
        static const bool stats = [] {
            const char* env = std::getenv("GOW3_FAULT_STATS");
            return env && env[0] == '1';
        }();
        if (stats) {
            static std::mutex mutex;
            static std::unordered_map<u64, u32> windows;
            static u32 reads, writes;
            static auto last = std::chrono::steady_clock::now();
            std::scoped_lock lock{mutex};
            ++windows[(device_addr >> 20) | (is_write ? 1ULL << 40 : 0)];
            ++(is_write ? writes : reads);
            if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
                last = now;
                std::vector<std::pair<u32, u64>> sorted;
                for (const auto& [key, n] : windows) {
                    sorted.emplace_back(n, key);
                }
                std::ranges::sort(sorted, std::greater{});
                std::printf("Download stats (5 s): %u for CPU reads, %u for CPU writes into GPU data\n", reads,
                            writes);
                const u32 overlap = g_write_fault_overlap.exchange(0);
                const u32 same_page = g_write_fault_same_page.exchange(0);
                const u32 window_only = g_write_fault_window.exchange(0);
                const u64 gpu_bytes = g_write_fault_gpu_bytes.exchange(0);
                std::printf("  CPU write faults: %u on GPU-written bytes, %u elsewhere in a GPU-written page, "
                            "%u elsewhere in the window; %.1f GPU-written bytes per faulting page\n",
                            overlap, same_page, window_only,
                            double(gpu_bytes) / std::max(1u, overlap + same_page + window_only));
                std::printf("  lazy writes (no download): %u; GPU-written bytes the CPU overwrote later: %u; "
                            "writes into pages shared with GPU data in flight: %llu\n",
                            g_lazy_writes.exchange(0), g_lazy_cpu_wins.exchange(0),
                            static_cast<unsigned long long>(std::exchange(g_shared_writes_count, 0)));
                std::printf("  tracking: %zu lazy/shared pages, %zu pending readbacks, %zu pages with "
                            "GPU write records, %zu GPU-written ranges\n",
                            lazy_pages.size(), pending_readbacks.size(), page_writes.size(),
                            gpu_modified_ranges.Count());
                {
                    const u32 on_bytes = g_read_on_gpu_bytes.exchange(0);
                    const u32 same_page = g_read_same_page.exchange(0);
                    const u64 total_us = g_read_queue_us.exchange(0);
                    const u64 wait_us = g_read_wait_us.exchange(0);
                    std::printf("  CPU read faults on readback pages: %u on the GPU bytes, %u elsewhere on "
                                "their page; %.1f ms in all, %.1f ms of it waiting for the GPU; readback "
                                "pieces: %u recurring (readable), %u new (protected)\n",
                                on_bytes, same_page, total_us / 1e3, wait_us / 1e3,
                                g_pieces_recurring.exchange(0), g_pieces_protected.exchange(0));
                    std::scoped_lock lock{g_read_pieces_mutex};
                    std::vector<std::pair<u32, u64>> pieces;
                    for (const auto& [key, n] : g_read_pieces) {
                        pieces.emplace_back(n, key);
                    }
                    std::ranges::sort(pieces, std::greater{});
                    for (size_t i = 0; i < std::min<size_t>(pieces.size(), 6); ++i) {
                        std::printf("    read piece %#llx+%llu: %u\n",
                                    static_cast<unsigned long long>(pieces[i].second >> 16),
                                    static_cast<unsigned long long>(pieces[i].second & 0xffff), pieces[i].first);
                    }
                    g_read_pieces.clear();
                }
                {
                    const u32 n = g_deferred_fences.exchange(0);
                    std::printf("  ray-cast fences written after their readback: %u, %.2f ms on average, "
                                "%.2f ms at most\n",
                                n, n ? g_deferred_us.exchange(0) / 1e3 / n : 0.0,
                                g_deferred_max_us.exchange(0) / 1e3);
                }
                std::printf("  readback bytes: %.1f MB; swept: %u pages with finished GPU writes, %u cooled "
                            "hot windows\n",
                            g_readback_bytes.exchange(0) / 1e6, g_swept_pages.exchange(0),
                            g_swept_windows.exchange(0));
                std::printf("  async readbacks: %u; CPU writes that waited for one: %u, CPU reads: %u; "
                            "CPU writes into pending ray-cast results: %u\n",
                            g_async_readbacks.exchange(0), g_readback_waits.exchange(0),
                            g_readback_read_waits.exchange(0), g_readback_yields.exchange(0));
                for (size_t i = 0; i < std::min<size_t>(sorted.size(), 10); ++i) {
                    const auto [n, key] = sorted[i];
                    const u64 window = key & ((1ULL << 40) - 1);
                    std::printf("  %s %#llx (1 MiB): %u%s\n", key >> 40 ? "write" : "read",
                                (unsigned long long)(window << 20), n, WindowWriters(window).c_str());
                }
                {
                    std::unordered_map<u64, std::pair<u32, std::map<VAddr, u32>>> byte_reads;
                    {
                        std::scoped_lock lock{g_writers_mutex};
                        byte_reads.swap(g_gpu_byte_reads);
                    }
                    for (const auto& [window, entry] : byte_reads) {
                        std::string top;
                        std::vector<std::pair<u32, VAddr>> sorted_addresses;
                        for (const auto& [address, n] : entry.second) {
                            sorted_addresses.emplace_back(n, address);
                        }
                        std::ranges::sort(sorted_addresses, std::greater{});
                        for (size_t i = 0; i < std::min<size_t>(sorted_addresses.size(), 8); ++i) {
                            top += fmt::format(" {:#x}x{}", sorted_addresses[i].second,
                                               sorted_addresses[i].first);
                        }
                        std::printf("  reads of GPU bytes in %#llx (1 MiB): %u at%s%s\n",
                                    static_cast<unsigned long long>(window << 20), entry.first,
                                    top.c_str(), WindowWriters(window).c_str());
                    }
                }
                std::fflush(stdout);
                windows.clear();
                reads = writes = 0;
            }
        }
    }
    const auto flush_request = [this, device_addr, size, is_write] {
        if (!is_write && FaultStatsEnabled() && gpu_modified_ranges.Intersects(device_addr, size)) {
            std::scoped_lock lock{g_writers_mutex};
            auto& [reads, addresses] = g_gpu_byte_reads[device_addr >> 20];
            ++reads;
            if (addresses.size() < 64 || addresses.contains(device_addr)) {
                ++addresses[device_addr];
            }
        }
        if (!is_write && !pending_readbacks.empty()) {
            // A read of a page whose GPU data is still on its way back (read-protected by
            // DownloadRangesAsync): that readback completes and unprotects it. Other GPU data
            // in the window stays as relaxed readbacks leave it.
            const VAddr page = Common::AlignDown(device_addr, 4096);
            const VAddr end = Common::AlignUp(device_addr + size, 4096);
            if (const u64 tick = PendingReadbackTick(page, end - page, false);
                tick && tick < scheduler.CurrentTick()) { // submitted (only those are read-protected)
                ++g_readback_read_waits;
                VAddr hit = 0;
                u64 hit_size = 0;
                for (const auto& readback : pending_readbacks) {
                    for (const auto& [piece, piece_size] : readback.pieces) {
                        if (piece < device_addr + size && device_addr < piece + piece_size) {
                            hit = piece;
                            hit_size = piece_size;
                        }
                    }
                }
                if (hit) {
                    ++g_read_on_gpu_bytes;
                    std::scoped_lock lock{g_read_pieces_mutex};
                    ++g_read_pieces[(hit << 16) | std::min<u64>(hit_size, 0xffff)];
                } else {
                    // False sharing: the CPU reads its own data beside GPU bytes still on their
                    // way back. Nothing to wait for: the page stops being read-protected, now
                    // and (noisy_pages) for the next readbacks of the coming 10 seconds.
                    ++g_read_same_page;
                    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
                    for (VAddr p = page; p < end; p += 4096) {
                        noisy_pages[p >> 12] = until;
                        UnwatchPage(p);
                    }
                    return;
                }
                const auto wait_start = std::chrono::steady_clock::now();
                CompleteReadbacks(true, tick);
                g_read_wait_us += std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - wait_start)
                                      .count();
                if (EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Relaxed) {
                    return;
                }
            }
        }
        if (is_write) {
            cpu_write_hot[device_addr >> 20] = std::chrono::steady_clock::now(); // GPU-side (command or inline)
            // gow3 statistics: does the CPU write hit GPU-written bytes, or only their page?
            const VAddr page = Common::AlignDown(device_addr, 4096);
            u64 gpu_bytes_in_page = 0;
            gpu_modified_ranges.ForEachInRange(page, 4096, [&](VAddr lo, VAddr hi) { gpu_bytes_in_page += hi - lo; });
            if (gpu_modified_ranges.Intersects(device_addr, size)) {
                ++g_write_fault_overlap;
            } else if (gpu_bytes_in_page) {
                ++g_write_fault_same_page;
            } else {
                ++g_write_fault_window;
            }
            g_write_fault_gpu_bytes += gpu_bytes_in_page;
            // gow3: ray-cast result readbacks (yield_to_cpu): a CPU write into one is the guest
            // refilling the buffer for its next batch, so the pending GPU values of that piece
            // are dropped, not waited for. With multithreaded rendering the recording thread
            // runs behind and every such write waited for the GPU (Uncharted 2: about 8 ms per
            // frame, a third of it).
            // 16 Oct: dropping them threw away nearly every ray-cast batch's results in
            // Uncharted 2 (~250 per 5 s): the guest had read the stale buffer at once (the fence
            // was written when recorded) and refilled it, and NPCs placed with those results fell
            // from the sky. Ray-cast fences now wait for their readback (RunEventWriteEop), so the
            // guest reads first; a write into pending results waits for them below (counted).
            for (auto& readback : pending_readbacks) {
                if (!readback.yield_to_cpu) {
                    continue;
                }
                for (const auto& [piece, piece_size] : readback.pieces) {
                    if (piece < device_addr + size && device_addr < piece + piece_size) {
                        ++g_readback_yields;
                    }
                }
            }
            // gow3: bulk GPU data read back for a CPU-hot window (pieces over 64 KiB: skinning
            // output and the like in the per-frame rings). A CPU write into one of its pages is
            // the guest reusing that ring memory: the page's part of the piece is dead and is
            // dropped instead of waited for. With multithreaded rendering every such write waited
            // about 18 ms for the GPU (Uncharted 2: 20+ per second, a third of the frame). Small
            // pieces (constants the CPU reads back) still wait.
            {
                const VAddr pages_lo = Common::AlignDown(device_addr, 4096);
                const VAddr pages_hi = Common::AlignUp(device_addr + size, 4096);
                for (auto& readback : pending_readbacks) {
                    for (const auto& [piece, piece_size] : readback.pieces) {
                        if (piece_size <= 64_KB || piece >= device_addr + size ||
                            device_addr >= piece + piece_size) {
                            continue;
                        }
                        const VAddr lo = std::max(piece, pages_lo);
                        const VAddr hi = std::min(piece + piece_size, pages_hi);
                        readback.rewritten.Add(lo, hi - lo);
                        GpuRangesSubtract(lo, hi - lo);
                        ++g_readback_yields;
                    }
                }
            }
            // The newest GPU data of the written bytes is on its way back: wait for that
            // readback only, not for all GPU work.
            if (const u64 tick = PendingReadbackTick(device_addr, size, true)) {
                ++g_readback_waits;
                static std::atomic<u32> described{0};
                if (described.fetch_add(1) < 24) {
                    for (const auto& readback : pending_readbacks) {
                        for (const auto& [piece, piece_size] : readback.pieces) {
                            if (piece < device_addr + size && device_addr < piece + piece_size) {
                                std::printf("Readback wait: CPU write %#llx+%llu into piece %#llx+%llu "
                                            "(tick %llu, now %llu)\n",
                                            static_cast<unsigned long long>(device_addr),
                                            static_cast<unsigned long long>(size),
                                            static_cast<unsigned long long>(piece),
                                            static_cast<unsigned long long>(piece_size),
                                            static_cast<unsigned long long>(readback.tick),
                                            static_cast<unsigned long long>(scheduler.CurrentTick()));
                            }
                        }
                    }
                    std::fflush(stdout);
                }
                CompleteReadbacks(true, tick);
            }
            // gow3: finished readbacks of this page first (no wait): it may be clean already.
            CompleteReadbacks(false);
            if (!gpu_modified_ranges.Intersects(page, 4096)) {
                lazy_pages.erase(page);
                memory_tracker->UnmarkRegionAsGpuModified(page, 4096);
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                return;
            }
            // gow3: the GPU bytes in this page come from work still in flight (recorded in this
            // submission or not finished): the guest, a frame ahead, writes next to them (false
            // sharing: ~90 per second in heavy U2 scenes, each a download waiting for all GPU
            // work). The write goes ahead; the page's GPU bytes stay the GPU's (uploads skip
            // them) until the next readback brings them. No guest bytes are compared (the
            // lazy-write merge did, and put stale GPU bytes back into rewritten data).
            if (!gpu_modified_ranges.Intersects(device_addr, size)) {
                if (GpuBytesInFlight(page)) {
                    auto& slot = lazy_pages[page];
                    if (!slot) {
                        slot = std::make_unique<LazyPage>();
                    }
                    slot->gpu_wins = true;
                    gpu_modified_ranges.ForEachInRange(page, 4096, [&](VAddr lo, VAddr hi) {
                        for (VAddr byte = lo; byte < hi; ++byte) {
                            slot->tracked.set(byte - page);
                        }
                    });
                    shared_readbacks.emplace_back(page, 4096);
                    NoteReadbackKind(page, 4096, ReadbackSharedPage);
                    memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                    ++g_shared_writes_count;
                    return;
                }
            }
            if (TryLazyWrite(device_addr, size)) {
                return;
            }
        }
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        DownloadMemory(arena, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        const auto queued = std::chrono::steady_clock::now();
        liverpool->SendCommand<true>(std::move(flush_request));
        if (!is_write) {
            g_read_queue_us += std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - queued)
                                   .count();
        }
    }
}

void BufferCache::DownloadRanges(std::span<const std::pair<VAddr, u64>> ranges) {
    struct Pending {
        const Buffer* arena;
        VAddr address;
        u64 size;
        u64 offset;
    };
    std::vector<Pending> pending;
    u64 total_size_bytes = 0;
    // Older asynchronous readbacks of these bytes go first (theirs is older data).
    u64 readback_tick = 0;
    for (const auto& [address, size] : ranges) {
        readback_tick = std::max(readback_tick, PendingReadbackTick(address, size, false));
    }
    if (readback_tick) {
        CompleteReadbacks(true, readback_tick);
    }
    for (const auto& [address, size] : ranges) {
        if (size == 0) {
            continue;
        }
        const u64 first_block = address >> block_shift;
        const u64 last_block = (address + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);
        memory_tracker->ForEachDownloadRange<false>(address, size, [&](u64 range, u64 range_size) {
            gpu_modified_ranges.ForEachInRange(range, range_size, [&](VAddr start, VAddr end) {
                pending.push_back({arena, start, end - start, total_size_bytes});
                total_size_bytes += (end - start + 63) & ~u64(63);
            });
            GpuRangesSubtract(range, range_size);
        });
    }
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (size_t i = 0; i < pending.size();) {
        boost::container::small_vector<vk::BufferCopy, 16> copies;
        const Buffer* arena = pending[i].arena;
        for (; i < pending.size() && pending[i].arena == arena; ++i) {
            copies.push_back(vk::BufferCopy{
                .srcOffset = pending[i].address - arena->cpu_addr,
                .dstOffset = download.offset + pending[i].offset,
                .size = pending[i].size,
            });
        }
        runtime.CopyBuffer(arena, download.buffer, copies);
    }
    scheduler.Finish();
    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& p : pending) {
        WriteBackGpuData(p.address, download.mapped + p.offset, p.size);
    }
    for (const auto& [address, size] : ranges) {
        if (size != 0) {
            EraseLazyPages(address, size);
            memory_tracker->UnmarkRegionAsGpuModified(address, size);
        }
    }
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    // Older asynchronous readbacks of these bytes go first (theirs is older data).
    if (const u64 readback_tick = PendingReadbackTick(device_addr, size, false)) {
        CompleteReadbacks(true, readback_tick);
    }
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        GpuRangesSubtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    ++gow3_downloads;
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    scheduler.Finish();

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        WriteBackGpuData(arena_base + copy.srcOffset,
                         download.mapped + (copy.dstOffset - download.offset), copy.size);
    }
    EraseLazyPages(device_addr, size);
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

bool BufferCache::TryLazyWrite(VAddr device_addr, u64 size) {
    static_assert(LAZY_PAGE_SIZE == TRACKER_BYTES_PER_PAGE);
    // Fault-sized writes only (unmapping invalidates whole ranges).
    if (size > 64 || !LazyWritesEnabled() || gpu_modified_ranges.Intersects(device_addr, size)) {
        return false;
    }
    SnapshotLazyPages(device_addr, size);
    memory_tracker->MarkRegionAsCpuModified(device_addr, size);
    ++g_lazy_writes;
    return true;
}

bool BufferCache::PrepareHostWrite(VAddr address, u64 size) {
    if (gpu_modified_ranges.Intersects(address, size)) {
        return false;
    }
    if (!LazyWritesEnabled()) {
        if (memory_tracker->IsRegionGpuModified(address, size)) {
            return false;
        }
    } else {
        SnapshotLazyPages(address, size);
    }
    memory_tracker->MarkRegionAsCpuModified(address, size);
    return true;
}

bool BufferCache::GpuBytesInFlight(VAddr page) {
    std::bitset<4096> covered;
    if (const auto it = page_writes.find(page >> 12); it != page_writes.end()) {
        auto* semaphore = scheduler.GetWorkSemaphore();
        semaphore->Refresh(); // the cached value lags: finished work would count as running
        for (const auto& write : it->second) {
            // Recorded since the last readback (the next one brings it) or not finished yet.
            if (write.tick >= last_readback_tick || !semaphore->IsFree(write.tick)) {
                for (u32 byte = write.lo; byte < write.hi; ++byte) {
                    covered.set(byte);
                }
            }
        }
    }
    for (const auto& readback : pending_readbacks) {
        for (const auto& [address, size] : readback.pieces) {
            const VAddr lo = std::max(address, page);
            const VAddr hi = std::min(address + size, page + 4096);
            for (VAddr byte = lo; byte < hi; ++byte) {
                covered.set(byte - page);
            }
        }
    }
    bool all = true;
    gpu_modified_ranges.ForEachInRange(page, 4096, [&](VAddr lo, VAddr hi) {
        for (VAddr byte = lo; byte < hi && all; ++byte) {
            all = covered.test(byte - page);
        }
    });
    return all;
}

static std::atomic<u32> g_labels_in_readbacks{0};

u32 BufferCache::TakeLabelsInReadbacks() {
    return g_labels_in_readbacks.exchange(0, std::memory_order_relaxed);
}

void BufferCache::NoteCpuWrite(VAddr address, u64 size) {
    bool inside = false;
    for (auto& readback : pending_readbacks) {
        readback.rewritten.Add(address, size);
        for (const auto& [lo, n] : readback.pieces) {
            inside |= address < lo + n && lo < address + size;
        }
    }
    if (inside) {
        g_labels_in_readbacks.fetch_add(1, std::memory_order_relaxed);
    }
    if (!gpu_modified_ranges.Intersects(address, size)) {
        return;
    }
    GpuRangesSubtract(address, size);
    for (VAddr page = Common::AlignDown(address, LAZY_PAGE_SIZE); page < address + size;
         page += LAZY_PAGE_SIZE) {
        if (!gpu_modified_ranges.Intersects(page, LAZY_PAGE_SIZE)) {
            lazy_pages.erase(page);
            memory_tracker->UnmarkRegionAsGpuModified(page, LAZY_PAGE_SIZE);
        }
    }
}

bool BufferCache::WriteThrough(VAddr address, const void* data, u64 size) {
    if (size == 0 || size > 64_KB || address % 4 != 0 || size % 4 != 0) {
        return false;
    }
    if (!memory->TryWriteBacking(reinterpret_cast<void*>(address), data, size)) {
        return false;
    }
    // The GPU's copy, in order with the work recorded so far (resident and synchronized first).
    const auto [arena, offset] = ObtainBuffer(address, static_cast<u32>(size), false);
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached, 4);
    std::memcpy(staging.mapped, data, size);
    staging.Flush();
    const vk::BufferCopy copy{
        .srcOffset = staging.offset,
        .dstOffset = offset,
        .size = size,
    };
    runtime.CopyBuffer(staging.buffer, arena, std::span{&copy, 1});
    // Newest value in both views: not GPU data any more; readbacks recorded earlier must not
    // write their older values over it.
    if (gpu_modified_ranges.Intersects(address, size)) {
        GpuRangesSubtract(address, size);
        for (VAddr page = Common::AlignDown(address, LAZY_PAGE_SIZE); page < address + size;
             page += LAZY_PAGE_SIZE) {
            if (!gpu_modified_ranges.Intersects(page, LAZY_PAGE_SIZE)) {
                lazy_pages.erase(page);
                memory_tracker->UnmarkRegionAsGpuModified(page, LAZY_PAGE_SIZE);
            }
        }
    }
    for (auto& readback : pending_readbacks) {
        readback.rewritten.Add(address, size);
    }
    return true;
}

void BufferCache::SnapshotLazyPages(VAddr address, u64 size) {
    const VAddr end = address + size;
    for (VAddr page = Common::AlignDown(address, LAZY_PAGE_SIZE); page < end;
         page += LAZY_PAGE_SIZE) {
        LazyPage* record = nullptr;
        std::array<u8, LAZY_PAGE_SIZE> guest;
        gpu_modified_ranges.ForEachInRange(page, LAZY_PAGE_SIZE, [&](VAddr lo, VAddr hi) {
            if (!record) {
                auto& slot = lazy_pages[page];
                if (!slot) {
                    slot = std::make_unique<LazyPage>();
                }
                record = slot.get();
                if (!memory->TryReadBacking(page, guest.data(), LAZY_PAGE_SIZE)) {
                    memory->CopySparseMemory(page, guest.data(), LAZY_PAGE_SIZE);
                }
            }
            for (VAddr byte = lo; byte < hi; ++byte) {
                const size_t i = byte - page;
                // A byte recorded by an earlier lazy write keeps that record.
                if (!record->tracked.test(i)) {
                    record->tracked.set(i);
                    record->bytes[i] = guest[i];
                }
            }
        });
    }
}

template <typename Func>
void BufferCache::ForEachLazyUploadPiece(VAddr address, u64 size, Func&& func) {
    const VAddr end = address + size;
    auto it = lazy_pages.lower_bound(Common::AlignDown(address, LAZY_PAGE_SIZE));
    if (it == lazy_pages.end() || it->first >= end) {
        func(address, size);
        return;
    }
    boost::container::small_vector<std::pair<VAddr, VAddr>, 16> skips;
    while (it != lazy_pages.end() && it->first < end) {
        const VAddr page = it->first;
        LazyPage& record = *it->second;
        std::array<u8, LAZY_PAGE_SIZE> guest;
        if (!memory->TryReadBacking(page, guest.data(), LAZY_PAGE_SIZE)) {
            memory->CopySparseMemory(page, guest.data(), LAZY_PAGE_SIZE);
        }
        const VAddr lo_limit = std::max(page, address);
        const VAddr hi_limit = std::min(page + LAZY_PAGE_SIZE, end);
        boost::container::small_vector<std::pair<VAddr, VAddr>, 8> cpu_wins;
        const auto add_run = [](auto& runs, VAddr byte) {
            if (!runs.empty() && runs.back().second == byte) {
                ++runs.back().second;
            } else {
                runs.emplace_back(byte, byte + 1);
            }
        };
        // Records outside gpu_modified_ranges are stale (downloaded meanwhile): ignored.
        gpu_modified_ranges.ForEachInRange(lo_limit, hi_limit - lo_limit, [&](VAddr lo, VAddr hi) {
            for (VAddr byte = lo; byte < hi; ++byte) {
                const size_t i = byte - page;
                if (!record.tracked.test(i)) {
                    continue;
                }
                if (record.gpu_wins || guest[i] == record.bytes[i]) {
                    add_run(skips, byte);
                } else {
                    record.tracked.reset(i);
                    add_run(cpu_wins, byte);
                }
            }
        });
        // The CPU overwrote these after the GPU: they are uploaded and stop being GPU data.
        for (const auto& [lo, hi] : cpu_wins) {
            GpuRangesSubtract(lo, hi - lo);
            g_lazy_cpu_wins += u32(hi - lo);
        }
        it = record.tracked.none() ? lazy_pages.erase(it) : std::next(it);
    }
    VAddr cursor = address;
    for (const auto& [lo, hi] : skips) {
        if (lo > cursor) {
            func(cursor, lo - cursor);
        }
        cursor = hi;
    }
    if (cursor < end) {
        func(cursor, end - cursor);
    }
}

void BufferCache::WriteBackGpuData(VAddr address, const u8* data, u64 size,
                                   const RangeSet* rewritten) {
    const VAddr end = address + size;
    auto it = lazy_pages.lower_bound(Common::AlignDown(address, LAZY_PAGE_SIZE));
    VAddr cursor = address;
    while (it != lazy_pages.end() && it->first < end) {
        const VAddr page = it->first;
        LazyPage& record = *it->second;
        std::array<u8, LAZY_PAGE_SIZE> guest;
        // Through the backing view: the page may still be read-protected for this very readback
        // (CompleteReadbacks unwatches after the write-back); a read fault there completed the
        // same readback again, recursively, until the stack ran out (U2 silent exit, 15 Oct).
        if (!memory->TryReadBacking(page, guest.data(), LAZY_PAGE_SIZE)) {
            memory->CopySparseMemory(page, guest.data(), LAZY_PAGE_SIZE);
        }
        const VAddr lo = std::max(page, address);
        const VAddr hi = std::min(page + LAZY_PAGE_SIZE, end);
        for (VAddr byte = lo; byte < hi; ++byte) {
            const size_t i = byte - page;
            if (!record.tracked.test(i)) {
                continue;
            }
            const bool still_gpu = rewritten && rewritten->Contains(byte, 1);
            if (record.gpu_wins) {
                // The GPU's bytes: written back; tracked while newer GPU data is still to come.
                if (!still_gpu) {
                    record.tracked.reset(i);
                }
                continue;
            }
            if (guest[i] == record.bytes[i]) {
                if (rewritten) {
                    // Written now; the GPU's newer value is stale in guest memory again.
                    if (still_gpu) {
                        record.bytes[i] = data[byte - address];
                    } else {
                        record.tracked.reset(i);
                    }
                }
                continue;
            }
            // The CPU wrote this byte after the GPU: keep the CPU value. The page is still
            // CPU-modified, so the next upload sends it to the GPU copy too.
            if (byte > cursor) {
                memory->TryWriteBacking(std::bit_cast<u8*>(cursor), data + (cursor - address),
                                        byte - cursor);
            }
            cursor = byte + 1;
            ++g_lazy_cpu_wins;
            if (rewritten && !still_gpu) {
                record.tracked.reset(i);
            }
        }
        it = record.tracked.none() ? lazy_pages.erase(it) : std::next(it);
    }
    if (cursor < end) {
        memory->TryWriteBacking(std::bit_cast<u8*>(cursor), data + (cursor - address),
                                end - cursor);
    }
}

void BufferCache::DownloadRangesAsync(std::span<const std::pair<VAddr, u64>> ranges, bool protect,
                                      bool yield_to_cpu) {
    last_readback_tick = scheduler.CurrentTick();
    PendingReadback readback;
    struct Copy {
        const Buffer* arena;
        VAddr address;
        u64 size;
    };
    std::vector<Copy> pending;
    u64 total_size_bytes = 0;
    for (const auto& [address, size] : ranges) {
        if (size == 0) {
            continue;
        }
        const u64 first_block = address >> block_shift;
        const u64 last_block = (address + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);
        memory_tracker->ForEachDownloadRange<false>(address, size, [&](u64 range, u64 range_size) {
            gpu_modified_ranges.ForEachInRange(range, range_size, [&](VAddr start, VAddr end) {
                pending.push_back({arena, start, end - start});
                readback.pieces.emplace_back(start, end - start);
                readback.offsets.push_back(total_size_bytes);
                total_size_bytes += (end - start + 63) & ~u64(63);
            });
        });
    }
    if (total_size_bytes == 0) {
        return;
    }
    // Held until the readback completes (a ring allocation could be reused before).
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached,
                                               0, true);
    for (size_t i = 0; i < pending.size();) {
        boost::container::small_vector<vk::BufferCopy, 16> copies;
        const Buffer* arena = pending[i].arena;
        for (; i < pending.size() && pending[i].arena == arena; ++i) {
            copies.push_back(vk::BufferCopy{
                .srcOffset = pending[i].address - arena->cpu_addr,
                .dstOffset = download.offset + readback.offsets[i],
                .size = pending[i].size,
            });
        }
        runtime.CopyBuffer(arena, download.buffer, copies);
    }
    if (SafeReadbacks() || protect) {
        std::vector<VAddr> pages;
        const auto now = std::chrono::steady_clock::now();
        for (const auto& [address, size] : readback.pieces) {
            if (!protect) {
                if (const auto it = landed_pieces.find(address);
                    it != landed_pieces.end() && it->second.first == size &&
                    now - it->second.second < RecurringWindow) {
                    ++g_pieces_recurring;
                    continue;
                }
                ++g_pieces_protected;
            }
            for (VAddr page = Common::AlignDown(address, 4096); page < address + size; page += 4096) {
                if (!protect) {
                    if (const auto it = noisy_pages.find(page >> 12); it != noisy_pages.end()) {
                        if (it->second > now) {
                            continue;
                        }
                        noisy_pages.erase(it);
                    }
                }
                pages.push_back(page);
            }
        }
        std::ranges::sort(pages);
        const auto [first, last] = std::ranges::unique(pages);
        pages.erase(first, last);
        for (size_t i = 0; i < pages.size();) {
            size_t j = i + 1;
            while (j < pages.size() && pages[j] == pages[j - 1] + 4096) {
                ++j;
            }
            readback.watched.emplace_back(pages[i], (pages[j - 1] + 4096) - pages[i]);
            i = j;
        }
    }
    readback.tick = scheduler.CurrentTick();
    readback.yield_to_cpu = yield_to_cpu;
    readback.buffer = download.buffer;
    readback.offset = download.offset;
    readback.size = download.size;
    readback.mapped = download.mapped;
    g_readback_bytes += readback.size;
    if (ReadbackStatsEnabled()) {
        static auto last = std::chrono::steady_clock::now();
        std::string text;
        {
            std::scoped_lock lock{g_rb_stats_mutex};
            for (const auto& [address, size] : readback.pieces) {
                auto& entry = g_rb_window_bytes[address >> 20];
                entry[0] += size;
                ++entry[1];
                if (protect) {
                    g_rb_window_kinds[address >> 20] |= ReadbackEop;
                }
            }
            if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
                last = now;
                std::vector<std::pair<u64, u64>> top;
                u64 total = 0;
                for (const auto& [window, entry] : g_rb_window_bytes) {
                    top.emplace_back(entry[0], window);
                    total += entry[0];
                }
                std::ranges::sort(top, std::greater{});
                text = fmt::format("Readback windows (5 s): {:.1f} MB in {} windows\n", total / 1e6, top.size());
                for (size_t i = 0; i < std::min<size_t>(top.size(), 16); ++i) {
                    const u64 window = top[i].second;
                    const u32 kinds = g_rb_window_kinds[window];
                    text += fmt::format("  {:#x}: {:.1f} MB, {} pieces, {}{}{}{}{}{}\n", window << 20,
                                        top[i].first / 1e6, g_rb_window_bytes[window][1],
                                        kinds & ReadbackSmallWrite ? "small " : "",
                                        kinds & ReadbackHotWindow ? "hot " : "",
                                        kinds & ReadbackCpWrite ? "cp " : "",
                                        kinds & ReadbackSharedPage ? "shared " : "",
                                        kinds & ReadbackEop ? "eop " : "", "");
                }
                g_rb_window_bytes.clear();
                g_rb_window_kinds.clear();
            }
        }
        if (!text.empty()) {
            // Writers per window (NoteGpuWriter is on with these statistics).
            std::string out;
            size_t pos = 0;
            while (pos < text.size()) {
                const size_t end = text.find('\n', pos);
                std::string line = text.substr(pos, end - pos);
                if (line.starts_with("  0x")) {
                    const u64 window = std::strtoull(line.c_str() + 2, nullptr, 16) >> 20;
                    line += WindowWriters(window);
                }
                out += line + "\n";
                pos = end + 1;
            }
            std::fputs(out.c_str(), stdout);
            std::fflush(stdout);
        }
    }
    pending_readbacks.push_back(std::move(readback));
    ++g_async_readbacks;
    SweepTracking();
}

void BufferCache::SweepTracking() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_tracking_sweep < std::chrono::seconds{2}) {
        return;
    }
    last_tracking_sweep = now;
    // Pages whose small GPU writes all finished before the last readback: GpuBytesInFlight
    // ignores such entries, so the page's record only cost memory and hash-table size.
    auto* semaphore = scheduler.GetWorkSemaphore();
    semaphore->Refresh();
    g_swept_pages += static_cast<u32>(std::erase_if(page_writes, [&](const auto& entry) {
        return std::ranges::all_of(entry.second, [&](const PageWrite& w) {
            return w.tick < last_readback_tick && semaphore->IsFree(w.tick);
        });
    }));
    if (HotWindowTtl().count()) {
        g_swept_windows += static_cast<u32>(std::erase_if(cpu_write_hot, [&](const auto& entry) {
            return now - entry.second >= HotWindowTtl();
        }));
    }
}

void BufferCache::CompleteReadbacks(bool wait, u64 wait_until) {
    // Pages of a readback are read-protected once its submission is out: a read fault waits
    // for submitted work only (never for the command buffer this thread is still recording).
    for (auto& readback : pending_readbacks) {
        if (readback.watching || readback.watched.empty() || readback.tick >= scheduler.CurrentTick()) {
            continue;
        }
        for (const auto& [address, size] : readback.watched) {
            page_manager->UpdatePageWatchers<true, true>(address, size);
        }
        readback.watching = true;
    }
    auto* semaphore = scheduler.GetWorkSemaphore();
    while (!pending_readbacks.empty()) {
        PendingReadback& readback = pending_readbacks.front();
        if (!semaphore->IsFree(readback.tick)) {
            if (wait && readback.tick <= wait_until) {
                scheduler.Wait(readback.tick);
            } else {
                // Querying the timeline semaphore is an ioctl: once per 16 polls.
                if (!wait && (++readback_polls & 15) != 0) {
                    return;
                }
                semaphore->Refresh();
                if (!semaphore->IsFree(readback.tick)) {
                    return;
                }
            }
        }
        readback.buffer->Invalidate(readback.offset, readback.size);
        // Bytes downloaded meanwhile, or uploaded after a CPU write, are no longer GPU data.
        for (size_t i = 0; i < readback.pieces.size(); ++i) {
            const auto [address, size] = readback.pieces[i];
            const u8* data = readback.mapped + readback.offsets[i];
            gpu_modified_ranges.ForEachInRange(address, size, [&](VAddr lo, VAddr hi) {
                WriteBackGpuData(lo, data + (lo - address), hi - lo, &readback.rewritten);
            });
        }
        for (const auto& [address, size] : readback.pieces) {
            readback.rewritten.ForEachNotInRange(address, size, [&](VAddr lo, size_t gap) {
                GpuRangesSubtract(lo, gap);
            });
        }
        {
            const auto now = std::chrono::steady_clock::now();
            if (landed_pieces.size() > 8192) {
                std::erase_if(landed_pieces, [&](const auto& entry) {
                    return now - entry.second.second >= RecurringWindow;
                });
            }
            for (const auto& [address, size] : readback.pieces) {
                landed_pieces[address] = {size, now};
            }
        }
        // Pages left without GPU data stop being GPU-modified (pieces are in address order).
        VAddr last_page = ~0ULL;
        for (const auto& [address, size] : readback.pieces) {
            for (VAddr page = Common::AlignDown(address, LAZY_PAGE_SIZE); page < address + size;
                 page += LAZY_PAGE_SIZE) {
                if (page == last_page) {
                    continue;
                }
                last_page = page;
                if (!gpu_modified_ranges.Intersects(page, LAZY_PAGE_SIZE)) {
                    lazy_pages.erase(page);
                    memory_tracker->UnmarkRegionAsGpuModified(page, LAZY_PAGE_SIZE);
                }
            }
        }
        if (readback.watching) {
            for (const auto& [address, size] : readback.watched) {
                page_manager->UpdatePageWatchers<false, true>(address, size);
            }
        }
        staging_pool.FreeDeferred(Vulkan::StagingBufferRef{
            .buffer = readback.buffer,
            .offset = readback.offset,
            .size = readback.size,
            .mapped = readback.mapped,
            .type = VideoCore::MemoryType::HostCached,
        });
        auto landed = std::move(readback.on_landed);
        pending_readbacks.pop_front();
        for (auto& fn : landed) {
            --deferred_signals;
            fn();
        }
    }
}

bool BufferCache::DeferUntilLastReadback(std::function<void()> fn, VAddr label, u64 value, u32 bytes) {
    if (pending_readbacks.empty()) {
        return false;
    }
    ++deferred_signals;
    ++g_deferred_fences;
    {
        static std::mutex labels_mutex;
        static std::unordered_map<VAddr, u32> seen;
        std::scoped_lock lock{labels_mutex};
        if (seen.size() < 12 && seen[label]++ == 0) {
            std::printf("Deferred fence: label %#llx (tick %llu)\n", static_cast<unsigned long long>(label),
                        static_cast<unsigned long long>(pending_readbacks.back().tick));
            std::fflush(stdout);
        }
    }
    fn = [fn = std::move(fn), start = std::chrono::steady_clock::now()] {
        const u64 us = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        g_deferred_us += us;
        u64 max = g_deferred_max_us.load();
        while (us > max && !g_deferred_max_us.compare_exchange_weak(max, us)) {
        }
        fn();
    };
    if (label) {
        deferred_labels.insert(label);
        deferred_values[label] = {value, bytes ? bytes : 4u, pending_readbacks.back().tick};
        pending_readbacks.back().on_landed.emplace_back([this, label, fn = std::move(fn)] {
            if (const auto it = deferred_labels.find(label); it != deferred_labels.end()) {
                deferred_labels.erase(it);
            }
            if (!deferred_labels.contains(label)) {
                deferred_values.erase(label);
            }
            fn();
        });
    } else {
        pending_readbacks.back().on_landed.emplace_back(std::move(fn));
    }
    return true;
}

bool BufferCache::DeferredFenceTick(VAddr address, u64& tick, u32* value) const {
    for (const auto& [label, deferred] : deferred_values) {
        if (address >= label && address + sizeof(u32) <= label + deferred.bytes) {
            tick = deferred.tick;
            if (value) {
                *value = static_cast<u32>(deferred.value >> ((address - label) * 8));
            }
            return true;
        }
    }
    return false;
}

void BufferCache::LandReadbacks(u64 tick) {
    CompleteReadbacks(true, tick);
}

bool BufferCache::HasUnsubmittedReadback() const {
    return !pending_readbacks.empty() && pending_readbacks.back().tick >= scheduler.CurrentTick();
}

void BufferCache::PollReadbacksNow() {
    if (pending_readbacks.empty()) {
        return;
    }
    scheduler.GetWorkSemaphore()->Refresh();
    readback_polls = 0;
    CompleteReadbacks(false);
}

void BufferCache::UnwatchPage(VAddr page) {
    for (auto& readback : pending_readbacks) {
        for (size_t i = 0; i < readback.watched.size(); ++i) {
            const auto [address, size] = readback.watched[i];
            if (page < address || page >= address + size) {
                continue;
            }
            if (readback.watching) {
                page_manager->UpdatePageWatchers<false, true>(page, 4096);
            }
            // Split the range around the page.
            const VAddr tail = page + 4096;
            const u64 tail_size = address + size - tail;
            if (page > address) {
                readback.watched[i].second = page - address;
                if (tail_size) {
                    readback.watched.emplace(readback.watched.begin() + i + 1, tail, tail_size);
                }
            } else if (tail_size) {
                readback.watched[i] = {tail, tail_size};
            } else {
                readback.watched.erase(readback.watched.begin() + i);
            }
            break;
        }
    }
}

u64 BufferCache::PendingReadbackTick(VAddr address, u64 size, bool newest_only) const {
    u64 tick = 0;
    const VAddr end = address + size;
    for (const auto& readback : pending_readbacks) {
        for (const auto& [piece, piece_size] : readback.pieces) {
            const VAddr lo = std::max(piece, address);
            const VAddr hi = std::min(piece + piece_size, end);
            if (lo >= hi) {
                continue;
            }
            if (!newest_only || (gpu_modified_ranges.Intersects(lo, hi - lo) &&
                                 !readback.rewritten.Contains(lo, hi - lo))) {
                tick = std::max(tick, readback.tick);
                break;
            }
        }
    }
    return tick;
}

void BufferCache::EraseLazyPages(VAddr address, u64 size) {
    if (lazy_pages.empty()) {
        return;
    }
    const auto first = lazy_pages.lower_bound(Common::AlignDown(address, LAZY_PAGE_SIZE));
    const auto last = lazy_pages.lower_bound(address + size);
    lazy_pages.erase(first, last);
}

namespace {
// gow3: GOW3_BUFFER_STATS=1 — how buffer bindings reach the GPU, by guest region (256 MiB),
// printed every 5 s: small read-only copies into the stream buffer, arena bindings, and the
// bytes those re-upload after CPU writes. Input for engine-level short paths.
struct BufferStats {
    struct Region {
        u64 stream_count{}, stream_bytes{}, arena_count{}, arena_bytes{}, upload_bytes{};
    };
    std::map<u64, Region> regions;
    /// Hot window: frame of the last binding per 64 KiB block, and the distribution of the
    /// frames between uses (1, 2, 3, 4, 5-8, 9-16, 17+): the ring's reuse distance.
    std::unordered_map<u64, u64> last_use;
    std::array<u64, 7> reuse{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 calls = 0;
};
/// 256 MiB regions; 1 MiB inside the hot 0x104xxxxxxx window (the engine's frame data).
BufferStats& Stats();
void NoteHotUse(VAddr address, u32 size) {
    if ((address >> 28) != 0x104) {
        return;
    }
    auto& st = Stats();
    const u64 frame = Gow3Stats::gpu_frames.load(std::memory_order_relaxed);
    for (u64 block = address >> 16; block <= (address + size - 1) >> 16; ++block) {
        auto [it, inserted] = st.last_use.try_emplace(block, frame);
        if (!inserted && it->second != frame) {
            const u64 d = frame - it->second;
            ++st.reuse[d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : 6];
            it->second = frame;
        }
    }
}
u64 RegionKey(VAddr address) {
    return (address >> 28) == 0x104 ? (address >> 20) | (1ull << 40) : address >> 28;
}
bool BufferStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("GOW3_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
BufferStats& Stats() {
    static BufferStats stats;
    return stats;
}
void PrintBufferStats() {
    auto& st = Stats();
    if ((++st.calls & 4095) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds < 5.0) {
        return;
    }
    std::printf("Buffer stats (%.1f s, per second): region, stream copies/bytes, arena bindings, "
                "re-uploaded bytes\n", seconds);
    std::printf("  hot window reuse distance in frames (1,2,3,4,5-8,9-16,17+): %llu %llu %llu %llu "
                "%llu %llu %llu\n",
                (unsigned long long)st.reuse[0], (unsigned long long)st.reuse[1],
                (unsigned long long)st.reuse[2], (unsigned long long)st.reuse[3],
                (unsigned long long)st.reuse[4], (unsigned long long)st.reuse[5],
                (unsigned long long)st.reuse[6]);
    st.reuse = {};
    for (const auto& [region, r] : st.regions) {
        const u64 base = (region >> 40) ? (region & ((1ull << 40) - 1)) << 20 : region << 28;
        if ((region >> 40) && r.stream_bytes + r.upload_bytes < 1e6 * seconds) {
            continue; // quiet MiB of the hot window
        }
        std::printf("  %#012llx: %8.0f copies %8.2f MB, %8.0f arena %8.2f MB bound, %8.2f MB uploads\n",
                    static_cast<unsigned long long>(base), r.stream_count / seconds,
                    r.stream_bytes / seconds / 1e6, r.arena_count / seconds,
                    r.arena_bytes / seconds / 1e6,
                    r.upload_bytes / seconds / 1e6);
    }
    st.regions.clear();
    st.window = now;
}
} // namespace

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferObtain);
    const bool stats = BufferStatsEnabled();
    if (stats) {
        PrintBufferStats();
        NoteHotUse(device_addr, size);
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        if (stats) {
            auto& region = Stats().regions[RegionKey(device_addr)];
            ++region.stream_count;
            region.stream_bytes += size;
        }
        // gow3: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !Gow3Toggle::Disabled(Gow3Toggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    const u64 uploaded_before = Gow3Stats::buffer_upload_bytes.load(std::memory_order_relaxed);
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (stats) {
        auto& region = Stats().regions[RegionKey(device_addr)];
        ++region.arena_count;
        region.arena_bytes += size;
        region.upload_bytes +=
            Gow3Stats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before;
    }
    if (is_written) {
        // Small writes only (counters, constant blocks: at most two pages): the CPU writes next
        // to these; larger GPU data in a page the CPU writes is downloaded as before.
        if (size <= 4096) {
            const u64 tick = scheduler.CurrentTick();
            const VAddr end = device_addr + size;
            for (VAddr page = Common::AlignDown(device_addr, 4096); page < end; page += 4096) {
                auto& writes = page_writes[page >> 12];
                const auto lo = static_cast<u16>(std::max(device_addr, page) - page);
                const auto hi = static_cast<u16>(std::min(end, page + 4096) - page);
                // The same range written again (every frame): its newest tick is what counts.
                // Appending instead grew each page's list by one entry per frame, and the prune
                // below scanned it all on every binding (Uncharted 2: about 1 MB of memory per
                // second, the frame rate falling from 42 to 31 FPS over a few minutes).
                const auto same = std::find_if(writes.begin(), writes.end(), [&](const PageWrite& w) {
                    return w.lo == lo && w.hi == hi;
                });
                if (same != writes.end()) {
                    same->tick = std::max(same->tick, tick);
                    continue;
                }
                if (writes.size() >= 16) {
                    // Entries GpuBytesInFlight no longer counts: finished, older than the last
                    // readback. If the page is still full, the oldest go (its CPU writes may
                    // then download as before).
                    auto* semaphore = scheduler.GetWorkSemaphore();
                    writes.erase(std::remove_if(writes.begin(), writes.end(),
                                                [&](const PageWrite& w) {
                                                    return w.tick < last_readback_tick &&
                                                           semaphore->IsFree(w.tick);
                                                }),
                                 writes.end());
                    if (writes.size() >= 16) {
                        std::sort(writes.begin(), writes.end(),
                                  [](const PageWrite& a, const PageWrite& b) { return a.tick < b.tick; });
                        writes.erase(writes.begin(), writes.begin() + (writes.size() - 8));
                    }
                }
                writes.push_back({lo, hi, tick});
            }
        }
        GpuRangesAdd(device_addr, size);
        for (auto& readback : pending_readbacks) {
            readback.rewritten.Add(device_addr, size);
        }
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferForImage);
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!Gow3Toggle::Disabled(Gow3Toggle::DeferredUploads)) {
        // gow3: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const Gow3Copy::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                Gow3Copy::Async([item] { item.run(item); });
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (arena_page_bits - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

void BufferCache::SynchronizeDmaRange(VAddr address, u64 size) {
    const u64 first = address >> block_shift;
    const u64 last = (address + size + (1ULL << block_shift) - 1) >> block_shift;
    resident_ranges.ForEachInRange(first, last, [&](const Backing& backing) {
        const u64 start = std::max(first, backing.start);
        const u64 end = std::min(last, backing.end);
        if (start >= end) {
            return;
        }
        // One arena page at a time: SynchronizeMemory copies relative to a single arena.
        const u64 blocks_per_page = 1ULL << (arena_page_bits - block_shift);
        for (u64 block = start; block < end;) {
            const u64 page = block >> (arena_page_bits - block_shift);
            const u64 stop = std::min(end, (page + 1) * blocks_per_page);
            if (const auto* arena = address_space[page]) {
                SynchronizeMemory(arena, block << block_shift,
                                  static_cast<u32>((stop - block) << block_shift), false, false);
            }
            block = stop;
        }
    });
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << arena_page_bits, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << arena_page_bits);
    const u64 first_size = first_arena ? first_arena->size_bytes : arena_page_size;
    const u64 last_size = last_arena ? last_arena->size_bytes : arena_page_size;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = backing.offset + ((start - backing.start) << block_shift),
        });
    });

    u64 base_page = first_addr >> arena_page_bits;
    for (u32 page = 0; page < (first_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }
    Gow3Stats::Timer timer{Gow3Stats::t_resident};

    const vk::MemoryAllocateInfo alloc_info = {
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    const auto device_memory = Vulkan::Check(instance.GetDevice().allocateMemory(alloc_info));

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    u64 memory_offset{};
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            ForEachLazyUploadPiece(addr, size, [&](VAddr piece, u64 piece_size) {
                copies.emplace_back(total_size_bytes, piece, piece_size);
                total_size_bytes += piece_size;
            });
        },
        [&] { src_buffer = UploadCopies(arena, copies, total_size_bytes); });

    if (src_buffer) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    // Resolve before launching host copies: a copy-thread fault could deadlock submission.
    for (const auto& copy : copies) {
        texture_cache.ResolveReadbacks(copy.dstOffset, copy.size, true, ReadbackSource::BufferUpload);
    }
    Gow3Stats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // gow3: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!Gow3Toggle::Disabled(Gow3Toggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!Gow3Toggle::Disabled(Gow3Toggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            Gow3Copy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        Gow3Copy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    // gow3: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !Gow3Toggle::Disabled(Gow3Toggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // gow3: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    if (submit_result == vk::Result::eErrorDeviceLost) {
        Vulkan::DumpGpuCheckpoints(instance);
        Vulkan::DumpDeviceFault(instance);
    }
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore
