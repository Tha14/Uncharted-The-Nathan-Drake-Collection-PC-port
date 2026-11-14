// dctools/dc.hpp: Naughty Dog DC modules for the C++ recipe steps (u3fix.exe).
//   Dc      reader of a DC file: PS3 'DC00' (big endian, 4-byte pointers) or PS4 '00CD' (little endian, 8-byte
//           pointers); symbols (sid, type, offset) and the relocation bitmap (which words are pointers).
//   DcEdit  append-only editor of a PS4 DC: the original bytes [0, reloc) stay, new objects go after them, and every
//           pointer slot gets its bit in the rebuilt bitmap (one bit per 8-byte word, LSB first).
// Same rules as the dctools Python reader and editor that the older steps still use.
#pragma once
#include <algorithm>   // newer libc++ no longer brings these in through other headers (the CI build failed on it)
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace u3 {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

// the game the converters write for: Uncharted 3 (u3fix.exe), or Uncharted 2 when u2conv.exe sets g_u2 at its start
// (before any thread). U2 differs in the PS4 texture cook path and in some item layouts (u2mp/docs/IMPORT.md).
inline bool g_u2 = false;
inline const std::string &cook_prefix() {
    static const std::string u3 = "d:/dev/CP9/Data/U3/CookData/ND/build/texture2/", u2 = "d:/dev/CP9/Data/U2/CookData/ND/build/texture2/";
    return g_u2 ? u2 : u3;
}

// a step stops with this message (exit 1, nothing written)
struct Stop : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string fmt(const char *f, ...);            // printf into a std::string
std::string hex8(uint32_t v);                   // "%08x"
std::string utf8(const fs::path &p);            // a path for messages
Bytes read_file(const fs::path &p);
void write_file_replace(const fs::path &p, const Bytes &data);   // <p>.tmp, then replaces p
void copy_once(const fs::path &src, const fs::path &dst);       // backup: only when dst does not exist yet
std::string sha256_hex(const Bytes &data);
uint32_t sid(const std::string &s);             // CRC32 poly 0x04C11DB7 MSB-first, init 0 (string id)

inline uint32_t rd32le(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t rd64le(const uint8_t *p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline void wr32le(uint8_t *p, uint32_t v) { std::memcpy(p, &v, 4); }
inline void wr64le(uint8_t *p, uint64_t v) { std::memcpy(p, &v, 8); }

struct Sym {
    uint32_t sid, type;
    uint64_t off;
};

class Dc {
public:
    explicit Dc(const fs::path &path);
    Dc(Bytes data, std::string name);

    Bytes d;
    std::string name;
    bool be = false;
    int ps = 8;                                 // pointer size
    uint64_t reloc = 0;
    std::vector<Sym> syms;
    std::map<uint32_t, std::pair<uint32_t, uint64_t>> bysid;   // the last symbol of a sid wins
    std::set<uint64_t> ptrs;                    // file offsets of pointer words

    uint32_t u32(uint64_t o) const;
    uint64_t ptr(uint64_t o) const;
    bool isptr(uint64_t o) const { return ptrs.count(o) != 0; }
    std::optional<std::string> cstr(uint64_t o) const;   // nullopt for 0 or past the end
    std::string str_or_empty(uint64_t o) const { auto s = cstr(o); return s ? *s : std::string(); }

private:
    void parse();
};

class DcEdit {
public:
    explicit DcEdit(const fs::path &path);

    Bytes orig;
    Bytes d;                                    // [0, reloc) of the original, then appended data
    uint64_t reloc = 0;
    std::set<uint64_t> ptrs;
    std::vector<std::pair<uint64_t, int>> patched;   // in-place changes to original bytes (offset, length)

    uint32_t u32(uint64_t o) const { check(o, 4); return rd32le(&d[o]); }
    void put32(uint64_t o, uint32_t v);         // records an original-byte change
    void putptr(uint64_t o, uint64_t target);   // sets the pointer bit
    void raw32(uint64_t o, uint32_t v) { check(o, 4); wr32le(&d[o], v); }   // new data, nothing recorded
    void align(size_t a) { while (d.size() % a) d.push_back(0); }
    uint64_t alloc(size_t size, size_t align_to = 16);
    uint64_t add_string(const std::string &s);
    void copy_words(uint64_t src, uint64_t dst, size_t size);   // original bytes, with their pointer bits
    Bytes build();

private:
    void check(uint64_t o, size_t n) const;
};

// file offset of the u64 .bin size word of module `name` in modules.bin (by name: list type 0x61183179,
// entries 0x28 bytes: name pointer +0, size +0x10)
uint64_t module_size_slot(const Dc &modules, const std::string &name);

}  // namespace u3
