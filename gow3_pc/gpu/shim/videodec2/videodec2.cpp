// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/alignment.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define ASSERT(x) do { if (!(x)) { std::fputs("STOP: video decoder invariant failed\n",stderr); std::abort(); } } while (0)
#define UNREACHABLE() std::abort()


#include "videodec2.h"
#include "videodec2_impl.h"
#include "videodec_error.h"

namespace Libraries::Videodec2 {

// gow3: when a movie frame was last decoded (steady clock, ns). The Collection's cutscenes are
// prerecorded movies; with the frame rate unlocked the present thread gives the game 60 Hz
// vblanks while one plays (videoout/driver.cpp, after the reference Uncharted port).
std::atomic<s64> last_movie_frame_ns{0};

static constexpr u64 kMinimumMemorySize = 16_MB; ///> Fake minimum memory size for querying

static u64 ComputeFrameSizeBytes(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }

    const u32 aligned_width = Common::AlignUp<u32>((u32)width, 64);
    const u32 aligned_height = Common::AlignUp<u32>((u32)height, 16);

    const u64 pixels = (u64)aligned_width * (u64)aligned_height;
    return (pixels * 3) / 2;
}

static s32 ComputeDpbCount(const OrbisVideodec2DecoderConfigInfo& cfg) {
    if (cfg.max_dpb_frame_count > 0) {
        return cfg.max_dpb_frame_count;
    }

    return 8;
}

static void ComputeWorstCaseDimensions(const OrbisVideodec2DecoderConfigInfo& cfg, s32& out_width,
                                       s32& out_height) {
    if (cfg.max_frame_width > 0 && cfg.max_frame_height > 0) {
        out_width = cfg.max_frame_width;
        out_height = cfg.max_frame_height;
        return;
    }

    out_width = 1920;
    out_height = 1080;

    if (cfg.max_level >= 150) {
        out_width = 3840;
        out_height = 2160;
    }
}

s32 PS4_SYSV_ABI
sceVideodec2QueryComputeMemoryInfo(OrbisVideodec2ComputeMemoryInfo* compute_mem_info) {
    ((void)0);

    if (!compute_mem_info) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER;
    }
    if (compute_mem_info->this_size != sizeof(OrbisVideodec2ComputeMemoryInfo)) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE;
    }

    compute_mem_info->cpu_gpu_memory = nullptr;
    compute_mem_info->cpu_gpu_memory_size = kMinimumMemorySize;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI
