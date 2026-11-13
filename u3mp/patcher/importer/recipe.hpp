// importer/recipe.hpp: the overlay recipe (see recipe.cpp).
#pragma once
#include "importer.hpp"

#include <map>

namespace u3imp {

// u3mp/patcher (recipe/, tools/, build/ ...): the folder above u3import.exe's importer/ folder in a checkout, or
// u3mp/patcher of the launcher package
fs::path patcher_dir();
fs::path recipe_dir();
fs::path tools_dir();
std::string recipe_version();   // recipe/VERSION

std::map<std::string, Value> parse_sfo(const fs::path &path);
std::optional<fs::path> find_sfo(const fs::path &folder);

// The validated input folders (recipe_lib.Inputs)
struct Inputs {
    std::optional<fs::path> ps4;
    std::map<std::string, Value> ps4_sfo;
    std::map<std::string, fs::path> ps3;                         // kind (disc, update, data2, dlcmaps) -> USRDIR
    std::map<std::string, std::map<std::string, std::string>> ps3_sfo;
    std::optional<fs::path> u2;
    std::map<std::string, std::string> u2_sfo;
    std::vector<std::string> problems, notes;

    void set_ps4(const fs::path &folder);
    void add_ps3_u3(const std::vector<fs::path> &folders, bool scan = true);
    void set_ps3_u2(const std::vector<fs::path> &folders, bool scan = true);
    std::vector<std::string> speech_langs() const;
    std::vector<std::pair<std::string, std::string>> placeholders(const fs::path &out) const;
    std::string describe() const;
    // every file a PS3 input read opened (the "keep only the PS3 files updates need" record)
};

std::vector<std::string> full_import_problems(Inputs &inp);
std::vector<fs::path> scan_dirs(const std::vector<fs::path> &roots, int max_depth = 4);

// rebuild_overlays: the recipe over the input folders into `out`; returns its exit code (0, 1 failed steps, 2 inputs)
struct RecipeArgs {
    std::string ps4;
    std::vector<std::string> ps3_u3, ps3_u2;
    std::string out, only, from, skip, speech_langs = "english";
    bool force = false, dry_run = false, continue_on_error = false, all_files = false, allow_missing_u2 = false, allow_partial_inputs = false;
    int jobs = 0;
    std::vector<std::pair<std::string, std::string>> env;   // extra environment of the steps
};
int rebuild_overlays(const RecipeArgs &a, const std::function<void(const std::string &)> &out_line);

// PS3 files this process opened under the recorded roots (ps3keep: <data>/ps3-reads.txt)
void record_reads(const std::vector<fs::path> &roots, const fs::path &log_path);
void note_read(const fs::path &p);

}  // namespace u3imp

namespace u3imp {
// (step id, fingerprint) of every step in ORDER.txt (maintainer checks: the records of an import must match)
std::vector<std::pair<std::string, std::string>> step_fingerprints();
}  // namespace u3imp

namespace u3imp {
// maintainers: a rebuilt output folder against recipe/manifest.tsv (u3import --verify-output <out> [--report <tsv>])
int verify_output(const fs::path &out, const fs::path &report);
}  // namespace u3imp

namespace u3imp {
void recipe_list();   // the step table (u3import --recipe --list)
}  // namespace u3imp
