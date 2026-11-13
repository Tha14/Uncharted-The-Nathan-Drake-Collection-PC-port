// dctools/vischeat.cpp: PS3 -> PS4 converter of Uncharted 3 '-killvis' level packages (item VIS_CHEAT, the multiplayer
// out-of-bounds voxel grid; the latest converter's vischeat.py). The PS4 game ships every killvis pak as an empty stub;
// the multiplayer helper reads the item in this layout (u3_lan_profiles.c vis_cheat_corners).
//
// PS3 (big endian, 4-byte pointers)                    PS4 (little endian, 8-byte pointers)
//  +00 u32 type (1 = no origin, 2 = with origin)        same
//  +04 i32 min / max x, y, z (cells, max exclusive)     same
//  +1c u8 axis0, axis1, axis2, pad                      same
//  +20 f32 scale x, y, z, pad; type 2: +30 origin       same (byte-swapped)
//  +H ptr runs, +H+4 u32 slice count, +H+8 slices[]     +H ptr runs, +H+8 count, 0, +H+0x10 slices[]
//  runs {u32 n, ptr rows[n]}                            {u32 n, u32 0, ptr rows[n]}
//  slice {u16 n, u16 entry[n]} (entry = rep<<14|row)    same, little endian
//  row: u8 value, u8 run lengths (bytes unchanged). Rows and slices shared by several pointers stay shared.
#include "vischeat.hpp"

#include "ndpak.hpp"
#include "ps4writer.hpp"
#include "u3fix.hpp"

namespace u3 {
namespace {

std::string utf8_latin1(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x80) o += char(c);
        else { o += char(0xc0 | c >> 6); o += char(0x80 | (c & 0x3f)); }
    }
    return o;
}

struct Src {
    const Pak &p;
    uint32_t pg;
    uint64_t hb;
    uint8_t u8(uint64_t o) const {
        if (hb + o >= p.d.size()) throw Stop("IndexError: VIS_CHEAT read past the end");
        return p.d[hb + o];
    }
    uint16_t u16(uint64_t o) const { return p.u16(hb + o); }
    uint32_t u32(uint64_t o) const { return p.u32(hb + o); }
    int32_t i32(uint64_t o) const { return p.i32(hb + o); }
    Bytes f32b(uint64_t o) const { return Bytes{u8(o + 3), u8(o + 2), u8(o + 1), u8(o)}; }   // BE float -> LE bytes
    std::optional<uint64_t> ptr(uint64_t o) const {
        auto r = p.resolve(pg, uint32_t(o));
        if (!r) return std::nullopt;
        if (r->first != pg) throw Stop("ValueError: VIS_CHEAT pointer to another page");
        return r->second;
    }
    uint64_t need(uint64_t o) const {
        auto r = ptr(o);
        if (!r) throw Stop("TypeError: VIS_CHEAT null pointer");
        return *r;
    }
};

uint64_t row_len(const Src &s, uint64_t o, int64_t rowlen) {   // bytes of one RLE row: value byte + run bytes covering rowlen
    uint64_t n = 1;
    int64_t left = rowlen;
    while (left > 0) {
        left -= s.u8(o + n);
        n++;
    }
    if (left != 0) throw Stop("ValueError: row overruns its length");
    return n;
}

