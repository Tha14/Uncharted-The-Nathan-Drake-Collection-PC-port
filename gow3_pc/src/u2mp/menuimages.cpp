// u2mp menu images: why no multiplayer menu image drew on PS4, and the image types the PS4 menu
// renderer lost, ported from PS3 1.10. Notes: u2mp/docs/MENUS.md section 7.
//
// 1. Net image ids. A type 3 image of the net image table (0x1130008+0x70 = the "net" package's
//    material table, package+0x940) is found by id (0x5558c0, and inline copies in the queries such
//    as 0x655ec0 / 0x656a22). The menu data and code use the PS3 ids: sid("general/net/<set>/<name>")
//    (mode table 0xdef390+8, map records +0xc, our playlists). The PS4 net.pak names its materials
//    "general/net/<set>:<name>", and the table's ids are the string id of that name
//    (ConstructFromBoundMaterialTable 0xbe81f0: name up to ',' -> [0x10666c8] = 0xb26fc0, the CRC
//    string id), so no id the menus ask for is in the table: every lookup returned NULL, the image got
//    size 0 (converter 0x5bc290 -> 0x5bbd90) and nothing was drawn. The lookup 0x5558c0 is ported
//    whole with one addition: an id not in the table is looked up again among the entries' names
//    with ':' read as '/'.
// 2. Missing renderer cases. The renderer 0x5bcd70 dispatches types 4..0x29 at 0x5bd852 through the
//    table 0x5c2ec4; 0x1e, 0x22..0x25 and 0x27 point at its exit 0x5c1ab1. Their entries are pointed
//    at a stub that calls draw_missing_type and then jumps to that exit. The drawing follows the PS3
//    renderer 0x41a790 (cases at 0x41aa48, 0x41c2d8, 0x41eb50, 0x41f418) and draws with the PS4's own
//    textured quad call (0xbdce90: draw context, {material, 0, 0, 0}, 4 vertices of 0x40 bytes) as the
//    type 3 case does (0x5bdff9).
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <utility>
#include <system_error>
#include <vector>

#include "frontend3.h"
#include "u2mp_internal.h"

extern "C" int runtime_file_translate(const char *guest, char *out, size_t size);

namespace u2mp {
namespace {

constexpr uint64_t kImageTables = 0x1130008;   // +0x68 hud, +0x70 net, +0x78 menu material tables
constexpr uint64_t fLookup = 0x5558c0;         // image lookup (id | selector << 32, index)
constexpr uint64_t fDrawQuad = 0xbdce90;       // (ctx, material info, vertices)
constexpr uint64_t fDcLookup = 0xa15e70;
constexpr uint64_t kDispatch = 0x5bd852;       // add eax,-4; cmp eax,0x25; ja; ...; jmp rax
constexpr uint64_t kJumpTable = 0x5c2ec4;      // rel32 entries for types 4..0x29
constexpr uint64_t kRendererExit = 0x5c1ab1;   // canary check, epilogue
constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t NGM_PLAYLISTS = 0x20a0, NGM_PLAYLIST_COUNT = 0x21a0;
constexpr uint32_t kBoosterTable = 0x916e03b0; // {s32 count; rec* +8}, 0x20 bytes: +8 image, +0xc id
constexpr uint32_t kMapTable = 0x0dec97a4;     // {s32 count; rec* +8}, 0x38 bytes: +0 id, +0xc image

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }
template <typename T> T rd(const void *p, ptrdiff_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t *>(p) + off, sizeof v);
    return v;
}

// ---- material tables ----------------------------------------------------------------------------
// {u16 ?, u16 count at +2, entries at +8}, entry 0x28 bytes: +0 -> +0x10 -> [0] -> [0] the name
// "set:name,guid,shader" (0xbe8150), +8 the id.
struct MaterialTable {
    uint16_t pad, count;
    uint32_t pad2;
    uint8_t *entries;
};

const char *entry_name(const uint8_t *entry) {
    const uint8_t *p = rd<const uint8_t *>(entry, 0);
    if (!p) return nullptr;
    p = rd<const uint8_t *>(p, 0x10);
    if (!p) return nullptr;
    p = rd<const uint8_t *>(p, 0);
    if (!p) return nullptr;
    return rd<const char *>(p, 0);
}

