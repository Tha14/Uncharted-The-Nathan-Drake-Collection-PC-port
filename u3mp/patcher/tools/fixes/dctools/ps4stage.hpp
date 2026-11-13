// dctools/ps4stage.hpp: PS3 -> PS4 converter of Uncharted 3 background (stage) level paks: the latest converter's
// ps4stage.py with mkplan / fxextract (stageplan.cpp), fxremap.py, waterdata.py, collision.py (collision.cpp),
// visinfo.py (visinfo.cpp) and, for the 'mb' rows, paramfix.py and cubefix.py, in C++.
//
// The PS3 PM_COLLECTION_2 / BACKGROUND_SECTION / MATERIAL_TABLE_1 / ... items have no PS4 counterpart: the PS4 build
// keeps everything in one background GEOMETRY_1 (version 0x10):
//   GEOMETRY_1  +0x08 n/ptr submeshes (0x88), +0x18 prototypes*, +0x28 instances*, +0x38 textures (0x28), +0x48 materials
//   submesh     one per run of <= 20 PS3 blocks of a material group ('<stage>.Prototype<p>.Submesh<first block>');
//               streams in PS3 attribute order (pos f32x3, uv f16x2, colour / normal / tangent f16x4), streams2 = the
//               defaults of the per-instance attributes (ilight f16x4, lightmap uv f16x2, iconst f32x3)
//   prototype   0x78 {3, nsub, 0, hash, name, subname table, flags[16], 0, cb3, cb4, ..., +0x48 fade distances*}
//   instance    0xc0 {3, id, proto name, -1, 0, flags, 1, nsub, njoints, 0, instsub*, joints*, ..., AABB, matrix}
//   instsub     0x50 {submesh name, -1, 0, nstreams, 0, streams*, flags, flags2, 0x00ff0000, -1, AABB}
// Materials, effects and textures come from retail PS4 paks (stageplan.cpp); EFFECT string indices are remapped into
// the new pak's string table. Textures only on PS3 are converted (DXT re-tiled, A8R8G8B8 / remapped DXT re-encoded
// BC3, cube maps in the retail layout); the PS3 lightmaps become the stage's resident lightmaps.
// Uniform vertex streams (every vertex equal) share one buffer per value, sized by a first pass over the stage.
#pragma once
#include "json.h"   // launcher/native/json.h
#include "stageplan.hpp"

namespace u3::stage {

struct Options {
    fs::path res, pak_dir, texdict, plan_dir;
    bool mapbuild = false;                                     // 'mb' rows: paramfix + cubefix
    const json::Value *gobo = nullptr;                         // {retail pak: {PS3 texture hash hex: PS4 short path}}
    std::shared_ptr<std::vector<BundleTex>> default_lightmaps;   // the two retail default lightmaps
};
Bytes stage_convert(const fs::path &ps3, const Options &o);

// the cube texels of a PS3 cube map in the retail PS4 layout (mip-major, 8 slots per mip, PS4 tiled); none when the
// PS3 data is short
std::optional<Bytes> cube_texels(const PakTexture &t3, const Bytes &data, int bpb);
std::optional<Bytes> cube_texels_argb(const PakTexture &t3, const Bytes &data, uint32_t remap);

}  // namespace u3::stage
