// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "gow3_copy.h"

#include <array>
#include <atomic>
#include <bitset>
#include <deque>
#include <map>
#include <memory>
#include <unordered_set>
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
    [[nodiscard]] bool IsCpuWriteHot(VAddr address, u64 size) const {
        for (u64 window = address >> 20; window <= (address + size - 1) >> 20; ++window) {
            if (cpu_write_hot.contains(window)) {
                return true;
            }
        }
        return false;
    }

    /// gow3: copies the GPU-modified parts of the ranges back to guest memory with one GPU wait
    /// (GPU thread). For data the GPU writes and CPU-side code (the SRT walker) reads.
    void DownloadRanges(std::span<const std::pair<VAddr, u64>> ranges);

    /// gow3: GPU-written data newer than guest memory lies in [address, address + size).
    [[nodiscard]] bool IsRangeGpuWritten(VAddr address, u64 size) const {
        return gpu_modified_ranges.Intersects(address, size);
    }

    /// gow3: the GPU thread is about to write [address, address + size) in guest memory itself
    /// (shader HLE). Unprotects it like a guest write fault would, without a download. False
    /// when GPU-written bytes are in the range: the GPU must do that write.
    bool PrepareHostWrite(VAddr address, u64 size);

    /// gow3: DownloadRanges without the GPU wait. The copies run after the work already
    /// recorded; PollReadbacks writes them to guest memory once the GPU has run them. Until
    /// then the ranges stay GPU-modified (a CPU write into them waits for its readback).
    void DownloadRangesAsync(std::span<const std::pair<VAddr, u64>> ranges);

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
    std::unordered_set<u64> cpu_write_hot; ///< see IsCpuWriteHot

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
    };
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
    };
    PageManager* page_manager = nullptr;
    std::deque<PendingReadback> pending_readbacks;
    u32 readback_polls = 0;

    u32 arena_memory_type_index{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
