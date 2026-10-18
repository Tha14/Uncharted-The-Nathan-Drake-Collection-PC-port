#pragma once
// Keyboard and mouse button prompts (gow3_prompts.cpp).

#include <memory>
#include <vector>

#include "common/types.h"

namespace Gow3Prompts {

// Texels to upload instead of an image's guest data: linear, per mip its offset, row length and
// height in texels (BC: whole blocks).
struct Texels {
    std::vector<u8> data;
    struct Mip {
        u32 offset, row, height;
    };
    std::vector<Mip> mips;
};

// Texture cache upload (TextureCache::RefreshImage) of a 512x512 RGBA8 image or a BC1 one (64-512):
// when its first mip is one of the games' button icon atlases or button pictures and other icons
// than the PlayStation ones show, the texels to upload instead; nullptr otherwise.
std::shared_ptr<const Texels> Replacement(VAddr address, u32 guest_size, u32 mip0_size, u32 width, u32 height,
                                          u32 levels, bool bc1, u32 tile_mode);

} // namespace Gow3Prompts

// Pad side (runtime_pad.c): the labels per PS4 button (GOW3_PROMPT_* in gow3gpu.h) of set 0 (keys
// and mouse buttons) or 1 (other controllers), and the device whose icons show.
extern "C" void gow3gpu_prompts_set_labels(int set, const char* const* labels, int count);
extern "C" void gow3gpu_prompts_set_device(int device);
