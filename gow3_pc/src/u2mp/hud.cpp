// u2mp hud: the multiplayer HUD panel PS4 1.02 stripped, ported from PS3 1.10 (notes:
// u2mp/docs/PORT-HUD.md).
//
// PS3: the HUD manager (game/hud/hud.cpp, init 0x8a13e8 = PS4 0x552c70) creates a multiplayer element
// (ctor 0x8ac330 = PS4 0x55aa80, kept at hud+0x320, element bits 0x2000) with the vtable PS3 0xd87db8 =
// PS4 0x103f550: slot 0 Update 0x8ac1e8, slot 1 Draw 0x8aef68, slot 3 Reset 0x8a9b90.
//   - PS4 slot 0 0x55abe0 is the whole Update (sub-update +0x40, player names +0x860, the HUD flag bits
//     0xe0000 while the match runs, the fade +0x1920): intact.
//   - PS4 slot 1 0x55c5b0 is `ret`: the multiplayer Draw and every helper only it called are gone
//     (the text formats "%i:" / "%02i" and the DC ids it used are not in the PS4 image). The HUD
//     loop 0x554710 still calls slot 1 for this element every frame (bits 0x2000 in the flags), so
//     nothing draws the score, team scores, limit or time.
// This file puts a C++ Draw in slot 1 with the PS3 0x8aef68 panel logic:
//   - not multiplayer, no local tracker or no *net-games* record: nothing (PS3 0x8af000..0x8af114);
//   - competitive (game type not 4/8/9/12, PS3 0x4a0bcc) and not type 11, flag 0x40000
//     (PS3 rlwinm 13,13) set:
//       type 10 (elimination, PS3 0x8af648): frame, own / other team score, label e1167dfa, round
//         ngm+0x1d70 + 1, round time left from the match start ngm+0x1d60;
//       other types (PS3 0x8af220): frame, own / other team score, label d38a7d18, score limit
//         ngm+0x21b4, time left from the round start ngm+0x1d5c;
//       time left (PS3 0x49e0e8): limit = ngm+0x21b8 seconds * 300, shown as "%i:" minutes and "%02i"
//         seconds when > 0 (PS3 0x8af878 / 0x8af7a0);
//       type 7 (Turf War, PS3 0x8af960): the territory panel instead (draw_type7_panel);
//       other types but 1 / 2: the objective carrier icon (PS3 0x8af360, draw_carrier).
//   Team scores: u16 at [0x104ae70] + 0x3dee + team * 0x34 (PS3 0xf2efc4 + 15840 + 14 + team * 52, the
//   table CheckGameOver 0x63f3f7 reads), team = tracker+0x134 (PS3 +300), other team = (team == 0).
// The rest of the PS3 Draw, in its order (net_hud_draw): the co-op type 9 money / wave panel, the booster
// icons (film and loadout sites), the message list (element+0x40), the names over heads (+0x860) and
// world markers (+0x1560) under flag 0x20000, and the centre message (+0x1a90) every frame. The theater
// HUD (PS3 0x8a8ed8 / 0x8ac3f0, element+0x1928) is not ported (notes, section 5).
//
// PS4 pieces (PS3 -> PS4): HUD settings record LookupPointer(9353ed16) (0x151d4c -> 0xa15e70); its
// multiplayer widget set is the 15th pointer: PS3 rec+0x58 -> PS4 rec+0x90 (the PS4 dc1 hud-settings has
// the same 16-pointer list, the SP timer element 0x559770 reads the 14th at +0x88 = PS3 +0x54). Widget
// pointers in the set: PS3 +4k -> PS4 +8k for the ones used here (+4, +8, +24, +28, +32, +36, +44 ->
// +0x08, +0x10, +0x30, +0x38, +0x40, +0x48, +0x58). Text: PS3 0x8a0f04(widget, pos, scale, fmt, ...) ->
// PS4 0x5555a0(widget, pos, fmt, float scale, ...) (variadic; xmm0 is the fixed scale, as the SP timer
// calls it). Frame image: PS3 0x8a10fc(widget, pos, scale) -> PS4 0x5559f0(draw ctx, widget, pos, scale).
// Texts: TextDb::Find(0x1066660, id) (PS3 0x9294c4(0x109d3f0, id)). HUD flags: [[0x1130008 + 0x60]]
// (PS3 [[0x1026a88 + 56]]).
//
// UNCHARTED_U2MP_HUD=0 leaves the stub alone (test switch). The first draw and every 600th are logged.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "u2mp_internal.h"

