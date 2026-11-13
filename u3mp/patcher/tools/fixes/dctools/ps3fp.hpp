// dctools/ps3fp.hpp: what the sky converter (paktools-sky) reads from a PS3 material's NV40 fragment program (main
// pass): ps3fp_nv40.py (the disassembly text), ps3blendpol.py, ps3blendsrc.py and ps3specmask.py, in C++.
//
// A material's first technique-pass node starts with the u16 program id; MATERIAL_TABLE_1 +0x1c lists the programs
// (0x10 each, +12 ptr program {u32 size, ..., +0x10 code offset}); the node's +0x18 lists its texture references
// (0x10 each: texture-table pointer, address word, 0, gamma word with bits 20-23 = R, G, B, A).
//  * blend polarity: 'inv' when the program computes (1 - map) * K (MAD x, -map.y, K, K) from a texture read into .y;
//  * blend source: the term f = sat(t + M / (2s + 0.01)) traced back to one channel of the texture on a sampler unit
//    (units -> references: the three lighting buffers are three consecutive 'missingtexture' references on units
//    12-14; references after them take the used units below 12 in ascending order without unit 0, then the one before
//    them, else the last, takes unit 0), or a vertex-colour-only blend f = sat(1 - v);
//  * spec mask scale: the mean of the mask channel the program reads from the material's '-spec' texture.
#pragma once
#include "ps3bg.hpp"

namespace u3::fp {

std::vector<std::string> dis(const Bytes &code, size_t maxn = 2000);   // the disassembly lines

struct Ps3Programs {   // per PS3 pak: the tables the three readers share (built on first use)
    explicit Ps3Programs(const Pak &p) : p(p) {}
    const Pak &p;
    std::optional<std::map<std::string, std::string>> polarity;   // material -> 'inv' / 'non'
    struct Ref_ {
        std::string shrt;
        uint32_t fl[3];
        std::string full;
    };
    std::optional<std::map<std::string, std::pair<std::vector<std::string>, std::vector<Ref_>>>> table;
    std::map<std::string, std::optional<double>> mask;
};

std::optional<std::string> material_polarity(Ps3Programs &P, const std::string &name);
struct BlendSource {
    bool vc_only = false;
    std::string full, chan, pol;
    bool gamma = false;
};
std::optional<BlendSource> blend_source(Ps3Programs &P, const std::string &name);
// one channel of a PS3 texture as grey BC1 (PS4 tiled, the full mip chain): (texels, w, h, mips)
std::optional<std::tuple<Bytes, uint32_t, uint32_t, uint32_t>> derive_texels(const Pak &p, const PakTexture &t, const std::string &chan, bool gamma, bool invert);
std::optional<double> mask_scale(Ps3Programs &P, const std::string &name);

}  // namespace u3::fp
