// importer/netpatch.cpp: dc1/ps4/net-patch9.bin, the PS3 server live-data DC module (weapon sets the PS4 net.bin names
// but does not define, playlists ...), converted to the PS4 format. PS3 1.10 downloaded it into the game-data folder
// and bound its symbols at net start; the runtime (gow3_pc/src/u2mp/netpatch.cpp) loads the converted file the same way.
//
// The conversion is the Uncharted 3 DC converter (dctools dcconv: the layout schema learned from the game's own PS3 /
// PS4 module pairs) with the Uncharted 2 module format (u3::g_u2):
//   PS3 'DC00' (big endian): +8 u32 reloc, +0x10 u32 root, the root's type word 0x916aa605 before it, root = {u32 1,
//                            u32 count, {u32 sid, u32 ptr}[count]}; at reloc a list of pointer slot offsets
//   PS4 '00CD' (little endian): +8 u64 reloc, +0x10 u64 root (a slot), +0x18 [u32 0][u32 0x916aa605], +0x20 root {u32 1,
//                            u32 count, {u32 sid, u32 0, u64 ptr}[count]}; at reloc {u32 byte count, a bitmap: bit k of
//                            byte i marks the 8-byte slot (8i + k) * 8}
// The schema is learned from every PS3 dc1/<x>.bin that has a PS4 dc1/ps4/<x>.bin (pairs whose graphs differ are learned
// partially) plus the PS3 net-patch9 against the PS4 net.bin (the PS4 tables are a later version of it). The converter
// writes the U3 layout (symbol table at 0x18); it is moved 0x10 bytes up into the U2 header (alignment is kept: 0x18 + 16n
// and 0x28 + 16n are equal modulo 16). The schema is kept in work/dc (it holds layouts and an opcode map only).
#include "dcconv.hpp"
#include "u2conv.hpp"

#include <cstring>

