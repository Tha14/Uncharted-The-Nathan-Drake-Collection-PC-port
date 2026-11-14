// u2mp HUD: the multiplayer HUD element of the PS4 build still fills its data every frame, but
// its Draw slot is an empty stub, so nothing of the match HUD (scores, time, territories, carrier,
// co-op meter, boosters, kill feed, names over heads, world markers, theater controls, centre
// message) reaches the screen. This file puts a host Draw into that vtable slot and draws the data
// with the HUD primitives the PS4 build kept. It also shows the weapon panel for a few seconds
// after the local player switches weapons, with two counting probes for diagnostics.
//
// Reimplemented from the behaviour description u2mp/docs/spec/hud.md.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <xmmintrin.h>

#include "u2mp_internal.h"

// The game's results depend on every float operation being rounded on its own.
#pragma clang fp contract(off)

namespace u2mp {
namespace {

// ---------------------------------------------------------------------------------------------
// Basic access to game memory and guest functions
// ---------------------------------------------------------------------------------------------

uint64_t abs_of(uint64_t offset) { return reinterpret_cast<uint64_t>(image.base) + offset; }

template <typename T> T load(uint64_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof value);
    return value;
}
template <typename T> void store(uint64_t address, T value) {
    std::memcpy(reinterpret_cast<void *>(address), &value, sizeof value);
}
template <typename T> T global(uint64_t offset) { return load<T>(abs_of(offset)); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }

// Integer arithmetic on game clocks wraps like the game's 32-bit registers do.
int32_t wrap_sub(int32_t a, int32_t b) { return int32_t(uint32_t(a) - uint32_t(b)); }
int32_t wrap_add(int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); }

// Float to int the way the hardware truncates (NaN and overflow give INT_MIN, never UB).
int32_t trunc_int(float v) { return _mm_cvtt_ss2si(_mm_set_ss(v)); }

float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

struct V2 {
    float x, y;
};
struct alignas(16) V4 {
    float x, y, z, w;
};

V4 argb(uint32_t c) {
    return V4{float((c >> 16) & 0xff) / 255.0f, float((c >> 8) & 0xff) / 255.0f,
              float(c & 0xff) / 255.0f, float(c >> 24) / 255.0f};
}

uint32_t colour_byte(float c) { return uint32_t(trunc_int(clamp01(c) * 255.0f)) & 0xff; }

uint32_t pack(const V4 &v) {
    return colour_byte(v.w) << 24 | colour_byte(v.x) << 16 | colour_byte(v.y) << 8 | colour_byte(v.z);
}

// Interoperability facts: where things live in the 1.02 image.
constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kNetState = 0x14ce97c;
constexpr uint64_t kMessageClock = 0x11161c4;
constexpr uint64_t kClock1Frame = 0x1116204;
constexpr uint64_t kClock1Delta = 0x1116210;
constexpr uint64_t kClock2Frame = 0x1116284;
constexpr uint64_t kFrames = 0x1116384;
constexpr uint64_t kHudFlagsPtr = 0x1130068;
constexpr uint64_t kMaterialTables = 0x1130070;
constexpr uint64_t kHudOffsetX = 0x1130348;
constexpr uint64_t kScoreTable = 0x104ae70;
constexpr uint64_t kHudAlpha = 0x104ed0c;
constexpr uint64_t kMarkerMask = 0x104f4c4;
constexpr uint64_t kWaveAlpha = 0x104f564;
constexpr uint64_t kReplaySpeed = 0x104f1bc;
constexpr uint64_t kTextDb = 0x1066660;
constexpr uint64_t kBoosterListCount = 0x113a788;
constexpr uint64_t kBoosterList = 0x113a790;
constexpr uint64_t kCinemaMode = 0x113b008;
constexpr uint64_t kFilmStart = 0x113b030;
constexpr uint64_t kFilmLength = 0x113ab68;
constexpr uint64_t kLoadoutBooster0 = 0x1147544;
constexpr uint64_t kLoadoutBooster1 = 0x1147548;
constexpr uint64_t kMenuManager = 0x1136640;
constexpr uint64_t kPadButtons = 0x14b7fb0;
constexpr uint64_t kLocalPlayerCell = 0x115bd20;
constexpr uint64_t kLocalPlayerId = 0x115bd28;

constexpr uint64_t kDrawVtable = 0x103f550;
constexpr uint64_t kDrawSlot = 0x103f558;
constexpr uint64_t kDrawStub = 0x55c5b0;
constexpr uint64_t kShowWeaponSlot = 0x1027030;
constexpr uint64_t kPanelDrawSlot = 0x103f768;

constexpr uint64_t fLookupPointer = 0xa15e70;
constexpr uint64_t fTextWidget = 0x5555a0;
constexpr uint64_t fImageWidget = 0x5559f0;
constexpr uint64_t fTextFind = 0xcddc00;
constexpr uint64_t fMaterial = 0x5558c0;
constexpr uint64_t fQuad = 0x555c60;
constexpr uint64_t fMenuQuad = 0xbdce90;
constexpr uint64_t fProject = 0x5566b0;
constexpr uint64_t fDrawText = 0xcc9a70;
constexpr uint64_t fHandleByName = 0x555140;
constexpr uint64_t fTeamColours = 0x64aeb0;
constexpr uint64_t fCoopMoney = 0x64cbd0;
constexpr uint64_t fDoCallback = 0x5c8840;
constexpr uint64_t fSoundInit = 0x3dd780;
constexpr uint64_t fSoundPlay = 0x9b5c20;
constexpr uint64_t fCurrentWeapon = 0x71dec0;
constexpr uint64_t fShowWeaponHud = 0x954740;
constexpr uint64_t fPanelDraw = 0x563a20;

constexpr float kPi = 3.14159265f;
constexpr float kHalfPi = 1.57079633f;
constexpr float kTwoPi = 6.28318531f;
constexpr float kPerTick = 1.0f / 300.0f;   // clocks run at 300 ticks per second

// Net image ids (table 1).
constexpr uint32_t kSquare = 0x12ffe6ae;
constexpr uint32_t kRing = 0x68951e57;

// The draw context of the current Draw call; passed through to every primitive untouched.
void *g_ctx = nullptr;

void *lookup_pointer(uint32_t id) { return guest<void *(U2MP_GUEST *)(uint32_t)>(fLookupPointer)(id); }

const char *text_find(uint32_t id) {
    return guest<const char *(U2MP_GUEST *)(void *, uint32_t)>(fTextFind)(image.at<void>(kTextDb), id);
}

// The text widget draw is variadic in the game: the scale is a named float (xmm0) and one value
// follows. A variadic prototype makes the compiler set al, as the game's prologue expects.
using TextWidgetFn = void(U2MP_GUEST *)(void *, const void *, const char *, float, ...);

void text_widget_int(uint64_t widget, const float *pos, const char *fmt, int32_t value) {
    guest<TextWidgetFn>(fTextWidget)(reinterpret_cast<void *>(widget), pos, fmt, 1.0f, value);
}
void text_widget_str(uint64_t widget, const float *pos, const char *fmt, const char *value) {
    guest<TextWidgetFn>(fTextWidget)(reinterpret_cast<void *>(widget), pos, fmt, 1.0f, value);
}

float hud_alpha() { return global<float>(kHudAlpha); }
int32_t frames() { return global<int32_t>(kFrames); }

// ---------------------------------------------------------------------------------------------
// Material tables, the shared log-once set and the resolve cache
// ---------------------------------------------------------------------------------------------

struct Handle {
    uint32_t id, table;
    int32_t index;
};

Handle handle_at(uint64_t address) {
    return Handle{load<uint32_t>(address), load<uint32_t>(address + 4), load<int32_t>(address + 8)};
}
Handle net_image(uint32_t sid_value) { return Handle{sid_value, 1, -1}; }
uint64_t handle_key(uint32_t id, uint32_t table) { return uint64_t(id) | uint64_t(table) << 32; }

uint64_t material_table(uint32_t table) {
    if (table > 2) return 0;
    return global<uint64_t>(kMaterialTables + 8 * uint64_t(table));
}
uint32_t table_count(uint64_t table) { return table ? load<uint16_t>(table + 2) : 0; }
const char *table_name(uint32_t table) {
    switch (table) {
    case 0: return "hud";
    case 1: return "net";
    case 2: return "menu";
    default: return "?";
    }
}

void *material(uint64_t key, int32_t index) {
    return guest<void *(U2MP_GUEST *)(uint64_t, int32_t)>(fMaterial)(key, index);
}

// One set for every "not resolved" style message, so each image is reported only once per run.
struct LogKey {
    uint32_t id, table;
};
LogKey g_logged[96];
int g_logged_count = 0;

bool first_report(uint32_t id, uint32_t table) {
    for (int i = 0; i < g_logged_count; ++i)
        if (g_logged[i].id == id && g_logged[i].table == table) return false;
    if (g_logged_count >= 96) return false;
    g_logged[g_logged_count++] = LogKey{id, table};
    return true;
}

struct Resolved {
    bool ok;
    Handle handle;
};

// Remembers the outcome of a material lookup per (handle, table state) so the game is not asked
// again every frame. Its size and replacement order are observable through the guest calls.
struct CacheEntry {
    Handle input;
    uint64_t table;
    uint32_t count;
    Resolved result;
};
CacheEntry g_cache[64];
int g_cache_used = 0;
int g_cache_next = 0;   // round-robin victim once full

