// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "core/memory.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/host_shaders/buffer_multi_copy_comp.h"
#include "gow3_toggles.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Vulkan {

static constexpr u64 COPY_SHADER_HASH = 0xfefebf9f;

// gow3: the copy shader runs ~57 times per frame with ~1024 small ranges each. As one
// vkCmdCopyBuffer with that many regions it cost ~37 ns of GPU time per range (~2.1 ms per
// frame) plus the recording; buffer_multi_copy.comp copies all ranges in one dispatch.
static bool MultiCopy(Rasterizer& rasterizer, const VideoCore::Buffer* src,
                      const VideoCore::Buffer* dst, std::span<const vk::BufferCopy> copies) {
    if (copies.size() < 8 || Gow3Toggle::Disabled(Gow3Toggle::MultiCopyShader)) {
        return false;
    }
    u64 src_min = ~0ull, src_max = 0, dst_min = ~0ull, dst_max = 0;
    for (const auto& copy : copies) {
        if ((copy.srcOffset | copy.dstOffset | copy.size) & 3) {
            return false; // dword copies only
        }
        src_min = std::min(src_min, copy.srcOffset);
        src_max = std::max(src_max, copy.srcOffset + copy.size);
        dst_min = std::min(dst_min, copy.dstOffset);
        dst_max = std::max(dst_max, copy.dstOffset + copy.size);
    }
    auto& runtime = rasterizer.GetRuntime();
    auto& scheduler = runtime.GetScheduler();
    const auto& instance = runtime.GetInstance();
    const u64 align = instance.StorageMinAlignment();
    src_min = Common::AlignDown(src_min, align);
    dst_min = Common::AlignDown(dst_min, align);

    struct Pipeline {
        vk::UniqueDescriptorSetLayout set_layout;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    static Pipeline pipe = [&] {
        const auto device = instance.GetDevice();
        Pipeline p;
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        for (u32 i = 0; i < 3; ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = vk::DescriptorType::eStorageBuffer,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        p.set_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = sizeof(u32)};
        p.layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*p.set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto module = CompileSPV(BUFFER_MULTI_COPY_COMP, device);
        p.pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *p.layout,
                }));
        device.destroyShaderModule(module);
        return p;
    }();

    // The region table goes into the stream buffer (host writes, visible at submission).
    thread_local std::vector<u32> regions;
    regions.resize(copies.size() * 4);
    for (size_t i = 0; i < copies.size(); ++i) {
        regions[i * 4 + 0] = static_cast<u32>((copies[i].srcOffset - src_min) / 4);
        regions[i * 4 + 1] = static_cast<u32>((copies[i].dstOffset - dst_min) / 4);
        regions[i * 4 + 2] = static_cast<u32>(copies[i].size / 4);
        regions[i * 4 + 3] = 0;
    }
    auto& stream = rasterizer.GetBufferCache().GetStreamBuffer();
    const u64 table_size = regions.size() * sizeof(u32);
    const u64 table_offset = stream.Copy(regions.data(), table_size, align);

    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xC0B1ull, [] { return std::string{"copy shader HLE: barrier + dispatch"}; });
    }
    scheduler.EndRendering();
    const u64 src_size = src_max - src_min, dst_size = dst_max - dst_min;
    if (runtime.IsBufferAccessed(src, src_min, src_size) ||
        runtime.IsBufferAccessed(dst, dst_min, dst_size, true)) {
        runtime.FlushBarriers();
    }
    const u32 count = static_cast<u32>(copies.size());
    scheduler.Record([src = src->Handle(), dst = dst->Handle(), table = stream.Handle(), src_min,
                      src_size, dst_min, dst_size, table_offset, table_size,
                      count](vk::CommandBuffer cmdbuf) {
        const std::array<vk::DescriptorBufferInfo, 3> infos{{
            {src, src_min, src_size},
            {dst, dst_min, dst_size},
            {table, table_offset, table_size},
        }};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (u32 i = 0; i < 3; ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe.pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipe.layout, 0, writes);
        cmdbuf.pushConstants(*pipe.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(count),
                             &count);
        cmdbuf.dispatch(count, 1, 1);
    });
    runtime.AccessBuffer(src, src_min, src_size, vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderRead);
    runtime.AccessBuffer(dst, dst_min, dst_size, vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);
    return true;
}

