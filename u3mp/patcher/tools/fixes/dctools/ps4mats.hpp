// dctools/ps4mats.hpp: PS4 packages that are only materials and textures; the actor toolset's convert_net.py and
// wpnmods.py in C++.
//
// convert_net: a PS3 UI texture / material package (net.pak: MATERIAL_TABLE_1 + VRAM_DESC only) as a PS4 package.
//   Page 0: TEXTURE_TABLE (sorted by hash), a RAW_DATA name + VRAM_DESC per texture, the EFFECT_TABLE and its EFFECTs
//   copied from the PS4 menu.pak (same page offset modulo 0x1000), and a GEOMETRY_1 (version 0x10) without meshes:
//   one texture entry per texture and one material per PS3 material (the PS3 name, so the menu's string id lookups
//   match; shader and effect of menu.pak's 'menu-logo' material; g_tColorMap / g_tTransparencyMap bound to the PS3
//   textures). Page 1: the login table (EFFECT_TABLE, TEXTURE_TABLE, GEOMETRY_1) and the VRAM desc table. VRAM: the PS3
//   linear DXT texels in the PS4 tiled layout. Colour maps are sRGB unless a material uses them as transparency.
//
// wpnmods: weapon skins the MP unlock table sells but a retail PS4 weapon pak lacks, appended as alias materials: a
//   copy of a retail sibling skin (same shader, features, parameters, effect) named '<dir>:<skin>,<md5 guid>,<shader>'
//   whose colour bindings point at the PS3 skin's colour texture. Retail pages 0 and 1 are kept byte for byte apart
//   from the GEOMETRY_1 / TEXTURE_TABLE / EFFECT_TABLE counts and arrays, which move to a new page 2 (retail entries
//   first); the VRAM desc table is rebuilt at the end of page 1; new textures follow the retail VRAM (top mips cut to
//   max_tex).
#pragma once
#include "ndpak.hpp"

namespace u3::mats {

// a PS3 net material: its name and texture slots (an index into the texture list, or none); a texture: hash and name
struct NetMat {
    std::string name;
    std::vector<std::optional<uint32_t>> textures;
};
struct NetTex {
    uint32_t hash;
    std::string name;
};
// convert_net options; the defaults are the Uncharted 3 net.pak conversion. The Uncharted 2 menu images: the materials
// and textures given (its PS3 material table differs), the effect of a named reference material, one effect hash per
// material, colour maps UNORM.
struct NetOptions {
    std::string effect_material = "general/menu/menu-logo:naughtydog";
    bool srgb_color = true;
    std::map<std::string, std::string> material_effect;   // material name -> effect hash (else the effect material's)
    const std::vector<NetMat> *mats = nullptr;            // given: used instead of reading the PS3 material table
    const std::vector<NetTex> *mtex = nullptr;
    const std::vector<PakTexture> *textures = nullptr;    // given: the PS3 textures to convert (else all of the pak)
    size_t *materials_written = nullptr;
};

Bytes convert_net(const fs::path &ps3, const fs::path &menu_ref, const NetOptions &o = {});
Bytes wpnmods(const fs::path &ps4, const fs::path &ps3, const std::string &src_skin, const std::vector<std::string> &skins, int max_tex,
              int stream_flags);

}  // namespace u3::mats