Resolved resolve(const Handle &h, const char *what) {
    if (h.id == 0) return Resolved{false, h};
    uint64_t table = material_table(h.table);
    uint32_t count = table_count(table);
    for (int i = 0; i < g_cache_used; ++i) {
        const CacheEntry &e = g_cache[i];
        if (e.input.id == h.id && e.input.table == h.table && e.input.index == h.index && e.table == table &&
            e.count == count)
            return e.result;
    }

    void *found = table ? material(handle_key(h.id, h.table), h.index) : nullptr;
    Resolved r{found != nullptr, h};
    if (found) {
        uint64_t entries = load<uint64_t>(table + 8);
        uint64_t at = reinterpret_cast<uint64_t>(found);
        if (entries && at >= entries && at < entries + 0x28 * uint64_t(count)) {
            r.handle.index = int32_t((at - entries) / 0x28);
            r.handle.id = load<uint32_t>(at + 8);
        }
    }

    if (!r.ok) {
        if (first_report(h.id, h.table))
            log("hud: image %s id %08x table %u (%s) index %d not resolved: %s table %s, %u materials%s", what, h.id,
                h.table, table_name(h.table), h.index, table_name(h.table), table ? "set" : "NULL", count,
                h.table == 1 ? " (PS4 net.pak by id and by PS3 name, then net-u2mp)" : "");
    } else if (r.handle.id != h.id) {
        if (first_report(h.id, h.table))
            log("hud: image %s id %08x table %u -> PS4 id %08x index %d", what, h.id, h.table, r.handle.id,
                r.handle.index);
    }

    CacheEntry entry{h, table, count, r};
    if (g_cache_used < 64) {
        g_cache[g_cache_used++] = entry;
    } else {
        g_cache[g_cache_next] = entry;
        g_cache_next = (g_cache_next + 1) % 64;
    }
    return r;
}

// ---------------------------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------------------------

// Image widget: the game draws it; when its material cannot be found we say so once.
void image_widget(uint64_t widget, const float *pos, float alpha, const char *what) {
    if (!widget) return;
    guest<void(U2MP_GUEST *)(void *, void *, const void *, float)>(fImageWidget)(
        g_ctx, reinterpret_cast<void *>(widget), pos, alpha);
    uint32_t table = load<uint32_t>(widget + 0x10) == 0x4240ef2e ? 1 : 0;
    uint32_t id = load<uint32_t>(widget + 0x14);
    if (load<int32_t>(widget + 0x18) >= 0) return;
    if (id != 0 && material(handle_key(id, table), -1)) return;
    uint32_t key_id = id ? id : uint32_t(widget);
    if (!first_report(key_id, table | 0x100)) return;
    uint64_t name = load<uint64_t>(widget);
    uint64_t mt = material_table(table);
    log("hud: image %s (widget %s) id %08x table %u (%s) not resolved: %s table %s, %u materials", what,
        name ? reinterpret_cast<const char *>(name) : "(no name)", id, table, table_name(table), table_name(table),
        mt ? "set" : "NULL", table_count(mt));
}

void frame_image(uint64_t set, uint64_t field) {
    const float origin[2] = {0.0f, 0.0f};
    image_widget(load<uint64_t>(set + field), origin, 1.0f, "frame");
}

void widget_int(uint64_t set, uint64_t field, const char *fmt, int32_t value) {
    uint64_t widget = load<uint64_t>(set + field);
    if (!widget) return;
    const float origin[2] = {0.0f, 0.0f};
    text_widget_int(widget, origin, fmt, value);
}

void widget_label(uint64_t set, uint64_t field, uint32_t text_id) {
    uint64_t widget = load<uint64_t>(set + field);
    if (!widget) return;
    const char *text = text_find(text_id);
    if (!text) return;
    const float origin[2] = {0.0f, 0.0f};
    text_widget_str(widget, origin, "%s", text);
}

using QuadFn = void(U2MP_GUEST *)(void *, uint64_t, int32_t, const V4 *, bool, bool, V2, V2, V2, V2, V2);

void quad(const Handle &h, V4 colour, V2 pos, V2 size, bool centred = false, V2 dir = {0.0f, -1.0f},
          V2 uv0 = {0.0f, 0.0f}, V2 uv1 = {1.0f, 1.0f}, const char *what = "quad") {
    if (colour.w <= 0.0f) return;
    Resolved r = resolve(h, what);
    if (!r.ok) return;
    guest<QuadFn>(fQuad)(g_ctx, handle_key(r.handle.id, r.handle.table), r.handle.index, &colour, centred, true, pos,
                         size, dir, uv0, uv1);
}

void rect(const Handle &h, V4 colour, V2 p0, V2 p1, V2 uv0 = {0.0f, 0.0f}, V2 uv1 = {1.0f, 1.0f},
          const char *what = "rect") {
    quad(h, colour, p0, V2{p1.x - p0.x, p1.y - p0.y}, false, V2{0.0f, -1.0f}, uv0, uv1, what);
}

// A quad with four free corners goes through the menu quad, which takes a raw vertex block.
void free_quad(const Handle &h, const V4 &colour, const V2 (&corner)[4], const V2 (&uv)[4], const char *what) {
    if (colour.w <= 0.0f) return;
    Resolved r = resolve(h, what);
    if (!r.ok) return;
    void *mat = material(handle_key(r.handle.id, r.handle.table), r.handle.index);
    if (!mat) return;

    alignas(16) unsigned char context[48] = {};
    alignas(16) unsigned char info[32] = {};
    alignas(16) unsigned char verts[0x110] = {};
    std::memcpy(context, &g_ctx, 8);
    std::memcpy(info, &mat, 8);
    const V4 white{1.0f, 1.0f, 1.0f, 1.0f};
    for (int i = 0; i < 4; ++i) {
        unsigned char *v = verts + 0x40 * i;
        const float z = 0.0f;
        std::memcpy(v + 0x00, &corner[i].x, 4);
        std::memcpy(v + 0x04, &corner[i].y, 4);
        std::memcpy(v + 0x08, &z, 4);
        std::memcpy(v + 0x0c, &uv[i].x, 4);
        std::memcpy(v + 0x10, &uv[i].y, 4);
        std::memcpy(v + 0x20, &colour, 16);
        std::memcpy(v + 0x30, &white, 16);
    }
    guest<void(U2MP_GUEST *)(void *, void *, void *)>(fMenuQuad)(context, info, verts);
}

// A circular meter made of thin segments; more segments when it is fuller or bigger.
void ring_meter(V2 centre, const Handle &h, V4 colour, float scale, float fill) {
    const float r_inner = 30.0f, r_outer = 50.0f;
    Resolved r = resolve(h, "ring meter");
    if (!r.ok) return;
    float c = clamp01(fill);
    float m = scale > 1.0f ? scale : 1.0f;
    int32_t n = trunc_int(m * ((1.0f - c) * 10.0f + c * 30.0f));
    if (n <= 0) return;
    colour.w = colour.w * hud_alpha();
    float ri = scale * r_inner, ro = scale * r_outer;
    float uv_out = 0.45f;
    float uv_in = r_inner * 0.45f / r_outer;
    float step = fill * kTwoPi / float(n);
    for (int32_t i = 0; i < n; ++i) {
        float a0 = float(i) * step + kHalfPi;
        float a1 = float(i + 1) * step + kHalfPi;
        float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
        const V2 corner[4] = {{centre.x + ri * c0, centre.y + ri * s0},
                              {centre.x + ro * c0, centre.y + ro * s0},
                              {centre.x + ro * c1, centre.y + ro * s1},
                              {centre.x + ri * c1, centre.y + ri * s1}};
        const V2 uv[4] = {{0.5f + uv_in * c0, 0.5f + uv_in * s0},
                          {0.5f + uv_out * c0, 0.5f + uv_out * s0},
                          {0.5f + uv_out * c1, 0.5f + uv_out * s1},
                          {0.5f + uv_in * c1, 0.5f + uv_in * s1}};
        free_quad(r.handle, colour, corner, uv, "ring meter");
    }
}

// An arrow pointing at something; pushed back from the point so its tip sits there.
void edge_arrow(const Handle &h, V2 pos, uint32_t colour, float scale, float angle, bool turned) {
    float size = scale * 40.0f;
    float back = size * (turned ? -0.25f : -0.5f);
    V2 dir = turned ? V2{cosf(angle - kHalfPi), -sinf(angle - kHalfPi)} : V2{0.0f, -1.0f};
    V2 at{pos.x + back * -cosf(angle), pos.y + back * sinf(angle)};
    quad(h, argb(colour), at, V2{size, size}, true, dir, V2{0.0f, 0.0f}, V2{1.0f, 1.0f}, "arrow");
}

struct Extent {
    float w, h;
};

enum : uint32_t { kTextCentred = 1, kTextNormal = 8, kTextMeasure = 12 };

