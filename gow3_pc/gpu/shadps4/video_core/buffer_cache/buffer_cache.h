// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <shared_mutex>

#include <format>
#include <string>

#include "gow3_copy.h"

#include <array>
#include <atomic>
#include <bitset>
#include <deque>
#include <map>
#include <memory>
#include <unordered_map>
#include <functional>
#include <unordered_set>
#include <utility>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

/// gow3 diagnostics (GOW3_FAULT_STATS=1 GOW3_READ_SOURCES=1): records which shader wrote GPU data into each 1 MiB
/// window, listed with the windows the CPU reads GPU data from.
void NoteGpuWriter(VAddr address, u64 size, u64 shader_hash);
/// gow3 diagnostics (GOW3_READBACK_STATS=1): why a range joins the per-submission readback
/// (ReadbackKind bits), shown per 1 MiB window with the readback bytes every 5 s.
enum ReadbackKind : u32 {
    ReadbackSmallWrite = 1,  ///< storage write of 4 KiB or less (64 KiB window around it)
    ReadbackHotWindow = 2,   ///< part of a large storage write in a CPU-write-hot 1 MiB window
    ReadbackCpWrite = 4,     ///< CP fill or copy of 4 KiB or less
    ReadbackSharedPage = 8,  ///< page the CPU shares with GPU data in flight
    ReadbackEop = 16,        ///< ray-cast results / 6a6422a9 copies (fence waits)
};
bool ReadbackStatsEnabled();
void NoteReadbackKind(VAddr address, u64 size, u32 kind);

/// gow3: GPU-to-guest downloads done by this thread (fault statistics, page_manager.cpp).
inline thread_local u64 gow3_downloads = 0;


class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    // gow3: arena pages are 4 GiB unless the driver's buffer size limit is lower (AMD's
    // Windows driver: 2 GiB); an arena spans up to two pages, so they shrink to half the limit.
    static constexpr u64 MAX_ARENA_PAGE_BITS = 32;
    static constexpr u64 MIN_ARENA_PAGE_BITS = 30;
    static constexpr u64 MAX_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - MIN_ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;