sceVideodec2AllocateComputeQueue(const OrbisVideodec2ComputeConfigInfo* compute_cfg_info,
                                 const OrbisVideodec2ComputeMemoryInfo* compute_mem_info,
                                 OrbisVideodec2ComputeQueue* compute_queue) {
    ((void)0);
    if (!compute_cfg_info || !compute_mem_info || !compute_queue) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER;
    }
    if (compute_cfg_info->this_size != sizeof(OrbisVideodec2ComputeConfigInfo) ||
        compute_mem_info->this_size != sizeof(OrbisVideodec2ComputeMemoryInfo)) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE;
    }
    if (compute_cfg_info->reserved0 != 0 || compute_cfg_info->reserved1 != 0) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_CONFIG_INFO;
    }
    if (compute_cfg_info->compute_pipe_id > 4) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_COMPUTE_PIPE_ID;
    }
    if (compute_cfg_info->compute_queue_id > 7) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_COMPUTE_QUEUE_ID;
    }
    if (!compute_mem_info->cpu_gpu_memory) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_MEMORY_POINTER;
    }

    // The real library returns a pointer to memory inside cpuGpuMemory
    *compute_queue = compute_mem_info->cpu_gpu_memory;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideodec2ReleaseComputeQueue(OrbisVideodec2ComputeQueue compute_queue) {
    ((void)0);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI
sceVideodec2QueryDecoderMemoryInfo(const OrbisVideodec2DecoderConfigInfo* decoder_cfg_info,
                                   OrbisVideodec2DecoderMemoryInfo* decoder_mem_info) {
    ((void)0);

    if (!decoder_cfg_info || !decoder_mem_info) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER;
    }
    if (decoder_cfg_info->this_size != sizeof(OrbisVideodec2DecoderConfigInfo) ||
        decoder_mem_info->this_size != sizeof(OrbisVideodec2DecoderMemoryInfo)) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE;
    }

    s32 width = 0;
    s32 height = 0;
    ComputeWorstCaseDimensions(*decoder_cfg_info, width, height);

    const u64 frame_size = ComputeFrameSizeBytes(width, height);
    u64 max_frame_buffer = 0;
    if (frame_size == 0) {
        max_frame_buffer = kMinimumMemorySize;
    } else {
        max_frame_buffer = Common::AlignUp<u64>(frame_size, 256) + 0x4000;
    }

    decoder_mem_info->cpu_memory = nullptr;
    decoder_mem_info->gpu_memory = nullptr;
    decoder_mem_info->cpu_gpu_memory = nullptr;

    decoder_mem_info->cpu_gpu_memory_size = kMinimumMemorySize;
    decoder_mem_info->cpu_memory_size = kMinimumMemorySize;
    decoder_mem_info->gpu_memory_size = kMinimumMemorySize;

    decoder_mem_info->max_frame_buffer_size = max_frame_buffer;
    decoder_mem_info->frame_buffer_alignment = 0x100;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideodec2CreateDecoder(const OrbisVideodec2DecoderConfigInfo* decoder_cfg_info,
                                           const OrbisVideodec2DecoderMemoryInfo* decoder_mem_info,
                                           OrbisVideodec2Decoder* decoder) {
    ((void)0);

    if (!decoder_cfg_info || !decoder_mem_info || !decoder) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER;
    }
    if (decoder_cfg_info->this_size != sizeof(OrbisVideodec2DecoderConfigInfo) ||
        decoder_mem_info->this_size != sizeof(OrbisVideodec2DecoderMemoryInfo)) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE;
    }

    if (decoder_cfg_info->codec_type!=OrbisVideodec2CodecType::Avc && decoder_cfg_info->codec_type!=OrbisVideodec2CodecType::Hevc) return ORBIS_VIDEODEC2_ERROR_CODEC_TYPE;
    std::puts("Runtime: Videodec2 FFmpeg decoder created");
    *decoder = new VdecDecoder(*decoder_cfg_info, *decoder_mem_info);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideodec2DeleteDecoder(OrbisVideodec2Decoder decoder) {
    ((void)0);

    if (!decoder) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_DECODER_INSTANCE;
    }

    delete decoder;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideodec2Decode(OrbisVideodec2Decoder decoder,
                                    const OrbisVideodec2InputData* input_data,
                                    OrbisVideodec2FrameBuffer* frame_buffer,
                                    OrbisVideodec2OutputInfo* output_info) {
    ((void)0);

    if (!decoder) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_DECODER_INSTANCE;
    }
    if (!input_data || !frame_buffer || !output_info) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER;
    }
    if (input_data->this_size != sizeof(OrbisVideodec2InputData) ||
        frame_buffer->this_size != sizeof(OrbisVideodec2FrameBuffer) ||
        (output_info->this_size | 8) != sizeof(OrbisVideodec2OutputInfo)) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE;
    }

    last_movie_frame_ns.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                              std::memory_order_relaxed);
    return decoder->Decode(*input_data, *frame_buffer, *output_info);
}

s32 PS4_SYSV_ABI sceVideodec2Flush(OrbisVideodec2Decoder decoder,
                                   OrbisVideodec2FrameBuffer* frame_buffer,
                                   OrbisVideodec2OutputInfo* output_info) {
    ((void)0);

    if (!decoder) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_DECODER_INSTANCE;
    }
    if (!frame_buffer || !output_info) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER;
    }
    if (frame_buffer->this_size != sizeof(OrbisVideodec2FrameBuffer) ||
        (output_info->this_size | 8) != sizeof(OrbisVideodec2OutputInfo)) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE;
    }

    return decoder->Flush(*frame_buffer, *output_info);
}

s32 PS4_SYSV_ABI sceVideodec2Reset(OrbisVideodec2Decoder decoder) {
    ((void)0);

    if (!decoder) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_DECODER_INSTANCE;
    }

    return decoder->Reset();
}