Extent draw_text(float x, float y, float scale, const V4 &colour, uint32_t flags, float box_w, float box_h,
                 const char *text) {
    if (!text || !*text) return Extent{0.0f, 0.0f};
    alignas(16) unsigned char state[0xc0] = {};
    auto put = [&](size_t at, float v) { std::memcpy(state + at, &v, 4); };
    auto put_u32 = [&](size_t at, uint32_t v) { std::memcpy(state + at, &v, 4); };
    put(0x00, x);
    put(0x04, y);
    put(0x14, -1.0f);
    put(0x1c, -1.0f);
    put(0x20, scale);
    put(0x24, scale);
    state[0x28] = uint8_t(trunc_int(clamp01(colour.x) * 255.0f));
    state[0x29] = uint8_t(trunc_int(clamp01(colour.y) * 255.0f));
    state[0x2a] = uint8_t(trunc_int(clamp01(colour.z) * 255.0f));
    state[0x2b] = uint8_t(trunc_int(clamp01(colour.w) * 255.0f));
    put(0x2c, 1.0f);
    put_u32(0x30, 0xffffffffu);
    put_u32(0x34, flags);
    put(0x50, 1.0f);
    put(0x64, 1.0f);
    put(0x78, 1.0f);
    put(0x8c, 1.0f);
    put(0x90, 1.0f);
    put(0x94, box_w);
    put(0x98, box_h);
    guest<void(U2MP_GUEST *)(void *, const char *)>(fDrawText)(state, text);
    if (!(flags & 4)) return Extent{0.0f, 0.0f};
    Extent e;
    std::memcpy(&e.w, state + 0x9c, 4);
    std::memcpy(&e.h, state + 0x98, 4);
    return e;
}

// A fixed-size text field from game memory, cut at its first NUL.
template <size_t N> struct FieldText {
    char s[N + 1];
    explicit FieldText(uint64_t address) {
        std::memcpy(s, reinterpret_cast<const void *>(address), N);
        s[N] = 0;
    }
};

// ---------------------------------------------------------------------------------------------
// Game state helpers
// ---------------------------------------------------------------------------------------------

uint64_t net_game_record() {
    uint64_t list = reinterpret_cast<uint64_t>(lookup_pointer(0x3b9a067d));
    if (!list) return 0;
    int16_t index = global<int16_t>(kNgm + 0x1ca0);
    if (index < 0 || index >= load<int32_t>(list)) return 0;
    uint64_t records = load<uint64_t>(list + 8);
    if (!records) return 0;
    return records + 0x90 * uint64_t(index);
}

bool is_coop_type(int32_t type) { return type == 4 || type == 8 || type == 9 || type == 12; }

uint32_t team_score(int32_t team) {
    uint64_t table = global<uint64_t>(kScoreTable);
    if (!table || team < 0 || team > 7) return 0;
    return load<uint16_t>(table + 0x3dee + 0x34 * uint64_t(team));
}

uint32_t team_colour(int32_t team) {
    alignas(16) unsigned char out[0x40] = {};
    guest<void(U2MP_GUEST *)(void *, void *, int32_t, int32_t)>(fTeamColours)(out, image.at<void>(kNgm), team, -1);
    return load<uint32_t>(reinterpret_cast<uint64_t>(out) + 0x10);
}

// The object behind a {slot pointer, id} handle, or 0 when the handle no longer names it.
uint64_t live_object(uint64_t slot, uint32_t id) {
    if (!slot || !id) return 0;
    uint64_t object = load<uint64_t>(slot);
    if (!object || load<uint32_t>(object + 0x44) != id) return 0;
    return object;
}

uint64_t tracker(int i) { return abs_of(kNgm) + 0x10 + 0x2c0 * uint64_t(i); }

// Shows "minutes:seconds" left of the time limit, counted from the given start-frame field.
void time_left(uint64_t set, uint64_t start_field) {
    int32_t limit = trunc_int(global<float>(kNgm + 0x21b8) * 300.0f);
    if (limit <= 0) return;
    int32_t elapsed = wrap_sub(frames(), global<int32_t>(kNgm + start_field));
    int32_t secs = trunc_int(float(wrap_sub(limit, elapsed)) * kPerTick);
    int32_t mins = secs / 60;
    widget_int(set, 0x40, "%i:", mins % 60);
    widget_int(set, 0x48, "%02i", secs % 60);
}

// ---------------------------------------------------------------------------------------------
// Score panel, Turf War panel and carrier icon
// ---------------------------------------------------------------------------------------------

// How far the territory capture (or loss) has run, 0..1.
float territory_progress(uint64_t obj) {
    int32_t now = frames();
    auto part = [&](uint64_t end_field, uint64_t length_field) {
        int32_t len = trunc_int(float(load<int32_t>(obj + length_field)) * 300.0f);
        int32_t sum = wrap_add(wrap_sub(now, load<int32_t>(obj + end_field)), len);
        return (float(sum) * kPerTick) / (float(len) * kPerTick);
    };
    if (load<int32_t>(obj + 0x258) != 0) return clamp01(part(0x258, 0x268));
    if (load<int32_t>(obj + 0x25c) != 0) return clamp01(1.0f - part(0x25c, 0x26c));
    return 0.0f;
}

void turf_war_panel(uint64_t set, uint64_t local) {
    frame_image(set, 0x60);
    time_left(set, 0x1d5c);
    uint64_t img = load<uint64_t>(set + 0x60);
    uint64_t tw = load<uint64_t>(set + 0x78);
    if (!img) return;

    float rx = load<float>(set + 0x68), ry = load<float>(set + 0x6c);
    float rw = load<float>(set + 0x70), rh = load<float>(set + 0x74);
    float step = rw * 0.2f;
    float half = rh * 0.5f;
    float x0 = (load<float>(img + 0x24) + rx) + step * 0.5f;
    float top = load<float>(img + 0x28) + ry;

    alignas(16) unsigned char templ[0x40] = {};
    if (tw) std::memcpy(templ, reinterpret_cast<const void *>(tw), 0x2c);

    uint64_t ngm = abs_of(kNgm);
    bool reversed = local && load<int32_t>(local + 0x134) == 1;
    for (int i = 0; i < 5; ++i) {
        int32_t number = reversed ? 5 - i : i + 1;

        uint32_t handles = load<uint32_t>(ngm + 0x1dd0);
        if (handles > 5) handles = 5;
        uint64_t obj = 0;
        for (uint32_t j = 0; j < handles && !obj; ++j) {
            uint64_t slot = load<uint64_t>(ngm + 0x1d80 + 0x10 * uint64_t(j));
            uint32_t id = load<uint32_t>(ngm + 0x1d88 + 0x10 * uint64_t(j));
            if (!slot || !id) continue;
            uint64_t candidate = load<uint64_t>(slot);
            if (candidate && load<uint32_t>(candidate + 0x44) == id && load<int32_t>(candidate + 0x250) == number)
                obj = candidate;
        }
        if (!obj) continue;

        int32_t owner = load<int32_t>(obj + 0x234);
        V4 col = argb((owner < 0 ? 0xb0f0e030u : team_colour(owner)) | 0xff000000u);
        V2 p0{(x0 + step * float(i)) - half, top};

        int32_t capturer = load<int32_t>(obj + 0x238);
        if (capturer >= 0) {
            float progress = territory_progress(obj);
            if (progress < 0.99f)
                ring_meter(V2{p0.x + half, p0.y + half}, net_image(kRing), argb(team_colour(capturer) | 0xe0000000u),
                           0.4f, progress);
        }
        rect(net_image(kSquare), V4{col.x * 0.5f, col.y * 0.5f, col.z * 0.5f, col.w}, p0,
             V2{p0.x + rh, p0.y + rh});
        if (!tw) continue;

        char text[16];
        std::snprintf(text, sizeof text, "%i", i + 1);
        alignas(16) unsigned char widget[0x40];
        std::memcpy(widget, templ, sizeof widget);
        auto put = [&](size_t at, float v) { std::memcpy(widget + at, &v, 4); };
        auto get = [&](size_t at) {
            float v;
            std::memcpy(&v, widget + at, 4);
            return v;
        };
        // Dark shadow first, then the number in the territory colour 2 units higher.
        put(0x08, rh);
        put(0x0c, rh);
        put(0x14, 0.7f);
        put(0x18, 0.0f);
        put(0x1c, 0.0f);
        put(0x20, 0.0f);
        put(0x24, 0.5647059f);
        const float pos_a[2] = {p0.x, p0.y};
        text_widget_str(reinterpret_cast<uint64_t>(widget), pos_a, "%s", text);
        put(0x04, get(0x04) - 2.0f);
        put(0x18, col.x);
        put(0x1c, col.y);
        put(0x20, col.z);
        put(0x24, col.w);
        const float pos_b[2] = {p0.x, p0.y};
        text_widget_str(reinterpret_cast<uint64_t>(widget), pos_b, "%s", text);
    }
}

// True when the flag's blink tag is set (a boolean value that is non-zero).
bool blink_tag(uint64_t flag) {
    uint64_t tags = load<uint64_t>(flag + 0x68);
    if (!tags) return false;
    uint64_t entries = load<uint64_t>(tags);
    uint32_t count = load<uint32_t>(tags + 8);
    if (!entries || count < 1 || count > 4096) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t entry = entries + 0x10 * uint64_t(i);
        if (load<uint32_t>(entry) != 0xe9a3dd67) continue;
        uint64_t value = load<uint64_t>(entry + 8);
        if (!value || load<uint16_t>(value) != 1) return false;
        uint64_t p = load<uint64_t>(value + 8);
        return p && load<uint32_t>(p) != 0;
    }
    return false;
}

// When the carry handle is gone, the flag may still be in a character's hands.
uint64_t held_capture_flag() {
    for (int i = 0; i < 10; ++i) {
        uint64_t t = tracker(i);
        uint64_t character = live_object(load<uint64_t>(t + 0x10), load<uint32_t>(t + 0x18));
        if (!character) continue;
        uint64_t obj = load<uint64_t>(character + 0x2928);
        if (!obj) continue;
        uint64_t vtable = load<uint64_t>(obj);
        if (!vtable) continue;
        uint64_t fn = load<uint64_t>(vtable + 0xe8);
        if (!fn) continue;
        void *held = reinterpret_cast<void *(U2MP_GUEST *)(void *, int)>(fn)(reinterpret_cast<void *>(obj), 0);
        uint64_t h = reinterpret_cast<uint64_t>(held);
        if (h && load<uint32_t>(h + 0x30) == 0x310f465e) return h;
    }
    return 0;
}