static bool ExecuteCopyShaderHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer) {
    auto& runtime = rasterizer.GetRuntime();
    auto& buffer_cache = rasterizer.GetBufferCache();

    // Copy shader defines three formatted buffers as inputs: control, source, and destination.
    const auto ctl_buf_sharp = info.buffers[0].GetSharp(info);
    const auto src_buf_sharp = info.buffers[1].GetSharp(info);
    const auto dst_buf_sharp = info.buffers[2].GetSharp(info);
    const auto buf_stride = src_buf_sharp.GetStride();
    ASSERT(buf_stride == dst_buf_sharp.GetStride());

    struct CopyShaderControl {
        u32 dst_idx;
        u32 src_idx;
        u32 end;
    };
    static_assert(sizeof(CopyShaderControl) == 12);
    ASSERT(ctl_buf_sharp.GetStride() == sizeof(CopyShaderControl));
    const auto ctl_buf = reinterpret_cast<const CopyShaderControl*>(ctl_buf_sharp.base_address);

    static std::vector<vk::BufferCopy> copies;
    copies.clear();
    copies.reserve(cs_program.dim_x);

    for (u32 i = 0; i < cs_program.dim_x; i++) {
        const auto& [dst_idx, src_idx, end] = ctl_buf[i];
        const u32 local_dst_offset = dst_idx * buf_stride;
        const u32 local_src_offset = src_idx * buf_stride;
        const u32 local_size = (end + 1) * buf_stride;
        copies.emplace_back(local_src_offset, local_dst_offset, local_size);
    }

    // gow3: 64 KiB instead of 64 MiB. The copies are a few KiB spread over up to 57 MiB, and
    // each batch synchronizes (uploads, marks GPU-modified) its whole range: GPU time of the copy
    // shader 1.5 -> 0.6 ms/frame, GPU busy 85% -> 77%, frame rate no lower.
    // GOW3_COPY_MERGE_KB overrides it.
    static const vk::DeviceSize MaxDistanceForMerge = [] {
        const char* env = std::getenv("GOW3_COPY_MERGE_KB");
        return env ? vk::DeviceSize(std::strtoull(env, nullptr, 10)) * 1024 : vk::DeviceSize(64_KB);
    }();
    u32 batch_start = 0;
    u32 batch_end = 0;

    while (batch_end < copies.size()) {
        // Place first copy into the current batch
        const auto& copy = copies[batch_start];
        auto src_offset_min = copy.srcOffset;
        auto src_offset_max = copy.srcOffset + copy.size;
        auto dst_offset_min = copy.dstOffset;
        auto dst_offset_max = copy.dstOffset + copy.size;

        for (++batch_end; batch_end < copies.size(); batch_end++) {
            // Compute new src and dst bounds if we were to batch this copy
            const auto& [src_offset, dst_offset, size] = copies[batch_end];
            auto new_src_offset_min = std::min(src_offset_min, src_offset);
            auto new_src_offset_max = std::max(src_offset_max, src_offset + size);
            if (new_src_offset_max - new_src_offset_min > MaxDistanceForMerge) {
                break;
            }

            auto new_dst_offset_min = std::min(dst_offset_min, dst_offset);
            auto new_dst_offset_max = std::max(dst_offset_max, dst_offset + size);
            if (new_dst_offset_max - new_dst_offset_min > MaxDistanceForMerge) {
                break;
            }

            // We can batch this copy
            src_offset_min = new_src_offset_min;
            src_offset_max = new_src_offset_max;
            dst_offset_min = new_dst_offset_min;
            dst_offset_max = new_dst_offset_max;
        }

        // Obtain buffers for the total source and destination ranges.
        const auto [src_buf, src_buf_offset] = buffer_cache.ObtainBuffer(
            src_buf_sharp.base_address + src_offset_min, src_offset_max - src_offset_min, false);
        const auto [dst_buf, dst_buf_offset] = buffer_cache.ObtainBuffer(
            dst_buf_sharp.base_address + dst_offset_min, dst_offset_max - dst_offset_min, true);

        // Apply found buffer base.
        const auto vk_copies = std::span{copies}.subspan(batch_start, batch_end - batch_start);
        for (auto& copy : vk_copies) {
            copy.srcOffset = copy.srcOffset - src_offset_min + src_buf_offset;
            copy.dstOffset = copy.dstOffset - dst_offset_min + dst_buf_offset;
        }

        // Execute buffer copies.
        LOG_TRACE(Render_Vulkan, "HLE buffer copy: src_size = {}, dst_size = {}",
                  src_offset_max - src_offset_min, dst_offset_max - dst_offset_min);
        if (!MultiCopy(rasterizer, src_buf, dst_buf, vk_copies)) {
            runtime.CopyBuffer(src_buf, dst_buf, vk_copies);
        }
        batch_start = batch_end;
    }

    return true;
}

// gow3: Uncharted's constant copies. Disassembled (bin.psarc):
//   cs 2d215c3b: i = group                  (one dword per workgroup)
//   cs 6a6422a9: i = group * 64 + thread    (64 dwords per workgroup, U2)
//   v = buffer_load_format_x(s[0:3], i); if (i < s6) buffer_store_format_x(s[4:7], i, v)
// Loads past src.num_records read 0, stores past dst.num_records are dropped. Done here, the
// result is in guest memory at once (the CPU-side SRT walker reads these constant blocks) and in
// the GPU's copy in stream order (BufferCache::WriteThrough), and never becomes GPU data: the
// game's later CPU writes into the per-frame ring need no download. Before 15 Oct the count
// ignored s6 (elements past it were overwritten) and only 2d215c3b was handled.
// GOW3_SCALAR_COPY_GPU=1 keeps the GPU copy.
static constexpr u64 SCALAR_COPY_SHADER_HASH = 0x2d215c3b;
static constexpr u64 SCALAR_COPY64_SHADER_HASH = 0x6a6422a9;

