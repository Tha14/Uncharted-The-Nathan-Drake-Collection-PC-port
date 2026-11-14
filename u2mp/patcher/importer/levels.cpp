// importer/levels.cpp: Uncharted 2 level paks (pak22), PS3 -> PS4.
//
// Two kinds, told apart by their items:
//   * stage (background) paks: PM_COLLECTION_2 / BACKGROUND_SECTION geometry, materials, textures, light, visibility,
//     collision and water -> the Uncharted 3 stage converter (dctools: stageplan, ps4mats, ps4stage) with the U2
//     differences (u3::g_u2). Materials come from the retail PS4 U2 paks: the one of the same name, else the closest.
//   * data paks ('-ingame10', '-npc'): spawners, regions, splines, nav meshes, the feature db -> dctools ps4ingame with
//     the U2 differences.
#include "ndpak.hpp"
#include "ps4ingame.hpp"
#include "u2conv.hpp"

namespace u2imp {
namespace {

const std::set<std::string> STAGE_TYPES = {"PM_COLLECTION_2", "BACKGROUND_SECTION"};
const std::set<std::string> DATA_TYPES = {"SPAWNER_GROUP", "REGION_TREE", "FEATURE_DB_2", "SPLINE_TABLE", "NAV_MESH_1", "TAG_INT"};
// a pak of only these items is a data pak too
const std::set<std::string> DATA_ONLY = {"LEVEL_INFO_1", "PAK_LOGIN_TABLE", "VRAM_DESC_TABLE", "TAG_INT",
                                         "SPLINE_TABLE", "LEVEL_OFFSET_1", "DEBUG_INFO_1", "LEVEL_PATS_1"};

std::string kind(const std::set<std::string> &types) {
    for (const std::string &t : types)
        if (STAGE_TYPES.count(t)) return "stage";
    bool data = true;
    for (const std::string &t : types) {
        if (DATA_TYPES.count(t)) return "data";
        data = data && DATA_ONLY.count(t);
    }
    return data ? "data" : "other";
}

std::string joined(const std::set<std::string> &s) {
    std::string o;
    for (const std::string &x : s) o += (o.empty() ? "" : ", ") + x;
    return o;
}

}  // namespace

std::vector<std::string> convert_stage(const Context &c, const u3::Pak &p, const fs::path &src, const fs::path &out);   // stage.cpp

std::vector<std::string> convert_level(const Context &c, const fs::path &src, const fs::path &out) {
    std::unique_ptr<u3::Pak> p;
    try {
        p = std::make_unique<u3::Pak>(src);
    } catch (const std::exception &e) {
        throw Unsupported(std::string("pak layout: ") + e.what());
    }
    std::set<std::string> types;
    for (const auto &it : p->items) types.insert(it.type);
    const std::string k = kind(types);
    if (k == "stage") {
        std::set<std::string> data;
        for (const std::string &t : {"SPAWNER_GROUP", "REGION_TREE", "FEATURE_DB_2"})
            if (types.count(t)) data.insert(t);
        if (!data.empty()) {
            std::set<std::string> all;
            for (const std::string &t : types)
                if (DATA_TYPES.count(t)) all.insert(t);
            throw Unsupported("stage pak with level data items (" + joined(all) + ")");
        }
        std::vector<std::string> notes = {k};
        for (const std::string &n : convert_stage(c, *p, src, out)) notes.push_back(n);
        return notes;
    }
    if (k == "data") {
        Bytes data;
        std::vector<std::string> notes = {k};
        try {
            data = u3::ps4ingame_convert(*p, {}, {}, c.drop_navmesh);
        } catch (const u3::NavMeshUnsupported &e) {
            throw Unsupported(e.what());
        }
        if (c.drop_navmesh && !p->items_of("NAV_MESH_1").empty())
            notes.push_back(fmt("%zu NAV_MESH_1 left out (drop_navmesh)", p->items_of("NAV_MESH_1").size()));
        u3::Pak check(data, S(out));   // a broken writer result fails here
        const fs::path tmp = S(out) + ".tmp";
        write_bytes(tmp, data);
        std::error_code ec;
        fs::rename(tmp, out, ec);
        if (ec) throw std::runtime_error("cannot write " + S(out) + ": " + ec.message());
        return notes;
    }
    throw Unsupported("unknown level pak kind (items: " + joined(types) + ")");
}

}  // namespace u2imp
