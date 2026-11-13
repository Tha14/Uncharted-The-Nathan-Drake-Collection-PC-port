// dctools/visinfo.hpp: VIS_INFO_1 (occlusion / PVS) of Uncharted 3 levels, PS3 (32-bit BE) -> PS4 (64-bit LE); the
// latest converter's visinfo.py in C++ (its default: the PVS bit streams are kept in the PS3 word order, as the
// retail PS4 paks do).
//
// PS3 header 0xa8: +00 version 9, +04 nRegions, +08 regions* (0x30), +0c u32, +10/+30/+50/+70 4 x {ptr bits, u32 x7},
//   +90 nDict, +94 dict*, +98 nG, +9c G*, +a0 H*, +a4 nH.
// PS4 header 0xe0: +00 9, nRegions, +08 regions* (0x40), +10 u32, 0, +18/+40/+68/+90 4 x {ptr, u32 x7} (+ u32 0 between),
//   +b4 nDict, +b8 dict*, +c0 nG, aa, +c8 G*, +d0 H*, +d8 nH, aa.
// Region {nPlanes, planes*, nodes*, aabb f32[6], nFaces, faces*, x} -> {planes*, nodes*, faces*, aabb, nPlanes, nFaces,
//   x, aa}; node {u32 a, i32 b, u32 split, bits*} -> {a, b, split, aa, bits*} (a leaf has a bits pointer; a = left << 2 |
//   axis, b = right); face {nVerts, verts*} -> {nVerts, aa, verts*}; dict = u16 pairs (Huffman tree, root last; < 0x100
//   a byte, 0x100 the end, 0x101 + i entry i); leaf streams keep exactly the bytes up to the end code; G {u32, bits*} ->
//   {u32, aa, bits*}; H {sid, region*} -> {sid, aa, null}. Words, planes, AABBs and vertices are byte-swapped per u32.
// Objects in the retail order (header, regions, H, per region planes, nodes, leaf streams, faces, vertices; then the 4
// buffers, dict, G, G bits), 16-aligned with 0xaa fill; above 1 MB the rest goes into extra blobs (RAW_DATA items).
#pragma once
#include "ndpak.hpp"
#include "ps4writer.hpp"

namespace u3 {

// [item blob, extra blobs...] for the first VIS_INFO_1 of a PS3 pak (empty when it has none)
std::vector<std::shared_ptr<Blob>> vis_blobs(const Pak &p, size_t limit = 0x100000);

}  // namespace u3