static bool ExecuteScalarCopyHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer, u32 per_group) {
    static std::atomic<u32> on_cpu, on_gpu;
    // Why a copy stays on the GPU: the first two of each kind per shader (GOW3_FAULT_STATS).
    const auto to_gpu = [&](const char* reason, VAddr src_addr = 0, VAddr dst_addr = 0,
                            u64 size = 0) {
        static std::mutex mutex;
        static std::map<std::pair<u64, std::string>, u32> reports;
        ++on_gpu;
        std::scoped_lock lock{mutex};
        if (reports[{info.pgm_hash, reason}]++ < 2) {
            std::printf("Scalar copy %llx on the GPU: %s (%#llx -> %#llx, %llu bytes)\n",
                        static_cast<unsigned long long>(info.pgm_hash), reason,
                        static_cast<unsigned long long>(src_addr),
                        static_cast<unsigned long long>(dst_addr),
                        static_cast<unsigned long long>(size));
            std::fflush(stdout);
        }
        return false;
    };
    static const bool keep_gpu = std::getenv("GOW3_SCALAR_COPY_GPU") != nullptr;
    const auto ud = info.UserData();
    if (keep_gpu) {
        return false;
    }
    if (info.buffers.size() < 2 || ud.size() <= 6) {
        return to_gpu("bindings");
    }
    if (cs_program.dim_y != 1 || cs_program.dim_z != 1) {
        return to_gpu("2D dispatch");
    }
    if (per_group > 1 && cs_program.num_thread_x.full != per_group) {
        return to_gpu("workgroup size");
    }
    const auto src = info.buffers[0].GetSharp(info);
    const auto dst = info.buffers[1].GetSharp(info);
    const auto plain_dword = [](const AmdGpu::Buffer& buffer) {
        return buffer.data_format == u32(AmdGpu::DataFormat::Format32) && buffer.stride == 4 &&
               buffer.base_address != 0 && buffer.base_address % 4 == 0;
    };
    if (!plain_dword(src) || !plain_dword(dst)) {
        return to_gpu(!plain_dword(src) ? "source format/stride" : "destination format/stride",
                      src.base_address, dst.base_address);
    }
    const u64 count = std::min<u64>({u64(cs_program.dim_x) * per_group, ud[6], dst.num_records});
    if (count == 0) {
        return true;
    }
    const u64 readable = std::min<u64>(count, src.num_records);
    const VAddr src_addr = src.base_address;
    const VAddr dst_addr = dst.base_address;
    const u64 size = count * 4;
    auto& buffer_cache = rasterizer.GetBufferCache();
    // GPU-written source bytes: the GPU copies them in order (the SRT walker then downloads the
    // destination when it reads it, Rasterizer::SyncSrtTables). Surface copies (depth, MiBs)
    // stay on the GPU too.
    if (size > 64_KB) {
        return to_gpu("size", src_addr, dst_addr, size);
    }
    if (!rasterizer.IsMapped(dst_addr, size)) {
        return to_gpu("destination unmapped", src_addr, dst_addr, size);
    }
    if (readable && !rasterizer.IsMapped(src_addr, readable * 4)) {
        return to_gpu("source unmapped", src_addr, dst_addr, size);
    }
    if (readable && buffer_cache.IsRangeGpuWritten(src_addr, readable * 4)) {
        return to_gpu("GPU-written source", src_addr, dst_addr, size);
    }
    // Uploads recorded before this dispatch read guest memory on the copy threads.
    rasterizer.GetRuntime().GetScheduler().WaitHostCopies();
    std::vector<u32> values(count, 0u);
    if (readable) {
        auto* memory = Core::Memory::Instance();
        if (!memory->TryReadBacking(src_addr, values.data(), readable * 4)) {
            std::memcpy(values.data(), reinterpret_cast<const void*>(src_addr), readable * 4);
        }
    }
    if (!buffer_cache.WriteThrough(dst_addr, values.data(), size)) {
        return to_gpu("write-through refused", src_addr, dst_addr, size);
    }
    ++on_cpu;
    static const bool stats = std::getenv("GOW3_FAULT_STATS") != nullptr;
    if (stats) {
        static auto last = std::chrono::steady_clock::now();
        if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
            last = now;
            std::printf("Scalar copy HLE (5 s): %u on the CPU, %u on the GPU (GPU-written data)\n",
                        on_cpu.exchange(0), on_gpu.exchange(0));
            std::fflush(stdout);
        }
    }
    return true;
}

bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer) {
    switch (info.pgm_hash) {
    case COPY_SHADER_HASH:
        return ExecuteCopyShaderHLE(info, cs_program, rasterizer);
    case SCALAR_COPY_SHADER_HASH:
        return ExecuteScalarCopyHLE(info, cs_program, rasterizer, 1);
    case SCALAR_COPY64_SHADER_HASH:
        return ExecuteScalarCopyHLE(info, cs_program, rasterizer, 64);
    default:
        return false;
    }
}

} // namespace Vulkan
