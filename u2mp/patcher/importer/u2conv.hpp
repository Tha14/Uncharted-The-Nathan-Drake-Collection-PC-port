// importer/u2conv.hpp: the converters of the Uncharted 2 import that work on paks and DC modules (u2mp/patcher/u2conv),
// as the import calls them.
#pragma once
#include "u2import.hpp"

namespace u2imp {

struct Context {
    const PS3 *ps3 = nullptr;
    const PS4 *ps4 = nullptr;
    fs::path work;                  // <out>/work: caches
    fs::path main;                  // <out>/u2data/build/main
    std::string ps4_game, ps3_elf, objdump;
    bool drop_navmesh = false;
};

struct Converted {
    std::string rel;                // the output under build/main
    std::vector<std::string> notes;
};

// actor27 paks: skins (PS4 campaign twin first, else the PS3 skin on a PS4 template), props, animation paks, ragdolls.
// `src` is the PS3 pak on disk. Throws Unsupported when there is no conversion for its content.
Converted convert_actor(const Context &c, const Entry &entry, const fs::path &src);
// pak22 level paks (stage or data pak) -> out; returns the kind and notes. Throws Unsupported.
std::vector<std::string> convert_level(const Context &c, const fs::path &src, const fs::path &out);
// the PS3 net.pak menu images the PS4 net.pak lacks, as pak22/ps4/net-u2mp.pak; the image names (empty: nothing made)
extern const char *NETIMG_OUT_NAME, *NETIMG_VERSION;
std::vector<std::string> convert_net_images(const fs::path &ps3_net, const fs::path &ps4_net, const fs::path &out);
// dc1/ps4/net-patch9.bin from the PS3 server module: the converted module and its note
extern const char *NETPATCH_VERSION;
Bytes convert_net_patch(const Context &c, const Bytes &data, std::string &note);

}  // namespace u2imp
