// u2mp menu images: PS3 menu image ids, net image sizes and the image types the PS4 renderer lost.
//
// The PS3 menus name net images by sid("general/net/<set>/<name>"), while the PS4 net package
// calls its materials "general/net/<set>:<name>", so the game's image lookup finds none of them.
// Our lookup also matches an entry by its name read with ':' as '/', and for net images falls back
// to the imported net-u2mp package (PS3 images the PS4 lacks). PS4 net textures have twice the PS3
// resolution and images take their size from the texture, so two size calls measure PS4 net images
// at a scale. The menu image renderer has no drawing for a few image types (rank badge, booster
// pair, map picture, playlist pictures); their jump table entries go to a small stub that calls
// our drawing and then leaves through the renderer's exit. A wrap of the image converter logs net
// image sizes for diagnostics.
//
// Reimplemented from the behaviour description u2mp/docs/spec/menus.md.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "frontend3.h"
#include "u2mp_internal.h"

extern "C" int runtime_file_translate(const char *guest, char *out, size_t size);

// Every float operation is rounded on its own, as the description requires.
#pragma clang fp contract(off)

namespace u2mp {
namespace {

template <typename T> T get(const uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <typename T> void put(uint8_t *p, T v) { std::memcpy(p, &v, sizeof v); }

uint8_t *abs_at(uint64_t offset) { return image.base + offset; }

// Game locations (image offsets of the 1.02 build).
constexpr uint64_t kImageTables = 0x1130008;   // +0x68 hud, +0x70 net, +0x78 menu
constexpr uint64_t kLookup = 0x5558c0;
constexpr uint64_t kImageSize = 0x5bbd90;
constexpr uint64_t kSizeCallType3 = 0x5bcc1a;
constexpr uint64_t kSizeCallFlag80 = 0x5bdb30;
constexpr uint64_t kConverter = 0x5bc290;
constexpr uint64_t kDrawQuad = 0xbdce90;
constexpr uint64_t kDispatch = 0x5bd852;
constexpr uint64_t kJumpTable = 0x5c2ec4;
constexpr uint64_t kRendererExit = 0x5c1ab1;
constexpr uint64_t kDcLookup = 0xa15e70;
constexpr uint64_t kNgm = 0x113b760;

constexpr uint32_t kDcBoosters = 0x916e03b0;
constexpr uint32_t kDcMaps = 0x0dec97a4;
constexpr uint32_t kNetU2mpPackage = 0x5e516de8;
constexpr uint32_t kRankSheet = 0xbda0f568;
constexpr uint32_t kRankSheetHigh = 0xf1b63553;
constexpr uint32_t kEntrySize = 0x28;

const char kExtraPath[] = "/app0/u2data/build/main/pak22/ps4/net-u2mp.pak";

// Takes one line of a shared log budget; false once it is used up.
bool take(std::atomic<int> &budget) {
    int left = budget.load();
    while (left > 0)
        if (budget.compare_exchange_weak(left, left - 1)) return true;
    return false;
}

void *dc_lookup(uint32_t id) {
    return reinterpret_cast<void *(U2MP_GUEST *)(uint32_t)>(abs_at(kDcLookup))(id);
}

using SizeFn = float *(U2MP_GUEST *)(float *, uint8_t *);
SizeFn image_size() { return reinterpret_cast<SizeFn>(abs_at(kImageSize)); }

// ---- material tables ---------------------------------------------------------------------------------

uint8_t *table_of(uint32_t sel) {
    const uint64_t slot = sel == 2 ? 0x78 : sel == 1 ? 0x70 : 0x68;
    return get<uint8_t *>(abs_at(kImageTables + slot));
}
uint32_t table_count(const uint8_t *table) { return get<uint16_t>(table + 2); }
uint8_t *table_entries(const uint8_t *table) { return get<uint8_t *>(table + 8); }

// True when `m` is an entry of the PS4 net table.
bool in_net_table(const uint8_t *m) {
    const uint8_t *table = table_of(1);
    if (!table) return false;
    const uint8_t *entries = table_entries(table);
    if (!entries) return false;
    return m >= entries && m < entries + uint64_t(kEntrySize) * table_count(table);
}

// "set:name,guid,shader" behind four pointers, or NULL.
const char *entry_name(const uint8_t *entry) {
    const uint8_t *p = get<uint8_t *>(entry);
    if (!p) return nullptr;
    p = get<uint8_t *>(p + 0x10);
    if (!p) return nullptr;
    p = get<uint8_t *>(p);
    if (!p) return nullptr;
    return get<const char *>(p);
}

// The id the PS3 gave this image: the name up to its first ',', ':' read as '/'.
uint32_t ps3_id(const char *name) {
    if (!name) return 0;
    std::string s;
    for (const char *c = name; *c && *c != ','; ++c) s += *c == ':' ? '/' : *c;
    return sid(s.c_str());
}

// ---- PS3-name search -------------------------------------------------------------------------------

struct NameCache {
    const uint8_t *table = nullptr;
    uint32_t count = 0;
    bool valid = false;
    std::vector<uint32_t> ids;
};
std::mutex name_mutex;
NameCache name_cache[2];   // net (sel 1), menu (sel 2)

int ps3_name_index(uint32_t sel, const uint8_t *table, uint32_t id) {
    std::lock_guard<std::mutex> lock(name_mutex);
    NameCache &c = name_cache[sel == 2 ? 1 : 0];
    const uint32_t count = table_count(table);
    if (!c.valid || c.table != table || c.count != count) {
        c.table = table;
        c.count = count;
        c.valid = true;
        c.ids.assign(count, 0);
        const uint8_t *entries = table_entries(table);
        for (uint32_t i = 0; i < count; ++i) c.ids[i] = ps3_id(entry_name(entries + uint64_t(kEntrySize) * i));
        log("menu images: %s table %p, %u materials", sel == 1 ? "net" : "menu", static_cast<const void *>(table),
            count);
        for (uint32_t i = 0; i < count && i < 6; ++i) {
            const uint8_t *e = entries + uint64_t(kEntrySize) * i;
            const char *name = entry_name(e);
            log("menu images:   [%u] id %08x  ps3 id %08x  %.60s", i, get<uint32_t>(e + 8), c.ids[i],
                name ? name : "(no name)");
        }
    }
    for (uint32_t i = 0; i < count; ++i)
        if (c.ids[i] == id) return int(i);
    return -1;
}

// ---- the imported net-u2mp package -----------------------------------------------------------------

std::atomic<const uint8_t *> extra_logged{nullptr};

uint8_t *extra_entry(uint32_t id) {
    if (!extra_net_package()) return nullptr;
    const uint8_t *pkg = package_by_id(kNetU2mpPackage);
    if (!pkg || get<int32_t>(pkg + 0x30) != 7) return nullptr;   // 7: logged in
    const uint8_t *table = get<uint8_t *>(pkg + 0x940);
    if (!table) return nullptr;
    uint8_t *entries = table_entries(table);
    if (!entries) return nullptr;
    const uint32_t count = table_count(table);
    if (extra_logged.exchange(table) != table)
        log("menu images: net-u2mp table %p, %u materials", static_cast<const void *>(table), count);
    for (uint32_t i = 0; i < count; ++i)
        if (get<uint32_t>(entries + uint64_t(kEntrySize) * i + 8) == id) return entries + uint64_t(kEntrySize) * i;
    return nullptr;
}

// ---- the lookup (replaces 0x5558c0) -----------------------------------------------------------------

std::atomic<int> lookup_log_budget{48};

uint8_t *entry_at(const uint8_t *table, int32_t index) {
    if (uint32_t(index) >= table_count(table)) return nullptr;
    return table_entries(table) + uint64_t(kEntrySize) * uint32_t(index);
}

U2MP_GUEST uint8_t *lookup(uint64_t id_sel, int32_t index) {
    const uint32_t id = uint32_t(id_sel), sel = uint32_t(id_sel >> 32);
    const uint8_t *table = table_of(sel);
    if (!table) return nullptr;
    if (index >= 0) return entry_at(table, index);
    if (sel != 1 && sel != 2) return nullptr;

    uint8_t *entries = table_entries(table);
    const uint32_t count = table_count(table);
    for (uint32_t i = 0; i < count; ++i)
        if (get<uint32_t>(entries + uint64_t(kEntrySize) * i + 8) == id) return entries + uint64_t(kEntrySize) * i;

    const int by_name = ps3_name_index(sel, table, id);
    uint8_t *extra = (by_name == -1 && sel == 1) ? extra_entry(id) : nullptr;
    if (take(lookup_log_budget))
        log("menu images: lookup %s id %08x -> %s (%d)", sel == 1 ? "net" : "menu", id,
            by_name >= 0 ? "found by PS3 name" : extra ? "found in net-u2mp" : "missing", by_name);
    if (extra) return extra;
    if (by_name == -1) return nullptr;
    return entry_at(table, by_name);
}

uint8_t *net_material(uint32_t id) {
    if (id == 0) return nullptr;
    return lookup((uint64_t(1) << 32) | id, -1);
}

// ---- sizes ------------------------------------------------------------------------------------------

float net_scale = 0.5f;

// The retargeted size calls: PS4 net images measured at the PS3 resolution.
U2MP_GUEST float *scaled_size(float *out, uint8_t *material) {
    image_size()(out, material);
    if (out && in_net_table(material)) {
        out[0] *= net_scale;
        out[1] *= net_scale;
    }
    return out;
}

// The converter's first 18 bytes, followed by a jump back into it.
using ConvertFn = uint64_t(U2MP_GUEST *)(uint8_t *, const uint8_t *, uint64_t, const float *, const void *,
                                          uint32_t, float);
ConvertFn converter_original = nullptr;

std::mutex size_mutex;
std::vector<std::pair<uint64_t, uint32_t>> size_logged;   // (caller, id), at most 96

U2MP_GUEST uint64_t converter_wrap(uint8_t *out, const uint8_t *src, uint64_t a2, const float *box,
                                    const void *colour, uint32_t a5, float scale) {
    const uint64_t ret = reinterpret_cast<uint64_t>(__builtin_return_address(0));
    const uint64_t r = converter_original(out, src, a2, box, colour, a5, scale);
    if (!out || !src || get<uint32_t>(src) != 3 || get<uint32_t>(src + 0x18) != 1) return r;
    const uint32_t id = get<uint32_t>(src + 0x14);
    const uint64_t base = reinterpret_cast<uint64_t>(image.base);
    const uint64_t caller = (ret >= base && ret < base + image.size) ? ret - base : ret;
    {
        std::lock_guard<std::mutex> lock(size_mutex);
        if (size_logged.size() >= 96) return r;
        for (const auto &p : size_logged)
            if (p.first == caller && p.second == id) return r;
        size_logged.emplace_back(caller, id);
    }
    uint8_t *m = get<uint8_t *>(out + 0x48);
    float tex[2] = {0.0f, 0.0f};
    if (m) image_size()(tex, m);
    log("menu images: size id %08x from 0x%llx: scale %.3f box %.0fx%.0f texture %.0fx%.0f (%s) -> %.1fx%.1f", id,
        (unsigned long long)caller, double(scale), box ? double(box[0]) : -1.0, box ? double(box[1]) : -1.0,
        double(tex[0]), double(tex[1]), !m ? "no material" : in_net_table(m) ? "PS4 net.pak" : "net-u2mp",
        double(get<float>(out + 0x3c)), double(get<float>(out + 0x40)));
    return r;
}

// ---- drawing ----------------------------------------------------------------------------------------

struct Colour {
    float r, g, b, a;
};

// One textured quad through the game's DrawQuad (nothing for no material or an empty box).
void draw_quad(uint64_t list, uint8_t *material, float x, float y, float w, float h, float u0, float v0, float u1,
               float v1, const Colour &c) {
    if (!material || w <= 0.0f || h <= 0.0f) return;
    alignas(16) uint64_t ctx[6] = {list, 0, 0, 0, 0, 0};
    alignas(16) uint64_t info[4] = {reinterpret_cast<uint64_t>(material), 0, 0, 0};
    alignas(16) uint8_t verts[0x110] = {};
    const float corners[4][4] = {
        {x, y, u0, v0}, {x + w, y, u1, v0}, {x + w, y + h, u1, v1}, {x, y + h, u0, v1}};
    for (int i = 0; i < 4; ++i) {
        uint8_t *v = verts + 0x40 * i;
        put<float>(v + 0x00, corners[i][0]);
        put<float>(v + 0x04, corners[i][1]);
        put<float>(v + 0x08, 0.0f);
        put<float>(v + 0x0c, corners[i][2]);
        put<float>(v + 0x10, corners[i][3]);
        put<float>(v + 0x20, c.r);
        put<float>(v + 0x24, c.g);
        put<float>(v + 0x28, c.b);
        put<float>(v + 0x2c, c.a);
    }
    reinterpret_cast<void(U2MP_GUEST *)(void *, void *, void *)>(abs_at(kDrawQuad))(ctx, info, verts);
}

// A booster's image id; 0 for no booster or a placeholder.
uint32_t booster_image(int32_t i) {
    const uint8_t *table = static_cast<const uint8_t *>(dc_lookup(kDcBoosters));
    if (!table || i < 0 || get<int32_t>(table) <= i) return 0;
    const uint8_t *records = get<uint8_t *>(table + 8);
    if (!records) return 0;
    const uint8_t *rec = records + 0x20 * i;
    const uint32_t key = get<uint32_t>(rec + 0xc);
    if (key == 0x05454b23 || key == 0x01845694) return 0;
    return get<uint32_t>(rec + 8);
}

uint32_t map_image(uint32_t map) {
    const uint8_t *table = static_cast<const uint8_t *>(dc_lookup(kDcMaps));
    if (!table) return 0;
    const int32_t count = get<int32_t>(table);
    const uint8_t *records = get<uint8_t *>(table + 8);
    if (!records) return 0;
    for (int32_t i = 0; i < count; ++i)
        if (get<uint32_t>(records + 0x38 * i) == map) return get<uint32_t>(records + 0x38 * i + 0xc);
    return 0;
}

// The picture of the playlist record the game holds with this id (menus.cpp builds them).
uint32_t playlist_image(uint32_t value) {
    const uint8_t *ngm = abs_at(kNgm);
    uint32_t n = get<uint32_t>(ngm + 0x21a0);
    if (n > 32) n = 32;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t *p = get<uint8_t *>(ngm + 0x20a0 + 8 * i);
        if (p && get<uint32_t>(p) == value) return get<uint32_t>(p + 0x10);
    }
    return 0;
}

std::atomic<int> draw_log_budget{24};

// Called by the stub for the image types the renderer has no case for.
U2MP_GUEST void draw_lost_type(uint8_t *desc, uint8_t *frame, uint32_t c2, uint32_t c1, uint32_t c0, uint32_t c3) {
    const uint64_t list = get<uint64_t>(frame);
    const float x = get<float>(frame - 0x10), y = get<float>(frame - 0x0c);
    const float w = get<float>(frame - 0x08), h = get<float>(frame - 0x04);
    const float alpha = get<float>(desc + 0x50);
    const Colour colour = {float((c2 >> 16) & 0xff) / 255.0f, float((c1 >> 8) & 0xff) / 255.0f,
                           float(c0 & 0xff) / 255.0f, (float((c3 >> 8) & 0xff) / 255.0f) * alpha};
    const uint32_t type = get<uint32_t>(desc), value = get<uint32_t>(desc + 0x10);

    uint32_t id = 0;
    uint8_t *m = nullptr;
    switch (type) {
    case 0x27: {   // rank badge: 8x8 cells per sheet
        const int32_t index = int32_t(value);
        id = index > 63 ? kRankSheetHigh : kRankSheet;
        m = net_material(id);
        const float u = float(index & 7) * 0.125f, v = float((index & 63) / 8) * 0.125f;
        draw_quad(list, m, x, y, w, h, u, v, u + 0.125f, v + 0.125f, colour);
        break;
    }
    case 0x1e: {   // two boosters, one above the other
        const float bottom = y + h, half = w * 0.5f;
        const uint32_t a = booster_image(int32_t(value & 0xffff));
        const uint32_t b = booster_image(int32_t(value >> 16));
        id = a;
        // The half-height offset is negated and then added: same result for finite values, and a NaN
        // width gives a NaN y with the sign bit set. The volatile keeps the compiler from turning the
        // add of a negated value back into a subtraction, and the negated product back into a
        // multiply by -0.5f (both would keep a NaN positive).
        volatile float offset = half * 0.5f;
        volatile float lift = -offset;
        m = net_material(a);
        draw_quad(list, m, x, ((y * 0.72f) + (bottom * 0.28f)) + lift, w, half, 0, 0, 1, 1, colour);
        uint8_t *mb = net_material(b);
        draw_quad(list, mb, x, ((y * 0.28f) + (bottom * 0.72f)) + lift, w, half, 0, 0, 1, 1, colour);
        break;
    }
    case 0x22:   // map picture, cropped to the box's aspect
        id = map_image(value);
        m = net_material(id);
        draw_quad(list, m, x, y, w, h, 0.08984375f, 0, 0.91015625f, 1, colour);
        break;
    case 0x24:
    case 0x25: {   // playlist picture; 0x25 leaves room for a caption below
        id = playlist_image(value);
        m = net_material(id);
        float hh = h;
        if (type == 0x25) {
            const float less = h - 50.0f;
            hh = less > 0.0f ? less : 0.0f;
        }
        draw_quad(list, m, x, y, w, hh, 0.0703125f, 0, 0.9296875f, 1, colour);
        break;
    }
    default: break;
    }
    if (take(draw_log_budget))
        log("menu images: draw type 0x%x value %08x image %08x %s at %.0f,%.0f size %.0fx%.0f", type, value, id,
            m ? "found" : "missing", double(x), double(y), double(w), double(h));
}

// The stub the renderer's jump table enters: pass the registers to draw_lost_type, then leave
// through the renderer's exit (it needs only rbp and callee-saved registers, which survive).
void write_stub(uint8_t *s) {
    static const uint8_t moves[] = {
        0x4c, 0x89, 0xdf,                           // mov rdi, r11       descriptor
        0x48, 0x8d, 0xb5, 0x20, 0xe0, 0xff, 0xff,   // lea rsi, [rbp-0x1fe0]  frame
        0x89, 0xda,                                 // mov edx, ebx
        0x44, 0x89, 0xf1,                           // mov ecx, r14d
        0x45, 0x89, 0xe0,                           // mov r8d, r12d
        0x45, 0x89, 0xe9,                           // mov r9d, r13d
    };
    size_t n = 0;
    std::memcpy(s, moves, sizeof moves);
    n += sizeof moves;
    const uint64_t host = reinterpret_cast<uint64_t>(&draw_lost_type);
    const uint64_t exit = reinterpret_cast<uint64_t>(abs_at(kRendererExit));
    s[n++] = 0x48, s[n++] = 0xb8;   // mov rax, host
    std::memcpy(s + n, &host, 8), n += 8;
    s[n++] = 0xff, s[n++] = 0xd0;   // call rax
    s[n++] = 0x48, s[n++] = 0xb8;   // mov rax, exit
    std::memcpy(s + n, &exit, 8), n += 8;
    s[n++] = 0xff, s[n++] = 0xe0;   // jmp rax
}

}  // namespace

const char *extra_net_package() {
    static const bool present = [] {
        char host[1024];
        bool found = false;
        if (runtime_file_translate(kExtraPath, host, sizeof host) == 0) {
            std::error_code ec;
            found = std::filesystem::is_regular_file(std::filesystem::path(host), ec);
        }
        log("menu images: net-u2mp.pak %s", found ? "imported (PS3 menu images PS4 lacks)" : "not imported");
        return found;
    }();
    return present ? "net-u2mp" : nullptr;
}

uint8_t *menu_net_material(uint32_t id) { return net_material(id); }

void menu_draw_quad(uint8_t *material, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                    float alpha, float rgb) {
    draw_quad(0, material, x, y, w, h, u0, v0, u1, v1, Colour{rgb, rgb, rgb, alpha});
}

void install_menuimages(int &installed) {
    // 1. Net image scale.
    if (const char *env = std::getenv("UNCHARTED_U2MP_NET_IMAGE_SCALE"); env && std::atof(env) > 0)
        net_scale = float(std::atof(env));
    if (net_scale != 1.0f &&
        retarget_call(kSizeCallType3, kImageSize, reinterpret_cast<void *>(&scaled_size), "net image size (type 3)") &&
        retarget_call(kSizeCallFlag80, kImageSize, reinterpret_cast<void *>(&scaled_size),
                      "net image size (flag 0x80)")) {
        log("menu images: PS4 net.pak images measured at %.2f of their texture size (PS3 resolution)",
            double(net_scale));
        installed += 1;
    }

    // 2. Converter size log; the original stays callable through a copy of its prologue.
    static const uint8_t converter_head[18] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                               0x54, 0x53, 0x48, 0x81, 0xec, 0xe0, 0x00, 0x00, 0x00};
    uint8_t *tramp = image.matches(kConverter, converter_head, sizeof converter_head) ? near_stub(48) : nullptr;
    if (tramp) {
        std::memcpy(tramp, converter_head, sizeof converter_head);
        static const uint8_t jmp_abs[6] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};   // jmp [rip+0]
        std::memcpy(tramp + 18, jmp_abs, sizeof jmp_abs);
        const uint64_t back = reinterpret_cast<uint64_t>(abs_at(kConverter + 18));
        std::memcpy(tramp + 24, &back, 8);
        converter_original = reinterpret_cast<ConvertFn>(tramp);
        if (replace_function(kConverter, 18, reinterpret_cast<void *>(&converter_wrap), "image converter (size log)"))
            installed += 1;
    } else {
        log("menu images: converter 0x%llx differs, sizes not logged", (unsigned long long)kConverter);
    }

    // 3. Lookup with PS3 names.
    static const uint8_t lookup_head[16] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x89,
                                            0xf3, 0x48, 0x89, 0xf8, 0x48, 0xc1, 0xe8, 0x20};
    if (image.matches(kLookup, lookup_head, sizeof lookup_head) &&
        replace_function(kLookup, 16, reinterpret_cast<void *>(&lookup), "menu image lookup (PS3 names)"))
        installed += 1;
    else
        log("menu images: lookup 0x%llx differs, left alone", (unsigned long long)kLookup);

    // 4. Renderer cases for the lost image types.
    static const uint8_t dispatch[36] = {0x83, 0xc0, 0xfc, 0x83, 0xf8, 0x25, 0x0f, 0x87, 0x53, 0x42, 0x00, 0x00,
                                         0x4c, 0x8d, 0xbd, 0x30, 0xe0, 0xff, 0xff, 0x49, 0xba, 0x00, 0x00, 0x00,
                                         0x00, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x0d, 0x4e, 0x56, 0x00, 0x00};
    static const uint8_t exit_bytes[7] = {0x48, 0x8b, 0x05, 0x48, 0x91, 0xa8, 0x00};
    static const uint8_t draw_head[16] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                          0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x40, 0x49};
    if (!image.matches(kDispatch, dispatch, sizeof dispatch) ||
        !image.matches(kRendererExit, exit_bytes, sizeof exit_bytes) ||
        !image.matches(kDrawQuad, draw_head, sizeof draw_head) || kJumpTable + 0x26 * 4 > image.size) {
        log("menu images: renderer 0x5bcd70 differs, missing image types not added");
        return;
    }
    static const uint32_t types[] = {0x1e, 0x22, 0x23, 0x24, 0x25, 0x27};
    auto slot = [](uint32_t t) { return abs_at(kJumpTable + 4 * (t - 4)); };
    for (uint32_t t : types)
        if (int64_t(kJumpTable) + get<int32_t>(slot(t)) != int64_t(kRendererExit)) {
            log("menu images: jump table entry of type 0x%x is not the exit, left alone", t);
            return;
        }
    uint8_t *stub = near_stub(64);
    if (!stub) {
        log("menu images: no stub memory near the image");
        return;
    }
    const int64_t diff = int64_t(reinterpret_cast<uint64_t>(stub) - reinterpret_cast<uint64_t>(abs_at(kJumpTable)));
    if (diff != int64_t(int32_t(diff))) {
        log("menu images: stub out of jump-table range");
        return;
    }
    write_stub(stub);
    for (uint32_t t : types) put<int32_t>(slot(t), int32_t(diff));
    log("menu images: renderer types 0x1e, 0x22..0x25, 0x27 -> C++ (stub %p)", static_cast<void *>(stub));
    installed += 1;
}

}  // namespace u2mp
