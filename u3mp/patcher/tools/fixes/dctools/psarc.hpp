// dctools/psarc.hpp: PSARC archives (PS3 / PS4 .psarc) for the C++ recipe steps, and the index of the player's PS3
// Uncharted 3 DC modules by their PS4 path. Same rules as paktools/psarc_lib.py and dcconv/ps3src.py.
//   Psarc     header 'PSAR' (big endian): TOC of entries (md5, first block, 40-bit length, 40-bit offset), the block
//             size table, entry 0 = the name list. Blocks are zlib streams, or stored (size 0 = a whole block, or a
//             block of the expected size without a zlib header).
//   Ps3Index  data2 net18.bin > dlc-patch18 > dlc-patch13 > goty-patch > goty-dlc > disc bin.psarc; every layer kept.
#pragma once
#include "dc.hpp"

#include <fstream>
#include <memory>

namespace u3 {

class Psarc {
public:
    explicit Psarc(const fs::path &path);
    std::vector<std::string> names;   // entry i + 1 is names[i]
    size_t entries() const { return toc.size(); }
    uint64_t length(size_t i) const { return i < toc.size() ? toc[i].length : 0; }   // entry i's size
    Bytes read(size_t i);             // entry i (0 = the name list)

private:
    struct Entry {
        uint32_t first_block;
        uint64_t length, offset;
    };
    std::ifstream f;
    fs::path path;
    uint32_t block_size = 0;
    std::vector<Entry> toc;
    std::vector<uint64_t> blocks;
};

class Ps3Index {
public:
    Ps3Index(const std::string &disc, const std::string &update, const std::string &data2);
    // the module at a PS4 relative path ('dc1/ps4/net.bin'), newest layer first or the given layer; nullopt if none
    std::optional<Bytes> read(const std::string &rel, const std::string &layer = "");
    std::vector<std::string> names() const {   // every PS4 relative path, sorted
        std::vector<std::string> v;
        for (const auto &[k, l] : map) v.push_back(k);
        return v;
    }

private:
    struct Layer {
        std::string layer;
        std::string archive;   // path; empty entry index = a plain file
        size_t entry;
    };
    std::map<std::string, std::unique_ptr<Psarc>> arch;
    std::map<std::string, std::vector<Layer>> map;
};

// 'build/main/bin/dc1/ss-multiplayer/x.bin' -> 'dc1/ps4/ss-multiplayer/x.bin' (empty when not a DC entry)
std::string ps3_to_ps4_rel(const std::string &entry);

}  // namespace u3