s32 PS4_SYSV_ABI sceVideodec2GetPictureInfo(const OrbisVideodec2OutputInfo* output_info,
                                            void* p_1st_picture_info, void* p_2nd_picture_info) {
    ((void)0);

    if (!output_info) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER;
    }
    if ((output_info->this_size | 8) != sizeof(OrbisVideodec2OutputInfo)) {
        ((void)0);
        return ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE;
    }
    if (output_info->picture_count == 0) {
        ((void)0);
        return ORBIS_OK;
    }

    if (p_1st_picture_info) {
        auto size = *reinterpret_cast<u64*>(p_1st_picture_info);
        const u64 maximum=output_info->codec_type==OrbisVideodec2CodecType::Avc ? sizeof(OrbisVideodec2AvcPictureInfo) : sizeof(OrbisVideodec2HevcPictureInfo);
        if (size<8 || size>maximum) return ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE;
        if (!output_info->frame_buffer) return ORBIS_VIDEODEC2_ERROR_FRAME_BUFFER_POINTER;
        auto* pictureInfo = (u8*)output_info->frame_buffer + output_info->frame_buffer_size;
        // Copy the requested picture data to the output omitting picture size field.
        memcpy((u8*)p_1st_picture_info + sizeof(u64), pictureInfo + sizeof(u64),
               size - sizeof(u64));
    }

    if (output_info->picture_count > 1) {
        UNREACHABLE();
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideodec2GetAvcPictureInfo(const OrbisVideodec2OutputInfo* output_info,
                                               void* p_1st_picture_info, void* p_2nd_picture_info) {
    ((void)0);
    return sceVideodec2GetPictureInfo(output_info, p_1st_picture_info, p_2nd_picture_info);
}

s32 PS4_SYSV_ABI sceVideodec2GetHevcPictureInfo(const OrbisVideodec2OutputInfo* output_info,
                                                OrbisVideodec2HevcPictureInfo* picture_info) {
    ((void)0);
    return sceVideodec2GetPictureInfo(output_info, picture_info, nullptr);
}

} // namespace Libraries::Videodec2

extern "C" uintptr_t runtime_videodec2_resolve(const char* name) {
    using namespace Libraries::Videodec2;
    if (!std::strcmp(name,"RnDibcGCPKw#libSceVideodec2")) return (uintptr_t)sceVideodec2QueryComputeMemoryInfo;
    if (!std::strcmp(name,"eD+X2SmxUt4#libSceVideodec2")) return (uintptr_t)sceVideodec2AllocateComputeQueue;
    if (!std::strcmp(name,"UvtA3FAiF4Y#libSceVideodec2")) return (uintptr_t)sceVideodec2ReleaseComputeQueue;
    if (!std::strcmp(name,"qqMCwlULR+E#libSceVideodec2")) return (uintptr_t)sceVideodec2QueryDecoderMemoryInfo;
    if (!std::strcmp(name,"CNNRoRYd8XI#libSceVideodec2")) return (uintptr_t)sceVideodec2CreateDecoder;
    if (!std::strcmp(name,"jwImxXRGSKA#libSceVideodec2")) return (uintptr_t)sceVideodec2DeleteDecoder;
    if (!std::strcmp(name,"852F5+q6+iM#libSceVideodec2")) return (uintptr_t)sceVideodec2Decode;
    if (!std::strcmp(name,"l1hXwscLuCY#libSceVideodec2")) return (uintptr_t)sceVideodec2Flush;
    if (!std::strcmp(name,"wJXikG6QFN8#libSceVideodec2")) return (uintptr_t)sceVideodec2Reset;
    if (!std::strcmp(name,"NtXRa3dRzU0#libSceVideodec2")) return (uintptr_t)sceVideodec2GetPictureInfo;
    if (!std::strcmp(name,"kjrLbcyhEiw#libSceVideodec2")) return (uintptr_t)sceVideodec2GetAvcPictureInfo;
    if (!std::strcmp(name,"7M+1UFqWOAI#libSceVideodec2")) return (uintptr_t)sceVideodec2GetHevcPictureInfo;
    return 0;
}
