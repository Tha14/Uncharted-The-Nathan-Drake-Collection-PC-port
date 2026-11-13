// dctools/collision.hpp: PS3 -> PS4 background collision of Uncharted 3 levels (COLLISION_DATA_HAVOK_BG), the latest
// converter's collision.py in C++.
//
// PS3 (ND mesh, 32-bit pointers): item +00 u32 version, +0c u32 count, +20..+24 flags, +30 ptr u32[count], +40 ptr
//   mesh {f32 aabb[6], u32 ?, u32 chunks, u32 groups, u32 materials, +28 groups, +2c group boxes, +30 chunk boxes, +34
//   chunk records {ptr data, u32 0, u16 indices, u16 vertices, u32 material}, +38 chunk quantisation boxes, +3c materials
//   {u32 0, u32 bf16 friction, u32 flags}}; chunk data = indices (21 x 6 bits MSB first per 16 bytes), vertices (3 x u16
//   BE over the chunk box), one u16 Havok welding code per triangle.
// PS4: a Havok 2011.3 64-bit packfile whose root object is an hkpBvCompressedMeshShape: sections of <= 127 triangles /
//   255 vertices (median splits along the longest centroid axis), each with a 4-byte-node tree, 11/11/10-bit packed and
//   21/21/22-bit shared vertices, per-triangle data runs {filter info | user data << 8 | welding << 16}; a 5-byte-node
//   tree over the sections. The filter info comes from the ND material flags (layer 2 water, 13 for 0x400, else 1).
#pragma once
#include "ndpak.hpp"
#include "ps4writer.hpp"

namespace u3 {

// one PS4 item blob per PS3 COLLISION_DATA_HAVOK_BG item that has a mesh, in PS3 order
// sky: the sky converter's version, which rotates degenerate triangles so no primitive reads as a Havok CUSTOM one
std::vector<std::shared_ptr<Blob>> havok_bg_blobs(const Pak &p, bool sky = false);

// an FG prototype's ND collision mesh (decode_mesh + build_from_ps3): the packfile and the materials (friction, flags)
struct FgMesh {
    Bytes pf;
    std::vector<std::pair<float, uint32_t>> mats;
};
FgMesh fg_mesh(const Pak &p, uint32_t pg, uint64_t off);

}  // namespace u3
