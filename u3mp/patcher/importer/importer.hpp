// importer/importer.hpp: u3import.exe, the Uncharted 3 multiplayer import the launcher runs (the former u3mp_import.py,
// recipe/rebuild_overlays.py, recipe/recipe_lib.py, mpimage.py, eboot_keys.py and ps3keep.py, in C++).
//
//   u3import.cpp   the import: game data, the multiplayer image, the overlay recipe, staged updates, state.json
//   recipe.cpp     the overlay recipe: input folders, steps (copy lists, u3fix.exe commands), step records
//   mpimage.cpp    the multiplayer image: base image, menu shell, helper link, online profile keys, the recipe's PS4 ELF
//   ps3keep.cpp    "keep only the PS3 files updates need"
//   common.cpp     files, hashes, JSON in Python's layout, logging
//
// Progress for the launcher: lines "@U3MP <json>" on stdout, as before. Nothing is written outside --data.
#pragma once
#include "dc.hpp"     // u3mp/patcher/tools/fixes/dctools: Bytes, fs, Stop
#include "json.h"     // launcher/native/json.h

#include <functional>
#include <optional>

namespace u3imp {

using u3::Bytes;
namespace fs = std::filesystem;
using json::Value;

// ------------------------------------------------------------------------------------------------ text and paths
fs::path P(const std::string &utf8);           // a path from UTF-8 text
std::string S(const fs::path &p);              // UTF-8 text of a path (native separators)
std::string fwd(const fs::path &p);            // os.path.normpath(p).replace('\\', '/')
std::string lower(std::string s);
bool starts(const std::string &s, const std::string &p);
bool ends(const std::string &s, const std::string &p);
std::string strip(const std::string &s);
std::vector<std::string> split(const std::string &s, char c);
std::string now_text();                        // time.strftime('%Y-%m-%d %H:%M:%S')
std::string fmt(const char *f, ...);

// ------------------------------------------------------------------------------------------------ output
void log(const std::string &line);             // a line on stdout (flushed)
void event(const Value &kw);                   // "@U3MP " + json.dumps(kw)
Value obj(std::initializer_list<std::pair<std::string, Value>> kv);
Value str(const std::string &s);
Value num(long long v);

// ------------------------------------------------------------------------------------------------ JSON in Python's layout
std::string dumps(const Value &v, bool sort_keys = false);   // json.dumps(v[, sort_keys=True]): one line, ', ' and ': '
std::string dump_indent1(const Value &v);                    // json.dump(v, f, indent=1)
Value load_json(const fs::path &p);                          // throws Stop when missing or not JSON
std::optional<Value> try_json(const fs::path &p);

// ------------------------------------------------------------------------------------------------ files and hashes
Bytes read_bytes(const fs::path &p);
std::string read_text(const fs::path &p);
void write_text(const fs::path &p, const std::string &t);   // as it is (no newline translation)
void write_bytes(const fs::path &p, const Bytes &b);
class Sha256 {
public:
    Sha256();
    ~Sha256();
    void update(const void *data, size_t n);
    std::string hex();          // finishes
    Bytes digest();             // finishes
private:
    void *alg = nullptr, *hash = nullptr;
};
std::string sha256_hex(const void *data, size_t n);
inline std::string sha256_hex(const Bytes &b) { return sha256_hex(b.data(), b.size()); }
inline std::string sha256_hex(const std::string &s) { return sha256_hex(s.data(), s.size()); }
Bytes sha256_raw(const void *data, size_t n);
std::string sha256_file(const fs::path &p);
// sha256 of a file, remembered by (path, size, modification time) for this run: the recipe hashes the files of a step
// before and after it, and the next step's check reads the same files again
std::string sha256_cached(const fs::path &p);
uint64_t tree_size(const fs::path &folder);
bool rmtree_retry(const fs::path &p, double seconds = 10);
void rmtree(const fs::path &p);   // shutil.rmtree (read-only files too)
std::string zlib_inflate(const std::string &z);

// ------------------------------------------------------------------------------------------------ processes
// runs argv (UTF-8) in cwd with extra environment variables; every output line goes to on_line; returns the exit code
int run(const std::vector<std::string> &argv, const fs::path &cwd, const std::vector<std::pair<std::string, std::string>> &env,
        const std::function<void(const std::string &)> &on_line);
// the same with stdout and stderr captured apart (subprocess.run(capture_output=True))
int run_split(const std::vector<std::string> &argv, const fs::path &cwd, const std::vector<std::pair<std::string, std::string>> &env, std::string &out,
              std::string &err);
fs::path exe_dir();                 // the folder of u3import.exe
std::string find_program(const std::vector<fs::path> &dirs, const std::string &name, const std::vector<std::string> &extra = {});

// ------------------------------------------------------------------------------------------------ the parts
struct PS3Inputs;   // recipe.cpp

}  // namespace u3imp
