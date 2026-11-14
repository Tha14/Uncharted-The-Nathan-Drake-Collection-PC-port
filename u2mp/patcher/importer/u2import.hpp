// importer/u2import.hpp: u2import.exe, the Uncharted 2 multiplayer import the launcher runs (C++).
//
//   u2import.cpp   the import: arguments, the steps, the import list, state.json, the launcher's queries
//   sources.cpp    where each file comes from: the PS3 copies (archives by precedence) and what the PS4 game has
//   sound.cpp      XVAG streams and SCREAM banks
//   registry.cpp   pak22_ps4.txt and banks.txt of the overlay
//   texts.cpp      the text banks completed with the multiplayer strings
//   levels/stage/netimg/netpatch/actor*/havok/twin.cpp   the converters of levels, menu images, the net-patch DC module, actors
//   stagecheck.cpp the check of each converted stage pak
//   overlay.cpp    the menu overlay (main menu with a Multiplayer button)
//   validate.cpp / devtools.cpp   --validate, and the maintainers' commands (--validate-twins in twin.cpp)
//
// Uses the Uncharted 3 importer's helpers (u3mp/patcher/importer/common.cpp: files, hashes, JSON, logging) and the
// Uncharted 3 converters (u3mp/patcher/tools/fixes/dctools) with their Uncharted 2 differences (u3::g_u2).
#pragma once
#include "importer.hpp"   // u3mp/patcher/importer
#include "psarc.hpp"      // u3mp/patcher/tools/fixes/dctools

#include <map>
#include <memory>
#include <set>

namespace u2imp {

using namespace u3imp;
using u3::Psarc;

// a converter has no conversion for this content (status "unsupported")
struct Unsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string sha1_hex(const Bytes &b);
// Python's str.splitlines (keepends): \n, \r\n, \r, \v, \f, \x1c, \x1d, \x1e and \x85 end a line (text read as latin-1)
std::vector<std::string> splitlines(const std::string &t, bool keepends = false);
std::vector<std::string> split_ws(const std::string &s, int max = -1);   // str.split(None[, max])
std::string pystrip(const std::string &s);                               // str.strip() on latin-1 text
std::string py_repr(const std::string &latin1);                          // repr() of a str
std::string py_list(const std::vector<std::string> &v);                  // repr() of a list of str
std::string latin1_utf8(const std::string &s);
std::string utf8_latin1(const std::string &s);
std::string basename(const std::string &key);                            // after the last '/'

// ------------------------------------------------------------------------------------------------ sources.cpp
// keys: paths under build/main without platform folders, compared lower-case ('actor27/x.pak', 'sound1/music/x.xvag')
struct Entry {
    std::string key, name, source;      // source: base, patch, dlc-map1/2, dlc-patch2/3/5/7
    std::shared_ptr<Psarc> arc;
    size_t index = 0;                   // archive entry (names[index - 1])
    fs::path path;                      // a loose file instead
    std::string arc_path;
    Bytes read() const;
    std::string where() const;
};

class PS3 {
public:
    PS3(const std::string &base, const std::string &dlc);
    std::vector<const Entry *> candidates(const std::string &key) const;   // newest source first
    std::vector<std::string> archives;
    fs::path base_main;

private:
    std::map<std::string, std::vector<Entry>> by_key;
    void put(Entry e);
    void add_arc(const fs::path &path, const std::string &source, const std::string &prefix);
    void add_tree(const fs::path &root, const std::string &source);
};
std::optional<fs::path> build_main(const fs::path &root);

class PS4 {
public:
    explicit PS4(const std::string &game);
    bool has(const std::string &key) const { return files.count(lower(key)) != 0; }
    std::set<std::string> music_refs() const;
    fs::path main, pak_dir, actor_dir, bank_dir, registry_path, banks_txt, music_settings;
    std::map<std::string, fs::path> files;                       // lower key -> file
    std::map<std::string, std::set<std::string>> speech;         // language -> lower names
};

// ------------------------------------------------------------------------------------------------ sound.cpp
const uint32_t XVAG_INTERLEAVE = 0x155;   // the PS4 block: frames per channel
Bytes xvag_convert(const Bytes &d, uint32_t interleave = XVAG_INTERLEAVE);
std::string find_ffmpeg(const std::string &arg);
Bytes bank_convert(const Bytes &ps3, const std::string &ffmpeg, std::string &note);

// ------------------------------------------------------------------------------------------------ registry.cpp
struct RegistryResult {
    std::string text;
    std::vector<std::string> added;
    std::map<std::string, std::string> skipped;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> dropped;
};
RegistryResult build_registry(const std::string &ps4_text, const std::map<std::string, std::string> &new_levels,
                              const std::function<bool(const std::string &)> &have_pak,
                              const std::function<bool(const std::string &)> &have_actor,
                              const std::function<bool(const std::string &)> &have_bank, const std::set<std::string> &failed_paks);
std::pair<std::string, std::vector<std::string>> build_banks_txt(const std::string &ps4_text, const std::vector<std::string> &new_banks);
std::vector<std::string> package_file_names(const std::string &pkg);

// ------------------------------------------------------------------------------------------------ texts.cpp
struct TextCounts {
    long long goty = 0, related = 0, left = 0;
};
// every language's bank of one category: {language: (bytes, or nullopt when the PS4 bank needs nothing; counts)}
std::map<int, std::pair<std::optional<Bytes>, TextCounts>> texts_complete(const std::vector<std::pair<int, Bytes>> &ps4,
                                                                          const std::map<int, std::vector<Bytes>> &goty);

// ------------------------------------------------------------------------------------------------ the import's paths
fs::path patcher_dir();      // u2mp/patcher (u2import.exe is in u2mp/bin)
fs::path launcher_root();    // the folder holding u2mp and u3mp

// stagecheck.cpp: the problems of a converted stage pak ([] = keep it)
std::vector<std::string> stage_check(const fs::path &path);

// validate.cpp: --validate, the converters checked on the files both platforms have; the report (also <work>/validation.json)
struct Context;
json::Value validate(const PS3 &ps3, const PS4 &ps4, const Context &ctx, const fs::path &work, const std::function<void(const std::string &)> &lg);
// twin.cpp: --validate-twins, the retail mp-hero skins with a PS4 twin built from PS3 and compared with retail
// (<work>/twin-validation.json)
json::Value twin_validate(const PS3 &ps3, const PS4 &ps4, const fs::path &work);

// devtools.cpp: --dc, --hk-tables, --twin-build, --psarc (maintainers); the exit code, or nullopt for other arguments
std::optional<int> dev_command(const std::vector<std::string> &v);

// overlay.cpp: the menu overlay (main menu with a Multiplayer button) in `out`, made or kept; throws with the reason
fs::path build_overlay(const fs::path &game, const fs::path &out, const std::optional<fs::path> &update);

}  // namespace u2imp
