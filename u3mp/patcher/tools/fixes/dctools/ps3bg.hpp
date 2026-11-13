// dctools/ps3bg.hpp: readers of PS3 Uncharted 3 background level paks (big endian, 4-byte pointers), used by the
// stage converter (ps4stage.cpp). The latest converter's bggeom.py / ps3geom.py / ps3params.py / waterdata.py and the
// material table reader of mkplan.py, in C++.
//
// PM_COLLECTION_2: {u32 n, ptr entries[n]}; entry {ptr header, ptr instances, u32 n instances}; header +0 u32 (byte0 =
//   LODs, byte1 flags, low 16 bits = joints), +4 u16 groups, +6 u16 blocks, +8 fade, +0x10 ptr group table (0x10 per
//   group: material*, -, u16 first block, u16 count, u32 flags), +0x14 ptr blocks*, +0x38 flags.
// Geometry block: +0 ptr stream table {ptr data, u32} per attribute, +4 ptr index info {u16 ntris, 0, u16 size, 0,
//   ptr data}, +0xc ptr skin, +0x24 ptr {u32 nverts}, +0x28 ptr vertex format.
// Vertex format: attributes of 8 bytes {u8 id, u8 fmt, u16 param offset, u8 bits[4]} from +8 up to the first parameter
//   block. fmt 0x4N: N components a*(1 + v*2^-21) + b; 0x6N: a*(1 + (4v + I)*2^-23) + b (N u32 I after the floats);
//   0x0N: signed normalised (see decode_attr). Every attribute is its own big-endian bit stream (MSB first).
// Indices: {u16 bits, u16 nExplicit, u16 nSlots, u16 start} and three 16-bit aligned bit streams: explicit indices,
//   2-bit triangle configs (0: p0 p2 new, 1: p2 p1 new, 3: three new), 1-bit slot flags (0 = running counter).
// Skin: u16 sizes s1..s4, nRanges, -, ptr s1..s4, 0, ptr ranges {u16 first joint, u16 count}; vertices in groups of 4
//   with a code byte each (s1): 4/6 new joint list (s2), 2/10 per-lane joint rows (s3, lane order 1 0 3 2), 0 repeat
//   the previous group, 8 previous joints and new weights; 6/8/10 carry u8 weights (s4), else 255 on the first joint.
#pragma once
#include "ndpak.hpp"

#include <array>
#include <optional>

namespace u3 {

using Ref = std::pair<uint32_t, uint64_t>;   // (page, offset)
using ORef = std::optional<Ref>;

// a decoded attribute: rows x cols doubles, row-major
struct Mat {
    size_t rows = 0, cols = 0;
    std::vector<double> v;
    Mat() = default;
    Mat(size_t r, size_t c) : rows(r), cols(c), v(r * c, 0.0) {}
    double &at(size_t r, size_t c) { return v[r * cols + c]; }
    double at(size_t r, size_t c) const { return v[r * cols + c]; }
};

namespace bg {

ORef res(const Pak &p, uint32_t pg, uint64_t off);   // p.resolve
Ref need(const Pak &p, uint32_t pg, uint64_t off);   // p.resolve, stops on a null pointer (Python's TypeError)
Ref need(const ORef &r);
uint8_t u8(const Pak &p, uint64_t o);
uint16_t be16(const Pak &p, uint64_t o);
float bef32(const Pak &p, uint64_t o);
std::string utf8_of(const std::string &latin1);   // str.encode() of a latin-1 decoded string
bool py_printable(const std::string &latin1);     // str.isprintable() of a latin-1 decoded string

struct Attr {
    uint8_t id, fmt;
    uint16_t off;
    uint8_t w[4];
    int n() const { return fmt & 0xf; }
};
std::vector<Attr> read_format(const Pak &p, uint32_t pg, uint64_t fmt);
// prec: the nominal bit depth of a 0x0N attribute (background blocks, block_prec), < 0 = none
Mat decode_attr(const Pak &p, uint32_t pg, uint64_t fmt, const Attr &a, uint32_t data_pg, uint64_t data_off, uint32_t nv, int prec = -1);
std::map<int, int> block_prec(const std::vector<Attr> &attrs);
std::vector<std::array<uint32_t, 3>> decode_indices(const uint8_t *raw, size_t len, uint32_t ntris);
using Influence = std::vector<std::pair<uint32_t, uint32_t>>;   // (joint, weight), ascending weight
std::vector<Influence> decode_skin(const Pak &p, uint32_t page, uint64_t info, uint32_t nverts);

struct Group {
    std::string mat;
    uint32_t first, count;
};
struct Proto {
    Ref entry, hdr;
    std::vector<Ref> blocks;
    std::vector<std::optional<std::string>> mats;   // per block
    std::vector<Group> groups;
    std::vector<ORef> insts;
};
std::vector<Proto> collection(const Pak &p);

struct Instance {
    Ref addr;
    uint32_t x8 = 0;
    double mat[16] = {};
    ORef blockdata;
};
Instance instance(const Pak &p, const Ref &ia);

// gen_tangents: per-vertex tangents (xyz + w sign) from UV derivatives, Gram-Schmidt against the normal
Mat gen_tangents(const Mat &pos, const Mat &nrm3, const Mat *uv, const std::vector<std::array<uint32_t, 3>> &tris);

// MATERIAL_TABLE_1: the materials (name, texture indices) and textures (name, hash)
struct Material3 {
    std::string name;
    std::vector<uint32_t> textures;
};
struct Texture3 {
    std::string name;
    uint32_t hash;
};
void read_materials(const Pak &p, std::vector<Material3> &mats, std::vector<Texture3> &texs);

// PS3 material constants: {material: {param: floats}} (ps3params.all_material_params, the ownership rule)
using Params = std::vector<std::pair<std::string, std::vector<double>>>;   // in insertion order
std::map<std::string, Params> all_material_params(const Pak &p);
const std::vector<double> *param(const Params &ps, const std::string &k);

// WATER_DATA objects: (object ref, PS3 material name or none)
std::vector<std::pair<Ref, std::optional<std::string>>> water_objects(const Pak &p);
std::vector<std::string> water_materials(const Pak &p);

}  // namespace bg
}  // namespace u3

namespace u3::bg {

// foreground (actor) geometry blocks (ps3geom.decode_block): attributes up to the first word that is no attribute id,
// 0x0N values v*2/(2^bits-1) - 1 without the 1-bit sign rule; the skin of a skinned block (errors propagate)
struct FgBlock {
    uint32_t nverts = 0;
    std::map<std::string, Mat> attr;   // pos, normal, tangent, uv0, uv1, uv2, color0, color1, attr<id>
    std::vector<std::array<uint32_t, 3>> tris;
    bool has_skin = false;
    std::vector<Influence> skin;
    const Mat *get(const std::string &k) const {
        auto it = attr.find(k);
        return it == attr.end() ? nullptr : &it->second;
    }
};
FgBlock decode_fg_block(const Pak &p, uint32_t page, uint64_t blk, bool skin = true);
struct FgProto {
    std::string name;
    Ref proto;
    std::vector<Ref> blocks;
};
std::vector<FgProto> fg_prototypes(const Pak &p);   // FOREGROUND_SECTION_2 name -> prototype + blocks (a later name replaces)

}  // namespace u3::bg
