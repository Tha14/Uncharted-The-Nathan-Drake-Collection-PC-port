// importer/overlay.cpp: u2import --overlay, the /app0 overlay the launcher lays over Uncharted 2: the main menu with a
// Multiplayer button. Made on the player's PC from their own PS4 game; nothing game-derived ships with the launcher.
//
//   u2data/build/main/dc1/ps4/menu.bin
//
// The Collection's menu script still holds the multiplayer pages (PAGE_MULTIPLAYER and the rest) and the
// MENU_MULTIPLAYER text, but no button leads there. Two main menus need one:
//   PAGE_NEWPROFILE (no save yet) keeps a hidden button for the multiplayer medals page (BUTTON_MULTIMEDALS, go-to-page
//     with a visibility condition): it becomes BUTTON_MULTIPLAYER (its id), labelled MENU_MULTIPLAYER, a code button
//     (action 0, always visible, no target page): selecting it calls the game's registered BUTTON_MULTIPLAYER callback,
//     which the runtime restores (gow3_pc/src/u2mp/frontend.cpp). The name string stays (dispatch uses the id).
//     PAGE_NEWPROFILE_BETA's copy is changed the same way.
//   PAGE_PROFILE (with saves) gets a new control list appended after the data: Continue, Chapters (hidden),
//     Multiplayer (a copy of that button with this page's button flags), the spacer (a row break), then Options and
//     Bonuses in the left column and New Game and Load Game in the right one, as in the original game.
//
// menu.bin is Naughty Dog DC data ('00CD', version 1): u32 data end at +8; 64-bit pointers are offsets into the file;
// after the data a u32 byte count and a bitmap with one bit per 8-byte slot (least significant bit first), set where
// the slot holds a pointer. A name's id is the CRC-32 of the name (MSB first, polynomial 0x04C11DB7, no initial value,
// no final xor). Page record: +0 name, +8 id, +0x38 pointer to {u64 count, u64 base} of 0x70-byte control records:
// +0 name, +8 id, +0x10 kind (2 button, 1 spacer), +0x14 flags, +0x20 label, +0x28 label id, +0x30 action (20 go to
// page), +0x34 visibility condition (0 always), +0x48 target page, +0x50 its id, +0x58 column (0xffffffff: spacer).
// Every value is checked before anything changes; the source is only read. Rebuilt only when the source changed
// (state.json).
#include "u2import.hpp"