namespace u2imp {

const char *NETPATCH_VERSION = "u2dc1";   // bump when the schema or the conversion changes

namespace {

const uint32_t ROOT_TYPE = 0x916aa605;

uint32_t le32(const Bytes &d, size_t o) { return uint32_t(d.at(o)) | uint32_t(d.at(o + 1)) << 8 | uint32_t(d.at(o + 2)) << 16 | uint32_t(d.at(o + 3)) << 24; }
uint64_t le64(const Bytes &d, size_t o) { return uint64_t(le32(d, o)) | uint64_t(le32(d, o + 4)) << 32; }
uint32_t be32(const Bytes &d, size_t o) { return uint32_t(d.at(o)) << 24 | uint32_t(d.at(o + 1)) << 16 | uint32_t(d.at(o + 2)) << 8 | d.at(o + 3); }
void put32(Bytes &d, size_t o, uint32_t v) {
    for (int k = 0; k < 4; k++) d.at(o + size_t(k)) = uint8_t(v >> (8 * k));
}
void put64(Bytes &d, size_t o, uint64_t v) {
    put32(d, o, uint32_t(v));
    put32(d, o + 4, uint32_t(v >> 32));
}

// the converter's output (U3 header, 16-byte symbol entries at 0x18) as a U2 PS4 module (0x10 bytes further up)
Bytes u3_to_u2(const Bytes &out3, const Bytes &ps3) {
    const uint64_t root3 = be32(ps3, 0x10);
    const uint32_t root_hdr = be32(ps3, size_t(root3 - 4)), root_word0 = be32(ps3, size_t(root3)), n = be32(ps3, size_t(root3 + 4));
    const uint64_t reloc3 = le64(out3, 8);
    const uint32_t nb3 = le32(out3, size_t(reloc3));
    const uint64_t ds3 = 0x18 + 16ull * n, delta = 0x10, reloc = reloc3 + delta;
    std::vector<uint64_t> slots3;
    for (uint32_t bi = 0; bi < nb3; bi++) {
        const uint8_t b = out3.at(size_t(reloc3 + 4 + bi));
        for (int k = 0; k < 8; k++)
            if (b >> k & 1) slots3.push_back((uint64_t(bi) * 8 + uint64_t(k)) * 8);
    }
    Bytes out(size_t(reloc), 0);
    out[0] = '0', out[1] = '0', out[2] = 'C', out[3] = 'D';
    put32(out, 4, 1);
    put64(out, 8, reloc);
    put64(out, 0x10, 0x20);
    put32(out, 0x18, 0);
    put32(out, 0x1c, root_hdr);
    put32(out, 0x20, root_word0);
    put32(out, 0x24, n);
    std::vector<uint64_t> slots = {0x10};
    for (uint32_t i = 0; i < n; i++) {
        const size_t o3 = 0x18 + 16 * size_t(i), o = 0x28 + 16 * size_t(i);
        const uint64_t v = le64(out3, o3 + 8);
        put32(out, o, le32(out3, o3));
        put32(out, o + 4, 0);
        put64(out, o + 8, v ? v + delta : 0);
        slots.push_back(o + 8);
    }
    std::copy(out3.begin() + std::ptrdiff_t(ds3), out3.begin() + std::ptrdiff_t(reloc3), out.begin() + std::ptrdiff_t(ds3 + delta));
    for (uint64_t p : slots3) {
        if (p < ds3) continue;   // the symbol entries' slots: written above
        const uint64_t q = p + delta, v = le64(out, size_t(q));
        put64(out, size_t(q), v ? v + delta : 0);
        slots.push_back(q);
    }
    const uint64_t nb = (reloc + 63) / 64;
    Bytes bm(size_t(nb), 0);
    for (uint64_t p : slots) {
        const uint64_t w = p / 8;
        bm.at(size_t(w / 8)) |= uint8_t(1 << (w % 8));
    }
    for (int k = 0; k < 4; k++) out.push_back(uint8_t(nb >> (8 * k)));
    out.insert(out.end(), bm.begin(), bm.end());
    return out;
}

// a check of the converted module: header, root, symbols sorted, every pointer inside the module; (symbols, pointers)
std::pair<size_t, size_t> check_module(const Bytes &d) {
    if (d.size() < 0x28 || std::string(d.begin(), d.begin() + 4) != "00CD") throw std::runtime_error("ValueError: not a PS4 module");
    const uint64_t reloc = le64(d, 8), root = le64(d, 0x10);
    if (root != 0x20 || le32(d, 0x1c) != ROOT_TYPE) throw std::runtime_error(fmt("ValueError: unexpected root %#llx / type %#x", (unsigned long long)root, le32(d, 0x1c)));
    const uint32_t n = le32(d, 0x24), nb = le32(d, size_t(reloc));
    std::vector<uint64_t> slots;
    for (uint32_t bi = 0; bi < nb; bi++) {
        const uint8_t b = d.at(size_t(reloc + 4 + bi));
        for (int k = 0; k < 8; k++)
            if (b >> k & 1) slots.push_back((uint64_t(bi) * 8 + uint64_t(k)) * 8);
    }
    size_t bad = 0;
    for (uint64_t s : slots) {
        if (s >= reloc) {
            bad++;
            continue;
        }
        const uint64_t v = le64(d, size_t(s));
        if (!(s == 0x10 || (root <= v && v < reloc) || v == 0)) bad++;
    }
    if (bad) throw std::runtime_error(fmt("ValueError: %zu pointer slots outside the module", bad));
    for (uint32_t i = 1; i < n; i++)
        if (le32(d, 0x28 + 16 * size_t(i)) < le32(d, 0x28 + 16 * size_t(i - 1))) throw std::runtime_error("ValueError: symbol table is not sorted");
    return {n, slots.size()};
}

}  // namespace

Bytes convert_net_patch(const Context &c, const Bytes &data, std::string &note) {
    if (data.size() < 4 || std::string(data.begin(), data.begin() + 4) != "DC00") throw std::runtime_error("ValueError: input is not a PS3 module");
    // the training pairs: every PS4 dc1/ps4/<x>.bin with a PS3 dc1/<x>.bin (newest source)
    const fs::path dcdir = c.ps4->main / "dc1" / "ps4";
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(dcdir, ec))
        if (ends(S(e.path().filename()), ".bin")) names.push_back(S(e.path().filename()));
    std::sort(names.begin(), names.end());
    std::vector<std::tuple<std::string, Bytes, Bytes>> pairs, partial;
    Sha256 h;
    h.update(NETPATCH_VERSION, std::strlen(NETPATCH_VERSION));
    for (const std::string &f : names) {
        const auto cands = c.ps3->candidates("dc1/" + f);
        if (cands.empty()) continue;
        Bytes d3 = cands[0]->read(), d4 = read_bytes(dcdir / P(f));
        const std::string k = fmt("%s %zu %zu;", f.c_str(), d3.size(), d4.size());
        h.update(k.data(), k.size());
        pairs.push_back({f, std::move(d3), std::move(d4)});
    }
    if (fs::is_regular_file(dcdir / "net.bin", ec)) {
        Bytes d4 = read_bytes(dcdir / "net.bin");
        const std::string k = "net-patch9.bin~net.bin " + sha1_hex(data) + fmt(" %zu;", d4.size());
        h.update(k.data(), k.size());
        partial.push_back({"net-patch9.bin~net.bin", data, std::move(d4)});
    }
    const fs::path cache = c.work / "dc" / ("u2dc-schema-" + h.hex().substr(0, 12) + ".json");
    std::string schema;
    if (fs::is_regular_file(cache, ec)) schema = read_text(cache);
    else {
        log(fmt("u2dc: learning the DC layout schema from %zu PS3/PS4 module pairs", pairs.size() + partial.size()));
        schema = u3::dcconv::learn_u2(pairs, partial, [](const std::string &s) { log(s); });
        fs::create_directories(cache.parent_path(), ec);
        write_text(S(cache) + ".tmp", schema);
        fs::rename(S(cache) + ".tmp", cache, ec);
    }
    u3::dcconv::Counts rep;
    const Bytes out = u3_to_u2(u3::dcconv::Converter({schema}).convert(data, &rep), data);
    const auto [nsym, nptr] = check_module(out);
    // the fallbacks among the report's events, in the order they first occurred
    static const std::set<std::string> FALLBACKS = {"key-unknown", "layout-extend-failed", "slot-not-ptr", "ptr-target-missing", "sym-target-missing"};
    std::string fb;
    for (const std::string &k : rep.order)
        if (FALLBACKS.count(k) && rep.n.at(k)) fb += (fb.empty() ? "" : ", ") + fmt("'%s': %lld", k.c_str(), (long long)rep.n.at(k));
    note = fmt("%zu symbols, %zu pointers; fallbacks %s", nsym, nptr, fb.empty() ? "none" : ("{" + fb + "}").c_str());
    return out;
}

}  // namespace u2imp