public:
    /// Read-only bindings up to this size are copied into a stream buffer (gow3: public for
    /// the draw pipeline's constant ring).
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// gow3: a guest write fault at `device_addr` was handled; unprotect its neighbourhood.
    void ExtendWriteFault(VAddr device_addr);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// gow3: 1 MiB windows where the CPU wrote into GPU-written memory (each write needed a
    /// download with a full GPU wait). GPU writes there join the per-submission download
    /// (Rasterizer::SyncSmallGpuWrites) so later CPU writes find the pages clean. GPU-side use.
    /// gow3 16 Oct: windows cool down 3 s after the last such CPU write (GOW3_HOT_WINDOW_TTL=<s>;
    /// 0 keeps them hot for good). Kept hot, every window the effect shaders (cs 8f067540,
    /// 7d7e7206) ever shared with the CPU was read back whole after each submission although the
    /// CPU no longer touched it: ~500 MB/s in Uncharted 2 and 3 (the CPU reads none of it, precise
    /// readback statistics). The cooldown was once blamed for Uncharted 2's floating NPCs; that
    /// was the 6a6422a9 copies (now EOP readbacks), and the NPCs stand on the floor with it.
    [[nodiscard]] bool IsCpuWriteHot(VAddr address, u64 size) const {
        if (cpu_write_hot.empty()) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        for (u64 window = address >> 20; window <= (address + size - 1) >> 20; ++window) {
            if (const auto it = cpu_write_hot.find(window);
                it != cpu_write_hot.end() && (!HotWindowTtl().count() || now - it->second < HotWindowTtl())) {
                return true;
            }
        }
        return false;
    }
    static std::chrono::seconds HotWindowTtl() {
        static const std::chrono::seconds ttl{[] {
            const char* env = std::getenv("GOW3_HOT_WINDOW_TTL");
            return env ? std::atoll(env) : 3LL;
        }()};
        return ttl;
    }

    /// gow3: copies the GPU-modified parts of the ranges back to guest memory with one GPU wait
    /// (GPU thread). For data the GPU writes and CPU-side code (the SRT walker) reads.
    void DownloadRanges(std::span<const std::pair<VAddr, u64>> ranges);

    /// gow3: GPU-written data newer than guest memory lies in [address, address + size).
    [[nodiscard]] bool IsRangeGpuWritten(VAddr address, u64 size) const {
        return gpu_modified_ranges.Intersects(address, size);
    }

    /// gow3: IsRangeGpuWritten from the GPU command thread while the draw recording thread may
    /// change the set (multithreaded rendering: SRT sync before a walk). The set is not thread
    /// safe: a lookup during an update followed a freed node (Uncharted 3 crashed within minutes).
    [[nodiscard]] bool IsRangeGpuWrittenShared(VAddr address, u64 size) const {
        std::shared_lock lock{gpu_ranges_mutex};
        return gpu_modified_ranges.Intersects(address, size);
    }

    /// gow3: the GPU thread is about to write [address, address + size) in guest memory itself
    /// (shader HLE). Unprotects it like a guest write fault would, without a download. False
    /// when GPU-written bytes are in the range: the GPU must do that write.
    bool PrepareHostWrite(VAddr address, u64 size);

    /// gow3: a small write the GPU thread makes for the guest (copy shader HLE, CP fills and
    /// copies), done at this point of the command stream in guest memory (backing view) and in
    /// the GPU's copy alike. The bytes stop being GPU data: no download, no protection change,
    /// and pending readbacks do not bring older GPU values back over them. False (nothing done)
    /// when the range is unsuitable (unaligned, over 64 KiB, unmapped).
    bool WriteThrough(VAddr address, const void* data, u64 size);

    /// gow3: the runtime writes [address, address + size) through the backing view (fence
    /// labels): the bytes stop being GPU data, and pending readbacks do not bring older GPU
    /// values back over them. Uncharted 2: a ray-cast readback covering the frame's end-of-pipe
    /// label restored its old value (assert m_gfxEopTick in FrameBegin).
    void NoteCpuWrite(VAddr address, u64 size);

    /// gow3 statistics: sizes of the cache's containers (growth over time).
    std::string DebugSizes() const {
        std::size_t entries = 0;
        for (const auto& [page, writes] : page_writes) {
            entries += writes.size();
        }
        return std::format("{} page write records ({} entries), {} lazy pages, {} hot windows, {} "
                           "arenas, {} pending readbacks, {} GPU-written ranges",
                           page_writes.size(), entries, lazy_pages.size(), cpu_write_hot.size(),
                           arenas.size(), pending_readbacks.size(), gpu_modified_ranges.Count());
    }
    /// Labels NoteCpuWrite found inside a pending readback since the last call (statistics).
    static u32 TakeLabelsInReadbacks();

    /// gow3: DownloadRanges without the GPU wait. The copies run after the work already
    /// recorded; PollReadbacks writes them to guest memory once the GPU has run them. Until
    /// then the ranges stay GPU-modified (a CPU write into them waits for its readback).
    /// `protect`: its pages are read-protected until it completes (a read waits for it), as
    /// for every readback with GOW3_ASYNC_READBACK=1.
    /// `yield_to_cpu`: a CPU write into a piece drops its GPU values instead of waiting for
    /// them (the guest refills the buffer; see Rasterizer::ReadBackEopResults).
    void DownloadRangesAsync(std::span<const std::pair<VAddr, u64>> ranges, bool protect = false,
                             bool yield_to_cpu = false);

    /// gow3: writes the readbacks the GPU has finished to guest memory (GPU-side, per draw).
    void PollReadbacks() {
        if (!pending_readbacks.empty()) {
            CompleteReadbacks(false);
        }
    }


    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();
    /// gow3: SynchronizeDmaBuffers limited to the resident memory in [address, address + size).
    void SynchronizeDmaRange(VAddr address, u64 size);

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    /// gow3: a CPU write fault next to, not on, GPU-written bytes: unprotects the page without
    /// a download (see LazyPage). False when the download is needed.
    bool TryLazyWrite(VAddr device_addr, u64 size);

    /// gow3: records the stale guest copy of the GPU-written bytes in the pages of a CPU write
    /// that is about to unprotect them (see LazyPage).
    void SnapshotLazyPages(VAddr address, u64 size);

    /// gow3: calls func(address, size) for the parts of an upload range to copy: GPU-written
    /// bytes of lazy pages that the CPU has not overwritten keep the GPU's value.
    template <typename Func>
    void ForEachLazyUploadPiece(VAddr address, u64 size, Func&& func);

    /// gow3: writes downloaded GPU data to guest memory, except bytes of lazy pages that the
    /// CPU overwrote after the GPU (the CPU value is newer). For an asynchronous readback,
    /// `rewritten` holds the bytes the GPU wrote again since: their lazy records are moved to
    /// the written value (still stale), the others are dropped.
    void WriteBackGpuData(VAddr address, const u8* data, u64 size,
                          const RangeSet* rewritten = nullptr);

    /// gow3: forgets the lazy pages in [address, address + size) (their GPU data was downloaded).
    void EraseLazyPages(VAddr address, u64 size);

    /// gow3: writes finished asynchronous readbacks to guest memory, oldest first. With
    /// `wait_until` set, waits for the GPU up to that readback tick (~0: all of them).
    void CompleteReadbacks(bool wait, u64 wait_until = ~0ULL);

    /// gow3: the latest tick of the pending readbacks with bytes in [address, address + size),
    /// or 0. With `newest_only`, only readbacks holding the newest GPU data of those bytes
    /// count (not rewritten by the GPU since).
    u64 PendingReadbackTick(VAddr address, u64 size, bool newest_only) const;

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// gow3: batched small copies on the copy threads (Gow3Copy::QueueCopy).
    static void RunGuestCopy(const Gow3Copy::Item& item);
    void SmallGuestCopy(const Gow3Copy::Item& item);

    const Buffer* UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                               size_t total_size_bytes);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    // gow3: texel buffer ranges known not to alias an image, per image registry generation.
    struct ImageMiss {
        VAddr address = 0;
        u32 size = 0;
        u64 generation = ~0ULL;
    };
    std::array<ImageMiss, 256> image_miss_cache{};
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;
    /// gow3: held exclusively while gpu_modified_ranges changes, shared by IsRangeGpuWrittenShared.
    mutable std::shared_mutex gpu_ranges_mutex;
    void GpuRangesAdd(VAddr address, u64 size) {
        std::unique_lock lock{gpu_ranges_mutex};
        gpu_modified_ranges.Add(address, size);
    }
    void GpuRangesSubtract(VAddr address, u64 size) {
        std::unique_lock lock{gpu_ranges_mutex};
        gpu_modified_ranges.Subtract(address, size);
    }

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;

    std::array<const Buffer*, MAX_ARENA_PAGES> address_space{};
    u64 arena_page_bits{MAX_ARENA_PAGE_BITS};
    u64 arena_page_size{u64{1} << MAX_ARENA_PAGE_BITS};
    u64 num_arena_pages{u64{1} << (ADDRESS_SPACE_BITS - MAX_ARENA_PAGE_BITS)};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    IntervalList<Backing> resident_ranges;
    std::unordered_map<u64, std::chrono::steady_clock::time_point> cpu_write_hot; ///< see IsCpuWriteHot
    /// gow3: page_writes and cpu_write_hot are swept every two seconds (SweepTracking).
    std::chrono::steady_clock::time_point last_tracking_sweep{};
    void SweepTracking();


    // gow3: a CPU write into a page with GPU-written bytes used to download the page with a full
    // GPU wait (~800 per 5 s in the U1 temple, half the frame time). The writes never hit the
    // GPU-written bytes themselves, so the page is unprotected without a download instead. The
    // guest copy of the GPU-written bytes stays stale and is recorded here; uploads leave those
    // bytes out and downloads fill them in. A guest byte that differs from its record was
    // written by the CPU after the GPU: the CPU value wins. GPU-side use, like
    // gpu_modified_ranges.
    static constexpr u64 LAZY_PAGE_SIZE = 4096;
    struct LazyPage {
        std::array<u8, LAZY_PAGE_SIZE> bytes{}; ///< guest bytes at the write fault
        std::bitset<LAZY_PAGE_SIZE> tracked;    ///< GPU-written bytes with a stale guest copy
        /// gow3: shared page (CPU writes next to GPU data still in flight): the tracked bytes
        /// are the GPU's, without comparing guest bytes (uploads skip them, readbacks write them).
        bool gpu_wins = false;
    };
    /// gow3: the GPU writes into each 4 KiB page (writes up to 1 MiB): byte range and tick.
    struct PageWrite {
        u16 lo, hi;
        u64 tick;
    };
    std::unordered_map<u64, boost::container::small_vector<PageWrite, 4>> page_writes;
    /// Scheduler tick when the last per-submission readback was recorded: GPU writes from then
    /// on come back with the next one.
    u64 last_readback_tick = 0;
    /// Every GPU byte in `page` is from work in flight, from this submission (the next
    /// readback brings it) or in a pending readback: the CPU can write next to it.
    bool GpuBytesInFlight(VAddr page);
    /// Pages shared with the CPU whose GPU bytes the next readback must bring back.
    std::vector<std::pair<VAddr, u64>> shared_readbacks;
    u64 g_shared_writes_count = 0;

