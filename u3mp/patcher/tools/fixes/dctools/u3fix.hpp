// dctools/u3fix.hpp: the steps of u3fix.exe (see u3fix.cpp)
#pragma once
#include "dc.hpp"
#include "json.h"   // launcher/native/json.h: JSON values, parse, Python-style quoting

namespace u3 {

fs::path upath(const std::string &utf8_path);

// --flag and --name value arguments
struct Args {
    Args(const std::vector<std::string> &argv, const std::set<std::string> &flags, const std::set<std::string> &values);
    bool has(const std::string &f) const { return set.count(f) != 0; }
    std::string need(const std::string &k) const;
    std::string get(const std::string &k, const std::string &def) const;
    std::set<std::string> set;
    std::map<std::string, std::string> val;
};

// helpers of the steps that replaced Python scripts (their messages and files stay as the scripts made them)
std::string md5_hex(const Bytes &data);
void copy2(const fs::path &src, const fs::path &dst);   // shutil.copy2: the bytes and the modification time
bool game_running(bool name_contains = false);          // a process uncharted.exe (or one whose name contains it)
std::string win_path(std::string p);                    // the form os.path.abspath gives on Windows: '/' -> '\'
std::string pyhex(uint64_t v);                          // Python hex() / '%#x': 0x0, 0x1f
json::Value read_json(const fs::path &p);               // stops on a file that is not JSON
std::string json_dump_indent1(const json::Value &v);    // json.dump(v, f, indent=1)
uint16_t rd16le(const Bytes &d, uint64_t o);            // stops past the end (Python's struct.error)
uint32_t rd32le(const Bytes &d, uint64_t o);
uint64_t rd64le(const Bytes &d, uint64_t o);
uint32_t be32(const Bytes &d, uint64_t o);              // big endian, stops past the end
// Python str rules for a latin-1 text: whitespace (str.split / rstrip) and line breaks (str.splitlines)
bool py_space(unsigned char c);
std::vector<std::string> py_split(const std::string &s);
std::string py_rstrip(const std::string &s);
std::vector<std::string> py_splitlines(const std::string &s);

int remove_lab(const std::vector<std::string> &argv);         // tools/fixes/no-lab/remove_lab.cpp
int remove_rpcn_page(const std::vector<std::string> &argv);   // tools/fixes/no-rpcn-menu/remove_rpcn_page.cpp
int netgame_order(const std::vector<std::string> &argv);      // tools/fixes/netgame-order/netgame_order.cpp
int emblem_cap3(const std::vector<std::string> &argv);        // tools/fixes/wall-emblem-test/emblem_cap3.cpp
int eula_box(const std::vector<std::string> &argv);           // tools/fixes/dctools/eula_box.cpp
int orange_wall(const std::vector<std::string> &argv);        // tools/fixes/orange-wall/apply_orange.cpp
int excess_wall(const std::vector<std::string> &argv);        // tools/fixes/excess-wall/apply_excess.cpp
int enemy_stall(const std::vector<std::string> &argv);        // tools/fixes/enemy-stall/apply_ss_option_defaults.cpp
int coop_desc(const std::vector<std::string> &argv);          // tools/fixes/coop-chars/apply_coop_desc_fix.cpp
int vram_gap(const std::vector<std::string> &argv);           // tools/fixes/coop-tab/fix_vram_gap.cpp
int igc3_retargets(const std::vector<std::string> &argv);     // tools/fixes/airport-igc3/airport_igc3.cpp
int igc3_install(const std::vector<std::string> &argv);       // tools/fixes/airport-igc3/airport_igc3.cpp
int text1_merge(const std::vector<std::string> &argv);        // tools/fixes/text1-merge/merge_text1.cpp
int coop_audit(const std::vector<std::string> &argv);         // tools/fixes/coop-audit/apply_option_defaults_2.cpp
int registry_levels(const std::vector<std::string> &argv);    // tools/fixes/registry/run_mkregistry.cpp
int registry_final(const std::vector<std::string> &argv);     // tools/fixes/pak23-registry/apply_registry.cpp
int hair_key(const std::vector<std::string> &argv);           // tools/fixes/hat-chloe/apply_hair_key.cpp
int levels(const std::vector<std::string> &argv);             // tools/fixes/pak23-levels/levels.cpp
int actors(const std::vector<std::string> &argv);             // tools/fixes/actor33/build_actors.cpp
int rebuild_am(const std::vector<std::string> &argv);          // tools/fixes/actor33/build_actors.cpp
int install_am(const std::vector<std::string> &argv);          // tools/fixes/actor33/build_actors.cpp
int convert_dc1(const std::vector<std::string> &argv);         // tools/fixes/dcconv/convert_dc1.cpp
int dc_recipes(const std::vector<std::string> &argv);          // tools/fixes/dcconv/apply_recipes.cpp
int sound_banks(const std::vector<std::string> &argv);         // tools/fixes/sound-banks/convert_banks.cpp
int part_modules(const std::vector<std::string> &argv);        // tools/fixes/part-emitters/build_part_modules.cpp
int actors_hk(const std::vector<std::string> &argv);           // tools/fixes/actor33/actors_hk.cpp
int coop_vo(const std::vector<std::string> &argv);             // tools/fixes/coop-vo/apply_vo.cpp
int coop_voice2(const std::vector<std::string> &argv);         // tools/fixes/coop-voice2/apply_coop_vox.cpp
int elim_pic(const std::vector<std::string> &argv);            // tools/fixes/dctools/net_edits.cpp
int classic(const std::vector<std::string> &argv);             // tools/fixes/dctools/net_edits.cpp
int skins_net_names(const std::vector<std::string> &argv);     // tools/fixes/dctools/net_edits.cpp
int shade_npc(const std::vector<std::string> &argv);           // tools/fixes/coop-audit/apply_shade_npc_paks.cpp
int facility_collapse(const std::vector<std::string> &argv);   // tools/fixes/inline/facility_collapse.cpp
int pakinfo(const std::vector<std::string> &argv);            // dctools/ndpak.cpp: a pak's summary (as ndpak.py prints it)

}  // namespace u3