namespace u2mp {
void install_hud(int &installed);

namespace {

constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kNetState = 0x14ce97c;
constexpr uint64_t kFrames = 0x1116180 + 0x204;    // 300 Hz frame counter (PS3 [0xe60a00+460])
constexpr uint64_t kHud = 0x1130008;               // HUD manager (PS3 0x1026a88)
constexpr uint64_t HUD_FLAGS_PTR = 0x60;           // -> u32 visibility flags (PS3 +56)
constexpr uint64_t kTeamScorePtr = 0x104ae70;      // pointer slot; + 0x3dee: u16 team scores, stride 0x34
constexpr uint64_t kTextDb = 0x1066660;
constexpr uint64_t fLookup = 0xa15e70;             // ScriptManager LookupPointer(id)
constexpr uint64_t fText = 0x5555a0;               // HUD text (widget, pos, fmt, scale, ...)
constexpr uint64_t fImage = 0x5559f0;              // HUD image (ctx, widget, pos, scale)
constexpr uint64_t fTextFind = 0xcddc00;           // TextDb::Find(db, id)
constexpr uint32_t kHudSettings = 0x9353ed16, kNetGames = 0x3b9a067d;
constexpr uint32_t kLabelScoreLimit = 0xd38a7d18, kLabelRound = 0xe1167dfa;
constexpr uint64_t vtNetHud = 0x103f550, hDrawStub = 0x55c5b0;
constexpr uint64_t NGM_GAME = 0x1ca0, NGM_ROUND_START = 0x1d5c, NGM_MATCH_START = 0x1d60, NGM_ROUND = 0x1d70,
                   NGM_SCORE_LIMIT = 0x21b4, NGM_TIME_LIMIT = 0x21b8;
constexpr uint64_t T_TEAM = 0x134;
constexpr uint64_t REC_MP_WIDGETS = 0x90;
constexpr uint64_t W_SCORE = 0x08, W_SCORE_OTHER = 0x10, W_VALUE = 0x30, W_LABEL = 0x38, W_MINUTES = 0x40,
                   W_SECONDS = 0x48, W_FRAME = 0x58;
constexpr uint32_t kFlagScorePanel = 0x40000;      // PS3 rlwinm 0,25,0,13,13
constexpr uint32_t kFlagNames = 0x20000;           // PS3 rlwinm 0,25,0,14,14 (names + markers)
constexpr float kScale = 1.0f;                     // PS3 0xdd09ec
constexpr float kFramesPerSecond = 300.0f;         // PS3 0xdb1ec8

struct Pos {
    float x = 0.0f, y = 0.0f;   // PS3: the two zero words at sp+N before every call
};

using LookupFn = const uint8_t *(U2MP_GUEST *)(uint32_t);
using TextFn = void(U2MP_GUEST *)(const void *, const Pos *, const char *, float, ...);
using ImageFn = void(U2MP_GUEST *)(void *, const void *, const Pos *, float);
using FindFn = const char *(U2MP_GUEST *)(void *, uint32_t);

uint64_t g_draws = 0;

template <typename T> T G(uint64_t off) {
    T v;
    std::memcpy(&v, image.base + off, sizeof v);
    return v;
}
template <typename T> T R(const uint8_t *p, uint64_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}
const uint8_t *ptr_at(const uint8_t *p, uint64_t off) {
    return reinterpret_cast<const uint8_t *>(uintptr_t(R<uint64_t>(p, off)));
}

const uint8_t *lookup(uint32_t id) { return reinterpret_cast<LookupFn>(image.base + fLookup)(id); }

const uint8_t *net_game_record() {
    const uint8_t *t = lookup(kNetGames);
    const int16_t game = G<int16_t>(kNgm + NGM_GAME);
    if (!t || game < 0 || game >= R<int32_t>(t, 0)) return nullptr;
    const uint8_t *records = ptr_at(t, 8);
    return records ? records + 0x90 * game : nullptr;
}

unsigned team_score(int team) {
    const uint64_t table = G<uint64_t>(kTeamScorePtr);
    if (!table || team < 0 || team > 7) return 0;
    uint16_t s;
    std::memcpy(&s, reinterpret_cast<const void *>(uintptr_t(table + 0x3dee + 0x34 * uint64_t(team))), 2);
    return s;
}

void text(const uint8_t *set, uint64_t field, const char *fmt, int value) {
    const uint8_t *w = ptr_at(set, field);
    if (!w) return;
    const Pos pos;
    reinterpret_cast<TextFn>(image.base + fText)(w, &pos, fmt, kScale, value);
}
void label(const uint8_t *set, uint64_t field, uint32_t id) {
    const uint8_t *w = ptr_at(set, field);
    if (!w) return;
    const char *s = reinterpret_cast<FindFn>(image.base + fTextFind)(image.base + kTextDb, id);
    if (!s) return;
    const Pos pos;
    reinterpret_cast<TextFn>(image.base + fText)(w, &pos, "%s", kScale, s);
}

// ---- image resolution and its log ------------------------------------------------------------------
// The draw context. The HUD manager's frame update 0x554340 is called with rsi = 0 (0x3fc529 xor esi,esi;
// 0x3fc52d call), and the element loop 0x554710 hands that r12 to every slot 1 Draw. So `ctx` is NULL on
// PS4 for every HUD element (the SP timer 0x559770 passes it on to 0x5559f0 as it is): it is the draw
// list, and NULL means the deferred 2D arena (0xbdce90 takes NULL, as menu_draw_quad does). The first
// port returned early on `!ctx` in every image path, so no HUD image was ever drawn (run 46: the score
// texts without their mini-score frame). Nothing here tests ctx any more.
//
// Every image this file draws is resolved once per (id, table, index) and material table (pointer and
// count), through 0x5558c0 (menuimages.cpp's port: PS4 id, then the PS3 name read the PS3 way, then
// net-u2mp). A PS3 sid found among the PS4 names is replaced by the PS4 id and index, so the draw finds it
// directly (the PS4 asset; menuimages.cpp's rule). An image that does not resolve is logged once per
// (id, table) with its table's state ("hud: image ... not resolved"), and skipped.
constexpr uint64_t kImageTables = 0x1130008;   // +0x68 hud, +0x70 net, +0x78 menu material tables
constexpr uint32_t kTagNet = 0x4240ef2e;       // sid("net"): an image widget's +0x10 tag -> table 1, else 0

struct MatTable {   // {u16 ?, u16 count at +2, entries of 0x28 at +8 (+8 in an entry: its id)}
    uint16_t pad, count;
    uint32_t pad2;
    const uint8_t *entries;
};
const MatTable *material_table(uint32_t table) {
    if (table > 2) return nullptr;
    return reinterpret_cast<const MatTable *>(uintptr_t(G<uint64_t>(kImageTables + 0x68 + 8 * uint64_t(table))));
}
const char *table_name(uint32_t table) { return table == 0 ? "hud" : table == 1 ? "net" : table == 2 ? "menu" : "?"; }

struct Logged {
    uint32_t id, table;
};
Logged g_logged[96];
int g_logged_n = 0;
// true the first time (id, table) is seen (bounded: later ones are not logged)
bool first_time(uint32_t id, uint32_t table) {
    for (int i = 0; i < g_logged_n; ++i)
        if (g_logged[i].id == id && g_logged[i].table == table) return false;
    if (g_logged_n >= int(sizeof g_logged / sizeof g_logged[0])) return false;
    g_logged[g_logged_n++] = Logged{id, table};
    return true;
}


// PS3 0x49e0e8 + 0x8af878: time left as "%i:" minutes, "%02i" seconds, when the game has a time limit.
void time_left(const uint8_t *set, uint64_t start_field) {
    const int32_t limit = int32_t(G<float>(kNgm + NGM_TIME_LIMIT) * kFramesPerSecond);   // fctiwz
    if (limit <= 0) return;
    const int32_t elapsed = G<int32_t>(kFrames) - G<int32_t>(kNgm + start_field);
    const int32_t secs = int32_t(float(limit - elapsed) * (1.0f / kFramesPerSecond));
    const int32_t mins = secs / 60;
    text(set, W_MINUTES, "%i:", mins - (mins / 60) * 60);
    text(set, W_SECONDS, "%02i", secs - mins * 60);
}

// ---- message list (kill feed, medal / money messages), PS3 0x8abc68 + 0x8ab600 --------------------
// Data (filled by the intact PS4 code): list at element+0x40, {s32 count; entry[16] of 0x80 at +8}, added by
// 0x55d140 (PS3 AddMessage; callers are the net event Applies) and expired by the Update 0x55abe0:
//   +0x00 text 1 (48 chars), +0x30 colour 1, +0x34 text 2 (48), +0x64 colour 2, +0x68 icon pointer,
//   +0x70 start time, +0x74 end time (start + 7 s), +0x78 flags, +0x7c count; times in seconds of
//   [0x1116180+0x44] / 300. Colours are 0x00RRGGBB (PS3 0x8ab700: clrlwi 8, or alpha, rotlwi 8).
// PS3 layout (0x8abcec..0x8abef0): appear = clamp((now - start) * 2.2222), vanish = clamp((end - now) * 2),
// x = 120 - 200 * (1 - appear)^2, alpha = appear * vanish, lines of 25 from y = 650 - sum(vanish * 25),
// newest on top. Each line is PS3 0x8ab600, ported in draw_message_line below (DrawText 0x73cf20 = PS4
// 0xcc9a70, quads 0x89ff3c = 0x555c60): measured first (r7 = 1), scaled down to fit 350 units, its
// background image (set PS3 +100 = PS4 +0xb8, general/net/message-back) at (x - 8, y - 2), then drawn.
constexpr uint64_t E_MSG_LIST = 0x40, MSG_ENTRIES = 0x8, MSG_SIZE = 0x80;
constexpr uint64_t M_TEXT1 = 0x00, M_COLOR1 = 0x30, M_TEXT2 = 0x34, M_COLOR2 = 0x64, M_START = 0x70, M_END = 0x74,
                   M_COUNT = 0x7c;
constexpr int kMaxMessages = 16;
constexpr uint64_t kClock = 0x1116180 + 0x44;
constexpr uint32_t kFlagMessages = 0x80000;   // PS3 rlwinm 0,25,0,12,12
constexpr float kMsgBaseY = 650.0f, kMsgLine = 25.0f, kMsgX = 120.0f, kMsgSlide = -200.0f, kMsgAppear = 2.2222223f,
                kMsgVanish = 2.0f;                 // PS3 0xdd0c18 / c38 / c30 / c28 / c20, fadds f13,f13
constexpr float kMsgMaxWidth = 350.0f, kMsgBackX = 8.0f, kMsgBackY = 2.0f;   // PS3 0xdd0c40 / c48 / c50
constexpr uint64_t S_MSG_BACK = 0xb8;                                        // PS3 set +100

struct alignas(16) TextWidget {   // dc HUD text widget (identical on PS3 and PS4, 0x2c bytes)
    float x, y, w, h;
    uint32_t align;               // 0 left, 1 centre, 2 right
    float scale, r, g, b, a;
    uint32_t font;
    uint8_t pad[0x14];
};

float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }


// ---- low-level primitives shared by the parts below -----------------------------------------------
// The PS3 Draw and its helpers drew with low-level calls. Their PS4 counterparts:
//   textured quad PS3 0x89ff3c(handle, index, pos, size, colour v2, centred r9, dir r10, uv0, uv1) = PS4
//     0x555c60(ctx, handle, index, &rgba, centred, global alpha, pos, size, dir, uv0, uv1) (the image
//     widget draw 0x5559f0 = PS3 0x8a10fc ends in it). centred: pos is the centre and dir turns the quad
//     (PS3 passes (0, -1) for an upright one, 0x8aa42c); otherwise pos is the top-left corner. The
//     global-alpha flag multiplies by the HUD alpha [0x104ed0c] (PS3 [0xe1a660]). It resolves the material
//     with 0x5558c0 (PS3 0x8a1f88) and draws through the menu quad path 0xbdce90.
//   handle (12 bytes, same on PS3 and PS4): {texture id or sid, table, index}; table 0 = hud+0x68, 1 =
//     hud+0x70, 2 = hud+0x78 (material tables of 0x28-byte entries, sid at +0); index -1 = look it up.
//     PS3 0x8a015c(out, sid, table) builds one from a sid; PS3 0x8a0260(out, name, table) = PS4 0x555140
//     loads one by name (the PS4 inits already did that for every handle used here).
//   hudquad batch PS3 0x8b0220 (begin, material) / 0x8b0de8 (4 vertices) / 0x8b12b8 (end), with the rect
//     helpers 0x8b1600 (corners p0..p1, uv0..uv1) + 0x8b1540 / 0x8b1598 (draw with a material / handle):
//     no PS4 function. An axis-aligned rect is quad() with pos p0 and size p1 - p0 (the rect helpers keep
//     the colour as given; quad() multiplies the HUD alpha, which PS3 multiplied in before 0x8b1600, so
//     callers pass the colour without it). A free 4-corner quad is quad4(): the material from 0x5558c0
//     drawn by 0xbdce90 in 0x555c60's vertex layout.
//   projection PS3 0x8a2388 = PS4 0x5566b0(out of 0x30 bytes, &world, edge margin).
//   DrawText PS3 0x73cf20(state, text) = PS4 0xcc9a70(state, text) (the PS4 one is only called by the
//     HUD text widget 0x5555a0, which builds its state the way draw_text below does).
//   sin / cos PS3 0xabc6ec(x, 0 / 1) -> std::sin / std::cos; fmod 0xabb130 -> std::fmod.
//   clocks: PS3 [0xe60a00] has clocks of 112 bytes from +12, PS4 [0x1116180] clocks of 0x80 from +0x04
//     (PS3 +124 = PS4 +0x84, +236 = +0x104, +248 = +0x110, +460 = +0x204); all in 1/300 s.
//   HUD render size PS3 [0x10288a4] +4 / +8: the HUD's 1280 x 720.
struct V2 {
    float x, y;
};
struct alignas(16) V4 {
    float x, y, z, w;
};
struct Handle {
    uint32_t id = 0, table = 0;
    int32_t index = -1;
};
constexpr uint64_t fQuad = 0x555c60, fProject = 0x5566b0, fMaterial = 0x5558c0,
                   fMenuQuad = 0xbdce90, fDrawText = 0xcc9a70;
constexpr uint64_t kHudAlpha = 0x104ed0c;                                     // PS3 [0xe1a660]
constexpr uint64_t kClock1Frame = 0x1116180 + 0x84, kClock1Delta = 0x1116180 + 0x90,
                   kClock2Frame = 0x1116180 + 0x104;                          // PS3 [0xe60a00] +124 / +136 / +236
constexpr uint64_t kNetHudMasks = 0x104f4b0;   // PS3 0xf2264c: +0x10 name mask, +0x14 marker type mask
constexpr float kVirtualW = 1280.0f, kVirtualH = 720.0f;
constexpr float kPi = 3.14159265f, kHalfPi = 1.57079633f, kTwoPi = 6.28318531f;
using QuadFn = void(U2MP_GUEST *)(void *, uint64_t, int32_t, const V4 *, bool, bool, V2, V2, V2, V2, V2);
using MaterialFn = void *(U2MP_GUEST *)(uint64_t, int32_t);
using MenuQuadFn = void(U2MP_GUEST *)(void *, void *, void *);
using DrawTextFn = void(U2MP_GUEST *)(void *, const char *);

template <typename T> void Wr(const uint8_t *p, uint64_t off, T v) {   // the HUD element data is writable
    std::memcpy(const_cast<uint8_t *>(p) + off, &v, sizeof v);
}
float clock_secs(uint64_t clock) { return float(G<int32_t>(clock)) * (1.0f / kFramesPerSecond); }
V4 argb(uint32_t c) {   // PS3 0xAARRGGBB colours (0x8aa2e0, 0x8ad8b8)
    return V4{float((c >> 16) & 0xff) / 255.0f, float((c >> 8) & 0xff) / 255.0f, float(c & 0xff) / 255.0f,
              float(c >> 24) / 255.0f};
}
uint32_t to_argb(const V4 &c) {   // PS3 fctiwz(clamp(c) * 255) packing before 0x8aa210
    auto b = [](float v) { return uint32_t(clamp01(v) * 255.0f); };
    return b(c.w) << 24 | b(c.x) << 16 | b(c.y) << 8 | b(c.z);
}
V4 vec4_at(const uint8_t *p) {
    V4 v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

Handle sid_handle(uint32_t sid, uint32_t table = 1) { return Handle{sid, table, -1}; }
Handle handle_at(const uint8_t *p) { return Handle{R<uint32_t>(p, 0), R<uint32_t>(p, 4), R<int32_t>(p, 8)}; }

// Resolution cache (see "image resolution" above): key (id, table, index) + the table's pointer / count.
struct Resolved {
    Handle key, out;
    const MatTable *table;
    uint16_t count;
    bool ok;
};
Resolved g_resolved[64];
int g_resolved_n = 0, g_resolved_next = 0;

// The handle to draw `h` with (PS4 id / index when the image is a PS4 net.pak / hud.pak one), or false
// when the image is not there. `what` names the HUD part in the log.
bool resolve(const Handle &h, const char *what, Handle &out) {
    if (!h.id) return false;
    const MatTable *t = material_table(h.table);
    const uint16_t count = t ? t->count : 0;
    for (int i = 0; i < g_resolved_n; ++i) {
        Resolved &r = g_resolved[i];
        if (r.key.id == h.id && r.key.table == h.table && r.key.index == h.index && r.table == t && r.count == count) {
            out = r.out;
            return r.ok;
        }
    }
    const uint8_t *m = t ? static_cast<const uint8_t *>(reinterpret_cast<MaterialFn>(image.base + fMaterial)(
                               uint64_t(h.id) | uint64_t(h.table) << 32, h.index))
                         : nullptr;
    Resolved r{h, h, t, count, m != nullptr};
    if (m && t && t->entries && m >= t->entries && m < t->entries + 0x28 * size_t(count)) {
        r.out.index = int32_t((m - t->entries) / 0x28);
        r.out.id = R<uint32_t>(m, 8);   // the PS4 id (differs from a PS3 sid found by its PS3 name)
    }
    if (!m && first_time(h.id, h.table))
        log("hud: image %s id %08x table %u (%s) index %d not resolved: %s table %s, %u materials%s", what, h.id, h.table,
            table_name(h.table), h.index, table_name(h.table), t ? "set" : "NULL", unsigned(count),
            h.table == 1 ? " (PS4 net.pak by id and by PS3 name, then net-u2mp)" : "");
    else if (m && h.id != r.out.id && first_time(h.id, h.table))
        log("hud: image %s id %08x table %u -> PS4 id %08x index %d", what, h.id, h.table, r.out.id, r.out.index);
    if (g_resolved_n < int(sizeof g_resolved / sizeof g_resolved[0]))
        g_resolved[g_resolved_n++] = r;
    else
        g_resolved[g_resolved_next++ % g_resolved_n] = r;
    out = r.out;
    return r.ok;
}

void quad(void *ctx, const Handle &h0, const V4 &rgba, V2 pos, V2 size, bool centred = false, V2 dir = {0.0f, -1.0f},
          V2 uv0 = {0.0f, 0.0f}, V2 uv1 = {1.0f, 1.0f}, const char *what = "quad") {
    Handle h;
    if (rgba.w <= 0.0f || !resolve(h0, what, h)) return;
    reinterpret_cast<QuadFn>(image.base + fQuad)(ctx, uint64_t(h.id) | uint64_t(h.table) << 32, h.index, &rgba, centred,
                                                 true, pos, size, dir, uv0, uv1);
}
// PS3 0x8b1600 + 0x8b1598: an axis-aligned rect p0..p1.
void rect(void *ctx, const Handle &h, const V4 &rgba, V2 p0, V2 p1, V2 uv0 = {0.0f, 0.0f}, V2 uv1 = {1.0f, 1.0f},
          const char *what = "rect") {
    quad(ctx, h, rgba, p0, V2{p1.x - p0.x, p1.y - p0.y}, false, V2{0.0f, -1.0f}, uv0, uv1, what);
}

// An image widget of the dc set (PS3 0x8a10fc = PS4 0x5559f0(ctx, widget, pos, alpha)): the PS4 function
// resolves it itself (tag +0x10 "net" -> table 1, else 0; id +0x14, loaded by name +0 through 0x555140
// when 0, as "set:name"; index +0x18) and caches id / index in the widget. Afterwards the cached handle is
// checked here so a missing image is logged once with the widget's material name.
void widget_image(void *ctx, const uint8_t *w, const Pos &pos, float alpha, const char *what) {
    if (!w) return;
    reinterpret_cast<ImageFn>(image.base + fImage)(ctx, w, &pos, alpha);
    const uint32_t table = R<uint32_t>(w, 0x10) == kTagNet ? 1 : 0;
    const Handle h{R<uint32_t>(w, 0x14), table, R<int32_t>(w, 0x18)};
    if (h.index >= 0) return;   // found in its table by the PS4 code
    const uint8_t *m = h.id ? static_cast<const uint8_t *>(reinterpret_cast<MaterialFn>(image.base + fMaterial)(
                                  uint64_t(h.id) | uint64_t(table) << 32, -1))
                            : nullptr;
    if (m || !first_time(h.id ? h.id : uint32_t(uintptr_t(w)), table | 0x100)) return;
    const MatTable *t = material_table(table);
    const char *name = reinterpret_cast<const char *>(uintptr_t(R<uint64_t>(w, 0)));
    log("hud: image %s (widget %s) id %08x table %u (%s) not resolved: %s table %s, %u materials", what,
        name ? name : "(no name)", h.id, table, table_name(table), table_name(table), t ? "set" : "NULL",
        t ? unsigned(t->count) : 0u);
}
void frame_image(void *ctx, const uint8_t *set, uint64_t field, const char *what = "frame") {
    widget_image(ctx, ptr_at(set, field), Pos{}, kScale, what);
}

// 4-vertex quad with free corners (PS3 hudquad 0x8b0de8): material of the handle drawn by 0xbdce90 with
// 0x555c60's vertex layout (pos, uv, colour at +0x20, ones at +0x30). The colour is used as given (the
// hudquad callers multiply the HUD alpha in themselves).
struct alignas(16) QVert {
    float x, y, z, u, v, pad[3];
    V4 rgba, one;
};
struct alignas(16) QBuf {
    QVert v[4];
    uint32_t tail, pad[3];
};
static_assert(sizeof(QVert) == 0x40 && sizeof(QBuf) == 0x110, "hud quad layout");
void quad4(void *ctx, const Handle &h0, const V4 &rgba, const float (&xy)[4][2], const float (&uv)[4][2],
           const char *what = "quad4") {
    Handle h;
    if (rgba.w <= 0.0f || !resolve(h0, what, h)) return;
    void *m = reinterpret_cast<MaterialFn>(image.base + fMaterial)(uint64_t(h.id) | uint64_t(h.table) << 32, h.index);
    if (!m) return;
    alignas(16) uint64_t c[6] = {uint64_t(uintptr_t(ctx)), 0, 0, 0, 0, 0};
    alignas(16) uint64_t info[4] = {uint64_t(uintptr_t(m)), 0, 0, 0};
    QBuf q{};
    for (int i = 0; i < 4; ++i) {
        q.v[i].x = xy[i][0], q.v[i].y = xy[i][1];
        q.v[i].u = uv[i][0], q.v[i].v = uv[i][1];
        q.v[i].rgba = rgba;
        q.v[i].one = V4{1.0f, 1.0f, 1.0f, 1.0f};
    }
    reinterpret_cast<MenuQuadFn>(image.base + fMenuQuad)(c, info, &q);
}

// PS3 0x8a2388 = PS4 0x5566b0 output (0x30 bytes): screen position, direction to the target, its angle,
// distance, distance scale, and 1 when the point was clamped to the screen edge / behind the camera.
struct alignas(16) Proj {
    V4 pos, dir;
    float angle, dist, scale;
    int32_t clamped;
};
static_assert(sizeof(Proj) == 0x30, "projection result");
Proj project_full(const uint8_t *world, float margin) {
    alignas(16) V4 w;
    std::memcpy(&w, world, sizeof w);
    Proj p{};
    reinterpret_cast<void(U2MP_GUEST *)(Proj *, const V4 *, float)>(image.base + fProject)(&p, &w, margin);
    return p;
}

// DrawText state (PS4 0xcc9a70, as 0x5555a0 builds it; PS3 0x73cf20 state in brackets):
//   +0x00 x, +0x04 y [+0 / +4]; +0x14 / +0x1c -1.0 [+28 / +36]; +0x20 / +0x24 scale x / y [+8 / +12];
//   +0x28 colour bytes r, g, b, a [+16]; +0x2c 1.0 [+20]; +0x30 font -1 = default; +0x34 alignment (0
//   left, 1 centre, 2 right) | flags (4: measure only, writes the size back; 8 as the PS3 names use it)
//   [+116]; +0x50..+0x8c identity matrix, +0x90 1.0; +0x94 box width [+120]; +0x98 box height [+124];
//   measuring writes the width to +0x94 / +0x9c [+128] and the height to +0x98 [+124]; +0xa0 font override 0.
// The PS4 HUD alpha is not applied by DrawText (0x5555a0 multiplies it in), so `rgba.w` here is final.
struct TextSize {
    float w = 0.0f, h = 0.0f;
};
TextSize draw_text(float x, float y, float scale, const V4 &rgba, uint32_t flags, float box_w, float box_h,
                   const char *s) {
    TextSize out;
    if (!s || !s[0]) return out;
    alignas(16) uint8_t st[0xc0] = {};
    auto f = [&](uint64_t off, float v) { std::memcpy(st + off, &v, 4); };
    auto u = [&](uint64_t off, uint32_t v) { std::memcpy(st + off, &v, 4); };
    f(0x00, x), f(0x04, y), f(0x14, -1.0f), f(0x1c, -1.0f), f(0x20, scale), f(0x24, scale);
    auto byte = [](float v) { return uint8_t(clamp01(v) * 255.0f); };
    st[0x28] = byte(rgba.x), st[0x29] = byte(rgba.y), st[0x2a] = byte(rgba.z), st[0x2b] = byte(rgba.w);
    f(0x2c, 1.0f), u(0x30, 0xffffffffu), u(0x34, flags);
    f(0x50, 1.0f), f(0x64, 1.0f), f(0x78, 1.0f), f(0x8c, 1.0f), f(0x90, 1.0f);
    f(0x94, box_w), f(0x98, box_h);
    reinterpret_cast<DrawTextFn>(image.base + fDrawText)(st, s);
    if (flags & 4) std::memcpy(&out.w, st + 0x9c, 4), std::memcpy(&out.h, st + 0x98, 4);
    return out;
}

// PS3 0x8aa210(handle, pos, colour 0xAARRGGBB, r8 centred, f1 scale, f2 angle): an arrow icon of 40 * scale.
// r8 set: turned to `angle` (dir (cos(a - pi/2), -sin(a - pi/2))) and pushed back by a quarter of its
// size along -(cos a, -sin a); r8 clear: upright (dir (0, -1)), pushed back by half its size.
void edge_arrow(void *ctx, const Handle &h, V2 pos, uint32_t color, float scale, float angle, bool turned = true) {
    const float size = scale * 40.0f, back = size * (turned ? -0.25f : -0.5f);
    const V2 dir = turned ? V2{std::cos(angle - kHalfPi), -std::sin(angle - kHalfPi)} : V2{0.0f, -1.0f};
    const V2 at{pos.x + back * -std::cos(angle), pos.y + back * std::sin(angle)};
    quad(ctx, h, argb(color), at, V2{size, size}, true, dir, V2{0.0f, 0.0f}, V2{1.0f, 1.0f}, "arrow");
}

// PS3 0x8a9e48: a ring meter made of n textured segments around `centre`, from angle pi/2 (straight
// down on screen) sweeping fill * 2pi clockwise.
//   centre  (r3)  screen centre;
//   h       (r6 = {id, table}, r7 high word = index)  material of the ring texture;
//   rgba    (r8)  colour; its alpha is multiplied by the HUD alpha;
//   scale   (f1)  multiplies both radii; also n = int(max(1, scale) * lerp(10, 30, clamp(fill)));
//   fill    (f2)  0..1 (the sweep uses it unclamped, the segment count clamped);
//   r_inner (f3), r_outer (f4)  radii before scaling; the texture's uv radius is 0.45 at r_outer and
//           0.45 * r_inner / r_outer at r_inner, around uv (0.5, 0.5).
// Vertex order inner a0, outer a0, outer a1, inner a1; each segment is one quad4().
void meter_8a9e48(void *ctx, const V2 &centre, const Handle &h, V4 rgba, float scale, float fill, float r_inner,
                  float r_outer) {
    Handle hr;
    if (!resolve(h, "ring meter", hr)) return;
    const float t = clamp01(fill);
    const int32_t n = int32_t((scale > 1.0f ? scale : 1.0f) * ((1.0f - t) * 10.0f + t * 30.0f));   // fctiwz
    if (n <= 0) return;
    rgba.w *= G<float>(kHudAlpha);
    const float ri = scale * r_inner, ro = scale * r_outer;
    const float uv_out = 0.45f, uv_in = r_inner * 0.45f / r_outer;
    const float step = fill * kTwoPi / float(n);
    for (int32_t i = 0; i < n; ++i) {
        const float a0 = float(i) * step + kHalfPi, a1 = float(i + 1) * step + kHalfPi;
        const float c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
        const float xy[4][2] = {{centre.x + ri * c0, centre.y + ri * s0},
                                {centre.x + ro * c0, centre.y + ro * s0},
                                {centre.x + ro * c1, centre.y + ro * s1},
                                {centre.x + ri * c1, centre.y + ri * s1}};
        const float uv[4][2] = {{0.5f + uv_in * c0, 0.5f + uv_in * s0},
                                {0.5f + uv_out * c0, 0.5f + uv_out * s0},
                                {0.5f + uv_out * c1, 0.5f + uv_out * s1},
                                {0.5f + uv_in * c1, 0.5f + uv_in * s1}};
        quad4(ctx, hr, rgba, xy, uv, "ring meter");
    }
}

// ---- world markers (PS3 0x8ad018) and the centre message (PS3 0x8ab4d0) ---------------------------
// Both sub-objects keep their PS3 layouts; only their place in the element moved.
// Markers sub-object: PS3 element+5408 (0x8ac388 init 0x8a85f8) = PS4 element+0x1560 (init 0x55c5c0 at
// 0x55ab69). Filled by PS3 0x8a9980 (via hud wrapper 0x8a0408) = PS4 0x555340(hud, id, type, &pos,
// colour, colour 2, fill): callers DcNetDrawObjectiveIcon 0x669250, 0x45f7e0, 0x6725ac.., every frame.
//   +0x000 s32 count (8 at most); entries of 0x40 at +0x10:
//          +0x00 id, +0x04 type (0..10), +0x10 vec4 world position, +0x20 colour, +0x24 ring colour
//          (both 0xAARRGGBB), +0x2c fade, +0x30 fade target (1 when set this frame), +0x34 ring fill,
//          +0x38 frame when added, +0x3c frame when last set;
//   +0x210 11 type records of 0x24: +0x00 icon handle, +0x0c second (pulsing) icon handle, +0x18 size,
//          +0x1c dark flag (icon drawn black over the pulsing one), +0x20 number label (-1 none);
//   +0x39c edge arrow handle, +0x3a8 ring meter handle.
constexpr uint64_t E_MARKERS = 0x1560, MK_ENTRIES = 0x10, MK_SIZE = 0x40, MK_TYPES = 0x210, MK_TYPE_SIZE = 0x24,
                   MK_ARROW = 0x39c, MK_RING = 0x3a8;
constexpr int kMaxMarkers = 8, kMarkerTypes = 11;

void draw_markers(void *ctx, const uint8_t *element, const uint8_t *set) {
    const uint8_t *mk = element + E_MARKERS;
    if (R<int32_t>(mk, 0) <= 0) return;
    (void)set;
    // PS3 0x8ad0c8..0x8ad198
    const int32_t frame = G<int32_t>(kClock1Frame);
    const float t = float(frame) * (1.0f / kFramesPerSecond);
    const float dt = float(G<int32_t>(kClock1Delta)) * (1.0f / kFramesPerSecond);
    const float fade_in = dt * 4.0f, fade_out = dt * -4.0f;
    const uint32_t type_mask = G<uint32_t>(kNetHudMasks + 0x14);
    for (int i = 0; i < R<int32_t>(mk, 0) && i < kMaxMarkers; ++i) {
        const uint8_t *e = mk + MK_ENTRIES + MK_SIZE * uint64_t(i);
        const uint32_t type = R<uint32_t>(e, 0x04);
        if (type >= 32 || !(type_mask & (1u << type))) continue;           // 0x8ad1c4
        if (frame - R<int32_t>(e, 0x3c) > 300) {                           // 0x8ad1dc: not set for 1 s
            Wr<float>(e, 0x30, 0.0f), Wr<float>(e, 0x2c, 0.0f);
            continue;
        }
        float a = R<float>(e, 0x2c);                                       // 0x8ad1f0: fade to the target
        const float target = R<float>(e, 0x30);
        if (a < target) a = a + fade_in > target ? target : a + fade_in;
        if (a > target) a = a + fade_out < target ? target : a + fade_out;
        Wr<float>(e, 0x2c, a);
        if (a == 0.0f || type >= uint32_t(kMarkerTypes)) continue;
        const uint8_t *rec = mk + MK_TYPES + MK_TYPE_SIZE * type;
        const V4 c = argb(R<uint32_t>(e, 0x20));
        Wr<float>(e, 0x30, 0.0f);                                          // 0x8ad2c0: re-armed by the next set
        const float alpha = a * 0.8f;
        const Proj p = project_full(e + 0x10, 100.0f);                     // 0x8ad2f0
        const float s = p.scale * R<float>(rec, 0x18), size = s * 40.0f;
        const float pulse = clamp01(std::fabs(std::cos(t * 4.0f)));        // 0x8ad344
        V4 col{c.x, c.y, c.z, alpha};
        if (type >= 5 && type <= 10) {                                     // 0x8ad78c
            const float k = clamp01((a - 0.5f) * 2.0f);
            col.w = ((1.0f - k) * 0.1f + k) * 0.8f;
        }
        const Handle second = handle_at(rec + 0x0c);
        if (second.id) {                                                   // 0x8ad7c4: pulsing icon
            const float ps = size * ((1.0f - pulse) * 1.5f + pulse * 2.5f);
            quad(ctx, second, col, V2{p.pos.x - ps * 0.5f, p.pos.y - ps * 0.5f}, V2{ps, ps}, false, V2{0.0f, -1.0f}, V2{0.0f, 0.0f}, V2{1.0f, 1.0f}, "marker pulse");
        }
        const V4 icon_col = rec[0x1c] ? V4{0.0f, 0.0f, 0.0f, alpha} : V4{c.x, c.y, c.z, alpha};   // 0x8ad3d0
        quad(ctx, handle_at(rec), icon_col, V2{p.pos.x - size * 0.5f, p.pos.y - size * 0.5f}, V2{size, size}, false,
             V2{0.0f, -1.0f}, V2{0.0f, 0.0f}, V2{1.0f, 1.0f}, "marker icon");
        if (p.clamped) {                                                   // 0x8ad9b8: off-screen arrow
            const float q = clamp01(1.0f - std::fabs(std::cos(t * 8.0f)));
            const float push = s * 30.0f * ((1.0f - q) * 0.8f + q * 1.1f);
            const uint32_t ac = uint32_t(clamp01(alpha) * 255.0f) << 24 | uint32_t(clamp01(c.x) * 255.0f) << 16 |
                                uint32_t(clamp01(c.y) * 255.0f) << 8 | uint32_t(clamp01(c.z) * 255.0f);
            edge_arrow(ctx, handle_at(mk + MK_ARROW), V2{p.pos.x + p.dir.x * push, p.pos.y + p.dir.y * push}, ac,
                       p.scale * 1.75f, p.angle);
        }
        const float fill = R<float>(e, 0x34);
        if (fill != 0.0f)                                                  // 0x8ad8b8: ring meter
            meter_8a9e48(ctx, V2{p.pos.x, p.pos.y}, handle_at(mk + MK_RING), argb(R<uint32_t>(e, 0x24)),
                         p.scale * 1.75f, fill, 30.0f, 50.0f);
        const int32_t label = R<int32_t>(rec, 0x20);
        if (label >= 0) {                                                  // 0x8ad470: "%i" over the icon
            // DrawText state: scale 2 * s (+8 / +12), box 400 x 400 (+120 / +124), centred (+116 = 1)
            char s16[16];
            std::snprintf(s16, sizeof s16, "%i", int(label));
            draw_text(p.pos.x - 200.0f, p.pos.y - 100.0f * p.scale, 2.0f * p.scale,
                      V4{c.x, c.y, c.z, clamp01(a) * G<float>(kHudAlpha)}, 1, 400.0f, 400.0f, s16);
        }
    }
}

// Centre message: PS3 element+6692 (0x8ac3a0, 128-byte memset) = PS4 element+0x1a90 (0x55ab81). Set by
// PS3 0x8a9910 (via hud wrapper 0x8a0438, callers 0x49e7b0 / 0x49fc70 = PS4 0x6499f0 / 0x646bf0, which
// inline it at 0x649b91 / 0x6470b9): +0x00 text (128), +0x80 start = clock 2 frame, +0x84 colour
// 0xAARRGGBB, +0x88 / +0x8c glyph counters (zeroed).
// PS3 0x8ab4d0 draws it every frame (whatever the HUD flags): centred in an 800-wide box at x = (W - 800)
// / 2, y = H * 0.4, scale 1.2, through DrawText with the per-glyph callback 0x8a7c58 -> 0x8a7a38: glyph n
// gets alpha ramp(u, 0, 1) - ramp(u, 9, 10), u = (clock2 - start) / 75 - 0.1 n (ramp 0x89fd8c =
// (1 - cos(pi x)) / 2 on [a, b]); the colour's own alpha is replaced; when the last glyph passes u = 10
// the text is cleared (byte 0 = 0). The PS4 text widget has no per-glyph hook, so the string is drawn
// whole with the first glyph's fade-in and the last glyph's fade-out.
constexpr uint64_t E_CENTRE_MSG = 0x1a90;

float ramp(float x, float a, float b) {   // PS3 0x89fd8c
    if (x < a) return 0.0f;
    if (x >= b) return 1.0f;
    return (std::cos(clamp01((x - a) / (b - a)) * kPi) - 1.0f) * -0.5f;
}

void draw_list(void *ctx, const uint8_t *element, const uint8_t *set) {
    (void)ctx;
    const uint8_t *m = element + E_CENTRE_MSG;
    if (!m[0]) return;                                                     // 0x8ab4f4
    (void)set;
    char text[129];
    std::memcpy(text, m, 128), text[128] = 0;
    int glyphs = 0;
    for (const char *s = text; *s; ++s)
        if ((uint8_t(*s) & 0xc0) != 0x80) ++glyphs;                        // UTF-8 code points
    const float u = float(G<int32_t>(kClock2Frame) - R<int32_t>(m, 0x80)) * (1.0f / 75.0f);
    const float u_last = u - 0.1f * float(glyphs - 1);
    if (u_last > 10.0f) {                                                  // 0x8a7c2c: all glyphs gone
        Wr<uint8_t>(m, 0, 0);
        return;
    }
    Wr<int32_t>(m, 0x88, 0);                                               // 0x8ab594
    const float alpha = ramp(u, 0.0f, 1.0f) - ramp(u_last, 9.0f, 10.0f);
    const V4 c = argb(R<uint32_t>(m, 0x84));
    // DrawText state: box 800 x 20 (+120 / +124), centred (+116 = 1), scale 1.2
    draw_text((kVirtualW - 800.0f) * 0.5f, kVirtualH * 0.4f, 1.2f, V4{c.x, c.y, c.z, clamp01(alpha) * G<float>(kHudAlpha)},
              1, 800.0f, 20.0f, text);
}

// ---- radial meter, booster icons, co-op money / wave panel (PS3 0x8a9e48, 0x8a9bbc, 0x8af4e0) --------
// PS3 -> PS4 pieces used here:
//   hudquad batch 0x8a1f88 (handle -> material) + 0x8b0220 / 0x8b0de8 / 0x8b12b8 (begin / 4-vertex quad /
//     end; no PS4 function) -> material 0x5558c0(handle, index) and one menu quad 0xbdce90(ctx, {material},
//     4 vertices of 0x40) per segment, the way 0x555c60 draws (vertex +0x30 = 1, 1, 1, 1).
//   HUD alpha [0xe1a660] (multiplied into every meter colour) -> [0x104ed0c] (0x555c60's global alpha).
//   sin / cos 0xabc6ec(x, 0 / 1) -> std::sin / std::cos.
//   hud manager +300 (x offset added to the booster column) -> hud 0x1130008 + 0x340 (a vec2 added to
//     positions the same way in the PS4 HUD draw at 0x560e38).
//   booster manager 0xf1b2c0 {count; rec* at +4} -> 0x113a788 {u32 count; pad; rec* at +8} (netinit.cpp);
//     *boosters* records (916e03b0, 0x20 bytes: +8 image sid, +0xc id) are the same layout.
//     0x4552b8(mgr, id): record with +0xc == id (0 / unknown -> none; PS3 logs "Invalid booster id").
//     0x4551b0(mgr, index): *boosters* record `index` (< 0 / out of range -> none).
//     0x45543c(mgr, slot): the local loadout's booster id, PS3 0x4c4f74(0xf2da60) + 8 / + 12 (big endian)
//       -> u32 at 0x11474f8 + 0x4c / + 0x50 (0x5f6080 = PS3 0x4557f8 reads them at 0x5f61a4).
//     0x45536c(mgr): boosters allowed; inlined in 0x5f6080 / 0x5f6410 (PS3 0x4557f8 / 0x455900): needs
//       NET_SM >= 2; co-op game type (4/8/9/12, PS3 0x441dd8): ngm+0x1e80 == 0 (PS3 +6796); otherwise
//       ngm+0x1e80 == 0 or the net game record's +0x6c == 0 (PS3 +100).
//   tracker +608 / +612 (booster index per slot) -> tracker +0x2a0 / +0x2a4 (SetBooster 0x662dd0 = PS3
//     0x4c87d0 stores [tracker + 0x2a0 + slot * 4]; the ids +600 / +604 are +0x298 / +0x29c).
//   ngm+7076 (u8, a film is playing; PS3 getter 0x438498) -> ngm+0x209c (0x5b2a00 = PS3 0x413118).
//   cinema manager 0xf1b590 +1640 (playback mode) -> 0x113a990 + 0x678 (0x5e7ddd, 0x5e9344).
//   wave screen 0xf2e150 (overlay entries of 20 bytes; DcNetHide/ShowWaveScreen 0x4ccfe4 / 0x4cd08c ->
//     0x668850 / 0x668890 write entry 1's +8 byte at 0x104f544 + 0x1c) +32 float -> 0x104f544 + 0x20.
//   net game record +112 (wave table sid) -> +0x7c; ngm wave start / end / wave +6568 / +6572 / +6576 ->
//     +0x1d64 / +0x1d68 / +0x1d6c (0x64cbd0 = PS3 0x49cca8, money multiplier "x%i", called as is).
//   set +104 (vec2 640, 85) / +112 (text) / +116 (image) -> set +0xc0 / +0xc8 / +0xd0 (both
//     hud-settings.bin: PS3 +52..+64 rect -> +0x68, +68..+100 -> +0x78..+0xb8, +120 sid -> +0xdc).
//   element +6368 fade -> +0x1920; element +6344 ring handle (marker init 0x8a85f8 stores sid 68951e57,
//     table 1, at markers +936) -> the same sid handle built here (index resolved on draw).
constexpr uint64_t fCoopMoney = 0x64cbd0;
constexpr uint64_t kBoosterList = 0x113a788, kLoadout = 0x11474f8, kCinemaMgr = 0x113a990,
                   kWaveScreen = 0x104f544;
constexpr uint64_t HUD_OFFSET = 0x340, LOADOUT_BOOSTERS = 0x4c, CINEMA_MODE = 0x678, WAVE_ALPHA = 0x20;
constexpr uint64_t NGM_BOOSTERS_OFF = 0x1e80, NGM_FILM = 0x209c, NGM_WAVE_START = 0x1d64, NGM_WAVE_END = 0x1d68,
                   NGM_WAVE = 0x1d6c;
constexpr uint64_t GAME_BOOSTERS_OFF = 0x6c, GAME_WAVE_TABLE = 0x7c;
constexpr uint64_t T_BOOSTER_INDEX = 0x2a0;
constexpr uint64_t E_FADE = 0x1920;
constexpr uint64_t S_COOP_POS = 0xc0, S_COOP_TEXT = 0xc8, S_COOP_IMAGE = 0xd0;
constexpr uint32_t kBoosterTableId = 0x916e03b0, kBoosterNone1 = 0x05454b23, kBoosterNone2 = 0x01845694;
constexpr uint32_t kRingSid = 0x68951e57;
constexpr uint32_t kFlagBoosters = 0x10, kFlagFilmBoosters = 0x40000, kFlagCoopPanel = 0x8000;

using CoopMoneyFn = int32_t(U2MP_GUEST *)(const void *);

// PS3 0x45536c: boosters count in this game (inlined in PS4 0x5f6080 / 0x5f6410).
bool boosters_allowed() {
    if (G<int32_t>(kNetState) < 2) return false;
    const uint8_t *game = net_game_record();
    const int32_t type = game ? R<int32_t>(game, 0xc) : -1;
    const bool coop = type == 4 || type == 8 || type == 9 || type == 12;
    const int32_t off = G<int32_t>(kNgm + NGM_BOOSTERS_OFF);
    if (coop) return off == 0;
    return off == 0 || !game || R<int32_t>(game, GAME_BOOSTERS_OFF) == 0;
}
// PS3 0x4552b8: the listed booster record with id `id`.
const uint8_t *booster_by_id(uint32_t id) {
    if (!id) return nullptr;
    const uint32_t n = G<uint32_t>(kBoosterList);
    for (uint32_t i = 0; i < n && i < 64; ++i) {
        const uint8_t *r = reinterpret_cast<const uint8_t *>(uintptr_t(G<uint64_t>(kBoosterList + 8 + 8 * uint64_t(i))));
        if (r && R<uint32_t>(r, 0xc) == id) return r;
    }
    return nullptr;
}
// PS3 0x4551b0: *boosters* record `index`.
const uint8_t *booster_by_index(int32_t index) {
    if (index < 0) return nullptr;
    const uint8_t *t = lookup(kBoosterTableId);
    if (!t || index >= R<int32_t>(t, 0)) return nullptr;
    const uint8_t *recs = ptr_at(t, 8);
    return recs ? recs + 0x20 * uint64_t(index) : nullptr;
}

// PS3 0x8a9bbc(a, b, fade): the two booster icons, 76.8 x 38.4 at x = hud offset + 90, y = 150 (and
// 180.72 for the second when the first is drawn), white with alpha fade * 0.75; the two placeholder ids
// draw nothing.
void booster_pair(void *ctx, const uint8_t *a, const uint8_t *b, float fade) {
    const V4 white{1.0f, 1.0f, 1.0f, fade * 0.75f};
    const float x = G<float>(kHud + HUD_OFFSET) + 90.0f;
    const V2 size{76.8f, 38.4f};
    float y = 150.0f;
    if (a && R<uint32_t>(a, 0xc) != kBoosterNone1) {
        quad(ctx, sid_handle(R<uint32_t>(a, 8), 1), white, V2{x, 150.0f}, size, false, V2{0.0f, -1.0f}, V2{0.0f, 0.0f}, V2{1.0f, 1.0f}, "booster");
        y = 180.72f;
    }
    if (b && R<uint32_t>(b, 0xc) != kBoosterNone2) quad(ctx, sid_handle(R<uint32_t>(b, 8), 1), white, V2{x, y}, size, false, V2{0.0f, -1.0f}, V2{0.0f, 0.0f}, V2{1.0f, 1.0f},
                                                            "booster");
}

// Both booster call sites of PS3 0x8aef68, in its order:
//   0x8af158 / 0x8af180 / 0x8af5c4: a film is playing (ngm+0x209c), flag 0x40000 and cinema mode != 0:
//     the watched player's boosters by index (tracker +0x2a0 / +0x2a4);
//   0x8af1a0 / 0x8af440: competitive games, flag 0x10 SET (rlwinm 27,27; bf eq -> draw) and boosters
//     allowed: the local loadout's two boosters by id.
// `local` is the local tracker (ngm+0); the fade is element+0x1920.
void draw_boosters(void *ctx, const uint8_t *element, const uint8_t *local, bool coop, uint32_t flags) {
    if (!element) return;
    const float fade = R<float>(element, E_FADE);
    if (G<uint8_t>(kNgm + NGM_FILM) && (flags & kFlagFilmBoosters) && G<int32_t>(kCinemaMgr + CINEMA_MODE) != 0 && local)
        booster_pair(ctx, booster_by_index(R<int32_t>(local, T_BOOSTER_INDEX)),
                     booster_by_index(R<int32_t>(local, T_BOOSTER_INDEX + 4)), fade);
    if (!coop && (flags & kFlagBoosters) && boosters_allowed())
        booster_pair(ctx, booster_by_id(G<uint32_t>(kLoadout + LOADOUT_BOOSTERS)),
                     booster_by_id(G<uint32_t>(kLoadout + LOADOUT_BOOSTERS + 4)), fade);
}

// PS3 0x49cae8 (inlined in PS4 0x7d6170): what is left of the current wave segment, 1 -> 0. The wave
// table (sid at net game record +0x7c) is {s32 count; entry* at +8}, entries of 12 bytes {wave, time
// (s), value}; elapsed = (wave end > start ? end : now) - start, in seconds of 300 frames.
float coop_wave_left() {
    const uint8_t *game = net_game_record();
    const uint32_t sid = game ? R<uint32_t>(game, GAME_WAVE_TABLE) : 0;
    const uint8_t *t = sid ? lookup(sid) : nullptr;
    if (!t) return 0.0f;
    const int32_t start = G<int32_t>(kNgm + NGM_WAVE_START), end = G<int32_t>(kNgm + NGM_WAVE_END);
    const float secs = float((end > start ? end : G<int32_t>(kFrames)) - start) * (1.0f / 300.0f);
    const int32_t wave = G<int32_t>(kNgm + NGM_WAVE) + 1;
    const int32_t n = R<int32_t>(t, 0);
    const uint8_t *e = ptr_at(t, 8);
    if (!e) return 0.0f;
    for (int32_t k = 0; k < n; ++k) {
        const uint8_t *cur = e + 12 * uint64_t(k), *prev = k ? cur - 12 : cur;
        const int32_t w = R<int32_t>(cur, 0);
        if (w != wave) {
            if (wave < w) return 0.0f;
            continue;
        }
        const float from = float(R<int32_t>(prev, 4)), to = float(R<int32_t>(cur, 4));
        if (secs < from || !(to > secs)) continue;
        return (from - secs) / float(R<int32_t>(cur, 4) - R<int32_t>(prev, 4)) + 1.0f;
    }
    return 0.0f;
}

// PS3 0x8af4e0..0x8af5c0: co-op type 9 (the money / wave game), flag 0x8000, wave screen not showing
// (its +32 float is 0): the image widget set+0xd0 and the "x%i" money multiplier (text widget set+0xc8)
// at set+0xc0, and a ring meter of the wave time left in the text widget's colour.
void draw_coop_panel(void *ctx, const uint8_t *element, const uint8_t *set, uint32_t flags) {
    (void)element;   // PS3 read the ring handle at element+6344; it is the constant sid built below
    if (!set || !(flags & kFlagCoopPanel)) return;
    const uint8_t *game = net_game_record();
    if (!game || R<int32_t>(game, 0xc) != 9) return;
    if (G<float>(kWaveScreen + WAVE_ALPHA) != 0.0f) return;
    const Pos *pos = reinterpret_cast<const Pos *>(set + S_COOP_POS);
    widget_image(ctx, ptr_at(set, S_COOP_IMAGE), *pos, kScale, "co-op meter back");
    const uint8_t *txt = ptr_at(set, S_COOP_TEXT);
    const int32_t money = reinterpret_cast<CoopMoneyFn>(image.base + fCoopMoney)(image.base + kNgm);
    if (txt) reinterpret_cast<TextFn>(image.base + fText)(txt, pos, "x%i", kScale, money);
    const float left = coop_wave_left();
    const V4 colour = txt ? V4{R<float>(txt, 0x18), R<float>(txt, 0x1c), R<float>(txt, 0x20), R<float>(txt, 0x24)}
                          : V4{1.0f, 1.0f, 1.0f, 1.0f};
    meter_8a9e48(ctx, V2{pos->x, pos->y}, sid_handle(kRingSid, 1), colour, 0.475f, left, 30.0f, 50.0f);
}

// layout_matches() additions (prologues / the instructions the offsets above come from):
//   (0x5558c0 is not checked: menuimages.cpp replaces it before install_hud runs; calling it is fine)
//   0xbdce90 55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 40 49      menu quad (menuimages.cpp checks it too)
//   0x64cbd0 55 48 89 e5 41 56 53 48 89 fb bf 7d 06 9a 3b e8      co-op money multiplier
//   0x662de6 41 89 9c 86 a0 02 00 00   mov [r14+rax*4+0x2a0],ebx  (tracker booster index)
//   0x5f61a4 47 39 74 ac 4c            cmp [r12+r13*4+0x4c],r14d  (loadout booster ids)
//   0x5e7ddd 83 b9 78 06 00 00 00      cmp dword [rcx+0x678],0     (cinema mode)
//   0x55c515 8a 96 9c 20 00 00         mov dl,[rsi+0x209c]         (film playing)
//   0x560e38 c5 fa 7e 80 40 03 00 00   vmovq xmm0,[rax+0x340]      (hud position offset)
//   0x66885f 48 8d 05 de 6c 9e 00 c6 40 1c 00   lea rax,[0x104f544]; mov byte [rax+0x1c],0 (wave screen)
bool boosters_layout_matches() {
    static const unsigned char menuquad[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                             0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x40, 0x49};
    static const unsigned char money[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48,
                                          0x89, 0xfb, 0xbf, 0x7d, 0x06, 0x9a, 0x3b, 0xe8};
    static const unsigned char tracker[] = {0x41, 0x89, 0x9c, 0x86, 0xa0, 0x02, 0x00, 0x00};
    static const unsigned char loadout[] = {0x47, 0x39, 0x74, 0xac, 0x4c};
    static const unsigned char cinema[] = {0x83, 0xb9, 0x78, 0x06, 0x00, 0x00, 0x00};
    static const unsigned char film[] = {0x8a, 0x96, 0x9c, 0x20, 0x00, 0x00};
    static const unsigned char offset[] = {0xc5, 0xfa, 0x7e, 0x80, 0x40, 0x03, 0x00, 0x00};
    static const unsigned char wave[] = {0x48, 0x8d, 0x05, 0xde, 0x6c, 0x9e, 0x00, 0xc6, 0x40, 0x1c, 0x00};
    return image.matches(fMenuQuad, menuquad, sizeof menuquad) &&
           image.matches(fCoopMoney, money, sizeof money) && image.matches(0x662de6, tracker, sizeof tracker) &&
           image.matches(0x5f61a4, loadout, sizeof loadout) && image.matches(0x5e7ddd, cinema, sizeof cinema) &&
           image.matches(0x55c515, film, sizeof film) && image.matches(0x560e38, offset, sizeof offset) &&
           image.matches(0x66885f, wave, sizeof wave);
}

// ---- names over heads (PS3 0x8aeeb0 -> 0x8adb60 per entry) -------------------------------------------
// Data: the names sub-object, PS3 element+2080 = PS4 element+0x860, filled every frame by the intact PS4
// Update 0x55abe0 (0x55b4f0..0x55c4e7, the inlined PS3 0x8aa448). Its offsets are the PS3 ones:
//   +0x000 entries of 0xa0 (20 at most), +0xc80 s32 count (PS3 +3200), +0xc84 cleared each frame;
//   handles (init PS3 0x8a9340 = PS4 0x55d4f0..0x55d63f): +0xca8 player-arrow, +0xcb4 player-ex,
//   +0xcc0 triangle button, +0xccc knocked-down, +0xcd8 grapple, +0xce4 sid a2a60dcf, +0xcf0 sid 68951e57.
// Entry (PS3 = PS4 up to +0x90, then the 4-byte pointer grew to 8):
//   +0x00 screen x, y (0x5566b0 at 0x55b6ec); +0x08 scale s (distance scale); +0x0c side: 0 above, 1
//   left, 2 right (clamped off screen), 3 on screen; +0x10 arrow: 0 player-ex, 1 / 2 player-arrow;
//   +0x20 arrow / status colour (team colour 0x64aeb0, alpha = the name fade +0x22c); +0x30 status: 0
//   none, 1 down (revive), 2 grappled; +0x34 status fill (1 - revive progress); +0x38 name (64 chars);
//   +0x80 name colour (alpha = fade +0x228); +0x90 emblem pointer (PS3 +144; tracker+0x2b0);
//   +0x98 talking (PS3 +148); +0x9c rank (PS3 +152; tracker+0x128).
constexpr uint64_t E_NAMES = 0x860, NM_COUNT = 0xc80, NM_SIZE = 0xa0, NM_ARROW = 0xca8, NM_EX = 0xcb4,
                   NM_TRIANGLE = 0xcc0, NM_DOWN = 0xccc, NM_GRAPPLE = 0xcd8, NM_PULSE = 0xce4, NM_RING = 0xcf0;
constexpr uint64_t N_POS = 0x00, N_SCALE = 0x08, N_SIDE = 0x0c, N_ARROW = 0x10, N_COLOR = 0x20, N_STATUS = 0x30,
                   N_FILL = 0x34, N_NAME = 0x38, N_NAME_COLOR = 0x80, N_EMBLEM = 0x90, N_TALK = 0x98, N_RANK = 0x9c;
constexpr int kMaxNames = 20;
constexpr uint64_t HUD_FLAG_REVIVE_BUTTON = 0x40c;   // PS3 [[hud+56]+1612] byte (PS3 +1608 = PS4 +0x408)
constexpr uint32_t kSidRankIcons = 0xbda0f568, kSidRankIconsHigh = 0xf1b63553, kSidTalk = 0xffa98ec5;

void draw_name(void *ctx, const uint8_t *sub, const uint8_t *e) {
    const float t = clock_secs(kClock1Frame);                                        // PS3 0x8adb88: clock 1 / 300
    const float s = R<float>(e, N_SCALE);
    const float x = R<float>(e, N_POS), y = R<float>(e, N_POS + 4);
    const char *name = reinterpret_cast<const char *>(e + N_NAME);
    char text[65];
    std::memcpy(text, name, 64), text[64] = 0;
    V4 name_col = vec4_at(e + N_NAME_COLOR);                                         // 0x8adc40
    const float name_alpha = name_col.w;
    name_col.w *= G<float>(kHudAlpha);
    // 0x8addc8: measure (flags 4 | 8, box 99999 x 40): text width W (+128) and height H (+124)
    const TextSize ts = draw_text(0.0f, 0.0f, s, name_col, 12, 99999.0f, 40.0f, text);
    const float W = ts.w, H = ts.h;
    // 0x8addd0..0x8adeac: the rank / emblem icon left of the name, the talk icon right of it
    const bool emblem = R<uint64_t>(e, N_EMBLEM) != 0;
    const int32_t rank = R<int32_t>(e, N_RANK);
    const float icon = emblem || rank > 58 ? s * 50.0f : 0.0f;
    const float talk = e[N_TALK] ? s * 40.0f : 0.0f;
    const float half = (W + icon) * 0.5f;
    float left = x - half;                                                           // clamp to 128 .. W - 128
    if (left < 128.0f) left = 128.0f;
    const float right_limit = kVirtualW - (W + icon) - 128.0f;
    if (left > right_limit) left = right_limit;
    const float cx = left + half;
    const float base = y - s * 25.0f;                                                // f24
    const V2 status_at{cx, y - s * 150.0f - s * 25.0f + s * 30.0f};                  // f12: y - 145 s
    // 0x8adedc..0x8ae660: the arrow's place, size and angle by side
    V2 ap;
    float ascale, angle;
    switch (R<int32_t>(e, N_SIDE)) {
    case 0: ap = {cx, base - H}, ascale = 1.5f, angle = kHalfPi; break;
    case 1: ap = {cx - half - 10.0f, base - H * 0.5f}, ascale = 1.2f, angle = kPi; break;
    case 2: {
        const float pad = text[0] && name_alpha != 0.0f ? talk * 0.5f + 6.0f : 0.0f;
        ap = {cx + half + pad + 10.0f, base - H * 0.5f}, ascale = 1.2f, angle = 0.0f;
        break;
    }
    default: ap = {cx, base}, ascale = 1.5f, angle = -kHalfPi; break;
    }
    const int32_t status = R<int32_t>(e, N_STATUS);
    const V4 col = vec4_at(e + N_COLOR);
    // 0x8adf08: with a status, an on-screen entry (side 3) has no arrow (bt -> 0x8adfc0); off screen the
    // arrow bobs along its angle by clamp(1 - |cos 8t|) * 20.
    const bool arrow = status == 0 || R<int32_t>(e, N_SIDE) != 3;
    if (status != 0 && arrow) {
        const float k = clamp01(1.0f - std::fabs(std::cos(t * 8.0f))) * 20.0f;
        ap.x += std::cos(angle) * k, ap.y -= std::sin(angle) * k;
    }
    if (arrow) switch (R<int32_t>(e, N_ARROW)) {                                     // 0x8adf94
        case 0: edge_arrow(ctx, handle_at(sub + NM_EX), ap, to_argb(col), ascale * s, angle, false); break;
        case 1:
        case 2: edge_arrow(ctx, handle_at(sub + NM_ARROW), ap, to_argb(col), ascale * s, angle, true); break;
        default: break;
        }
    if (status != 0) {                                                               // 0x8adfb0: down / grappled
        V4 c = status == 1 ? V4{0.9411765f, 0.0941177f, 0.0627451f, 0.0f} : V4{0.1254902f, 0.5019608f, 0.9411765f, 0.0f};
        c.w = col.w;
        V4 ring = c;
        const float fill = R<float>(e, N_FILL);
        if (fill < 0.25f && std::fmod(t * 3.0f, 1.0f) < 0.5f) ring = V4{1.0f, 1.0f, 1.0f, col.w};   // 0x8ae9d0
        const float p = clamp01(std::fabs(std::cos(t * 4.0f)));
        const float big = s * 50.0f;
        const float pulse = big * ((1.0f - p) * 3.5f + p * 5.0f), still = big * 4.5f;
        const V2 dir{std::cos(t), std::sin(t)};                                       // turning with the clock
        const Handle glow = handle_at(sub + NM_PULSE);
        quad(ctx, glow, V4{c.x, c.y, c.z, col.w * 0.75f}, status_at, V2{pulse, pulse}, true, dir, V2{0.0f, 0.0f}, V2{1.0f, 1.0f},
             "name status glow");   // 0x8ae170
        quad(ctx, glow, V4{0.0f, 0.0f, 0.0f, col.w}, status_at, V2{still, still}, true, dir, V2{0.0f, 0.0f}, V2{1.0f, 1.0f},
             "name status glow");       // 0x8ae22c
        meter_8a9e48(ctx, status_at, handle_at(sub + NM_RING), ring, s, fill, 30.0f, 50.0f);         // 0x8ae29c
        const float r = s * 30.0f;                                                   // 0x8aea2c / 0x8aedb8
        const uint64_t hflags = G<uint64_t>(kHud + HUD_FLAGS_PTR);
        const bool button = hflags && reinterpret_cast<const uint8_t *>(uintptr_t(hflags))[HUD_FLAG_REVIVE_BUTTON];
        const Handle h = handle_at(sub + (status == 1 ? (button ? NM_TRIANGLE : NM_DOWN) : NM_GRAPPLE));
        rect(ctx, h, V4{1.0f, 1.0f, 1.0f, col.w}, V2{status_at.x - r, status_at.y - r},
             V2{status_at.x + r, status_at.y + r});
    }
    if (!text[0]) return;                                                            // 0x8ae2b4
    const float ty = base - H;                                                       // f27
    if (!emblem && rank > 58) {                                                      // 0x8ae664: rank icon, 8 x 8 atlas
        const float u0 = float(rank & 7) * 0.125f, v0 = float((rank & 63) >> 3) * 0.125f;
        const Handle h = sid_handle(rank > 63 ? kSidRankIconsHigh : kSidRankIcons, 1);
        const float hx = icon * 0.5f, hy = icon * 0.5f;
        const float ix = left + hx + s * -8.0f, iy = ty + s * -4.0f + hy;
        rect(ctx, h, V4{1.0f, 1.0f, 1.0f, name_alpha}, V2{ix - hx, iy - hy}, V2{ix + hx, iy + hy}, V2{u0, v0},
             V2{u0 + 0.125f, v0 + 0.125f});
    }
    // emblem (+0x90 != 0, PS3 0x8ae2cc): material 6927b434 with the emblem texture set through the hudquad
    // batch (0x8b0220(material, emblem+16)); PS4 has no way to pass that texture: not drawn, its space kept.
    draw_text(left + icon, ty, s, name_col, 8, 99999.0f, 40.0f, text);              // 0x8ae458
    if (talk > 0.0f) {                                                               // 0x8ae46c: talking icon
        const float hx = talk * 0.5f, hy = talk * 0.5f;
        const float ix = cx + half + hx + s * 6.0f, iy = ty + hy;
        rect(ctx, sid_handle(kSidTalk, 1), V4{1.0f, 1.0f, 1.0f, name_alpha}, V2{ix - hx, iy - hy},
             V2{ix + hx, iy + hy});
    }
}

// PS3 0x8aeeb0 (element+2080): render state (0x93db60 2676 / 6204, PS4 keeps the HUD pass state) and
// every entry.
void draw_names(void *ctx, const uint8_t *element) {
    const uint8_t *sub = element + E_NAMES;
    const int32_t n = R<int32_t>(sub, NM_COUNT);
    for (int32_t i = 0; i < n && i < kMaxNames; ++i) draw_name(ctx, sub, sub + NM_SIZE * uint64_t(i));
}

// ---- type 7 territory panel (PS3 0x8af960..0x8afe6c) and the objective carrier icon (PS3 0x8af360 /
// 0x8afe70..0x8b00b8) -------------------------------------------------------------------------------
// Territories (DcNetSpawnTerritory PS3 0x4ccdf4 = PS4 0x669840): ngm list PS3 +6596 (5 handles of 8 bytes,
// count +6636) = PS4 ngm+0x1d80 (5 handles of 0x10: object pointer slot at +0, id at +8; count +0x1dd0);
// a handle is live when *slot != 0 and (*slot)+0x44 == id (PS3 +56). Territory object (PS3 0x4e49e0 =
// PS4 0x671cd0; all PS3 offsets + 0xb4):
//   +0x234 owner team (PS3 +384, -1 none), +0x238 capturing team (+388, -1 none), +0x250 index 1..5 (+412),
//   +0x258 / +0x25c capture / loss end frame (+420 / +424), +0x268 / +0x26c their lengths in s (+436 / +440).
// PS3 0x491b60(ngm, n): the territory whose index is n. PS3 0x4e3f60: owner colour = 0x495cf0 (= PS4
// 0x64aeb0(out, ngm, team, -1)) out +12 (PS4 out +0x10) or 0xb0f0e030 for no owner. PS3 0x4e3ab8 (inlined
// in PS4 0x6724cc..0x672592): capture progress, clamp((now - end + len) / len) for a capture, 1 - that for
// a loss, 0 otherwise; now = clock [0x1116180+0x204] (PS3 [0xe60a00+460]), len = int(secs * 300).
constexpr uint64_t NGM_TERRITORIES = 0x1d80, NGM_TERRITORY_COUNT = 0x1dd0;
constexpr uint64_t TR_OWNER = 0x234, TR_CAPTURER = 0x238, TR_INDEX = 0x250, TR_GAIN_END = 0x258, TR_LOSS_END = 0x25c,
                   TR_GAIN_SECS = 0x268, TR_LOSS_SECS = 0x26c;
constexpr uint64_t fTeamColors = 0x64aeb0;                     // PS3 0x495cf0
constexpr uint64_t S_T7_FRAME = 0x60, S_T7_RECT = 0x68, S_T7_TEXT = 0x78;   // PS3 set +48, +52 (rect), +68
constexpr uint32_t kSidTerritorySquare = 0x12ffe6ae, kSidTerritoryRing = 0x68951e57;

const uint8_t *territory(int32_t n) {
    const uint32_t count = G<uint32_t>(kNgm + NGM_TERRITORY_COUNT);
    for (uint32_t i = 0; i < count && i < 5; ++i) {
        const uint64_t h = kNgm + NGM_TERRITORIES + 0x10 * uint64_t(i);
        const uint8_t *slot = reinterpret_cast<const uint8_t *>(uintptr_t(G<uint64_t>(h)));
        const uint32_t id = G<uint32_t>(h + 8);
        if (!slot || !id) continue;
        const uint8_t *obj = ptr_at(slot, 0);
        if (obj && R<uint32_t>(obj, 0x44) == id && R<int32_t>(obj, TR_INDEX) == n) return obj;
    }
    return nullptr;
}
// PS3 0x495cf0 out +12 / PS4 0x64aeb0 out +0x10: team colour 0x??RRGGBB.
uint32_t team_color(int32_t team) {
    alignas(16) uint8_t out[0x40] = {};
    reinterpret_cast<void(U2MP_GUEST *)(void *, void *, int32_t, int32_t)>(image.base + fTeamColors)(
        out, image.base + kNgm, team, -1);
    return R<uint32_t>(out, 0x10);
}
float territory_progress(const uint8_t *obj) {
    const int32_t now = G<int32_t>(kFrames);
    auto part = [&](uint64_t end_f, uint64_t secs_f) {
        const int32_t len = int32_t(float(R<int32_t>(obj, secs_f)) * 300.0f);
        return (float(now - R<int32_t>(obj, end_f) + len) * (1.0f / 300.0f)) / (float(len) * (1.0f / 300.0f));
    };
    if (R<int32_t>(obj, TR_GAIN_END)) return clamp01(part(TR_GAIN_END, TR_GAIN_SECS));
    if (R<int32_t>(obj, TR_LOSS_END)) return clamp01(1.0f - part(TR_LOSS_END, TR_LOSS_SECS));
    return 0.0f;
}

// PS3 0x8af960: frame image set+0x60 at (0, 0), time left from the round start, then the 5 territories
// in the rect set+0x68 {x, y, w, h} offset by the frame image's position (+0x24): slot i is centred at
// x + w * 0.2 * (i + 0.5), square of h at the rect's top; the local team 1 sees them in reverse order.
// Per territory: when someone is capturing (+0x238 >= 0) and the progress is < 0.99, a ring meter (ring
// sid 68951e57, PS3 element+6344 = markers+936) of 0.4 around the square in the capturing team's colour
// with alpha 0xe0; the square (sid 12ffe6ae) in half the owner colour; its number i + 1 with the text
// widget set+0x78 (box = the square, scale 0.7): black at alpha 0.5647, then the owner colour 2 higher.
void draw_type7_panel(void *ctx, const uint8_t *set, const uint8_t *local) {
    frame_image(ctx, set, S_T7_FRAME);
    time_left(set, NGM_ROUND_START);                                               // 0x8af988 / 0x8aff14
    const uint8_t *img = ptr_at(set, S_T7_FRAME);
    const uint8_t *tw = ptr_at(set, S_T7_TEXT);
    if (!img) return;
    const float rx = R<float>(set, S_T7_RECT), ry = R<float>(set, S_T7_RECT + 4), rw = R<float>(set, S_T7_RECT + 8),
                rh = R<float>(set, S_T7_RECT + 12);
    const float step = rw * 0.2f, half = rh * 0.5f;                                // f21, f19
    const float x0 = R<float>(img, 0x24) + rx + step * 0.5f;                      // f22
    const float top = R<float>(img, 0x28) + ry;                                  // f20 = y + h / 2 - h / 2
    const Handle square = sid_handle(kSidTerritorySquare, 1), ring = sid_handle(kSidTerritoryRing, 1);
    TextWidget tpl{};
    if (tw) std::memcpy(&tpl, tw, 0x2c);
    for (int i = 0; i < 5; ++i) {
        const int n = local && R<int32_t>(local, T_TEAM) == 1 ? 5 - i : i + 1;    // 0x8afad4
        const uint8_t *obj = territory(n);
        if (!obj) continue;
        const int32_t owner = R<int32_t>(obj, TR_OWNER);
        const uint32_t c = (owner < 0 ? 0xb0f0e030u : team_color(owner)) | 0xff000000u;   // 0x8afb2c
        const V4 col = argb(c);
        const V2 p0{x0 + step * float(i) - half, top};
        const int32_t capturer = R<int32_t>(obj, TR_CAPTURER);
        if (capturer >= 0) {                                                       // 0x8afba4
            const float prog = territory_progress(obj);
            if (prog < 0.99f) {                                                    // 0x8afd5c
                const uint32_t rc = team_color(capturer) | 0xe0000000u;
                meter_8a9e48(ctx, V2{p0.x + half, p0.y + half}, ring, argb(rc), 0.4f, prog, 30.0f, 50.0f);
            }
        }
        rect(ctx, square, V4{col.x * 0.5f, col.y * 0.5f, col.z * 0.5f, col.w}, p0, V2{p0.x + rh, p0.y + rh});   // 0x8afc60
        if (!tw) continue;
        char num[8];
        std::snprintf(num, sizeof num, "%i", i + 1);
        TextWidget t = tpl;                                                        // 0x8afc78: words +0..+0x10
        t.w = rh, t.h = rh, t.scale = 0.7f;
        t.r = 0.0f, t.g = 0.0f, t.b = 0.0f, t.a = 0.5647059f;
        const Pos pos{p0.x, p0.y};
        reinterpret_cast<TextFn>(image.base + fText)(&t, &pos, "%s", kScale, num);
        t.y -= 2.0f;                                                               // 0x8afcf4
        t.r = col.x, t.g = col.y, t.b = col.z, t.a = col.w;
        reinterpret_cast<TextFn>(image.base + fText)(&t, &pos, "%s", kScale, num);
    }
}

// Objective carrier icon (PS3 0x8af360, competitive types other than 1 / 2 after the score panel, i.e.
// the default panel branch 0x8af220 only, not types 7 / 10):
//   the carry object is the ngm handle PS3 +6412 / +6416 = PS4 ngm+0x1cc0 (slot pointer) / +0x1cc8 (id)
//   (set by NetEventSpawnCarryObject PS3 0x47d2fc = PS4 0x6237ab and NetEventPickupPickedUp); its carrier
//   is the tracker whose id (PS3 tracker+280 = PS4 +0x120; PS3 0x4918e8) equals the flag's owner id PS3
//   +800 = PS4 +0x440 (DcNetCaptureFlag PS3 0x4cdb84 = PS4 0x669a77); image set+0x18 without a carrier,
//   +0x20 when the carrier is on the local team, +0x28 otherwise (PS3 set +12 / +16 / +20), at (0, 0).
// Not ported:
//   - the fallback when that handle is dead (0x8af3ac..0x8af414): the 10 trackers' character objects
//     (PS3 tracker+16 / +20) searched for one holding a 310f465e (capture flag) process through character
//     +44 bit 0x1000 (virtual slot 116 of +6696) or bit 0x200 (held object handle +7600, PS3 0x45da80);
//     the PS4 character layout for +44 / +6696 / +7600 is not mapped;
//   - the 0.5 s blink (fmod([[0xf116d4]+12] / 300, 1) < 0.5) when PS3 0x2b75c(flag) is false: 0x2b75c
//     (virtual slot 80 of the flag, tag e9a3dd67, a camera distance test) is inlined on PS4 (0x63e439,
//     0x649879) and not ported; the icon is drawn steadily.
constexpr uint64_t NGM_CARRY_SLOT = 0x1cc0, NGM_CARRY_ID = 0x1cc8, FLAG_OWNER = 0x440, T_ID = 0x120;
constexpr uint64_t S_CARRY_NONE = 0x18, S_CARRY_OWN = 0x20, S_CARRY_OTHER = 0x28;

void draw_carrier(void *ctx, const uint8_t *set, int32_t team) {
    const uint8_t *slot = reinterpret_cast<const uint8_t *>(uintptr_t(G<uint64_t>(kNgm + NGM_CARRY_SLOT)));
    const uint32_t id = G<uint32_t>(kNgm + NGM_CARRY_ID);
    if (!slot || !id) return;
    const uint8_t *flag = ptr_at(slot, 0);
    if (!flag || R<uint32_t>(flag, 0x44) != id) return;                          // 0x8af370..0x8af3a8
    const int32_t owner = R<int32_t>(flag, FLAG_OWNER);                            // 0x8afea0
    const uint8_t *carrier = nullptr;
    for (int i = 0; i < 10; ++i) {                                                 // PS3 0x4918e8
        const uint8_t *t = image.base + kNgm + 0x10 + 0x2c0 * uint64_t(i);
        if (R<int32_t>(t, T_ID) == owner) {
            carrier = t;
            break;
        }
    }
    const uint64_t w = !carrier ? S_CARRY_NONE : R<int32_t>(carrier, T_TEAM) == team ? S_CARRY_OWN : S_CARRY_OTHER;
    frame_image(ctx, set, w);                                                      // 0x8aff04 / 0x8b0088 / 0x8b0090
}

// layout_matches() additions:
//   0x669844 48 8d 05 15 1f ad 00 44 8b 80 d0 1d 00 00   lea rax,[ngm]; mov r8d,[rax+0x1dd0] (territory count)
//   0x66985e 48 05 88 1d 00 00                            add rax,0x1d88 (territory handle ids)
//   0x66988c 83 bf 50 02 00 00 ff                         cmp dword [rdi+0x250],-1 (territory index)
//   0x67235e 8b 93 34 02 00 00                            mov edx,[rbx+0x234] (owner team)
//   0x6722a8 8b 93 38 02 00 00                            mov edx,[rbx+0x238] (capturing team)
//   0x6724cc 8b 83 58 02 00 00 85 c0 74 47 c5 fa 2a 83 68 02 00 00   gain end / length
//   0x67251d 8b 83 5c 02 00 00                            loss end
//   0x67237f 44 8b b5 48 fe ff ff                         mov r14d,[rbp-0x1b8] (0x64aeb0 out +0x10)
//   0x64aeb0 55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18   team colours prologue
//   0x6237ab 48 89 8a c0 1c 00 00 89 82 c8 1c 00 00       carry object handle store
//   0x669a77 8b 83 40 04 00 00 41 3b 86 20 01 00 00       flag owner id vs tracker id
bool type7_layout_matches() {
    static const unsigned char count[] = {0x48, 0x8d, 0x05, 0x15, 0x1f, 0xad, 0x00, 0x44, 0x8b, 0x80, 0xd0, 0x1d, 0x00, 0x00};
    static const unsigned char ids[] = {0x48, 0x05, 0x88, 0x1d, 0x00, 0x00};
    static const unsigned char index[] = {0x83, 0xbf, 0x50, 0x02, 0x00, 0x00, 0xff};
    static const unsigned char owner[] = {0x8b, 0x93, 0x34, 0x02, 0x00, 0x00};
    static const unsigned char capturer[] = {0x8b, 0x93, 0x38, 0x02, 0x00, 0x00};
    static const unsigned char gain[] = {0x8b, 0x83, 0x58, 0x02, 0x00, 0x00, 0x85, 0xc0, 0x74,
                                         0x47, 0xc5, 0xfa, 0x2a, 0x83, 0x68, 0x02, 0x00, 0x00};
    static const unsigned char loss[] = {0x8b, 0x83, 0x5c, 0x02, 0x00, 0x00};
    static const unsigned char colour_out[] = {0x44, 0x8b, 0xb5, 0x48, 0xfe, 0xff, 0xff};
    static const unsigned char colours[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                            0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18};
    static const unsigned char carry[] = {0x48, 0x89, 0x8a, 0xc0, 0x1c, 0x00, 0x00, 0x89, 0x82, 0xc8, 0x1c, 0x00, 0x00};
    static const unsigned char flag_owner[] = {0x8b, 0x83, 0x40, 0x04, 0x00, 0x00, 0x41, 0x3b, 0x86, 0x20, 0x01, 0x00, 0x00};
    return image.matches(0x669844, count, sizeof count) && image.matches(0x66985e, ids, sizeof ids) &&
           image.matches(0x66988c, index, sizeof index) && image.matches(0x67235e, owner, sizeof owner) &&
           image.matches(0x6722a8, capturer, sizeof capturer) && image.matches(0x6724cc, gain, sizeof gain) &&
           image.matches(0x67251d, loss, sizeof loss) && image.matches(0x67237f, colour_out, sizeof colour_out) &&
           image.matches(fTeamColors, colours, sizeof colours) && image.matches(0x6237ab, carry, sizeof carry) &&
           image.matches(0x669a77, flag_owner, sizeof flag_owner);
}

// PS3 0x8ab600(entry, pos, f1 alpha, f2 scale s, r7 measure): one message line, left to right; returns
// its width. Every text is DrawText with the HUD message font, scale 0.5 s, box 999999 x 40, flags 8
// (12 when measuring), colour = the entry colour (0x00RRGGBB) with alpha * 255:
//   0x8ab8b0 (drawing only, count +0x7c != 0 and flags +0x78 & 6): a badge left of the line, a 50 x 25
//     box at (x - 58, y - 1) in 0x5495bb (flag 2 clear) or 0xd73e60 (flag 2 set) drawn by 0x4156cc, with
//     "%s%i" of TextDb 9b5b3c66 (flag 2 clear) or "" and the count, scale 0.4, centred in the box at
//     (x - 58, y + 4), black;
//   text 1 at (x, y); x += its width + 10 s;
//   the weapon icon +0x68 (0x8aba60; a hud.pak image by name, 0.45 s high), x += its width + 10 s;
//   flag 8 (+0x78): the headshot icon (sid 2fb98f98 general/net/headshot, net table), 28 s square at
//     (x - 4 s, y + 10 - 14 s), white; x += 30 s;
//   text 2 at (x, y).
// PS4 has no 0x4156cc (PS3's rounded rect): the badge box is the net table's square image tinted (square
// corners). The weapon icon record is mapped below (M_ICON).
constexpr uint32_t kSidHeadshot = 0x2fb98f98, kTextCountPrefix = 0x9b5b3c66;
constexpr uint64_t M_ICON = 0x68, fByName = 0x555140;
constexpr float kMsgIconScale = 0.45f;
struct NameHandle {
    uint64_t idtable;
    uint32_t index;
};
using ByNameFn = NameHandle(U2MP_GUEST *)(const char *, int);
float draw_message_line(void *ctx, const uint8_t *e, float x, float y, float alpha, float s, bool measure) {
    const uint32_t flags = measure ? 12 : 8;
    auto colour = [&](uint64_t field) {
        const uint32_t c = R<uint32_t>(e, field);
        return V4{float((c >> 16) & 0xff) / 255.0f, float((c >> 8) & 0xff) / 255.0f, float(c & 0xff) / 255.0f, alpha};
    };
    char t1[49], t2[49];
    std::memcpy(t1, e + M_TEXT1, 48), t1[48] = 0;
    std::memcpy(t2, e + M_TEXT2, 48), t2[48] = 0;
    const uint32_t eflags = R<uint32_t>(e, 0x78);
    const int32_t count = R<int32_t>(e, M_COUNT);
    if (!measure && count != 0 && (eflags & 6)) {                          // 0x8ab8b0: count badge
        const bool red = eflags & 2;
        const char *prefix = red ? "" : reinterpret_cast<FindFn>(image.base + fTextFind)(image.base + kTextDb,
                                                                                         kTextCountPrefix);
        char badge[24];
        std::snprintf(badge, sizeof badge, "%s%i", prefix ? prefix : "", int(count));
        const V4 bc = argb(red ? 0xffd73e60u : 0xff5495bbu);
        // PS3 0x4156cc's filled box: the net table's plain square (12ffe6ae, the Turf War square) tinted with
        // the badge colour; the count black on it, as PS3. Without that image the count keeps the box colour.
        Handle sq;
        const bool box = resolve(sid_handle(kSidTerritorySquare, 1), "message badge", sq);
        if (box)
            quad(ctx, sq, V4{bc.x, bc.y, bc.z, alpha}, V2{x - 58.0f, y - 1.0f}, V2{50.0f, 25.0f}, false,
                 V2{0.0f, -1.0f}, V2{0.0f, 0.0f}, V2{1.0f, 1.0f}, "message badge");
        draw_text(x - 58.0f, y + 4.0f, 0.4f, box ? V4{0.0f, 0.0f, 0.0f, alpha} : V4{bc.x, bc.y, bc.z, alpha}, 1, 50.0f,
                  25.0f, badge);
    }
    const float ts = 0.5f * s;
    float cx = x;
    if (t1[0]) {
        const float w1 = draw_text(cx, y, ts, colour(M_COLOR1), 12, 999999.0f, 40.0f, t1).w;
        if (!measure) draw_text(cx, y, ts, colour(M_COLOR1), flags, 999999.0f, 40.0f, t1);
        cx += w1;
    }
    cx += 10.0f * s;                                                       // 0x8ab754 / 0x8ab768
    if (const uint8_t *icon = ptr_at(e, M_ICON)) {                         // 0x8ab760: the weapon icon
        // PS4 record (hud-settings 9353ed16 +0x40 list, next +0x68; filled by 0x646560 from the weapon id):
        // +0x10 image name (loaded with 0x555140(name, 0) like the icon element 0x562b06), +0x18 size x / y,
        // +0x28 / +0x30 = PS3 +28 / +36 (advance = width * +36 / +28). PS3 draws it 0.45 s x size.
        const char *name = reinterpret_cast<const char *>(uintptr_t(R<uint64_t>(icon, 0x10)));
        const float sx = R<float>(icon, 0x18), sy = R<float>(icon, 0x1c);
        if (name && name[0] && sx > 0.0f && sy > 0.0f && sx < 1024.0f && sy < 1024.0f) {
            const float k = kMsgIconScale * s, w = sx * k, h = sy * k;
            const float div = R<float>(icon, 0x28), mul = R<float>(icon, 0x30);
            const float advance = (div > 0.0f && mul > 0.0f && mul / div < 4.0f) ? w * mul / div : w;
            if (!measure) {
                const NameHandle nh = reinterpret_cast<ByNameFn>(image.base + fByName)(name, 0);
                const Handle hd{uint32_t(nh.idtable), uint32_t(nh.idtable >> 32), int32_t(nh.index)};
                quad(ctx, hd, V4{1.0f, 1.0f, 1.0f, alpha}, V2{cx, y + 10.0f - h * 0.5f}, V2{w, h}, false,
                     V2{0.0f, -1.0f}, V2{0.0f, 0.0f}, V2{1.0f, 1.0f}, "message weapon icon");
            }
            cx += advance + 10.0f * s;
        }
    }
    if (eflags & 8) {                                                    // 0x8ab7d8: headshot icon
        const float hs = 28.0f * s;
        if (!measure)
            quad(ctx, sid_handle(kSidHeadshot, 1), V4{1.0f, 1.0f, 1.0f, alpha}, V2{cx - 4.0f * s, y + 10.0f - hs * 0.5f},
                 V2{hs, hs}, false, V2{0.0f, -1.0f}, V2{0.0f, 0.0f}, V2{1.0f, 1.0f}, "message headshot");
        cx += 2.0f * s + hs;
    }
    float w2 = 0.0f;
    if (t2[0]) {
        w2 = draw_text(cx, y, ts, colour(M_COLOR2), 12, 999999.0f, 40.0f, t2).w;
        if (!measure) draw_text(cx, y, ts, colour(M_COLOR2), flags, 999999.0f, 40.0f, t2);
    }
    return cx + w2 - x;
}

// PS3 0x8abc68 (list element+0x40): per entry appear / vanish as above, alpha = appear * vanish * HUD alpha
// ([0xe1a660] -> [0x104ed0c]); from the newest (count - 1) to the oldest at y = 650 - sum(vanish * 25),
// +25 per line: width = line(measure, scale 1); s = width > 350 ? 350 / width : 1; the background widget
// (0x8a10fc = 0x5559f0) at (x - 8, y - 2) with the line's alpha; the line at (x, y) with scale s.
void draw_messages(void *ctx, const uint8_t *element, const uint8_t *set) {
    const uint8_t *list = element + E_MSG_LIST;
    const int32_t count = R<int32_t>(list, 0);
    if (count <= 0 || count > kMaxMessages) return;
    const float now = float(G<int32_t>(kClock)) * (1.0f / kFramesPerSecond);
    const float hud_alpha = G<float>(kHudAlpha);
    float xs[kMaxMessages], alphas[kMaxMessages], used = 0.0f;
    for (int i = 0; i < count; ++i) {
        const uint8_t *e = list + MSG_ENTRIES + MSG_SIZE * i;
        const float appear = clamp01((now - R<float>(e, M_START)) * kMsgAppear);
        const float vanish = clamp01((R<float>(e, M_END) - now) * kMsgVanish);
        used += vanish * kMsgLine;
        xs[i] = kMsgX + kMsgSlide * (1.0f - appear) * (1.0f - appear);
        alphas[i] = appear * vanish * hud_alpha;
    }
    const uint8_t *back = set ? ptr_at(set, S_MSG_BACK) : nullptr;
    float y = kMsgBaseY - used;
    for (int i = count - 1; i >= 0; --i, y += kMsgLine) {
        const uint8_t *e = list + MSG_ENTRIES + MSG_SIZE * i;
        const float w = draw_message_line(ctx, e, xs[i], y, alphas[i], 1.0f, true);
        const float s = w > kMsgMaxWidth ? kMsgMaxWidth / w : 1.0f;
        if (back) widget_image(ctx, back, Pos{xs[i] - kMsgBackX, y - kMsgBackY}, alphas[i], "message back");
        draw_message_line(ctx, e, xs[i], y, alphas[i], s, false);
    }
}

// ---- weapon panel on a weapon switch (hudweapon.cpp element, bits 0x8) ----
// The weapon panel (icon, ammo, grenades; element vtable 0x103f760, Draw 0x563a20, widget 0x8632c9ad of
// hud-settings 9353ed16) draws while HUD flag 0x8 is set. Only the weapon controller's ShowWeaponHud
// (player+0x2768, vtable 0x1026d40 slot +0x2f0 = 0x954740 = PS3 0x69e538) sets it: it ORs 0x8, marks the
// element and stores the fade. In matches the panel never appeared on a weapon switch (tests 50-67), so
// here the multiplayer element's Draw (index 9 of the HUD loop 0x554710, before the weapon panel at 13)
// calls that same function for kWeaponHudSeconds after the local player's current weapon changes
// (player vtable slot +0x260 = 0x71dec0: the inventory entry at +0x4b8). The probe counts the game's own
// calls (slot hook) and the panel's draws, and logs them with the multiplayer draw line.
constexpr uint64_t kLocalPlayer = 0x115bd20;        // handle {cell *, id}; object id at +0x44 (0x9765d0)
constexpr uint64_t P_WEAPON_CTRL = 0x2768, VT_CURRENT_WEAPON = 0x260, VT_SHOW_WEAPON_HUD = 0x2f0;
constexpr uint64_t fCurrentWeapon = 0x71dec0, fShowWeaponHud = 0x954740;
constexpr uint64_t vtWeaponCtrlShow = 0x1027030;    // vtable 0x1026d40 slot +0x2f0
constexpr uint64_t vtWeaponPanelDraw = 0x103f768;   // vtable 0x103f760 slot 1
constexpr uint64_t fWeaponPanelDraw = 0x563a20;
constexpr float kWeaponHudSeconds = 3.0f, kWeaponHudArg = -100.0f;   // the player states pass -100 (0xe66364)
using ShowWeaponHudFn = void(U2MP_GUEST *)(void *, float);
using CurrentWeaponFn = void *(U2MP_GUEST *)(void *);
using PanelDrawFn = void(U2MP_GUEST *)(void *, void *);

bool g_weapon_hud = false;
uint64_t g_game_shows = 0, g_own_shows = 0, g_panel_draws = 0, g_switches = 0;
uint64_t g_show_callers[6] = {};
uint64_t g_last_weapon = 0;
int32_t g_show_until = 0;
bool g_weapon_warned = false;

U2MP_GUEST void weapon_ctrl_show_probe(void *ctrl, float arg) {
    ++g_game_shows;
    const uint64_t from = uint64_t(uintptr_t(__builtin_return_address(0))) - uint64_t(uintptr_t(image.base));
    for (uint64_t &c : g_show_callers) {
        if (c == from) break;
        if (!c) { c = from; break; }
    }
    reinterpret_cast<ShowWeaponHudFn>(image.base + fShowWeaponHud)(ctrl, arg);
}

U2MP_GUEST void weapon_panel_draw_probe(void *element, void *ctx) {
    ++g_panel_draws;
    reinterpret_cast<PanelDrawFn>(image.base + fWeaponPanelDraw)(element, ctx);
}

uint8_t *local_player() {
    const uint64_t cell = G<uint64_t>(kLocalPlayer);
    const uint32_t id = G<uint32_t>(kLocalPlayer + 8);
    if (!cell || !id) return nullptr;
    uint8_t *obj = reinterpret_cast<uint8_t *>(uintptr_t(*reinterpret_cast<const uint64_t *>(uintptr_t(cell))));
    if (!obj || R<uint32_t>(obj, 0x44) != id) return nullptr;
    return obj;
}

void weapon_panel_tick() {
    if (!g_weapon_hud) return;
    uint8_t *player = local_player();
    if (!player) { g_last_weapon = 0; return; }
    const uint64_t pvt = R<uint64_t>(player, 0);
    const uint64_t get = pvt ? *reinterpret_cast<const uint64_t *>(uintptr_t(pvt + VT_CURRENT_WEAPON)) : 0;
    uint8_t *ctrl = const_cast<uint8_t *>(ptr_at(player, P_WEAPON_CTRL));
    const uint64_t cvt = ctrl ? R<uint64_t>(ctrl, 0) : 0;
    const uint64_t show = cvt ? *reinterpret_cast<const uint64_t *>(uintptr_t(cvt + VT_SHOW_WEAPON_HUD)) : 0;
    const uint64_t base = uint64_t(uintptr_t(image.base));
    if (get != base + fCurrentWeapon ||
        (show != base + fShowWeaponHud && show != uint64_t(uintptr_t(&weapon_ctrl_show_probe)))) {
        if (!g_weapon_warned) {
            g_weapon_warned = true;
            log("hud: weapon panel: player slot +0x260 0x%llx / controller slot +0x2f0 0x%llx not the expected "
                "functions, weapon switch trigger off", (unsigned long long)(get ? get - base : 0),
                (unsigned long long)(show ? show - base : 0));
        }
        return;
    }
    const uint64_t weapon = uint64_t(uintptr_t(reinterpret_cast<CurrentWeaponFn>(uintptr_t(get))(player)));
    const int32_t now = G<int32_t>(kFrames);
    if (weapon != g_last_weapon) {
        if (weapon && g_last_weapon) ++g_switches;
        g_last_weapon = weapon;
        if (weapon) g_show_until = now + int32_t(kWeaponHudSeconds * kFramesPerSecond);
    }
    if (weapon && now - g_show_until < 0) {
        ++g_own_shows;
        reinterpret_cast<ShowWeaponHudFn>(image.base + fShowWeaponHud)(ctrl, kWeaponHudArg);
    }
}

void install_weapon_panel() {
    if (const char *env = std::getenv("UNCHARTED_U2MP_WEAPON_HUD"); env && env[0] == '0') {
        log("hud: UNCHARTED_U2MP_WEAPON_HUD=0, weapon panel trigger off");
        return;
    }
    // 0x954740: mov esi,0x2bbd20fa (booster check) / or [rax],0x8; 0x563a20 prologue + lookup 9353ed16
    static const unsigned char show[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xec, 0x10, 0xc5, 0xfa, 0x11, 0x45,
                                         0xfc, 0xbe, 0xfa, 0x20, 0xbd, 0x2b};
    static const unsigned char orbit[] = {0x83, 0x08, 0x08};
    static const unsigned char panel[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
    if (!image.matches(fShowWeaponHud, show, sizeof show) || !image.matches(0x95477a, orbit, sizeof orbit) ||
        !image.matches(fWeaponPanelDraw, panel, sizeof panel) || !image.matches(fCurrentWeapon,
            reinterpret_cast<const unsigned char *>("\x48\x63\x8f\xb8\x04\x00\x00"), 7)) {
        log("hud: weapon panel bytes differ, weapon switch trigger off");
        return;
    }
    uint64_t &s1 = *image.at<uint64_t>(vtWeaponCtrlShow);
    uint64_t &s2 = *image.at<uint64_t>(vtWeaponPanelDraw);
    const uint64_t base = uint64_t(uintptr_t(image.base));
    if (s1 == base + fShowWeaponHud) s1 = uint64_t(uintptr_t(&weapon_ctrl_show_probe));
    if (s2 == base + fWeaponPanelDraw) s2 = uint64_t(uintptr_t(&weapon_panel_draw_probe));
    g_weapon_hud = true;
    log("hud: weapon panel: shown for %.0f s after a weapon switch (ShowWeaponHud 0x954740 from the "
        "multiplayer draw), probes on", double(kWeaponHudSeconds));
}

// Slot 1 of the multiplayer HUD element (PS3 0x8aef68): (element, draw context). The PS3 order:
//   competitive (not co-op 4/8/9/12), not type 11, flag 0x40000 (0x8af1fc): type 7 panel (0x8af960),
//     type 10 panel (0x8af648), or the score panel (0x8af220) and, for types other than 1 / 2, the
//     objective carrier icon (0x8af360);
//   type 9: the co-op money / wave panel (0x8af4e0; flag 0x8000, wave screen hidden);
//   a film playing (ngm+0x209c): the theater HUD (0x8af620, flag 0x100000; not ported, see the notes),
//     then the watched player's boosters (0x8af180; flag 0x40000, cinema mode);
//   competitive: the loadout boosters (0x8af440; flag 0x10) and, not type 11, the message list (0x8af424;
//     flag 0x80000);
//   flag 0x20000: names over heads (0x8af4c4) and world markers (0x8af4d8);
//   always: the centre message (0x8af1e0 -> 0x8ab4d0).
// The PS3 DrawText flushes 0x73bf94(0) between the parts are not needed: PS4 queues HUD text in order.
U2MP_GUEST void net_hud_draw(void *element_v, void *ctx) {
    const uint8_t *element = static_cast<const uint8_t *>(element_v);
    if (G<int32_t>(kNetState) < 2) return;                       // PS3 IsMultiplayer 0x4384b0
    const uint64_t flags_ptr = G<uint64_t>(kHud + HUD_FLAGS_PTR);
    if (!flags_ptr) return;
    uint32_t flags;
    std::memcpy(&flags, reinterpret_cast<const void *>(uintptr_t(flags_ptr)), 4);
    const uint8_t *rec = lookup(kHudSettings);
    const uint8_t *set = rec ? ptr_at(rec, REC_MP_WIDGETS) : nullptr;
    const uint8_t *local = reinterpret_cast<const uint8_t *>(uintptr_t(G<uint64_t>(kNgm)));   // PS3 0x4919d4
    const uint8_t *game = net_game_record();                                                  // PS3 0x499908
    if (!set || !local || !game) return;
    const int32_t type = R<int32_t>(game, 0xc);
    const bool coop = type == 4 || type == 8 || type == 9 || type == 12;   // PS3 0x4a0bcc
    const int32_t team = R<int32_t>(local, T_TEAM);
    if (++g_draws == 1 || g_draws % 600 == 0)
        log("hud: multiplayer draw #%llu: type %d, flags 0x%x, team %d, scores %u/%u, limit %d, time limit %.0f s, "
            "names %d, markers %d, messages %d; draw list %p, image tables hud %u / net %u",
            (unsigned long long)g_draws, type, flags, team, team_score(team), team_score(team == 0),
            G<int32_t>(kNgm + NGM_SCORE_LIMIT), double(G<float>(kNgm + NGM_TIME_LIMIT)),
            R<int32_t>(element + E_NAMES, NM_COUNT), R<int32_t>(element + E_MARKERS, 0), R<int32_t>(element + E_MSG_LIST, 0),
            ctx, material_table(0) ? unsigned(material_table(0)->count) : 0u,
            material_table(1) ? unsigned(material_table(1)->count) : 0u);
    if (g_weapon_hud && (g_draws == 1 || g_draws % 600 == 0))
        log("hud: weapon panel: %llu switches, ShowWeaponHud %llu by the game (from %llx %llx %llx %llx) / %llu "
            "here, panel draws %llu, flags 0x%x",
            (unsigned long long)g_switches, (unsigned long long)g_game_shows, (unsigned long long)g_show_callers[0],
            (unsigned long long)g_show_callers[1], (unsigned long long)g_show_callers[2],
            (unsigned long long)g_show_callers[3], (unsigned long long)g_own_shows,
            (unsigned long long)g_panel_draws, flags);
    weapon_panel_tick();
    if (!coop && type != 11 && (flags & kFlagScorePanel)) {
        if (type == 7) {
            draw_type7_panel(ctx, set, local);
        } else {
            frame_image(ctx, set, W_FRAME);
            text(set, W_SCORE, "%i", int(team_score(team)));
            text(set, W_SCORE_OTHER, "%i", int(team_score(team == 0)));
            if (type == 10) {
                label(set, W_LABEL, kLabelRound);
                text(set, W_VALUE, "%i", G<int32_t>(kNgm + NGM_ROUND) + 1);
                time_left(set, NGM_MATCH_START);
            } else {
                label(set, W_LABEL, kLabelScoreLimit);
                text(set, W_VALUE, "%i", G<int32_t>(kNgm + NGM_SCORE_LIMIT));
                time_left(set, NGM_ROUND_START);
                if (type != 1 && type != 2) draw_carrier(ctx, set, team);   // 0x8af360
            }
        }
    }
    draw_coop_panel(ctx, element, set, flags);                   // 0x8af150: type 9 only (checked inside)
    draw_boosters(ctx, element, local, coop, flags);              // 0x8af158 film site, 0x8af1a0 loadout site
    if (!coop && type != 11 && (flags & kFlagMessages)) draw_messages(ctx, element, set);   // 0x8af424
    if (flags & kFlagNames) {                                     // 0x8af1d4
        draw_names(ctx, element);
        draw_markers(ctx, element, set);
    }
    draw_list(ctx, element, set);                                 // 0x8af1e0, every frame
}

struct Check {
    uint64_t at;
    std::initializer_list<unsigned char> bytes;
    const char *what;
};

bool layout_matches() {
    static const Check checks[] = {
        {fText, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x81, 0xec, 0x10, 0x02, 0x00, 0x00, 0x84, 0xc0}, "text 0x5555a0"},
        {fImage, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec}, "image 0x5559f0"},
        // ctor 0x55aab2: lea rax,[rip+..] (-> 0x103f550); element bits 0x2000 at 0x55ab94
        {0x55aab2, {0x48, 0x8d, 0x05, 0x97, 0x4a, 0xae, 0x00}, "element vtable"},
        {0x55ab94, {0xc7, 0x43, 0x08, 0x00, 0x20, 0x00, 0x00}, "element bits"},
        {fQuad, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x81, 0xec, 0xb8, 0x02, 0x00, 0x00}, "quad 0x555c60"},
        {fProject, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x89, 0xfb, 0xc5, 0x78}, "projection 0x5566b0"},
        {fMenuQuad, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x40, 0x49}, "menu quad 0xbdce90"},
        {fDrawText, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x58, 0x12, 0x00, 0x00}, "DrawText 0xcc9a70"},
        // 0x5557e7: 0x5555a0 hands its state (rbp-0xd0) to DrawText
        {0x5557e7, {0x48, 0x8d, 0xbd, 0x30, 0xff, 0xff, 0xff, 0x48, 0x89, 0xde, 0xe8, 0x7a, 0x42, 0x77, 0x00}, "text -> DrawText"},
        // names: init stores the arrow handle at sub+0xca8; Update: count element+0x14e0, entry base +0x860
        // (stride 0xa0), side +0x86c, emblem +0x8f0, talking +0x8f8, rank +0x8fc
        {0x55d557, {0x48, 0x89, 0x83, 0xa8, 0x0c, 0x00, 0x00, 0x89, 0x93, 0xb0, 0x0c, 0x00, 0x00}, "names handles"},
        {0x55b8d2, {0x49, 0x63, 0x8f, 0xe0, 0x14, 0x00, 0x00}, "names count"},
        {0x55b8e3, {0x48, 0x8d, 0x1c, 0x89, 0x48, 0xc1, 0xe3, 0x05}, "names stride"},
        {0x55b918, {0x49, 0x89, 0x8c, 0x1f, 0x60, 0x08, 0x00, 0x00}, "names position"},
        {0x55b953, {0x41, 0xc7, 0x84, 0x1f, 0x6c, 0x08, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00}, "names side"},
        {0x55c0e1, {0x88, 0x84, 0x1e, 0xf8, 0x08, 0x00, 0x00}, "names talking"},
        {0x55c0e8, {0x49, 0x8b, 0x85, 0xb0, 0x02, 0x00, 0x00, 0x48, 0x89, 0x84, 0x1e, 0xf0, 0x08, 0x00, 0x00}, "names emblem"},
        {0x55c08f, {0x41, 0x8b, 0x85, 0x28, 0x01, 0x00, 0x00, 0x89, 0x84, 0x1e, 0xfc, 0x08, 0x00, 0x00}, "names rank"},
        {0x8921bb, {0xc6, 0x80, 0x0c, 0x04, 0x00, 0x00, 0x01}, "HUD flags +0x40c"},
        // markers: add 0x555340 (hud+0x320 -> +0x1560), ctor inits of +0x1560 / +0x1a90, message start +0x1b10
        {0x555340, {0x48, 0x8b, 0xbf, 0x20, 0x03, 0x00, 0x00, 0xc5, 0xf8, 0x28, 0x09, 0x4c, 0x8d, 0x97, 0x60, 0x15}, "marker add"},
        {0x55ab69, {0x48, 0x8d, 0xbb, 0x60, 0x15, 0x00, 0x00}, "markers init"},
        {0x55ab81, {0x48, 0x8d, 0xbb, 0x90, 0x1a, 0x00, 0x00}, "centre message init"},
        {0x649bc1, {0x41, 0x89, 0x87, 0x10, 0x1b, 0x00, 0x00}, "centre message start"},
    };
    for (const Check &c : checks) {
        const std::vector<unsigned char> b(c.bytes);
        if (!image.matches(c.at, b.data(), b.size())) {
            log("hud: %s bytes at 0x%llx differ, multiplayer HUD not installed", c.what, (unsigned long long)c.at);
            return false;
        }
    }
    if (!is_ret_stub(hDrawStub)) {
        log("hud: 0x%llx is not the ret stub, multiplayer HUD not installed", (unsigned long long)hDrawStub);
        return false;
    }
    if (!boosters_layout_matches() || !type7_layout_matches()) {
        log("hud: booster / co-op / territory bytes differ, multiplayer HUD not installed");
        return false;
    }
    return vtNetHud + 0x10 <= image.size && kTeamScorePtr + 8 <= image.size;
}

}  // namespace

void install_hud(int &installed) {
    if (!image.base) return;
    if (const char *env = std::getenv("UNCHARTED_U2MP_HUD"); env && env[0] == '0') {
        log("hud: UNCHARTED_U2MP_HUD=0, multiplayer HUD draw left as the PS4 stub");
        return;
    }
    if (!layout_matches()) return;
    uint64_t &slot = *image.at<uint64_t>(vtNetHud + 8);
    if (slot != uint64_t(uintptr_t(image.base + hDrawStub))) {
        log("hud: vtable 0x%llx slot 1 is not the stub 0x%llx, left alone", (unsigned long long)vtNetHud,
            (unsigned long long)hDrawStub);
        return;
    }
    slot = uint64_t(uintptr_t(&net_hud_draw));
    log("hud: multiplayer HUD draw (PS3 0x8aef68) -> C++ (vtable 0x%llx slot 1)", (unsigned long long)vtNetHud);
    ++installed;
    install_weapon_panel();
}

}  // namespace u2mp
