// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/sampler.h"

namespace VideoCore {

Sampler::Sampler(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                 const AmdGpu::BorderColorBuffer border_color_base, const bool is_depth, float extra_lod_bias) {
    using namespace Vulkan;
    const bool anisotropy_enable = instance.IsAnisotropicFilteringSupported() &&
                                   (AmdGpu::IsAnisoFilter(sampler.xy_mag_filter) ||
                                    AmdGpu::IsAnisoFilter(sampler.xy_min_filter));
    const float max_anisotropy =
        anisotropy_enable ? std::clamp(sampler.MaxAniso(), 1.0f, instance.MaxSamplerAnisotropy())
                          : 1.0f;
    auto border_color = LiverpoolToVK::BorderColor(sampler.border_color_type);
    if (border_color == vk::BorderColor::eFloatCustomEXT &&
        !instance.IsCustomBorderColorSupported()) {
        LOG_WARNING(Render_Vulkan, "Custom border color is not supported, falling back to black");
        border_color = vk::BorderColor::eFloatOpaqueBlack;
    }

    const auto custom_color = [&]() -> std::optional<vk::SamplerCustomBorderColorCreateInfoEXT> {
        if (border_color == vk::BorderColor::eFloatCustomEXT) {
            const auto border_color_index = sampler.border_color_ptr.Value();
            const auto border_color_buffer = border_color_base.Address<std::array<float, 4>*>();
            const auto custom_border_color_array = border_color_buffer[border_color_index];

            const vk::SamplerCustomBorderColorCreateInfoEXT ret{
                .customBorderColor =
                    vk::ClearColorValue{
                        .float32 = custom_border_color_array,
                    },
                .format = vk::Format::eR32G32B32A32Sfloat,
            };
            return ret;
        } else {
            return std::nullopt;
        }
    }();

    if (is_depth) {
        // gow3: each distinct shadow-lookup sampler setup (compare, clamp, border), once.
        static std::mutex mutex;
        static std::vector<u64> seen;
        const u64 key = u64(sampler.depth_compare_func.Value()) | (u64(sampler.clamp_x.Value()) << 4) |
                        (u64(sampler.clamp_y.Value()) << 8) | (u64(sampler.clamp_z.Value()) << 12) |
                        (u64(sampler.border_color_type.Value()) << 16) |
                        (u64(sampler.border_color_ptr.Value()) << 20);
        std::scoped_lock lock{mutex};
        if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            std::printf("GPU: compare sampler: depth compare %u, clamp %u/%u/%u, border type %u ptr %u, "
                        "filter %u/%u mip %u\n",
                        u32(sampler.depth_compare_func.Value()), u32(sampler.clamp_x.Value()),
                        u32(sampler.clamp_y.Value()), u32(sampler.clamp_z.Value()),
                        u32(sampler.border_color_type.Value()), u32(sampler.border_color_ptr.Value()),
                        u32(sampler.xy_mag_filter.Value()), u32(sampler.xy_min_filter.Value()),
                        u32(sampler.mip_filter.Value()));
            std::fflush(stdout);
        }
    }
    // gow3 diagnostic overrides for compare samplers (sampler.h): 3 = clamp to an opaque black
    // border, 4 = clamp to a white border, 5 = clamp to the edge texel.
    const int override_mode = is_depth ? sampler_compare_override.load() : 0;
    if (override_mode == 3 || override_mode == 4) {
        border_color = override_mode == 3 ? vk::BorderColor::eFloatOpaqueBlack : vk::BorderColor::eFloatOpaqueWhite;
    }
    const auto address = [&](AmdGpu::ClampMode mode) {
        if (override_mode == 3 || override_mode == 4) {
            return vk::SamplerAddressMode::eClampToBorder;
        }
        if (override_mode == 5) {
            return vk::SamplerAddressMode::eClampToEdge;
        }
        return LiverpoolToVK::ClampMode(mode);
    };
    const vk::SamplerCreateInfo sampler_ci = {
        .pNext = custom_color && border_color == vk::BorderColor::eFloatCustomEXT ? &*custom_color : nullptr,
        .magFilter = LiverpoolToVK::Filter(sampler.xy_mag_filter),
        .minFilter = LiverpoolToVK::Filter(sampler.xy_min_filter),
        .mipmapMode = LiverpoolToVK::MipFilter(sampler.mip_filter),
        .addressModeU = address(sampler.clamp_x),
        .addressModeV = address(sampler.clamp_y),
        .addressModeW = address(sampler.clamp_z),
        .mipLodBias = std::clamp(sampler.LodBias() + extra_lod_bias, -instance.MaxSamplerLodBias(),
                                 instance.MaxSamplerLodBias()),
        .anisotropyEnable = anisotropy_enable,
        .maxAnisotropy = max_anisotropy,
        // GCN compares per instruction; a plain read of a compare sampler is undefined in Vulkan.
        .compareEnable = is_depth,
        .compareOp = sampler_compare_override == 1   ? vk::CompareOp::eNever
                     : sampler_compare_override == 2 ? vk::CompareOp::eAlways
                                                      : LiverpoolToVK::DepthCompare(sampler.depth_compare_func),
        .minLod = sampler.MinLod(),
        .maxLod = sampler.MaxLod(),
        .borderColor = border_color,
        .unnormalizedCoordinates = false, // Handled in shader due to Vulkan limitations.
    };
    auto [sampler_result, smplr] = instance.GetDevice().createSamplerUnique(sampler_ci);
    ASSERT_MSG(sampler_result == vk::Result::eSuccess, "Failed to create sampler: {}",
               vk::to_string(sampler_result));
    handle = std::move(smplr);
}

Sampler::~Sampler() = default;

} // namespace VideoCore
