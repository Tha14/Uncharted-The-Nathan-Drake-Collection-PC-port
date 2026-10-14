// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/amdgpu/regs_texture.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

#include <atomic>

namespace VideoCore {

/// gow3 diagnostic (set live through GOW3_DMA_TRIGGER: "cmp normal|never|always"): replaces the
/// depth compare of compare samplers (shadow lookups) to test whether a lookup decides a look.
inline std::atomic<int> sampler_compare_override{0};

class Sampler {
public:
    explicit Sampler(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                     const AmdGpu::BorderColorBuffer border_color_base, bool is_depth,
                     float extra_lod_bias = 0.0f);
    ~Sampler();

    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    Sampler(Sampler&&) = default;
    Sampler& operator=(Sampler&&) = default;

    vk::Sampler Handle() const noexcept {
        return *handle;
    }

    size_t lru_id{};

private:
    vk::UniqueSampler handle;
};

} // namespace VideoCore