void carrier_icon(uint64_t set, int32_t team) {
    uint64_t ngm = abs_of(kNgm);
    uint64_t flag = live_object(load<uint64_t>(ngm + 0x1cc0), load<uint32_t>(ngm + 0x1cc8));
    if (!flag) flag = held_capture_flag();
    if (!flag) return;

    int32_t owner = load<int32_t>(flag + 0x440);
    uint64_t carrier = 0;
    for (int i = 0; i < 10 && !carrier; ++i)
        if (load<int32_t>(tracker(i) + 0x120) == owner) carrier = tracker(i);

    uint64_t field = !carrier ? 0x18 : load<int32_t>(carrier + 0x134) == team ? 0x20 : 0x28;
    if (carrier && blink_tag(flag) && fmodf(float(frames()) / 300.0f, 1.0f) >= 0.5f) return;
    frame_image(set, field);
}

void score_panel(uint64_t set, uint64_t local, int32_t type, int32_t team, int32_t other) {
    if (type == 7) {
        turf_war_panel(set, local);
        return;
    }
    frame_image(set, 0x58);
    widget_int(set, 0x08, "%i", int32_t(team_score(team)));
    widget_int(set, 0x10, "%i", int32_t(team_score(other)));
    if (type == 10) {
        widget_label(set, 0x38, 0xe1167dfa);
        widget_int(set, 0x30, "%i", wrap_add(global<int32_t>(kNgm + 0x1d70), 1));
        time_left(set, 0x1d60);
    } else {
        widget_label(set, 0x38, 0xd38a7d18);
        widget_int(set, 0x30, "%i", global<int32_t>(kNgm + 0x21b4));
        time_left(set, 0x1d5c);
        if (type != 1 && type != 2) carrier_icon(set, team);
    }
}

// ---------------------------------------------------------------------------------------------
// Centre message
// ---------------------------------------------------------------------------------------------

// Smooth 0 -> 1 step between a and b.
float ramp(float x, float a, float b) {
    if (x < a) return 0.0f;
    if (x >= b) return 1.0f;
    return (cosf(clamp01((x - a) / (b - a)) * kPi) - 1.0f) * -0.5f;
}

void centre_message(uint64_t element) {
    uint64_t msg = element + 0x1a90;
    if (load<uint8_t>(msg) == 0) return;
    FieldText<128> text(msg);
    int32_t glyphs = 0;
    for (const char *p = text.s; *p; ++p)
        if ((uint8_t(*p) & 0xc0) != 0x80) ++glyphs;

    float u = float(wrap_sub(global<int32_t>(kClock2Frame), load<int32_t>(msg + 0x80))) * (1.0f / 75.0f);
    float u_last = u - 0.1f * float(glyphs - 1);
    if (u_last > 10.0f) {
        store<uint8_t>(msg, 0);
        return;
    }
    store<int32_t>(msg + 0x88, 0);
    float alpha = ramp(u, 0.0f, 1.0f) - ramp(u_last, 9.0f, 10.0f);
    V4 colour = argb(load<uint32_t>(msg + 0x84));
    colour.w = clamp01(alpha) * hud_alpha();
    draw_text((1280.0f - 800.0f) * 0.5f, 720.0f * 0.4f, 1.2f, colour, kTextCentred, 800.0f, 20.0f, text.s);
}

// ---------------------------------------------------------------------------------------------
// Co-op money / wave panel
// ---------------------------------------------------------------------------------------------

// How much of the current wave's time window is left, 1 at its start down to 0.
float wave_left() {
    uint64_t record = net_game_record();
    uint32_t id = record ? load<uint32_t>(record + 0x7c) : 0;
    uint64_t table = id ? reinterpret_cast<uint64_t>(lookup_pointer(id)) : 0;
    if (!table) return 0.0f;
    int32_t start = global<int32_t>(kNgm + 0x1d64);
    int32_t end = global<int32_t>(kNgm + 0x1d68);
    float secs = float(wrap_sub(end > start ? end : frames(), start)) * kPerTick;
    int32_t wave = wrap_add(global<int32_t>(kNgm + 0x1d6c), 1);
    uint64_t entries = load<uint64_t>(table + 8);
    if (!entries) return 0.0f;
    int32_t count = load<int32_t>(table);
    for (int32_t k = 0; k < count; ++k) {
        uint64_t cur = entries + 12 * uint64_t(k);
        uint64_t prev = entries + 12 * uint64_t(k > 0 ? k - 1 : 0);
        int32_t cur_wave = load<int32_t>(cur);
        if (cur_wave != wave) {
            if (wave < cur_wave) return 0.0f;
            continue;
        }
        int32_t prev_time = load<int32_t>(prev + 4), cur_time = load<int32_t>(cur + 4);
        float from = float(prev_time), to = float(cur_time);
        if (secs < from || !(to > secs)) continue;
        return (from - secs) / float(wrap_sub(cur_time, prev_time)) + 1.0f;
    }
    return 0.0f;
}

void coop_panel(uint64_t set, uint32_t flags) {
    if (!(flags & 0x8000)) return;
    uint64_t record = net_game_record();
    if (!record || load<int32_t>(record + 0xc) != 9) return;
    if (global<float>(kWaveAlpha) != 0.0f) return;

    const float *pos = reinterpret_cast<const float *>(set + 0xc0);   // the widget set's own position
    image_widget(load<uint64_t>(set + 0xd0), pos, 1.0f, "co-op meter back");
    uint64_t txt = load<uint64_t>(set + 0xc8);
    int32_t money = guest<int32_t(U2MP_GUEST *)(void *)>(fCoopMoney)(image.at<void>(kNgm));
    if (txt) text_widget_int(txt, pos, "x%i", money);
    float left = wave_left();
    V4 colour = txt ? V4{load<float>(txt + 0x18), load<float>(txt + 0x1c), load<float>(txt + 0x20),
                         load<float>(txt + 0x24)}
                    : V4{1.0f, 1.0f, 1.0f, 1.0f};
    ring_meter(V2{load<float>(set + 0xc0), load<float>(set + 0xc4)}, net_image(kRing), colour, 0.475f, left);
}

// ---------------------------------------------------------------------------------------------
// Booster icons
// ---------------------------------------------------------------------------------------------

bool boosters_allowed() {
    if (global<int32_t>(kNetState) < 2) return false;
    uint64_t record = net_game_record();
    int32_t type = record ? load<int32_t>(record + 0xc) : -1;
    int32_t off = global<int32_t>(kNgm + 0x1e80);
    if (is_coop_type(type)) return off == 0;
    return off == 0 || !record || load<int32_t>(record + 0x6c) == 0;
}

uint64_t booster_by_id(uint32_t id) {
    if (id == 0) return 0;
    uint32_t count = global<uint32_t>(kBoosterListCount);
    if (count > 64) count = 64;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t record = global<uint64_t>(kBoosterList + 8 * uint64_t(i));
        if (record && load<uint32_t>(record + 0xc) == id) return record;
    }
    return 0;
}

uint64_t booster_by_index(int32_t index) {
    if (index < 0) return 0;
    uint64_t table = reinterpret_cast<uint64_t>(lookup_pointer(0x916e03b0));
    if (!table || index >= load<int32_t>(table)) return 0;
    uint64_t records = load<uint64_t>(table + 8);
    if (!records) return 0;
    return records + 0x20 * uint64_t(index);
}

void booster_pair(uint64_t a, uint64_t b, float fade) {
    V4 white{1.0f, 1.0f, 1.0f, fade * 0.75f};
    float x = global<float>(kHudOffsetX) + 90.0f;
    V2 size{76.8f, 38.4f};
    float second_y = 150.0f;
    if (a && load<uint32_t>(a + 0xc) != 0x05454b23) {
        quad(net_image(load<uint32_t>(a + 8)), white, V2{x, 150.0f}, size, false, V2{0.0f, -1.0f}, V2{0.0f, 0.0f},
             V2{1.0f, 1.0f}, "booster");
        second_y = 180.72f;
    }
    if (b && load<uint32_t>(b + 0xc) != 0x01845694)
        quad(net_image(load<uint32_t>(b + 8)), white, V2{x, second_y}, size, false, V2{0.0f, -1.0f},
             V2{0.0f, 0.0f}, V2{1.0f, 1.0f}, "booster");
}

void boosters(uint64_t element, uint64_t local, uint32_t flags, bool coop) {
    float fade = load<float>(element + 0x1920);
    if (load<uint8_t>(abs_of(kNgm) + 0x209c) != 0 && (flags & 0x40000) && global<int32_t>(kCinemaMode) != 0 &&
        local) {
        uint64_t a = booster_by_index(load<int32_t>(local + 0x2a0));
        uint64_t b = booster_by_index(load<int32_t>(local + 0x2a4));
        booster_pair(a, b, fade);
    }
    if (!coop && (flags & 0x10) && boosters_allowed())
        booster_pair(booster_by_id(global<uint32_t>(kLoadoutBooster0)),
                     booster_by_id(global<uint32_t>(kLoadoutBooster1)), fade);
}