public:
    /// gow3: the shared pages' GPU bytes for the next readback (Rasterizer::SyncSmallGpuWrites).
    std::vector<std::pair<VAddr, u64>> TakeSharedReadbacks() {
        return std::exchange(shared_readbacks, {});
    }

private:
    std::map<VAddr, std::unique_ptr<LazyPage>> lazy_pages;

    // gow3: DownloadRangesAsync. The per-submission readback of small GPU writes (the CPU-side
    // SRT walker reads them) waited for the whole frame on the GPU (~10 ms, 40% of the GPU
    // thread in the temple). Its copies now land in `download` and reach guest memory a
    // little later. The pieces stay in gpu_modified_ranges until then; `rewritten` holds GPU
    // writes recorded after the readback, which stay GPU-modified when it completes.
    struct PendingReadback {
        u64 tick = 0;
        Buffer* buffer = nullptr;
        u64 offset = 0;
        u64 size = 0;
        u8* mapped = nullptr;
        std::vector<std::pair<VAddr, u64>> pieces; ///< address, size
        std::vector<u64> offsets;                  ///< of each piece in `mapped`
        RangeSet rewritten;
        /// Pages to read-protect (merged ranges) until the data is back: a CPU read of them waits
        /// for this readback only (GOW3_ASYNC_READBACK=1; =2 leaves them readable, stale).
        std::vector<std::pair<VAddr, u64>> watched;
        bool watching = false; ///< `watched` is read-protected (from the first poll after submission)
        bool yield_to_cpu = false; ///< see DownloadRangesAsync
        /// gow3: run once the data is in guest memory (fence labels of ray-cast batches).
        std::vector<std::function<void()>> on_landed;
    };
    PageManager* page_manager = nullptr;
    std::deque<PendingReadback> pending_readbacks;
    /// gow3: pages where a CPU read faulted on other data than a pending readback's GPU bytes
    /// (false sharing: the CPU's own data next to small GPU results in the same 4 KiB) -> until
    /// when async readbacks leave them unprotected. Uncharted 2: 89% of the read faults, each one
    /// waiting ~14 ms for the GPU thread and the GPU.
    std::unordered_map<u64, std::chrono::steady_clock::time_point> noisy_pages;
    /// Stops read-protecting one page for every pending readback (a false-sharing read).
    void UnwatchPage(VAddr page);

