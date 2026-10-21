// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <vector>
#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {

// gow3 21 Oct 2026, diagnostics (GOW3_INVARIANT_POSITION=1, off by default): the games draw
// depth first and then shade with depth test EQUAL, which needs every vertex shader to compute
// the same position bit for bit. SPIR-V promises that only for outputs decorated Invariant (DXVK
// decorates all D3D positions). Adds the decoration to every output variable that is the Position
// built-in, also in SPIR-V cached by earlier builds. It did not change the Uncharted 3 museum's
// red floor (a cached binary paired with another permutation's description, see
// vk_pipeline_serialization.cpp BinaryMatchesMeta), so it stays opt-in.
constexpr u32 OpVariable = 59;
constexpr u32 OpDecorate = 71;
constexpr u32 DecorationBuiltIn = 11;
constexpr u32 DecorationInvariant = 18;
constexpr u32 BuiltInPosition = 0;
constexpr u32 StorageClassOutput = 3;

bool InvariantPositionEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_INVARIANT_POSITION");
        const bool on = env && env[0] == '1';
        if (on) {
            std::printf("GPU: vertex positions computed invariantly (GOW3_INVARIANT_POSITION=1)\n");
            std::fflush(stdout);
        }
        return on;
    }();
    return enabled;
}

// Returns the code with the decoration added, or an empty vector when nothing needs it.
std::vector<u32> WithInvariantPosition(std::span<const u32> code) {
    constexpr size_t HeaderWords = 5;
    if (code.size() <= HeaderWords || code[0] != 0x07230203u) {
        return {};
    }
    std::unordered_set<u32> outputs, invariant, positions;
    for (size_t at = HeaderWords; at < code.size();) {
        const u32 words = code[at] >> 16;
        const u32 opcode = code[at] & 0xffffu;
        if (words == 0 || at + words > code.size()) {
            return {};
        }
        if (opcode == OpVariable && words >= 4 && code[at + 3] == StorageClassOutput) {
            outputs.insert(code[at + 2]);
        } else if (opcode == OpDecorate && words >= 3) {
            if (code[at + 2] == DecorationInvariant) {
                invariant.insert(code[at + 1]);
            } else if (words >= 4 && code[at + 2] == DecorationBuiltIn &&
                       code[at + 3] == BuiltInPosition) {
                positions.insert(code[at + 1]);
            }
        }
        at += words;
    }
    std::vector<u32> patched;
    for (size_t at = HeaderWords; at < code.size(); at += code[at] >> 16) {
        const u32 words = code[at] >> 16;
        if ((code[at] & 0xffffu) != OpDecorate || words < 4 || code[at + 2] != DecorationBuiltIn ||
            code[at + 3] != BuiltInPosition) {
            continue;
        }
        const u32 id = code[at + 1];
        if (!outputs.contains(id) || invariant.contains(id)) {
            continue;
        }
        if (patched.empty()) {
            patched.assign(code.begin(), code.end());
        }
        const size_t insert_at = at + words + (patched.size() - code.size());
        const u32 decoration[] = {(3u << 16) | OpDecorate, id, DecorationInvariant};
        patched.insert(patched.begin() + insert_at, std::begin(decoration), std::end(decoration));
    }
    return patched;
}

} // Anonymous namespace

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {
    std::vector<u32> patched;
    if (InvariantPositionEnabled()) {
        patched = WithInvariantPosition(code);
        if (!patched.empty()) {
            code = patched;
        }
    }
    const vk::ShaderModuleCreateInfo shader_info = {
        .codeSize = code.size() * sizeof(u32),
        .pCode = code.data(),
    };

    auto [module_result, module] = device.createShaderModule(shader_info);
    ASSERT_MSG(module_result == vk::Result::eSuccess, "Failed to compile SPIR-V shader: {}",
               vk::to_string(module_result));
    return module;
}

} // namespace Vulkan