std::shared_ptr<Blob> convert_item(const Src &s, const PakItem &it) {
    const uint64_t g = it.data_off();
    const uint32_t t = s.u32(g);
    if (t != 1 && t != 2) throw Stop(fmt("ValueError: VIS_CHEAT type %u", t));
    const uint64_t H = t == 2 ? 0x40 : 0x30;
    int64_t mn[3], mx[3], dim[3];
    uint8_t ax[3];
    for (int i = 0; i < 3; i++) {
        mn[i] = s.i32(g + 4 + 8 * i);
        mx[i] = s.i32(g + 8 + 8 * i);
        ax[i] = s.u8(g + 0x1c + i);
        dim[i] = mx[i] - mn[i];
    }
    if (ax[0] > 2 || ax[1] > 2 || ax[2] > 2) throw Stop("IndexError: VIS_CHEAT axis");
    const int64_t rowlen = dim[ax[0]];
    const uint64_t runs = s.need(g + H);
    const uint32_t nsl = s.u32(g + H + 4);
    if (int64_t(nsl) != dim[ax[2]]) throw Stop(fmt("ValueError: slice count %u != dim %lld", nsl, (long long)dim[ax[2]]));
    auto root = std::make_shared<Blob>("VIS_CHEAT");
    root->u32(t);
    for (int i = 0; i < 6; i++) root->i32(s.i32(g + 4 + 4 * i));
    root->raw(Bytes{ax[0], ax[1], ax[2], 0});
    for (int i = 0; i < 3; i++) root->raw(s.f32b(g + 0x20 + 4 * i));
    root->u32(0);
    if (t == 2) {
        for (int i = 0; i < 3; i++) root->raw(s.f32b(g + 0x30 + 4 * i));
        root->u32(0);
    }
    const uint32_t nrows = s.u32(runs);
    auto rows_blob = std::make_shared<Blob>("rows");
    std::map<uint64_t, size_t> row_at;
    auto rt = std::make_shared<Blob>("runs");
    rt->u32(nrows).u32(0);
    for (uint32_t i = 0; i < nrows; i++) {
        const uint64_t ro = s.need(runs + 4 + 4ull * i);
        if (!row_at.count(ro)) {
            const uint64_t n = row_len(s, ro, rowlen);
            row_at[ro] = rows_blob->size();
            for (uint64_t k = 0; k < n; k++) rows_blob->u8(s.u8(ro + k));
        }
        rt->ptr(rows_blob, int64_t(row_at[ro]));
    }
    auto sl_blob = std::make_shared<Blob>("slices");
    std::map<uint64_t, size_t> sl_at;
    std::vector<size_t> slice_ptrs;
    for (uint32_t k = 0; k < nsl; k++) {
        const uint64_t so = s.need(g + H + 8 + 4ull * k);
        if (!sl_at.count(so)) {
            const uint16_t n = s.u16(so);
            int64_t rows = 0;
            sl_blob->pad(2);
            sl_at[so] = sl_blob->size();
            sl_blob->u16(n);
            for (uint32_t j = 0; j < n; j++) {
                const uint16_t e = s.u16(so + 2 + 2ull * j);
                if ((e & 0x3fffu) >= nrows) throw Stop(fmt("ValueError: slice entry row %u >= %u", e & 0x3fffu, nrows));
                rows += (e >> 14) + 1;
                sl_blob->u16(e);
            }
            if (rows != dim[ax[1]]) throw Stop(fmt("ValueError: slice %u decodes %lld rows, want %lld", k, (long long)rows, (long long)dim[ax[1]]));
        }
        slice_ptrs.push_back(sl_at[so]);
    }
    root->ptr(rt, 0);
    root->u32(nsl).u32(0);
    for (size_t o : slice_ptrs) root->ptr(sl_blob, int64_t(o));
    root->embed(rt, 16);
    root->embed(sl_blob, 16);
    root->embed(rows_blob, 16);
    root->pad(16);
    return root;
}

}  // namespace

Bytes vischeat_convert(const fs::path &src) {
    const Pak p(src);
    if (!p.ps3) throw Stop("ValueError: expected a PS3 pak");
    std::string others;
    for (const PakItem &i : p.items)
        if (i.type != "VIS_CHEAT" && i.type != "PAK_LOGIN_TABLE" && i.type != "VRAM_DESC_TABLE") others += (others.empty() ? "" : ", ") + i.type;
    if (!others.empty()) throw Stop("ValueError: unexpected items " + others);
    PakWriter w;
    Page &page = w.page();
    std::vector<std::shared_ptr<PakWItem>> made;
    for (const PakItem &it : p.items_of("VIS_CHEAT")) made.push_back(page.item("VIS_CHEAT", convert_item(Src{p, it.page, p.pbase(it.page)}, it), utf8_latin1(it.name)));
    Page &p1 = w.page();
    make_login_and_vram_page(w, p1, made, {});
    return w.build();
}

}  // namespace u3
