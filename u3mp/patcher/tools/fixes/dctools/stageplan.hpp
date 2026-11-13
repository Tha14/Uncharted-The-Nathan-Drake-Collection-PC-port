// dctools/stageplan.hpp: the resources a converted stage level reuses from the retail PS4 paks (the latest converter's
// mkplan.py, fxextract.py, lib/mkres.py and stage/make_default_lightmaps.py, in C++).
//
// The survey (res dir): ps4mats.jsonl.gz, one JSON line per retail PS4 pak23 pak (not -ingame12) with its GEOMETRY_1
// materials {n name, s shader, h effect hash, f features, b bindings [(param, texture short path)], fl}; ps4tex.tsv,
// one line per texture (pak, full path, width, height, format, mips, VRAM offset, size, 0, 0, 0).
//
// Plan, for every PS3 material of the stage (prototype material groups, then water materials):
//  * exact: a retail material named like it (PS3 'dir/file/variant' -> PS4 'dir/file:variant'), preferring prime-shader
//    materials and the paks that cover the most materials of the stage, then the smaller pak;
//  * water material without a twin: the water.fx material with the longest common directory, then the same variant
//    name, then the smallest pak;
//  * fallback: the best scoring prime-shader material: texture roles, render class (alpha test / blend / foliage),
//    directory, PS3 constants, wind / leaf variants, the v2 / v2.1 / v2.2 / v2.3 donor feature penalties.
// Every PS3 texture and every texture bound by a chosen material that some retail pak carries is requested too.
#pragma once
#include "ps3bg.hpp"

namespace u3::stage {

std::string short_of(const std::string &full);        // '.../texture1|2/<short>/<HASH>.ndb' -> short path
std::optional<std::string> tex_role(const std::string &s);
std::optional<std::string> tex_role_final(const std::string &s);
std::optional<std::string> param_role(const std::string &p);
std::string ps4key(const std::string &ps3name);       // 'dir/file/variant' -> 'dir/file:variant'

void make_res(const fs::path &out, const fs::path &pak_dir);   // the survey files (lib/mkres.py)

struct PlanMat {
    std::string key, pak, name;
    bool exact = false;
    std::vector<std::pair<std::string, std::string>> binds;
    bg::Params ps3params;
    std::vector<std::string> roles, cls;
    bool has_roles = false;
};
struct Plan {
    std::string stage;
    std::vector<std::pair<std::string, PlanMat>> mats;   // in plan order
    std::vector<std::pair<std::string, std::string>> req_mats, req_tex;
    const PlanMat *get(const std::string &n) const;
};
Plan make_plan(const fs::path &ps3, const fs::path &res);
std::string plan_json(const Plan &p);   // json.dump(plan, indent=1)

// the device bundle: retail material records with their EFFECT closures, and texture descriptors + texels
struct FxItem {
    std::string type, name;
    uint64_t off = 0;
    uint32_t page = 0;
    Bytes data;
    struct Ptr {
        uint64_t pos;
        size_t target;
        int64_t off;
    };
    std::vector<Ptr> ptrs;
};
struct MatParam {
    std::string name;
    uint32_t cnt, x;
    Bytes vals;
};
struct MatBind {
    std::string param, shrt, full;
};
struct MatTex {
    std::optional<Bytes> gtex;
    bool has_desc = false;
    std::string path;
    Bytes desc, texels;
};
struct MatRec {
    std::string name, shader, hash;
    std::vector<std::string> feats;
    std::vector<MatParam> params;
    std::vector<MatBind> binds;
    uint32_t x3 = 0, tail[2] = {};
    bool has_fx = false;
    uint32_t fx_idx = 0, fx_x = 0;
    int64_t fx_eoff = 0;
    std::vector<FxItem> fx;
    std::vector<std::pair<std::string, MatTex>> textures;   // full path -> entry (dict order)
};
struct BundleTex {
    std::string pak, path;
    Bytes desc, texels;
    std::optional<Bytes> gtex;
};
struct Bundle {
    std::vector<std::pair<std::pair<std::string, std::string>, MatRec>> mats;   // (pak, key) -> record
    std::vector<BundleTex> tex;
    std::map<std::string, std::vector<std::string>> techs;
    std::vector<std::pair<std::string, std::string>> gobo_tex;   // PS3 texture hash (hex) -> PS4 short path
};
Bundle extract(const fs::path &pak_dir, const Plan &plan);
// the two retail default lightmap textures (default-lightmap0/1, from grave-02-hollowhulk-front-low)
std::vector<BundleTex> default_lightmaps(const fs::path &pak_dir);

}  // namespace u3::stage
