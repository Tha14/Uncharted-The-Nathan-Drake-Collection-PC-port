// dctools/fgcoll.hpp: COLLISION_DATA_FOREGROUND, PS3 (32-bit BE) -> PS4 (64-bit LE); the actor toolset's fgcoll.py in C++.
//
// An ND wrapper around Havok packfiles (hkconvert.cpp) and ND collision meshes (collision.cpp -> hkpBvCompressedMesh
// packfiles). PS3 / PS4 offsets:
//   item     ver 0/0, nParts 4/4, parts** 8/8, nExtra c/10, extras** 10/18, flags 14/20, nCons 18/24, cons** 1c/28
//   part     xf f32[8] 0/0, joint 20/20, hk* 28/30
//   hk       size 0/0, type 4/4 (3 body, 5 breakable -> 2, 4), trailer* 10/18, u32 14/20, u32 18/28, f32 x2 1c/30,
//            mesh* | int id 24/38, packfile 30/40
//   trailer3 3 x {ptr, count} (elements 0x40 / 0x38 / 0x28), u32 18/30, sphere trees 1c..24 / 38..48
//   trailer5 (breakable) records of {trailer3*, u16 pair, u32 bitfield (bit order reversed)}
//   node     sphere f32[4], children** (inline after the node), u16 n, u16 tag, u32
//   con      size, type (1 -> 0), u32, u16 pair, packfile
//   mesh     PS3 ND mesh -> {mats*, packfile*, u32 size, u32 nmats}
#pragma once
#include "ndpak.hpp"
#include "ps4writer.hpp"

namespace u3::fgcoll {

std::shared_ptr<Blob> fg_blob(const Pak &p, const PakItem &it);

}  // namespace u3::fgcoll
