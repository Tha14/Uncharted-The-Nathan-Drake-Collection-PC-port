// dctools/texcodec.hpp: texture helpers of the latest converter (paktools-f525859/texcodec.py) in C++.
//
//  * PS3 RSX DXT textures in the paks are linear: row-major 4x4 blocks, the mips back to back without padding.
//  * PS4 textures are per mip padded to a multiple of 8x8 blocks (edge blocks repeated into the padding) and stored
//    as 8x8-block micro tiles in row-major tile order, the 64 elements of a tile in Morton order (x0 y0 x1 y1 x2 y2).
//  * PS3 A8R8G8B8 textures are swizzled (Morton, x bit first); the GCM remap word picks the output channels.
//  * decode(): BC1/BC2/BC3/A8R8G8B8 to RGBA, integer palettes; encode_bc3(): the converter's simple BC3 encoder
//    (bounding-box endpoints, nearest palette entry by float32 distance), bit-exact with the Python version.
#pragma once
#include "dc.hpp"

#include <map>
#include <optional>

namespace u3 {

struct PakTexture;

namespace tc {

struct Format {
    std::string kind;   // DXT1, DXT3, DXT5, A8R8G8B8
    int bpb;            // bytes per block (per texel when not compressed)
    bool compressed;
};

// DXGI format number (the PS4 VRAM_DESC format field) -> (name, bytes per block)
extern const std::map<uint32_t, std::pair<std::string, int>> DXGI;

std::optional<Format> ps3_format(const PakTexture &t);   // from the top byte of gcmRemapCtl
std::vector<std::pair<uint32_t, uint32_t>> mip_dims(uint32_t w, uint32_t h, uint32_t mips);
std::pair<uint32_t, uint32_t> blocks(uint32_t w, uint32_t h, bool compressed);
uint64_t ps3_linear_size(uint32_t w, uint32_t h, uint32_t mips, int bpb, bool compressed = true);
uint64_t ps4_tiled_size(uint32_t w, uint32_t h, uint32_t mips, int bpb, bool compressed = true);

Bytes tile_mip(const uint8_t *lin, size_t len, uint32_t bw, uint32_t bh, int bpb);   // linear blocks -> PS4 tiled (padded)
Bytes detile_mip(const uint8_t *data, size_t len, uint32_t bw, uint32_t bh, int bpb);
Bytes ps3_to_ps4_texels(const Bytes &data, uint32_t w, uint32_t h, uint32_t mips, int bpb, bool compressed = true);
Bytes ps4_mip0_linear(const Bytes &data, uint32_t w, uint32_t h, int bpb, bool compressed = true);

struct Img {   // RGBA, row-major
    uint32_t w = 0, h = 0;
    Bytes px;
    uint8_t *at(uint32_t x, uint32_t y) { return &px[(size_t(y) * w + x) * 4]; }
    const uint8_t *at(uint32_t x, uint32_t y) const { return &px[(size_t(y) * w + x) * 4]; }
};

Img decode(const Bytes &lin, uint32_t w, uint32_t h, const std::string &kind);   // DXT1/BC1, DXT3/BC2, DXT5/BC3, A8R8G8B8
Img ps3_deswizzle_argb8(const Bytes &data, uint32_t w, uint32_t h, uint32_t remap = 0xaae4);
void gcm_remap(const uint8_t argb[4], uint32_t remap, uint8_t rgba[4]);
Bytes encode_bc3(const Img &img);

}  // namespace tc
}  // namespace u3