namespace u2imp {
namespace {

const char *MENU = "u2data/build/main/dc1/ps4/menu.bin";
const int VERSION = 5;   // bump when the overlay changes for the same sources (5: Multiplayer is a code button)
const size_t RECORD = 0x70;

uint32_t name_id(const std::string &name) {
    uint32_t crc = 0;
    for (unsigned char b : name) {
        crc ^= uint32_t(b) << 24;
        for (int k = 0; k < 8; k++) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}
uint32_t u32(const Bytes &d, size_t o) {
    if (o + 4 > d.size()) throw std::runtime_error("struct.error: menu.bin");
    return uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24;
}
uint64_t u64(const Bytes &d, size_t o) { return uint64_t(u32(d, o)) | uint64_t(u32(d, o + 4)) << 32; }
void p32(Bytes &d, size_t o, uint32_t v) {
    for (int k = 0; k < 4; k++) d.at(o + size_t(k)) = uint8_t(v >> (8 * k));
}
void p64(Bytes &d, size_t o, uint64_t v) {
    p32(d, o, uint32_t(v));
    p32(d, o + 4, uint32_t(v >> 32));
}
// chr(b).isalnum() for a byte read as latin-1
bool py_alnum(uint8_t b) {
    if (isalnum(b)) return true;
    return b == 0xaa || b == 0xb2 || b == 0xb3 || b == 0xb5 || b == 0xb9 || b == 0xba || (b >= 0xbc && b <= 0xbe) || (b >= 0xc0 && b != 0xd7 && b != 0xf7);
}
std::optional<std::string> cstring(const Bytes &d, uint64_t off) {   // the NUL-terminated ASCII name at off
    if (!(0 < off && off < d.size())) return std::nullopt;
    const size_t lim = std::min<size_t>(off + 96, d.size());
    size_t e = off;
    while (e < lim && d[e]) e++;
    if (e >= lim || e <= off) return std::nullopt;
    std::string s(d.begin() + std::ptrdiff_t(off), d.begin() + std::ptrdiff_t(e));
    for (unsigned char c : s)
        if (c > 0x7f) return std::nullopt;
    return s;
}
// every whole copy of name (NUL-terminated, not the end of a longer name)
std::set<uint64_t> strings(const Bytes &d, const std::string &name) {
    std::set<uint64_t> found;
    std::string needle = name;
    needle.push_back('\0');
    for (auto it = std::search(d.begin(), d.end(), needle.begin(), needle.end()); it != d.end(); it = std::search(it + 1, d.end(), needle.begin(), needle.end())) {
        const size_t at = size_t(it - d.begin());
        const uint8_t before = at ? d[at - 1] : 0;
        if (!(py_alnum(before) || before == '_')) found.insert(at);
    }
    return found;
}
// 8-byte aligned pointers to a copy of name: the records that start with that name
std::vector<size_t> records_named(const Bytes &d, const std::string &name) {
    const std::set<uint64_t> targets = strings(d, name);
    std::vector<size_t> v;
    for (size_t at = 0; at + 0x58 < d.size(); at += 8)
        if (targets.count(u64(d, at))) v.push_back(at);
    return v;
}
std::pair<size_t, size_t> pointer_bitmap(const Bytes &d) {
    if (d.size() < 12 || std::string(d.begin(), d.begin() + 4) != "00CD") throw std::runtime_error("ValueError: menu.bin is not DC data");
    const uint32_t end = u32(d, 8), size = u32(d, end);
    if (size_t(end) + 4 + size != d.size() || uint64_t(size) * 8 < end / 8) throw std::runtime_error("ValueError: menu.bin's pointer bitmap is not where expected");
    return {size_t(end) + 4, size};
}
bool is_pointer(const Bytes &d, size_t bitmap, size_t off) { return d.at(bitmap + off / 8 / 8) >> (off / 8 % 8) & 1; }
void clear_pointer_bit(Bytes &d, size_t bitmap, size_t off) { d.at(bitmap + off / 8 / 8) &= uint8_t(~(1 << (off / 8 % 8))); }

std::pair<uint64_t, uint64_t> page_controls(const Bytes &d, const std::string &page) {
    std::vector<std::pair<uint64_t, uint64_t>> found;
    for (size_t r : records_named(d, page)) {
        if (u32(d, r + 8) != name_id(page)) continue;
        const uint64_t a = u64(d, r + 0x38);
        if (0 < a && a + 16 < d.size()) found.push_back({u64(d, a), u64(d, a + 8)});
    }
    if (found.size() != 1) throw std::runtime_error(fmt("ValueError: expected one %s page, found %zu", page.c_str(), found.size()));
    return found[0];
}

size_t patch_profile_page(Bytes &d, size_t button) {
    const auto [bitmap, bsz] = pointer_bitmap(d);
    (void)bsz;
    const uint32_t end = u32(d, 8);
    std::vector<size_t> pages;
    for (size_t r : records_named(d, "PAGE_PROFILE"))
        if (u32(d, r + 8) == name_id("PAGE_PROFILE")) pages.push_back(r);
    if (pages.size() != 1) throw std::runtime_error(fmt("ValueError: expected one PAGE_PROFILE page, found %zu", pages.size()));
    const size_t page = pages[0];
    if (!is_pointer(d, bitmap, page + 0x38)) throw std::runtime_error("ValueError: PAGE_PROFILE's control list pointer is not a pointer");
    const auto [count, base] = page_controls(d, "PAGE_PROFILE");
    if (count != 7) throw std::runtime_error(fmt("ValueError: PAGE_PROFILE has %llu controls, expected 7", (unsigned long long)count));
    const std::vector<std::optional<std::string>> want = {"BUTTON_CONTINUE", "BUTTON_PROFILE_CHAPTERS", std::nullopt, "BUTTON_NEWGAME", "BUTTON_LOADGAME",
                                                          "BUTTON_OPTIONS", "BUTTON_BONUSES"};
    for (size_t i = 0; i < 7; i++)
        if (cstring(d, u64(d, base + i * RECORD)) != want[i]) throw std::runtime_error("ValueError: unexpected PAGE_PROFILE controls");
    const size_t spacer = base + 2 * RECORD;
    bool sp_ptr = false;
    for (size_t f = 0; f < RECORD; f += 8) sp_ptr = sp_ptr || is_pointer(d, bitmap, spacer + f);
    if (u32(d, spacer + 0x10) != 1 || u32(d, spacer + 0x14) != 0 || u32(d, spacer + 0x58) != 0xffffffffu || sp_ptr)
        throw std::runtime_error("ValueError: PAGE_PROFILE's spacer is not as expected");
    for (const auto &[index, column] : std::vector<std::pair<size_t, uint32_t>>{{3, 0}, {4, 0}, {5, 1}, {6, 1}})
        if (u32(d, base + index * RECORD + 0x58) != column) throw std::runtime_error("ValueError: PAGE_PROFILE's columns are not as expected");
    struct Rec {
        Bytes b;
        std::vector<size_t> fields;   // pointer fields
    };
    auto old = [&](size_t index, std::optional<uint32_t> col = std::nullopt) {
        const size_t at = base + index * RECORD;
        Rec r{Bytes(d.begin() + std::ptrdiff_t(at), d.begin() + std::ptrdiff_t(at + RECORD)), {}};
        if (col) p32(r.b, 0x58, *col);
        for (size_t f = 0; f < RECORD; f += 8)
            if (is_pointer(d, bitmap, at + f)) r.fields.push_back(f);
        return r;
    };
    Rec mp{Bytes(d.begin() + std::ptrdiff_t(button), d.begin() + std::ptrdiff_t(button + RECORD)), {0x00, 0x20}};
    p32(mp.b, 0x14, u32(d, base + 3 * RECORD + 0x14));
    p32(mp.b, 0x58, 0);
    const std::vector<Rec> records = {old(0), old(1), mp, old(2), old(5, 0), old(6, 0), old(3, 1), old(4, 1)};
    // the new list: {u64 count, u64 base} then the records, 16-byte aligned after the old data
    const size_t start = (size_t(end) + 15) & ~size_t(15), header = start, first = header + 16;
    Bytes blob(start - end, 0);
    blob.resize(blob.size() + 16, 0);
    p64(blob, start - end, records.size());
    p64(blob, start - end + 8, first);
    std::vector<size_t> pointers = {header + 8};
    for (size_t i = 0; i < records.size(); i++) {
        blob.insert(blob.end(), records[i].b.begin(), records[i].b.end());
        for (size_t f : records[i].fields) pointers.push_back(first + i * RECORD + f);
    }
    const size_t new_end = end + blob.size(), slots = (new_end + 7) / 8;
    Bytes bits((slots + 7) / 8, 0);
    const Bytes old_bits(d.begin() + std::ptrdiff_t(bitmap), d.end());
    if (old_bits.size() > bits.size()) bits.resize(old_bits.size(), 0);   // (Python's slice assignment would grow it)
    std::copy(old_bits.begin(), old_bits.end(), bits.begin());
    for (size_t off : pointers) bits.at(off / 64) |= uint8_t(1 << (off / 8 % 8));
    Bytes rebuilt(d.begin(), d.begin() + end);
    rebuilt.insert(rebuilt.end(), blob.begin(), blob.end());
    p32(rebuilt, 8, uint32_t(new_end));
    p64(rebuilt, page + 0x38, header);
    for (int k = 0; k < 4; k++) rebuilt.push_back(uint8_t(bits.size() >> (8 * k)));
    rebuilt.insert(rebuilt.end(), bits.begin(), bits.end());
    d = rebuilt;
    return header;
}

size_t patch_menu(Bytes &d) {
    if (strings(d, "PAGE_MULTIPLAYER").empty()) throw std::runtime_error("ValueError: menu.bin has no PAGE_MULTIPLAYER (not Uncharted 2?)");
    const auto [bitmap, bsz] = pointer_bitmap(d);
    (void)bsz;
    std::vector<size_t> done;
    for (size_t r : records_named(d, "BUTTON_MULTIMEDALS")) {
        if (u32(d, r + 8) != name_id("BUTTON_MULTIMEDALS") || cstring(d, u64(d, r + 0x48)) != std::optional<std::string>("PAGE_MULTIMEDALS") ||
            u32(d, r + 0x50) != name_id("PAGE_MULTIMEDALS") || !is_pointer(d, bitmap, r + 0x48))
            continue;
        p64(d, r + 8, name_id("BUTTON_MULTIPLAYER"));
        p64(d, r + 0x28, name_id("MENU_MULTIPLAYER"));
        p32(d, r + 0x30, 0);   // a code button, always visible
        p32(d, r + 0x34, 0);
        p64(d, r + 0x48, 0);   // no target page (and a null target is not relocated)
        p64(d, r + 0x50, 0);
        clear_pointer_bit(d, bitmap, r + 0x48);
        done.push_back(r);
    }
    if (done.empty()) throw std::runtime_error("ValueError: menu.bin has no hidden medals button (not the Uncharted 2 1.02 menu?)");
    patch_profile_page(d, done[0]);
    return done.size() + 1;
}

}  // namespace

// the overlay folder `out` (made or kept); throws with the reason
fs::path build_overlay(const fs::path &game, const fs::path &out, const std::optional<fs::path> &update) {
    std::optional<fs::path> menu;
    std::error_code ec;
    for (const auto &root : {update, std::optional<fs::path>(game)}) {
        if (!root || menu) continue;
        for (const fs::path &base : {*root, *root / "uroot"})
            if (fs::is_regular_file(base / P(MENU), ec)) {
                menu = base / P(MENU);
                break;
            }
    }
    if (!menu) throw std::runtime_error(std::string("FileNotFoundError: ") + MENU + " not found in the game");
    const auto t = fs::last_write_time(*menu, ec);
    const long long mtime = (long long)std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    const std::string want = dumps(obj({{"version", num(VERSION)}, {"menu", [&] {
                                             Value a = Value::object();
                                             a.kind = Value::Array;
                                             a.items = {str(S(*menu)), num((long long)fs::file_size(*menu)), num(mtime)};
                                             return a;
                                         }()}}));
    const fs::path state = out / "state.json";
    if (fs::is_regular_file(state, ec) && read_text(state) == want && fs::is_regular_file(out / P(MENU), ec)) return out;
    Bytes data = read_bytes(*menu);
    const size_t n = patch_menu(data);
    const fs::path target = out / P(MENU), tmp = S(target) + ".tmp";
    fs::create_directories(target.parent_path(), ec);
    write_bytes(tmp, data);
    fs::rename(tmp, target, ec);
    if (ec) throw std::runtime_error("cannot write " + S(target) + ": " + ec.message());
    write_text(state, want);
    log(fmt("u2mp: Multiplayer button in the main menu (%zu records) -> %s", n, S(target).c_str()));
    return out;
}

}  // namespace u2imp