// The PS3 form of an entry's id: its name up to ',' with ':' read as '/'.
uint32_t slash_id(const char *name) {
    uint32_t crc = 0;
    for (const char *s = name; *s && *s != ','; ++s) {
        const char c = *s == ':' ? '/' : *s;
        crc ^= uint32_t(uint8_t(c)) << 24;
        for (int i = 0; i < 8; ++i) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

struct SlashMap {
    const MaterialTable *table = nullptr;
    uint16_t count = 0;
    std::vector<uint32_t> ids;   // slash id per entry
};
SlashMap slash_maps[3];
std::mutex slash_mutex;
std::atomic<int> log_budget{48};

void dump_table(int sel, const MaterialTable *t) {
    log("menu images: %s table %p, %u materials", sel == 1 ? "net" : "menu", static_cast<const void *>(t), t->count);
    for (unsigned i = 0; i < t->count && i < 6; ++i) {
        const uint8_t *e = t->entries + 0x28 * i;
        const char *n = entry_name(e);
        log("menu images:   [%u] id %08x  ps3 id %08x  %.60s", i, rd<uint32_t>(e, 8), n ? slash_id(n) : 0u,
            n ? n : "(no name)");
    }
}

int find_by_slash_id(int sel, const MaterialTable *t, uint32_t id) {
    std::lock_guard<std::mutex> lock(slash_mutex);
    SlashMap &m = slash_maps[sel];
    if (m.table != t || m.count != t->count) {
        m.table = t;
        m.count = t->count;
        m.ids.assign(t->count, 0);
        for (unsigned i = 0; i < t->count; ++i) {
            const char *n = entry_name(t->entries + 0x28 * i);
            m.ids[i] = n ? slash_id(n) : 0;
        }
        dump_table(sel, t);
    }
    for (unsigned i = 0; i < m.ids.size(); ++i)
        if (m.ids[i] == id) return int(i);
    return -1;
}

// ---- net-u2mp: the PS3 net.pak images the PS4 net.pak lacks (u2mp/patcher/u2conv/u2netimg.py) ----
// Leaderboards / Trophies mode buttons, DLC map photos, store skin portraits and icons, rank badges
// above 63... The import writes them as their own package, materials named with the PS3 names (so
// their ids are the ids the menus ask for). session.cpp requests it next to "net"; once logged in
// (+0x30 == 7) its material table (+0x940, as 0x554090 reads the net one) answers net ids the net
// table does not have.
constexpr const char *kExtraPak = "/app0/u2data/build/main/pak22/ps4/net-u2mp.pak";
constexpr uint64_t PKG_STATE = 0x30, PKG_MATERIALS = 0x940;

const MaterialTable *extra_table() {
    if (!extra_net_package()) return nullptr;
    static const uint32_t id = sid("net-u2mp");
    const uint8_t *pkg = package_by_id(id);
    if (!pkg || rd<int32_t>(pkg, PKG_STATE) != 7) return nullptr;
    return rd<const MaterialTable *>(pkg, PKG_MATERIALS);
}

uint8_t *extra_lookup(uint32_t id) {
    const MaterialTable *t = extra_table();
    if (!t || !t->entries) return nullptr;
    static const MaterialTable *logged = nullptr;
    if (logged != t) {
        logged = t;
        log("menu images: net-u2mp table %p, %u materials", static_cast<const void *>(t), t->count);
    }
    for (unsigned i = 0; i < t->count; ++i)
        if (rd<uint32_t>(t->entries + 0x28 * size_t(i), 8) == id) return t->entries + 0x28 * i;
    return nullptr;
}

// 0x5558c0 ported: the hud table (selector 0) by index only; net (1) and menu (2) by index or, with
// index < 0, by id; plus the PS3-name fallback and, for net ids, the net-u2mp package. Out-of-range
// indices return NULL (the original asserted).
U2MP_GUEST uint8_t *image_lookup(uint64_t id_sel, int32_t index) {
    const uint32_t id = uint32_t(id_sel), sel = uint32_t(id_sel >> 32);
    const uint64_t slot = sel == 2 ? 0x78 : sel == 1 ? 0x70 : 0x68;
    const auto *t = G<const MaterialTable *>(kImageTables + slot);
    if (!t) return nullptr;
    if (index < 0) {
        if (sel != 1 && sel != 2) return nullptr;
        for (unsigned i = 0; i < t->count; ++i)
            if (rd<uint32_t>(t->entries + 0x28 * size_t(i), 8) == id) return t->entries + 0x28 * i;
        // The PS4 image first (its name read the PS3 way), net-u2mp only for what PS4 lacks: the
        // user's rule (2026-11-09) is that an asset PS4 has is always the PS4 one.
        index = find_by_slash_id(int(sel), t, id);
        uint8_t *extra = index < 0 && sel == 1 ? extra_lookup(id) : nullptr;
        if (log_budget.load(std::memory_order_relaxed) > 0) {
            log_budget.fetch_sub(1, std::memory_order_relaxed);
            log("menu images: lookup %s id %08x -> %s (%d)", sel == 1 ? "net" : "menu", id,
                index >= 0 ? "found by PS3 name" : extra ? "found in net-u2mp" : "missing", index);
        }
        if (extra) return extra;
        if (index < 0) return nullptr;
    }
    if (unsigned(index) >= t->count) return nullptr;
    return t->entries + 0x28 * unsigned(index);
}

uint8_t *net_material(uint32_t id) { return id ? image_lookup(uint64_t(1) << 32 | id, -1) : nullptr; }

// ---- image sizes: PS4 net.pak textures are twice the PS3 resolution --------------------------------
// A type 3 image is as large as its largest texture times the descriptor scale (PS4 converter
// 0x5bc290 type 3 -> 0x5bcc1a call 0x5bbd90; PS3 0x41fd18 case 3 -> 0x419d08, the same rule), in the
// menu's layout units, which menu.bin keeps from PS3 (the multiplayer pages have the same widths on
// both, e.g. 0x140). Every texture of the PS4 net.pak that PS3 also has is exactly twice the PS3 one
// (mode buttons 256 / 128, player portraits 512 / 256, playlists and map cards 1024x512 / 512x256,
// rank badges 1024 / 512: PS4 net.pak against the PS3 copies in net-u2mp.pak). PS4 never shipped
// these pages, so nothing corrected it: the Profile page portraits and booster icons, the mode
// buttons and the cards drew at twice the PS3 size. An image from the PS4 net table is measured at
// half its texture size; net-u2mp images (PS3 textures) and the hud / menu tables are unchanged.
// UNCHARTED_U2MP_NET_IMAGE_SCALE=1 turns it off.
constexpr uint64_t fImageSize = 0x5bbd90;      // (float out[2], material) -> out
constexpr uint64_t kCallSize1 = 0x5bcc1a, kCallSize2 = 0x5bdb30;
bool in_ps4_net_table(const uint8_t *m) {
    const auto *t = G<const MaterialTable *>(kImageTables + 0x70);
    return m && t && t->entries && m >= t->entries && m < t->entries + 0x28 * size_t(t->count);
}
float net_image_scale = 0.5f;
// ---- measurement (run 43: "boosters and skins in the profile editor are too big") -------------------
// The converter 0x5bc290(out, src, ?, const float box[2], colour, ?, float scale) turns an image
// descriptor into its drawn size (out +0x3c w, h; +0x48 material). Every type 3 net image (selector 1)
// it sizes is logged once per (caller, id): the caller (the widget that draws it), the id, the scale
// and the box the widget passes, the texture size, whether it is a PS4 net.pak or a net-u2mp material,
// and the size that results. PS3's converter 0x41fd18 has the same rule, so a size that differs from
// PS3 comes from the caller's scale or box: the log names the site to compare with its PS3 caller.
constexpr uint64_t fConverter = 0x5bc290;
using ConverterFn = uint64_t(U2MP_GUEST *)(uint8_t *, const uint8_t *, uint64_t, const float *, const void *, uint32_t, float);
ConverterFn converter_original = nullptr;
std::mutex size_log_mutex;
std::vector<std::pair<uint64_t, uint32_t>> size_logged;
U2MP_GUEST uint64_t converter_logged(uint8_t *out, const uint8_t *src, uint64_t a2, const float *box, const void *colour,
                                     uint32_t a5, float scale) {
    const uint64_t ra = uint64_t(uintptr_t(__builtin_return_address(0)));
    const uint64_t r = converter_original(out, src, a2, box, colour, a5, scale);
    if (!out || !src || rd<uint32_t>(src, 0) != 3 || rd<uint32_t>(src, 0x18) != 1) return r;
    const uint32_t id = rd<uint32_t>(src, 0x14);
    const uint64_t base = uint64_t(uintptr_t(image.base));
    const uint64_t caller = ra >= base && ra < base + image.size ? ra - base : ra;
    {
        std::lock_guard<std::mutex> lock(size_log_mutex);
        if (size_logged.size() >= 96) return r;
        for (const auto &e : size_logged)
            if (e.first == caller && e.second == id) return r;
        size_logged.emplace_back(caller, id);
    }
    uint8_t *m = rd<uint8_t *>(out, 0x48);
    alignas(8) float tex[2] = {0, 0};
    if (m) guest<float *(U2MP_GUEST *)(float *, uint8_t *)>(0x5bbd90)(tex, m);
    log("menu images: size id %08x from 0x%llx: scale %.3f box %.0fx%.0f texture %.0fx%.0f (%s) -> %.1fx%.1f", id,
        (unsigned long long)caller, double(scale), box ? double(box[0]) : -1.0, box ? double(box[1]) : -1.0,
        double(tex[0]), double(tex[1]), !m ? "no material" : in_ps4_net_table(m) ? "PS4 net.pak" : "net-u2mp",
        double(rd<float>(out, 0x3c)), double(rd<float>(out, 0x40)));
    return r;
}

U2MP_GUEST float *image_size(float *out, uint8_t *material) {
    guest<float *(U2MP_GUEST *)(float *, uint8_t *)>(fImageSize)(out, material);
    if (out && in_ps4_net_table(material)) {
        out[0] *= net_image_scale;
        out[1] *= net_image_scale;
    }
    return out;
}

void *dc_lookup(uint32_t id) { return guest<void *(U2MP_GUEST *)(uint32_t)>(fDcLookup)(id); }

// ---- drawing --------------------------------------------------------------------------------------
struct alignas(16) Vertex {
    float x, y, z, u, v;
    float pad[3];
    float color[4];   // +0x20, as the renderer's (c2, c1, c0, c3 * alpha) / 255
    float pad2[4];
};
static_assert(sizeof(Vertex) == 0x40, "menu quad vertex");
struct alignas(16) Quad {
    Vertex v[4];
    uint32_t tail;    // the renderer clears this word after the 4 vertices (0x5bde26)
    uint32_t pad[3];
};

struct Frame {        // what the stub hands over from the renderer's frame
    void *list;       // [rbp-0x1fe0]: the renderer's rsi, the draw context's first field
    float x, y, w, h; // [rbp-0x1ff0]: position and size of the image box
    float color[4];
};

void draw_quad(const Frame &f, uint8_t *material, float x, float y, float w, float h, float u0, float v0,
               float u1, float v1) {
    if (!material || w <= 0 || h <= 0) return;
    alignas(16) uint64_t ctx[6] = {uint64_t(uintptr_t(f.list)), 0, 0, 0, 0, 0};
    alignas(16) uint64_t info[4] = {uint64_t(uintptr_t(material)), 0, 0, 0};
    Quad q{};
    const float px[4] = {x, x + w, x + w, x}, py[4] = {y, y, y + h, y + h};
    const float pu[4] = {u0, u1, u1, u0}, pv[4] = {v0, v0, v1, v1};
    for (int i = 0; i < 4; ++i) {
        q.v[i].x = px[i];
        q.v[i].y = py[i];
        q.v[i].u = pu[i];
        q.v[i].v = pv[i];
        std::memcpy(q.v[i].color, f.color, sizeof f.color);
    }
    guest<void(U2MP_GUEST *)(void *, void *, void *)>(fDrawQuad)(ctx, info, &q);
}

// PS3 0x45684c: the image of booster `index` (store table 916e03b0), 0 for none and the two
// placeholder boosters.
uint32_t booster_image(int32_t index) {
    const uint8_t *t = static_cast<const uint8_t *>(dc_lookup(kBoosterTable));
    if (!t || index < 0 || rd<int32_t>(t, 0) <= index) return 0;
    const uint8_t *rec = rd<const uint8_t *>(t, 8);
    if (!rec) return 0;
    rec += 0x20 * size_t(index);
    const uint32_t key = rd<uint32_t>(rec, 0xc);
    if (key == 0x05454b23u || key == 0x01845694u) return 0;
    return rd<uint32_t>(rec, 8);
}

// PS3 0x491f38: the map record of a net game's map id (+0xc its picture).
uint32_t map_image(uint32_t id) {
    const uint8_t *t = static_cast<const uint8_t *>(dc_lookup(kMapTable));
    if (!t) return 0;
    const int32_t n = rd<int32_t>(t, 0);
    const uint8_t *rec = rd<const uint8_t *>(t, 8);
    for (int32_t i = 0; rec && i < n; ++i)
        if (rd<uint32_t>(rec + 0x38 * size_t(i), 0) == id) return rd<uint32_t>(rec + 0x38 * size_t(i), 0xc);
    return 0;
}

// PS3 0x491a60: the playlist record with id `id` (+0x10 its picture; menus.cpp fills ours).
uint32_t playlist_image(uint32_t id) {
    const uint32_t n = G<uint32_t>(kNgm + NGM_PLAYLIST_COUNT);
    for (uint32_t i = 0; i < n && i < 32; ++i) {
        const uint8_t *rec = G<const uint8_t *>(kNgm + NGM_PLAYLISTS + 8 * uint64_t(i));
        if (rec && rd<uint32_t>(rec, 0) == id) return rd<uint32_t>(rec, 0x10);
    }
    return 0;
}

std::atomic<int> draw_log_budget{24};

// Called by the stub with the renderer's descriptor (r11), &[rbp-0x1fe0] and the four modulated
// colour bytes as the renderer keeps them at the dispatch (ebx = c2 << 16, r14d = c1 << 8, r12d = c0,
// r13d = c3 << 8).
U2MP_GUEST void draw_missing_type(uint8_t *desc, uint8_t *frame, uint32_t c2, uint32_t c1, uint32_t c0, uint32_t c3) {
    Frame f;
    f.list = rd<void *>(frame, 0);
    f.x = rd<float>(frame, -0x10);
    f.y = rd<float>(frame, -0x0c);
    f.w = rd<float>(frame, -0x08);
    f.h = rd<float>(frame, -0x04);
    const float alpha = rd<float>(desc, 0x50);
    f.color[0] = float((c2 >> 16) & 0xff) / 255.0f;
    f.color[1] = float((c1 >> 8) & 0xff) / 255.0f;
    f.color[2] = float(c0 & 0xff) / 255.0f;
    f.color[3] = float((c3 >> 8) & 0xff) / 255.0f * alpha;
    const uint32_t type = rd<uint32_t>(desc, 0), value = rd<uint32_t>(desc, 0x10);
    uint32_t id = 0;
    uint8_t *m = nullptr;
    switch (type) {
    case 0x27: {   // rank badge (PS3 0x41aa48): 8x8 cells of 1/8, a second sheet from index 64
        const int32_t index = int32_t(value);
        id = index > 63 ? sid("general/net/rank-badges/default2") : sid("general/net/rank-badges/default");
        m = net_material(id);
        const float u = float(index & 7) * 0.125f, v = float((index & 63) / 8) * 0.125f;
        draw_quad(f, m, f.x, f.y, f.w, f.h, u, v, u + 0.125f, v + 0.125f);
        break;
    }
    case 0x1e: {   // booster pair (PS3 0x41c2d8): two w x w/2 icons centred at 28 % and 72 % height
        const float bottom = f.y + f.h, half = f.w * 0.5f;
        const uint32_t a = booster_image(int32_t(value & 0xffff)), b = booster_image(int32_t(value >> 16));
        id = a;
        m = net_material(a);
        draw_quad(f, m, f.x, f.y * 0.72f + bottom * 0.28f - half * 0.5f, f.w, half, 0, 0, 1, 1);
        draw_quad(f, net_material(b), f.x, f.y * 0.28f + bottom * 0.72f - half * 0.5f, f.w, half, 0, 0, 1, 1);
        break;
    }
    case 0x22:     // map picture of a net game (PS3 0x41eb50), cropped to u 0.0898..0.9102
        id = map_image(value);
        m = net_material(id);
        draw_quad(f, m, f.x, f.y, f.w, f.h, 0.08984375f, 0, 0.91015625f, 1);
        break;
    case 0x24:
    case 0x25:     // playlist picture (PS3 0x41f418), cropped to u 0.0703..0.9297
        id = playlist_image(value);
        m = net_material(id);
        draw_quad(f, m, f.x, f.y, f.w, f.h, 0.0703125f, 0, 0.9296875f, 1);
        break;
    default:       // 0x23 (PS3 0x41cd34: several materials and text) is not ported
        break;
    }
    if (draw_log_budget.load(std::memory_order_relaxed) > 0) {
        draw_log_budget.fetch_sub(1, std::memory_order_relaxed);
        log("menu images: draw type 0x%x value %08x image %08x %s at %.0f,%.0f size %.0fx%.0f", type, value, id,
            m ? "found" : "missing", double(f.x), double(f.y), double(f.w), double(f.h));
    }
}

}  // namespace

uint8_t *menu_net_material(uint32_t id) { return net_material(id); }

// A textured quad outside the renderer (the overlay of loading.cpp): the deferred 2D arena takes it
// with a NULL draw list, as the menu primitives (menudraw.cpp). Screen units as the renderer's boxes.
void menu_draw_quad(uint8_t *material, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                    float alpha, float rgb) {
    Frame f{};
    f.list = nullptr;
    for (float &c : f.color) c = rgb;
    f.color[3] = alpha;
    draw_quad(f, material, x, y, w, h, u0, v0, u1, v1);
}

const char *extra_net_package() {
    static const int present = [] {
        char host[1024];
        std::error_code ec;
        const bool ok = runtime_file_translate(kExtraPak, host, sizeof host) == 0 &&
                        std::filesystem::is_regular_file(std::filesystem::path(host), ec);
        log("menu images: net-u2mp.pak %s", ok ? "imported (PS3 menu images PS4 lacks)" : "not imported");
        return ok ? 1 : 0;
    }();
    return present ? "net-u2mp" : nullptr;
}

void install_menuimages(int &installed) {
    static const unsigned char lookup_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x89,
                                                 0xf3, 0x48, 0x89, 0xf8, 0x48, 0xc1, 0xe8, 0x20};
    static const unsigned char quad_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                               0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x40, 0x49};
    // add eax,-4; cmp eax,0x25; ja 5c1ab1; lea r15,[rbp-0x1fd0]; movabs r10,..; lea rcx,[rip+0x564e]
    static const unsigned char dispatch_bytes[] = {0x83, 0xc0, 0xfc, 0x83, 0xf8, 0x25, 0x0f, 0x87, 0x53, 0x42,
                                                   0x00, 0x00, 0x4c, 0x8d, 0xbd, 0x30, 0xe0, 0xff, 0xff, 0x49,
                                                   0xba, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x48,
                                                   0x8d, 0x0d, 0x4e, 0x56, 0x00, 0x00};
    static const unsigned char exit_bytes[] = {0x48, 0x8b, 0x05, 0x48, 0x91, 0xa8, 0x00};

    if (const char *v = std::getenv("UNCHARTED_U2MP_NET_IMAGE_SCALE"); v && std::atof(v) > 0) net_image_scale = float(std::atof(v));
    if (net_image_scale != 1.0f && retarget_call(kCallSize1, fImageSize, reinterpret_cast<void *>(&image_size), "net image size (type 3)") &&
        retarget_call(kCallSize2, fImageSize, reinterpret_cast<void *>(&image_size), "net image size (flag 0x80)")) {
        log("menu images: PS4 net.pak images measured at %.2f of their texture size (PS3 resolution)", double(net_image_scale));
        ++installed;
    }
    {   // the converter's prologue moves to a trampoline: push rbp; mov rbp,rsp; push r15; push r14;
        // push r12; push rbx; sub rsp,0xe0 (18 bytes, position independent)
        static const unsigned char head[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                             0x54, 0x53, 0x48, 0x81, 0xec, 0xe0, 0x00, 0x00, 0x00};
        unsigned char *tramp = image.matches(fConverter, head, sizeof head) ? near_stub(48) : nullptr;
        if (tramp) {
            std::memcpy(tramp, head, sizeof head);
            const uint64_t back = uint64_t(uintptr_t(image.base + fConverter + sizeof head));
            tramp[18] = 0xff;   // jmp [rip+0]; dq back
            tramp[19] = 0x25;
            std::memset(tramp + 20, 0, 4);
            std::memcpy(tramp + 24, &back, 8);
            converter_original = reinterpret_cast<ConverterFn>(tramp);
            if (replace_function(fConverter, sizeof head, reinterpret_cast<void *>(&converter_logged), "image converter (size log)"))
                ++installed;
        } else {
            log("menu images: converter 0x%llx differs, sizes not logged", (unsigned long long)fConverter);
        }
    }
    if (image.matches(fLookup, lookup_bytes, sizeof lookup_bytes) &&
        replace_function(fLookup, 16, reinterpret_cast<void *>(&image_lookup), "menu image lookup (PS3 names)"))
        ++installed;
    else
        log("menu images: lookup 0x%llx differs, left alone", (unsigned long long)fLookup);

    if (!image.matches(kDispatch, dispatch_bytes, sizeof dispatch_bytes) ||
        !image.matches(kRendererExit, exit_bytes, sizeof exit_bytes) ||
        !image.matches(fDrawQuad, quad_bytes, sizeof quad_bytes) || kJumpTable + 0x26 * 4 > image.size) {
        log("menu images: renderer 0x5bcd70 differs, missing image types not added");
        return;
    }
    const uint32_t types[] = {0x1e, 0x22, 0x23, 0x24, 0x25, 0x27};
    for (uint32_t t : types) {
        int32_t rel = 0;
        std::memcpy(&rel, image.base + kJumpTable + 4 * (t - 4), 4);
        if (kJumpTable + int64_t(rel) != int64_t(kRendererExit)) {
            log("menu images: jump table entry of type 0x%x is not the exit, left alone", t);
            return;
        }
    }
    unsigned char *stub = near_stub(64);
    if (!stub) {
        log("menu images: no stub memory near the image");
        return;
    }
    const int64_t delta = int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + kJumpTable));
    if (delta < INT32_MIN || delta > INT32_MAX) {
        log("menu images: stub out of jump-table range");
        return;
    }
    // mov rdi,r11; lea rsi,[rbp-0x1fe0]; mov edx,ebx; mov ecx,r14d; mov r8d,r12d; mov r9d,r13d;
    // mov rax,draw_missing_type; call rax; mov rax,<exit>; jmp rax   (rsp is 16-aligned here)
    static const unsigned char head[] = {0x4c, 0x89, 0xdf, 0x48, 0x8d, 0xb5, 0x20, 0xe0, 0xff, 0xff, 0x89, 0xda,
                                         0x44, 0x89, 0xf1, 0x45, 0x89, 0xe0, 0x45, 0x89, 0xe9};
    size_t n = 0;
    std::memcpy(stub + n, head, sizeof head);
    n += sizeof head;
    const uint64_t fn = uint64_t(uintptr_t(&draw_missing_type));
    const uint64_t exit = uint64_t(uintptr_t(image.base + kRendererExit));
    stub[n++] = 0x48;
    stub[n++] = 0xb8;
    std::memcpy(stub + n, &fn, 8);
    n += 8;
    stub[n++] = 0xff;
    stub[n++] = 0xd0;
    stub[n++] = 0x48;
    stub[n++] = 0xb8;
    std::memcpy(stub + n, &exit, 8);
    n += 8;
    stub[n++] = 0xff;
    stub[n++] = 0xe0;
    const int32_t rel = int32_t(delta);
    for (uint32_t t : types) std::memcpy(image.base + kJumpTable + 4 * (t - 4), &rel, 4);
    log("menu images: renderer types 0x1e, 0x22..0x25, 0x27 -> C++ (stub %p)", static_cast<void *>(stub));
    ++installed;
}

}  // namespace u2mp