public:
    /// gow3: runs `fn` (GPU thread) when the newest pending readback has landed in guest memory;
    /// false when no readback is pending. Ray-cast batches' end-of-pipe fences: the guest reads
    /// the results once it sees the fence, so the fence is written after them (as on the console,
    /// where the GPU writes both in order), not when the command is recorded.
    bool DeferUntilLastReadback(std::function<void()> fn, VAddr label, u64 value = 0, u32 bytes = 0);
    /// The readback a deferred fence at `address` (4 bytes) waits for. A GPU wait (WAIT_REG_MEM)
    /// on such a fence lands that readback first (LandReadbacks), as the GPU would execute the
    /// fence before the wait. Counting the value as written instead let the GPU thread run past
    /// waits the guest relies on (Uncharted 2 read a command buffer before it was written).
    [[nodiscard]] bool DeferredFenceTick(VAddr address, u64& tick, u32* value = nullptr) const;
    /// gow3 diagnosis: GPU data or a readback in flight in [address, address + size).
    [[nodiscard]] u64 PendingReadbackTickFor(VAddr address, u64 size) const {
        return PendingReadbackTick(address, size, false);
    }
    /// Waits for the readbacks up to `tick` and lands them (their deferred fences are written).
    void LandReadbacks(u64 tick);
    /// A fence label still waiting for a readback: later fences to it wait behind it.
    [[nodiscard]] bool IsLabelDeferred(VAddr label) const {
        return deferred_labels.contains(label);
    }
    [[nodiscard]] bool HasDeferredSignals() const {
        return deferred_signals.load(std::memory_order_acquire) != 0;
    }
    /// The newest pending readback is in the command buffer still being recorded: fences waiting
    /// for it would wait forever if the guest waits for them before submitting more work.
    [[nodiscard]] bool HasUnsubmittedReadback() const;
    /// Lands finished readbacks now (the GPU thread polls while idle when fences wait on them).
    void PollReadbacksNow();

private:
    std::unordered_multiset<VAddr> deferred_labels;
    struct DeferredValue {
        u64 value;
        u32 bytes;
        u64 tick;
    };
    std::unordered_map<VAddr, DeferredValue> deferred_values; ///< newest value per label
    /// Atomic: with multithreaded rendering the draw recording thread defers fences while the GPU
    /// command thread checks for them (it drains that thread before touching the rest).
    std::atomic<u32> deferred_signals{0};
    /// gow3: readback pieces that landed (address -> size, when): a piece read back again at the
    /// same address and size within RecurringWindow is a recurring GPU result (Uncharted's
    /// 16-byte luminance at a fixed address, read by the CPU every frame). Its previous value,
    /// one frame old as on the console where the GPU runs behind, stays readable: no read
    /// protection. New pieces (ring memory reused for other data) are protected, so a CPU read
    /// waits for them instead of seeing unrelated old bytes.
    std::unordered_map<VAddr, std::pair<u64, std::chrono::steady_clock::time_point>> landed_pieces;
    u32 readback_polls = 0;

    u32 arena_memory_type_index{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
