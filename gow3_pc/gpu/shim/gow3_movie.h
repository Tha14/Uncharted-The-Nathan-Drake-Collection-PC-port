#pragma once
// Movie frames at a patched render resolution. Uncharted's movie player reads the decoded frames
// texel for texel with the display size it renders at (luma W x H, chroma W/2 x H/2 after
// W * align(H, 64) bytes), assuming movies are as large as the screen; its movies are 1080p and
// its frame buffers 4 MiB each, so above 1080p they came out green and tiled. The decoder
// (shim/videodec2) also writes each frame scaled to the display size into a scratch frame of
// guest memory, and the texture cache (ImageInfo) reads the planes from there. Without a scratch
// frame the planes are read as decoded (right colors, 1:1 in a corner).
// GOW3_MOVIE_SIZE=<width>x<height> is that display size (tools/probe_uncharted.py); unset, nothing
// changes.

#include <cstdint>

namespace Gow3Movie {

// The display size movies are read with (GOW3_MOVIE_SIZE); false when unset.
bool DisplaySize(std::uint32_t& width, std::uint32_t& height);

// A decoded NV12 frame at base: luma width x height with pitch bytes per row, chroma after rows.
void RecordFrame(std::uint64_t base, std::uint32_t width, std::uint32_t height, std::uint32_t pitch,
                 std::uint32_t rows);

// The scratch frame for the game's frame buffer at base (mapped on first use): the display size
// in NV12, luma pitch ScaledPitch(), chroma after ScaledRows() rows. Null when none can be mapped.
std::uint8_t* ScratchFrame(std::uint64_t base);
std::uint32_t ScaledPitch();
std::uint32_t ScaledRows();

// An image over a recorded frame described with the display size: the plane in the scratch
// frame, else as decoded (address, size and pitch in elements). bits: bits per element (8 luma,
// 16 chroma). True when changed.
bool Remap(std::uint64_t& address, std::uint32_t& width, std::uint32_t& height, std::uint32_t& pitch,
           std::uint32_t bits);

} // namespace Gow3Movie
