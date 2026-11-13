// dctools/ps4writer.hpp: builder of PS4 Uncharted 3 packages (version 0x0a79, little endian, 64-bit page-relative
// pointers and a fixup table). The latest converter's ps4writer.py (paktools-f525859) in C++; the layout rules are the
// ones described in ndpak.hpp.
//
//   PakWriter w; Page &p = w.page(); auto it = p.item("RAW_DATA", blob); blob->ptr(other, off);
//   make_login_and_vram_page(w, w.page(), login_items, vram_items); w.vram = ...; Bytes pak = w.build();
//
// A Blob is bytes plus pointer relocations (8-byte pointers to another blob + offset, resolved at build time to the
// target's page offset with a fixup entry); blobs can be embedded in other blobs. Pages hold items (a 0x20-byte header,
// the data, then the type and name strings for listed items), raw spans copied from another PS4 page, or (RawPage)
// a whole retail page kept as it is.
#pragma once
#include "dc.hpp"

#include <memory>
#include <optional>
#include <variant>

namespace u3 {

// something a pointer can target: a Blob (its root after embedding) or a RawPage
struct PtrTarget {
    virtual ~PtrTarget() = default;
    virtual std::pair<const void *, int64_t> root() const = 0;   // (root key, offset of this inside the root)
    std::string name;
};

class Blob : public PtrTarget {
public:
    explicit Blob(std::string n = "") { name = std::move(n); }
    Bytes b;
    struct Reloc {
        size_t pos;
        std::shared_ptr<PtrTarget> target;
        int64_t off;
    };
    std::vector<Reloc> relocs;
    Blob *parent = nullptr;
    size_t off_in_parent = 0;

    size_t size() const { return b.size(); }
    Blob &u8(uint32_t v);
    Blob &u16(uint32_t v);
    Blob &u32(uint64_t v);   // the low 32 bits
    Blob &i32(int32_t v);
    Blob &u64(uint64_t v);
    Blob &raw(const Bytes &d);
    Blob &raw(const std::string &d);
    Blob &pad(size_t a, uint8_t fill = 0);
    Blob &ptr(const std::shared_ptr<PtrTarget> &target, int64_t off = 0);   // null target: a zero pointer, no fixup
    size_t cstr(const std::string &s);                                    // appends s + NUL, returns its offset
    size_t embed(const std::shared_ptr<Blob> &child, size_t a = 16, uint8_t fill = 0);
    std::pair<const void *, int64_t> root() const override;
};

class StrPool {
public:
    explicit StrPool(std::shared_ptr<Blob> blob) : blob(std::move(blob)) {}
    size_t get(const std::string &s);
    std::shared_ptr<Blob> blob;
private:
    std::map<std::string, size_t> map;
};

class Page;
struct PakWItem {
    std::string type, name;
    std::shared_ptr<Blob> data;
    Page *page = nullptr;
    size_t off = 0;   // item header offset in its page
    bool listed = true;
    std::optional<std::pair<size_t, size_t>> phase;   // (modulo, required data offset % modulo)
    // a login target that is an item inside a RawSpan (an effect span copied from a retail pak): the span and the
    // item's offset in it (page and off then follow the span)
    std::shared_ptr<struct RawSpan> span_ref;
    size_t span_pos = 0;
    size_t data_off() const { return off + 0x20; }
};

struct RawSpan {
    Bytes data;
    std::vector<std::pair<size_t, uint64_t>> internal;     // (pos, target pos) pointer fields inside the span
    std::vector<std::pair<size_t, std::string>> headers;   // (item pos, type) ResItems inside the span
    std::vector<std::pair<size_t, size_t>> entries;        // (item pos, name pos) page entries to add
    uint64_t modulo = 0x1000, phase = 0;
    Page *page = nullptr;
    size_t off = 0;
};

class PakWriter;
class Page {
public:
    Page(PakWriter *w, size_t index) : w(w), index(index) {}
    virtual ~Page() = default;
    PakWriter *w;
    size_t index;
    std::vector<std::variant<std::shared_ptr<PakWItem>, std::shared_ptr<RawSpan>>> parts;
    size_t size = 0;
    Bytes bytes;
    struct Fix {
        size_t src, dst;
        uint64_t off;
    };
    std::vector<Fix> fixups;
    std::map<const void *, std::pair<Page *, size_t>> blob_abs;   // root key -> (page, offset in the page)

    std::shared_ptr<PakWItem> item(const std::string &type, std::shared_ptr<Blob> data = nullptr, const std::string &name = "", bool listed = true);
    std::shared_ptr<RawSpan> span(std::shared_ptr<RawSpan> s);
};

class RawPage : public Page, public PtrTarget {
public:
    RawPage(PakWriter *w, size_t index, Bytes data, std::vector<std::pair<uint64_t, size_t>> fixups);
    Bytes data;
    std::vector<std::pair<uint64_t, size_t>> src_fixups;   // (offset, dst page)
    struct Redirect {
        uint64_t off;
        std::shared_ptr<PtrTarget> target;
        int64_t toff;
    };
    std::vector<Redirect> redirects;
    std::vector<std::shared_ptr<Blob>> appended;   // blobs after the retail data (unlisted, no page entry)
    std::shared_ptr<Blob> append(std::shared_ptr<Blob> b) { appended.push_back(b); return b; }
    void redirect(uint64_t off, std::shared_ptr<PtrTarget> target, int64_t toff = 0) { redirects.push_back({off, std::move(target), toff}); }
    void set_u32(uint64_t off, uint32_t v);
    void set_u64(uint64_t off, uint64_t v);
    std::pair<const void *, int64_t> root() const override { return {static_cast<const PtrTarget *>(this), 0}; }
};

class PakWriter {
public:
    std::vector<std::unique_ptr<Page>> pages;
    Bytes vram;
    std::shared_ptr<PakWItem> login_item;
    std::vector<std::shared_ptr<PakWItem>> login_targets;
    std::shared_ptr<PakWItem> vram_table_item;
    uint16_t flags = 0;
    std::optional<std::pair<size_t, size_t>> login_page, vram_table_loc;   // set by the caller when the tables are in a RawPage
    std::shared_ptr<Blob> vram_table_blob;   // or: the VRAM desc table is this blob appended to a RawPage (found after layout)
    std::vector<Page::Fix> fixups;

    Page &page();
    RawPage &raw_page(Bytes data, std::vector<std::pair<uint64_t, size_t>> fixups);
    Bytes build();

private:
    void layout(Page &p);
    std::map<const void *, std::pair<Page *, size_t>> all_abs() const;
};

// page layout of the PS4 build: the PAK_LOGIN_TABLE body, then the VRAM_DESC table (unlisted items)
std::shared_ptr<PakWItem> make_login_and_vram_page(PakWriter &w, Page &page, const std::vector<std::shared_ptr<PakWItem>> &login_items,
                                                   const std::vector<std::shared_ptr<PakWItem>> &vram_items);

}  // namespace u3