// ---------------------------------------------------------------------------------------------
// Message list (kill feed)
// ---------------------------------------------------------------------------------------------

// Measures (draw = false) or draws one message line; returns its width.
float message_line(uint64_t entry, float x, float y, float alpha, float s, bool draw) {
    auto entry_colour = [&](uint64_t field) {
        uint32_t c = load<uint32_t>(entry + field);
        return V4{float((c >> 16) & 0xff) / 255.0f, float((c >> 8) & 0xff) / 255.0f, float(c & 0xff) / 255.0f,
                  alpha};
    };
    V4 colour1 = entry_colour(0x30), colour2 = entry_colour(0x64);
    FieldText<48> text1(entry + 0x00), text2(entry + 0x34);
    uint32_t flags = load<uint32_t>(entry + 0x78);
    int32_t count = load<int32_t>(entry + 0x7c);

    if (draw && count != 0 && (flags & 6)) {
        bool red = flags & 2;
        const char *prefix = "";
        if (!red) {
            const char *found = text_find(0x9b5b3c66);
            if (found) prefix = found;
        }
        char badge[24];
        std::snprintf(badge, sizeof badge, "%s%i", prefix, count);
        V4 bc = argb(red ? 0xffd73e60u : 0xff5495bbu);
        Resolved square = resolve(net_image(kSquare), "message badge");
        if (square.ok)
            quad(square.handle, V4{bc.x, bc.y, bc.z, alpha}, V2{x - 58.0f, y - 1.0f}, V2{50.0f, 25.0f}, false,
                 V2{0.0f, -1.0f}, V2{0.0f, 0.0f}, V2{1.0f, 1.0f}, "message badge");
        V4 tc = square.ok ? V4{0.0f, 0.0f, 0.0f, alpha} : V4{bc.x, bc.y, bc.z, alpha};
        draw_text(x - 58.0f, y + 4.0f, 0.4f, tc, kTextCentred, 50.0f, 25.0f, badge);
    }

    float ts = 0.5f * s;
    float cx = x;
    if (text1.s[0]) {
        Extent m = draw_text(cx, y, ts, colour1, kTextMeasure, 999999.0f, 40.0f, text1.s);
        if (draw) draw_text(cx, y, ts, colour1, kTextNormal, 999999.0f, 40.0f, text1.s);
        cx += m.w;
    }
    cx += 10.0f * s;

    uint64_t icon = load<uint64_t>(entry + 0x68);
    uint64_t name = icon ? load<uint64_t>(icon + 0x10) : 0;
    if (name && *reinterpret_cast<const char *>(name)) {
        float sx = load<float>(icon + 0x18), sy = load<float>(icon + 0x1c);
        if (sx > 0.0f && sx < 1024.0f && sy > 0.0f && sy < 1024.0f) {
            float k = 0.45f * s;
            float w = sx * k, h = sy * k;
            float div = load<float>(icon + 0x28), mul = load<float>(icon + 0x30);
            float advance = (div > 0.0f && mul > 0.0f && mul / div < 4.0f) ? (w * mul) / div : w;
            if (draw) {
                struct NameHandle {
                    uint64_t key;
                    int32_t index;
                };
                NameHandle nh = guest<NameHandle(U2MP_GUEST *)(const char *, int)>(fHandleByName)(
                    reinterpret_cast<const char *>(name), 0);
                quad(Handle{uint32_t(nh.key), uint32_t(nh.key >> 32), nh.index}, V4{1.0f, 1.0f, 1.0f, alpha},
                     V2{cx, (y + 10.0f) - h * 0.5f}, V2{w, h}, false, V2{0.0f, -1.0f}, V2{0.0f, 0.0f},
                     V2{1.0f, 1.0f}, "message weapon icon");
            }
            cx += advance + 10.0f * s;
        }
    }

    if (flags & 8) {
        float hs = 28.0f * s;
        if (draw)
            quad(net_image(0x2fb98f98), V4{1.0f, 1.0f, 1.0f, alpha}, V2{cx - 4.0f * s, (y + 10.0f) - hs * 0.5f},
                 V2{hs, hs}, false, V2{0.0f, -1.0f}, V2{0.0f, 0.0f}, V2{1.0f, 1.0f}, "message headshot");
        cx += 2.0f * s + hs;
    }

    float w2 = 0.0f;
    if (text2.s[0]) {
        w2 = draw_text(cx, y, ts, colour2, kTextMeasure, 999999.0f, 40.0f, text2.s).w;
        if (draw) draw_text(cx, y, ts, colour2, kTextNormal, 999999.0f, 40.0f, text2.s);
    }
    return (cx + w2) - x;
}

void message_list(uint64_t element, uint64_t set) {
    uint64_t list = element + 0x40;
    int32_t count = load<int32_t>(list);
    if (count <= 0 || count > 16) return;
    float now = float(global<int32_t>(kMessageClock)) * kPerTick;
    float ha = hud_alpha();

    float xs[16], alphas[16];
    float used = 0.0f;
    for (int32_t i = 0; i < count; ++i) {
        uint64_t e = list + 8 + 0x80 * uint64_t(i);
        float appear = clamp01((now - load<float>(e + 0x70)) * 2.2222223f);
        float vanish = clamp01((load<float>(e + 0x74) - now) * 2.0f);
        used += vanish * 25.0f;
        xs[i] = 120.0f + ((-200.0f * (1.0f - appear)) * (1.0f - appear));
        alphas[i] = (appear * vanish) * ha;
    }

    uint64_t back = load<uint64_t>(set + 0xb8);
    float y = 650.0f - used;
    for (int32_t i = count - 1; i >= 0; --i) {
        uint64_t e = list + 8 + 0x80 * uint64_t(i);
        float w = message_line(e, xs[i], y, alphas[i], 1.0f, false);
        float s = w > 350.0f ? 350.0f / w : 1.0f;
        if (back) {
            const float pos[2] = {xs[i] - 8.0f, y - 2.0f};
            image_widget(back, pos, alphas[i], "message back");
        }
        message_line(e, xs[i], y, alphas[i], s, true);
        y += 25.0f;
    }
}

// ---------------------------------------------------------------------------------------------
// Names over heads
// ---------------------------------------------------------------------------------------------

void names(uint64_t element) {
    uint64_t names = element + 0x860;
    int32_t count = load<int32_t>(names + 0xc80);
    if (count > 20) count = 20;
    const V4 down{0.9411765f, 0.0941177f, 0.0627451f, 0.0f};
    const V4 grappled{0.1254902f, 0.5019608f, 0.9411765f, 0.0f};

    for (int32_t i = 0; i < count; ++i) {
        uint64_t e = names + 0xa0 * uint64_t(i);
        float t = float(global<int32_t>(kClock1Frame)) * kPerTick;
        float s = load<float>(e + 0x08);
        float x = load<float>(e + 0x00), y = load<float>(e + 0x04);
        FieldText<64> text(e + 0x38);
        V4 name_colour = load<V4>(e + 0x80);
        float name_alpha = name_colour.w;
        name_colour.w = name_colour.w * hud_alpha();

        Extent size = draw_text(0.0f, 0.0f, s, name_colour, kTextMeasure, 99999.0f, 40.0f, text.s);
        float W = size.w, H = size.h;

        bool emblem = load<uint64_t>(e + 0x90) != 0;
        int32_t rank = load<int32_t>(e + 0x9c);
        float icon = (emblem || rank > 58) ? s * 50.0f : 0.0f;
        float talk = load<uint8_t>(e + 0x98) != 0 ? s * 40.0f : 0.0f;
        float half = (W + icon) * 0.5f;

        // Keep the name block on screen, with the right edge winning over the left.
        float left = x - half;
        if (left < 128.0f) left = 128.0f;
        float right_limit = (1280.0f - (W + icon)) - 128.0f;
        if (left > right_limit) left = right_limit;
        float cx = left + half;
        float base = y - s * 25.0f;
        V2 status_at{cx, ((y - s * 150.0f) - s * 25.0f) + s * 30.0f};

        int32_t side = load<int32_t>(e + 0x0c);
        V2 arrow;
        float arrow_scale, angle;
        if (side == 0) {
            arrow = V2{cx, base - H};
            arrow_scale = 1.5f;
            angle = kHalfPi;
        } else if (side == 1) {
            arrow = V2{(cx - half) - 10.0f, base - H * 0.5f};
            arrow_scale = 1.2f;
            angle = kPi;
        } else if (side == 2) {
            float pad = (text.s[0] && name_alpha != 0.0f) ? talk * 0.5f + 6.0f : 0.0f;
            arrow = V2{((cx + half) + pad) + 10.0f, base - H * 0.5f};
            arrow_scale = 1.2f;
            angle = 0.0f;
        } else {
            arrow = V2{cx, base};
            arrow_scale = 1.5f;
            angle = -kHalfPi;
        }

        int32_t status = load<int32_t>(e + 0x30);
        V4 col = load<V4>(e + 0x20);
        bool has_arrow = !(status != 0 && side == 3);
        if (status != 0 && has_arrow) {
            float k = clamp01(1.0f - fabsf(cosf(t * 8.0f))) * 20.0f;
            arrow.x = arrow.x + cosf(angle) * k;
            arrow.y = arrow.y - sinf(angle) * k;
        }
        if (has_arrow) {
            int32_t kind = load<int32_t>(e + 0x10);
            if (kind == 0)
                edge_arrow(handle_at(names + 0xcb4), arrow, pack(col), arrow_scale * s, angle, false);
            else if (kind == 1 || kind == 2)
                edge_arrow(handle_at(names + 0xca8), arrow, pack(col), arrow_scale * s, angle, true);
        }

        if (status != 0) {
            V4 c = status == 1 ? down : grappled;
            c.w = col.w;
            float fill = load<float>(e + 0x34);
            V4 ring = c;
            if (fill < 0.25f && fmodf(t * 3.0f, 1.0f) < 0.5f) ring = V4{1.0f, 1.0f, 1.0f, col.w};
            float p = clamp01(fabsf(cosf(t * 4.0f)));
            float big = s * 50.0f;
            float pulse = big * ((1.0f - p) * 3.5f + p * 5.0f);
            float still = big * 4.5f;
            V2 dir{cosf(t), sinf(t)};
            Handle glow = handle_at(names + 0xce4);
            quad(glow, V4{c.x, c.y, c.z, col.w * 0.75f}, status_at, V2{pulse, pulse}, true, dir, V2{0.0f, 0.0f},
                 V2{1.0f, 1.0f}, "name status glow");
            quad(handle_at(names + 0xce4), V4{0.0f, 0.0f, 0.0f, col.w}, status_at, V2{still, still}, true, dir,
                 V2{0.0f, 0.0f}, V2{1.0f, 1.0f}, "name status glow");
            ring_meter(status_at, handle_at(names + 0xcf0), ring, s, fill);

            float r = s * 30.0f;
            uint64_t hud_flags = global<uint64_t>(kHudFlagsPtr);
            bool button = hud_flags && load<uint8_t>(hud_flags + 0x40c) != 0;
            uint64_t which = status == 1 ? (button ? 0xcc0 : 0xccc) : 0xcd8;
            rect(handle_at(names + which), V4{1.0f, 1.0f, 1.0f, col.w}, V2{status_at.x - r, status_at.y - r},
                 V2{status_at.x + r, status_at.y + r});
        }

        if (!text.s[0]) continue;
        float ty = base - H;

        if (!emblem && rank > 58) {
            // Rank icons are an 8 x 8 sheet; ranks above 63 use the second sheet.
            float u0 = float(rank & 7) * 0.125f;
            float v0 = float((rank & 63) >> 3) * 0.125f;
            Handle sheet = net_image(rank > 63 ? 0xf1b63553u : 0xbda0f568u);
            float hx = icon * 0.5f, hy = icon * 0.5f;
            float ix = (left + hx) + s * -8.0f;
            float iy = (ty + s * -4.0f) + hy;
            rect(sheet, V4{1.0f, 1.0f, 1.0f, name_alpha}, V2{ix - hx, iy - hy}, V2{ix + hx, iy + hy}, V2{u0, v0},
                 V2{u0 + 0.125f, v0 + 0.125f});
        }

        draw_text(left + icon, ty, s, name_colour, kTextNormal, 99999.0f, 40.0f, text.s);

        if (talk > 0.0f) {
            float hx = talk * 0.5f, hy = talk * 0.5f;
            float ix = ((cx + half) + hx) + s * 6.0f;
            float iy = ty + hy;
            rect(net_image(0xffa98ec5), V4{1.0f, 1.0f, 1.0f, name_alpha}, V2{ix - hx, iy - hy}, V2{ix + hx, iy + hy});
        }
    }
}

