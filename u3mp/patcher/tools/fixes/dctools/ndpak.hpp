// dctools/ndpak.hpp: Uncharted 3 packages (.pak, version 0x0a79), PS3 (big endian, 4-byte pointers) and PS4 (little
// endian, 8-byte pointers). The latest converter's reader (paktools-f525859/ndpak.py) in C++.
//
// Layout: header (0x2c bytes: magic, flags, header size, login item page / offset, page count, page table offset,
// fixup directory count / offset, VRAM size, VRAM descriptor table page / offset), page table {file offset, size, -},
// the fixup directory (page = 0x80000-byte unit of the header block, offset, count) and its entries {u16 src page,
// u16 dst page, u32 offset} (the pointer at src page + offset is relative to dst page), then the pages (a page header
// and 16-byte entries naming each item) and the VRAM texel data that ends the file. An item = 0x20-byte header (name,
// type, data size, ...) and its data. VRAM_DESC items describe the textures.
#pragma once
#include "dc.hpp"

#include <map>
#include <optional>

namespace u3 {

struct PakItem {
    uint32_t page = 0, off = 0;   // item header offset in its page
    std::string name, type;
    uint32_t size = 0;
    bool listed = true;
    uint32_t data_off() const { return off + 0x20; }
};

struct PakTexture {
    uint32_t page = 0, off = 0;
    uint64_t runtime = 0, vramOffset = 0;
    uint32_t vramSize = 0, hash = 0, type = 0;
    uint32_t gcmRemapCtl = 0, gcmFormatCtl = 0, format = 0, unk20 = 0;   // PS3: gcm*, PS4: format, unk20
    uint32_t mips = 0, width = 0, height = 0, depth = 0, unk2c = 0, unk30 = 0, unk34 = 0;
    std::string path;
};

class Pak {
public:
    explicit Pak(const fs::path &path);
    Pak(Bytes data, std::string name);

    Bytes d;
    std::string path;
    bool ps3 = false;
    int ptrsize = 8, pagehdr_size = 0x14;
    uint32_t h[11] = {};   // magic|flags, hdrSize, loginPage, loginOff, pageCt, pageEntryOff, numFixupTables, fixupTableOff, vramSize, vramDescPage, vramDescOff
    struct Page {
        uint32_t file, size, unused;
    };
    std::vector<Page> pages;
    struct FixupDir {
        int32_t page, off, cnt;
    };
    std::vector<FixupDir> fixup_dirs;
    struct Fixup {
        uint16_t src, dst;
        uint32_t off;
    };
    std::vector<Fixup> fixups;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ptrmap;   // (src page, offset) -> dst page (the last fixup wins)
    int64_t vram_base = 0;
    std::vector<PakItem> items;
    struct PageEntries {
        uint32_t size;
        std::vector<std::pair<uint64_t, uint32_t>> entries;
    };
    std::vector<PageEntries> page_entries;
    PakItem login_item;
    std::vector<std::pair<uint32_t, uint32_t>> login;   // (page, offset)

    uint16_t u16(uint64_t o) const;
    uint32_t u32(uint64_t o) const;
    int32_t i32(uint64_t o) const { return int32_t(u32(o)); }
    uint64_t u64(uint64_t o) const;
    uint64_t ptr(uint64_t o) const { return ptrsize == 4 ? u32(o) : u64(o); }
    std::string cstr(uint64_t o) const;   // latin-1 bytes
    uint64_t pbase(uint32_t page) const;
    uint32_t pu32(uint32_t page, uint64_t off) const { return u32(pbase(page) + off); }
    uint64_t pptr(uint32_t page, uint64_t off) const { return ptr(pbase(page) + off); }
    std::string pstr(uint32_t page, uint64_t off) const { return cstr(pbase(page) + off); }

    PakItem item_at(uint32_t page, uint32_t off) const;
    std::vector<PakItem> items_of(const std::string &type) const;
    bool is_ptr(uint32_t page, uint32_t off) const { return ptrmap.count({page, off}) != 0; }
    std::optional<std::pair<uint32_t, uint64_t>> resolve(uint32_t page, uint32_t off) const;   // (dst page, dst offset)
    // the VRAM descriptor table: (page, item offset) of each VRAM_DESC, page nullopt for an unfixed entry
    std::vector<std::pair<std::optional<uint32_t>, uint64_t>> vram_desc_table() const;
    PakTexture vram_desc(uint32_t page, uint64_t data_off) const;
    std::vector<PakTexture> textures() const;
    Bytes vram_bytes(uint64_t off, uint64_t size) const;
    std::vector<std::string> validate() const;

private:
    void parse();
    PakItem item(uint32_t page, uint32_t off, bool listed = true) const;
};

// ndpak.py's summary() (and its --textures lines): the text it prints for a pak
std::string pak_summary(const Pak &p, bool items, bool textures);

}  // namespace u3