// ---------------------------------------------------------------------------------------------
// World markers
// ---------------------------------------------------------------------------------------------

void markers(uint64_t element) {
    uint64_t mk = element + 0x1560;
    if (load<int32_t>(mk) <= 0) return;
    int32_t frame = global<int32_t>(kClock1Frame);
    float t = float(frame) * kPerTick;
    float dt = float(global<int32_t>(kClock1Delta)) * kPerTick;
    float fade_in = dt * 4.0f;
    float fade_out = dt * -4.0f;
    uint32_t mask = global<uint32_t>(kMarkerMask);
    float ha = hud_alpha();

    for (int32_t i = 0; i < load<int32_t>(mk) && i < 8; ++i) {
        uint64_t e = mk + 0x10 + 0x40 * uint64_t(i);
        uint32_t type = load<uint32_t>(e + 0x04);
        if (type >= 32 || !((mask >> type) & 1)) continue;
        if (wrap_sub(frame, load<int32_t>(e + 0x3c)) > 300) {
            // Not refreshed for a second: fade it out for good.
            store<float>(e + 0x30, 0.0f);
            store<float>(e + 0x2c, 0.0f);
            continue;
        }
        float a = load<float>(e + 0x2c), target = load<float>(e + 0x30);
        if (a < target) a = (a + fade_in > target) ? target : a + fade_in;
        if (a > target) a = (a + fade_out < target) ? target : a + fade_out;
        store<float>(e + 0x2c, a);
        if (a == 0.0f || type >= 11) continue;

        uint64_t rec = mk + 0x210 + 0x24 * uint64_t(type);
        V4 c = argb(load<uint32_t>(e + 0x20));
        store<float>(e + 0x30, 0.0f);
        float alpha = a * 0.8f;

        V4 world = load<V4>(e + 0x10);
        alignas(16) unsigned char out[0x30] = {};
        guest<void(U2MP_GUEST *)(void *, const V4 *, float)>(fProject)(out, &world, 100.0f);
        uint64_t o = reinterpret_cast<uint64_t>(out);
        V2 pos{load<float>(o + 0x00), load<float>(o + 0x04)};
        V2 dir{load<float>(o + 0x10), load<float>(o + 0x14)};
        float out_angle = load<float>(o + 0x20);
        float out_scale = load<float>(o + 0x28);
        int32_t clamped = load<int32_t>(o + 0x2c);

        float sc = out_scale * load<float>(rec + 0x18);
        float size = sc * 40.0f;
        float pulse = clamp01(fabsf(cosf(t * 4.0f)));
        V4 col{c.x, c.y, c.z, alpha};
        if (type >= 5 && type <= 10) {
            float k = clamp01((a - 0.5f) * 2.0f);
            col.w = ((1.0f - k) * 0.1f + k) * 0.8f;
        }

        Handle pulsing = handle_at(rec + 0x0c);
        if (pulsing.id != 0) {
            float ps = size * ((1.0f - pulse) * 1.5f + pulse * 2.5f);
            quad(pulsing, col, V2{pos.x - ps * 0.5f, pos.y - ps * 0.5f}, V2{ps, ps}, false, V2{0.0f, -1.0f},
                 V2{0.0f, 0.0f}, V2{1.0f, 1.0f}, "marker pulse");
        }
        V4 icon_colour = load<uint8_t>(rec + 0x1c) != 0 ? V4{0.0f, 0.0f, 0.0f, alpha} : V4{c.x, c.y, c.z, alpha};
        quad(handle_at(rec), icon_colour, V2{pos.x - size * 0.5f, pos.y - size * 0.5f}, V2{size, size}, false,
             V2{0.0f, -1.0f}, V2{0.0f, 0.0f}, V2{1.0f, 1.0f}, "marker icon");

        if (clamped != 0) {
            float q = clamp01(1.0f - fabsf(cosf(t * 8.0f)));
            float push = (sc * 30.0f) * ((1.0f - q) * 0.8f + q * 1.1f);
            edge_arrow(handle_at(mk + 0x39c), V2{pos.x + dir.x * push, pos.y + dir.y * push},
                       pack(V4{c.x, c.y, c.z, alpha}), out_scale * 1.75f, out_angle, true);
        }
        float fill = load<float>(e + 0x34);
        if (fill != 0.0f)
            ring_meter(pos, handle_at(mk + 0x3a8), argb(load<uint32_t>(e + 0x24)), out_scale * 1.75f, fill);
        int32_t label = load<int32_t>(rec + 0x20);
        if (label >= 0) {
            char text[16];
            std::snprintf(text, sizeof text, "%i", label);
            draw_text(pos.x - 200.0f, pos.y - 100.0f * out_scale, 2.0f * out_scale,
                      V4{c.x, c.y, c.z, clamp01(a) * ha}, kTextCentred, 400.0f, 400.0f, text);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Theater (film playback) controls
// ---------------------------------------------------------------------------------------------

uint32_t g_prev_buttons = 0;
bool g_theater_logged = false;

void do_callback(uint32_t id) {
    guest<void(U2MP_GUEST *)(void *, uint32_t, uint32_t)>(fDoCallback)(image.at<void>(kMenuManager), id, 0);
}

void minutes_seconds(char (&out)[32], int32_t secs) {
    if (secs < 0) secs = 0;
    std::snprintf(out, sizeof out, "%d:%02d", secs / 60, secs % 60);
}

void theater(uint64_t element) {
    uint64_t th = element + 0x1928;
    int32_t cams = load<int32_t>(th + 0xfc);
    if (cams <= 0 || cams > 10) return;

    int32_t mode = global<int32_t>(kCinemaMode);
    int32_t n = 0;
    for (int32_t c = 0; c < cams; ++c) {
        uint64_t cam = th + 0xc + 0x18 * uint64_t(c);
        uint32_t id = load<uint32_t>(cam);
        bool shown = id == 0xe8184ce8   ? false
                     : id == 0xff502160 ? mode == 1
                     : id == 0x806c9824 ? mode == 2
                     : id == 0x887c7d6e ? mode == 0
                                        : true;   // includes the always-shown camera
        if (shown) store<uint64_t>(th + 0x100 + 8 * uint64_t(n++), cam);
    }
    store<int32_t>(th + 0x150, n);
    if (!g_theater_logged) {
        g_theater_logged = true;
        log("hud: theater HUD: %d cameras, %d shown in cinema mode %d", cams, n, mode);
    }
    if (n == 0) return;

    uint32_t buttons = global<uint32_t>(kPadButtons);
    uint32_t pressed = buttons & ~g_prev_buttons;
    g_prev_buttons = buttons;

    int32_t sel = load<int32_t>(th);
    if (!(buttons & 0x4000)) {
        uint32_t release = load<uint32_t>(th + 8);
        if (release != 0) {
            do_callback(release);
            store<uint32_t>(th + 8, 0);
        }
        if (pressed & 0x400) sel -= 1;
        if (pressed & 0x800) sel += 1;
    }
    if (sel > n - 1) sel = n - 1;
    if (sel < 0) sel = 0;
    if (sel != load<int32_t>(th)) {
        alignas(16) unsigned char sound[0x200] = {};
        guest<void(U2MP_GUEST *)(void *, uint32_t, uint32_t)>(fSoundInit)(sound, 0xf57d53ce, 0);
        guest<void(U2MP_GUEST *)(void *, uint32_t)>(fSoundPlay)(sound, 0);
    }
    store<int32_t>(th, sel);
    float smooth = load<float>(th + 4) * 0.5f + float(sel) * 0.5f;
    store<float>(th + 4, smooth);
    float fade = load<float>(th + 0x154) * 0.7f + 0.3f;
    store<float>(th + 0x154, fade);
    store<int32_t>(th + 0x158, frames());

    uint64_t selected = load<uint64_t>(th + 0x100 + 8 * uint64_t(sel));
    if (fade >= 0.9f && (pressed & 0x4000)) {
        uint32_t id = load<uint32_t>(selected);
        do_callback(id);
        store<uint32_t>(th + 8, load<uint32_t>(selected + 4));
        log("hud: theater HUD: camera %08x", id);
    }

    float A = clamp01(fade);
    Resolved square = resolve(net_image(kSquare), "theater panel");
    bool box = square.ok;
    float panel_w = float(n) * 72.0f + 40.0f;
    if (box)
        quad(square.handle, V4{0.0f, 0.0f, 0.0f, 0.6f * A}, V2{640.0f - panel_w * 0.5f, 600.0f}, V2{panel_w, 100.0f});
    for (int32_t i = 0; i < n; ++i) {
        uint64_t cam = load<uint64_t>(th + 0x100 + 8 * uint64_t(i));
        float d = float(i) - smooth;
        float ia = clamp01(1.0f - 0.3f * fabsf(d)) * A;
        float x = (640.0f + d * 72.0f) - 32.0f;
        if (i == sel && box)
            quad(square.handle, V4{1.0f, 1.0f, 1.0f, 0.35f * A}, V2{x - 4.0f, 618.0f}, V2{72.0f, 40.0f});
        quad(handle_at(cam + 8), V4{1.0f, 1.0f, 1.0f, ia}, V2{x, 622.0f}, V2{64.0f, 32.0f}, false, V2{0.0f, -1.0f},
             V2{0.0f, 0.0f}, V2{1.0f, 1.0f}, "theater camera");
    }

    const V4 white{1.0f, 1.0f, 1.0f, A};
    if (const char *name = text_find(load<uint32_t>(selected + 0x14)))
        draw_text(640.0f, 578.0f, 0.6f, white, kTextCentred, 600.0f, 20.0f, name);

    int32_t length = global<int32_t>(kFilmLength);
    int32_t pos = trunc_int(float(wrap_sub(frames(), global<int32_t>(kFilmStart))) / 300.0f);
    if (length > 0) {
        float fill = clamp01(float(pos) / float(length));
        if (box) {
            quad(square.handle, V4{1.0f, 1.0f, 1.0f, 0.25f * A}, V2{440.0f, 672.0f}, V2{400.0f, 6.0f});
            quad(square.handle, V4{1.0f, 0.82f, 0.25f, 0.9f * A}, V2{440.0f, 672.0f}, V2{400.0f * fill, 6.0f});
        }
        char text[32];
        minutes_seconds(text, pos < length ? pos : length);
        draw_text(380.0f, 666.0f, 0.5f, white, kTextCentred, 50.0f, 20.0f, text);
        minutes_seconds(text, length);
        draw_text(850.0f, 666.0f, 0.5f, white, kTextCentred, 50.0f, 20.0f, text);
    }

    float speed = global<float>(kReplaySpeed);
    if (speed != 1.0f && speed > 0.0f && speed < 100.0f) {
        char text[32];
        std::snprintf(text, sizeof text, "x%2.1f", double(speed));
        draw_text(905.0f, 666.0f, 0.5f, white, kTextCentred, 50.0f, 20.0f, text);
    }
}

// ---------------------------------------------------------------------------------------------
// Weapon panel trigger
// ---------------------------------------------------------------------------------------------

struct WeaponTrigger {
    bool on = false;
    uint64_t switches = 0;
    uint64_t game_shows = 0;
    uint64_t own_shows = 0;
    uint64_t panel_draws = 0;
    uint64_t callers[6] = {};
    uint64_t last_weapon = 0;
    int32_t show_until = 0;
    bool warned = false;
};
WeaponTrigger g_weapon;

using ShowWeaponFn = void(U2MP_GUEST *)(void *, float);

// Counts the game's own ShowWeaponHud calls and remembers who made them.
U2MP_GUEST void show_weapon_probe(void *controller, float arg) {
    g_weapon.game_shows += 1;
    uint64_t from = reinterpret_cast<uint64_t>(__builtin_return_address(0)) - reinterpret_cast<uint64_t>(image.base);
    for (uint64_t &slot : g_weapon.callers) {
        if (slot == from) break;
        if (slot == 0) {
            slot = from;
            break;
        }
    }
    guest<ShowWeaponFn>(fShowWeaponHud)(controller, arg);
}

U2MP_GUEST void panel_draw_probe(void *element, void *ctx) {
    g_weapon.panel_draws += 1;
    guest<void(U2MP_GUEST *)(void *, void *)>(fPanelDraw)(element, ctx);
}

void weapon_tick() {
    if (!g_weapon.on) return;
    uint64_t player = live_object(global<uint64_t>(kLocalPlayerCell), global<uint32_t>(kLocalPlayerId));
    if (!player) {
        g_weapon.last_weapon = 0;
        return;
    }
    uint64_t vtable = load<uint64_t>(player);
    uint64_t get = vtable ? load<uint64_t>(vtable + 0x260) : 0;
    uint64_t ctrl = load<uint64_t>(player + 0x2768);
    uint64_t ctrl_vtable = ctrl ? load<uint64_t>(ctrl) : 0;
    uint64_t show = ctrl_vtable ? load<uint64_t>(ctrl_vtable + 0x2f0) : 0;
    uint64_t probe = reinterpret_cast<uint64_t>(&show_weapon_probe);
    if (get != abs_of(fCurrentWeapon) || (show != abs_of(fShowWeaponHud) && show != probe)) {
        if (!g_weapon.warned) {
            g_weapon.warned = true;
            uint64_t base = reinterpret_cast<uint64_t>(image.base);
            log("hud: weapon panel: player slot +0x260 0x%llx / controller slot +0x2f0 0x%llx not the expected "
                "functions, weapon switch trigger off",
                (unsigned long long)(get ? get - base : 0), (unsigned long long)(show ? show - base : 0));
        }
        return;
    }

    uint64_t weapon = reinterpret_cast<uint64_t>(
        reinterpret_cast<void *(U2MP_GUEST *)(void *)>(get)(reinterpret_cast<void *>(player)));
    int32_t now = frames();
    if (weapon != g_weapon.last_weapon) {
        if (weapon && g_weapon.last_weapon) g_weapon.switches += 1;
        g_weapon.last_weapon = weapon;
        if (weapon) g_weapon.show_until = wrap_add(now, 900);   // 3 seconds
    }
    if (weapon && wrap_sub(now, g_weapon.show_until) < 0) {
        g_weapon.own_shows += 1;
        guest<ShowWeaponFn>(fShowWeaponHud)(reinterpret_cast<void *>(ctrl), -100.0f);
    }
}

// ---------------------------------------------------------------------------------------------
// The multiplayer Draw
// ---------------------------------------------------------------------------------------------

uint64_t g_draws = 0;

U2MP_GUEST void multiplayer_draw(void *element_ptr, void *draw_ctx) {
    if (global<int32_t>(kNetState) < 2) return;
    uint64_t flags_ptr = global<uint64_t>(kHudFlagsPtr);
    if (!flags_ptr) return;
    uint32_t flags = load<uint32_t>(flags_ptr);
    g_ctx = draw_ctx;

    uint64_t settings = reinterpret_cast<uint64_t>(lookup_pointer(0x9353ed16));
    uint64_t set = settings ? load<uint64_t>(settings + 0x90) : 0;
    uint64_t local = global<uint64_t>(kNgm);
    uint64_t record = net_game_record();
    if (!set || !local || !record) return;

    uint64_t element = reinterpret_cast<uint64_t>(element_ptr);
    int32_t type = load<int32_t>(record + 0xc);
    bool coop = is_coop_type(type);
    int32_t team = load<int32_t>(local + 0x134);
    int32_t other = team == 0 ? 1 : 0;

    g_draws += 1;
    bool periodic = g_draws == 1 || g_draws % 600 == 0;
    if (periodic)
        log("hud: multiplayer draw #%llu: type %d, flags 0x%x, team %d, scores %u/%u, limit %d, time limit %.0f s, "
            "names %d, markers %d, messages %d; draw list %p, image tables hud %u / net %u",
            (unsigned long long)g_draws, type, flags, team, team_score(team), team_score(other),
            global<int32_t>(kNgm + 0x21b4), double(global<float>(kNgm + 0x21b8)),
            load<int32_t>(element + 0x860 + 0xc80),
            load<int32_t>(element + 0x1560), load<int32_t>(element + 0x40), draw_ctx, table_count(material_table(0)),
            table_count(material_table(1)));
    if (g_weapon.on && periodic)
        log("hud: weapon panel: %llu switches, ShowWeaponHud %llu by the game (from %llx %llx %llx %llx) / %llu here, "
            "panel draws %llu, flags 0x%x",
            (unsigned long long)g_weapon.switches, (unsigned long long)g_weapon.game_shows,
            (unsigned long long)g_weapon.callers[0], (unsigned long long)g_weapon.callers[1],
            (unsigned long long)g_weapon.callers[2], (unsigned long long)g_weapon.callers[3],
            (unsigned long long)g_weapon.own_shows, (unsigned long long)g_weapon.panel_draws, flags);

    weapon_tick();
    if (!coop && type != 11 && (flags & 0x40000)) score_panel(set, local, type, team, other);
    coop_panel(set, flags);
    if (load<uint8_t>(abs_of(kNgm) + 0x209c) != 0 && (flags & 0x100000) && !std::getenv("UNCHARTED_U2MP_NO_THEATER"))
        theater(element);
    boosters(element, local, flags, coop);
    if (!coop && type != 11 && (flags & 0x80000)) message_list(element, set);
    if (flags & 0x20000) {
        names(element);
        markers(element);
    }
    centre_message(element);
}

// ---------------------------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------------------------

struct ByteCheck {
    uint64_t offset;
    const char *bytes;   // hex, space separated
    const char *name;
};

bool bytes_match(uint64_t offset, const char *hex) {
    auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    unsigned char buf[32];
    size_t n = 0;
    for (const char *p = hex; *p;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        buf[n++] = uint8_t(nibble(p[0]) << 4 | nibble(p[1]));
        p += 2;
    }
    return image.matches(offset, buf, n);
}

bool env_starts_with_zero(const char *name) {
    const char *v = std::getenv(name);
    return v && v[0] == '0';
}

void install_weapon_trigger() {
    if (env_starts_with_zero("UNCHARTED_U2MP_WEAPON_HUD")) {
        log("hud: UNCHARTED_U2MP_WEAPON_HUD=0, weapon panel trigger off");
        return;
    }
    if (!bytes_match(0x954740, "55 48 89 e5 48 83 ec 10 c5 fa 11 45 fc be fa 20 bd 2b") ||
        !bytes_match(0x95477a, "83 08 08") || !bytes_match(0x563a20, "55 48 89 e5 41 57 41 56 41 55 41 54") ||
        !bytes_match(0x71dec0, "48 63 8f b8 04 00 00")) {
        log("hud: weapon panel bytes differ, weapon switch trigger off");
        return;
    }
    if (global<uint64_t>(kShowWeaponSlot) == abs_of(fShowWeaponHud))
        store<uint64_t>(abs_of(kShowWeaponSlot), reinterpret_cast<uint64_t>(&show_weapon_probe));
    if (global<uint64_t>(kPanelDrawSlot) == abs_of(fPanelDraw))
        store<uint64_t>(abs_of(kPanelDrawSlot), reinterpret_cast<uint64_t>(&panel_draw_probe));
    g_weapon.on = true;
    log("hud: weapon panel: shown for %.0f s after a weapon switch (ShowWeaponHud 0x954740 from the multiplayer "
        "draw), probes on",
        3.0);
}

}  // namespace

void install_hud(int &installed) {
    if (!image.base) return;
    if (env_starts_with_zero("UNCHARTED_U2MP_HUD")) {
        log("hud: UNCHARTED_U2MP_HUD=0, multiplayer HUD draw left as the PS4 stub");
        return;
    }

    static const ByteCheck kChecks[] = {
        {0x5555a0, "55 48 89 e5 41 56 53 48 81 ec 10 02 00 00 84 c0", "text 0x5555a0"},
        {0x5559f0, "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec", "image 0x5559f0"},
        {0x55aab2, "48 8d 05 97 4a ae 00", "element vtable"},
        {0x55ab94, "c7 43 08 00 20 00 00", "element bits"},
        {0x555c60, "55 48 89 e5 41 57 41 56 53 48 81 ec b8 02 00 00", "quad 0x555c60"},
        {0x5566b0, "55 48 89 e5 41 56 53 48 83 ec 30 48 89 fb c5 78", "projection 0x5566b0"},
        {0xbdce90, "55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 40 49", "menu quad 0xbdce90"},
        {0xcc9a70, "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 58 12 00 00", "DrawText 0xcc9a70"},
        {0x5557e7, "48 8d bd 30 ff ff ff 48 89 de e8 7a 42 77 00", "text -> DrawText"},
        {0x55d557, "48 89 83 a8 0c 00 00 89 93 b0 0c 00 00", "names handles"},
        {0x55b8d2, "49 63 8f e0 14 00 00", "names count"},
        {0x55b8e3, "48 8d 1c 89 48 c1 e3 05", "names stride"},
        {0x55b918, "49 89 8c 1f 60 08 00 00", "names position"},
        {0x55b953, "41 c7 84 1f 6c 08 00 00 03 00 00 00", "names side"},
        {0x55c0e1, "88 84 1e f8 08 00 00", "names talking"},
        {0x55c0e8, "49 8b 85 b0 02 00 00 48 89 84 1e f0 08 00 00", "names emblem"},
        {0x55c08f, "41 8b 85 28 01 00 00 89 84 1e fc 08 00 00", "names rank"},
        {0x8921bb, "c6 80 0c 04 00 00 01", "HUD flags +0x40c"},
        {0x555340, "48 8b bf 20 03 00 00 c5 f8 28 09 4c 8d 97 60 15", "marker add"},
        {0x55ab69, "48 8d bb 60 15 00 00", "markers init"},
        {0x55ab81, "48 8d bb 90 1a 00 00", "centre message init"},
        {0x649bc1, "41 89 87 10 1b 00 00", "centre message start"},
    };
    for (const ByteCheck &c : kChecks) {
        if (!bytes_match(c.offset, c.bytes)) {
            log("hud: %s bytes at 0x%llx differ, multiplayer HUD not installed", c.name, (unsigned long long)c.offset);
            return;
        }
    }
    if (!is_ret_stub(kDrawStub)) {
        log("hud: 0x%llx is not the ret stub, multiplayer HUD not installed", (unsigned long long)kDrawStub);
        return;
    }

    // Data the booster, co-op and territory code reads, checked as one group.
    static const ByteCheck kGroup[] = {
        {0xbdce90, "55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 40 49", nullptr},
        {0x64cbd0, "55 48 89 e5 41 56 53 48 89 fb bf 7d 06 9a 3b e8", nullptr},
        {0x662de6, "41 89 9c 86 a0 02 00 00", nullptr},
        {0x5f61a4, "47 39 74 ac 4c", nullptr},
        {0x5e7ddd, "83 b9 78 06 00 00 00", nullptr},
        {0x55c515, "8a 96 9c 20 00 00", nullptr},
        {0x560e38, "c5 fa 7e 80 40 03 00 00", nullptr},
        {0x66885f, "48 8d 05 de 6c 9e 00 c6 40 1c 00", nullptr},
        {0x669844, "48 8d 05 15 1f ad 00 44 8b 80 d0 1d 00 00", nullptr},
        {0x66985e, "48 05 88 1d 00 00", nullptr},
        {0x66988c, "83 bf 50 02 00 00 ff", nullptr},
        {0x67235e, "8b 93 34 02 00 00", nullptr},
        {0x6722a8, "8b 93 38 02 00 00", nullptr},
        {0x6724cc, "8b 83 58 02 00 00 85 c0 74 47 c5 fa 2a 83 68 02 00 00", nullptr},
        {0x67251d, "8b 83 5c 02 00 00", nullptr},
        {0x67237f, "44 8b b5 48 fe ff ff", nullptr},
        {0x64aeb0, "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18", nullptr},
        {0x6237ab, "48 89 8a c0 1c 00 00 89 82 c8 1c 00 00", nullptr},
        {0x669a77, "8b 83 40 04 00 00 41 3b 86 20 01 00 00", nullptr},
    };
    for (const ByteCheck &c : kGroup) {
        if (!bytes_match(c.offset, c.bytes)) {
            log("hud: booster / co-op / territory bytes differ, multiplayer HUD not installed");
            return;
        }
    }

    if (kDrawVtable + 0x10 > image.size || kScoreTable + 8 > image.size) return;
    if (global<uint64_t>(kDrawSlot) != abs_of(kDrawStub)) {
        log("hud: vtable 0x%llx slot 1 is not the stub 0x%llx, left alone", (unsigned long long)kDrawVtable,
            (unsigned long long)kDrawStub);
        return;
    }
    store<uint64_t>(abs_of(kDrawSlot), reinterpret_cast<uint64_t>(&multiplayer_draw));
    log("hud: multiplayer HUD draw (PS3 0x8aef68) -> C++ (vtable 0x%llx slot 1)", (unsigned long long)kDrawVtable);
    installed += 1;

    install_weapon_trigger();
}

}  // namespace u2mp
