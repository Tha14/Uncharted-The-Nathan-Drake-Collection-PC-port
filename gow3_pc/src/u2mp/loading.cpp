// u2mp loading: the multiplayer loading screen, ported from PS3 1.10. Trace and per-element status:
// u2mp/docs/PORT-FRONTEND5.md.
//
// PS3: while 0xf119c5 is set (LoadLevels::Enter 0x4405c4 / WaitReady::Enter 0x43bd28 set it,
// LoadLevels::Exit 0x438448 / WaitReady::Exit 0x443f00 clear it), the per-frame net display 0x447248
// runs the loading object 0xf1ff04: 0x4adaf8 (show / hide) and 0x4ae638 (draw). Everything is drawn in
// the menu's 1280x720 units through the menu item calls with a 10-float colour transform:
//   [0] scale, [1..4] colour multipliers r g b a, [5] [6] adds, [7] multiplier, [8] [9] translation;
//   identity 0x420f58, packed colour 0x421018, copy 0x421548, compose 0x4212ec (multiply 0..4 and 7,
//   add 5, 6, 8, 9), menu colour by id 0x42159c / 0x3ec768 (the MenuColor table built by 0x3ebc18).
// PS4 keeps the same transform layout (the widget composition at 0x57a80b does 0x4212ec's arithmetic)
// and the same item calls: PS3 0x4153d0 (text in a rect) = PS4 0x5b5450, PS3 0x4156cc (descriptor in a
// rect) = PS4 0x5b5920; PS4 adds a draw context (rsi) that is NULL here: everything then goes to the
// frame's deferred 2D arena (the renderer tests it at 0x5be5cd), as the menu quads of menuimages.cpp.
//
// Ported, in PS3 order: the fade (0x4aefe4..), the black full-screen fill (type 0x1b, 0x4ae850), the
// two edge quads (0x4ae944..0x4aead0), the team rosters (PAGE_MULTIPLAYER_LOADING drawn in place,
// PS3 0x423410: PS4 page object menu+0x370[], layout 0x5a2670 and item render only, no frame or prompts,
// with the PS3 slide-in and fade-in), the title-back band (0x4aeb68), the map name (0x4aec24, MenuColor cac46a80
// = 0xffcd4100, scale 1.4) and the game description (0x4aed68, PS3 0x49b5f0 = PS4 0x64abd0, MenuColor
// f1758b4d = 0xffdfcf8f), and the card systems (section "cards" below, PORT-FRONTEND5.md 7): the four
// particle systems of the loading object (start 0x4ad600, spawn 0x4ac9f4, frame 0x4ac7d8, camera
// 0x4ad124, draw 0x4adbd0) with the gallery picture gallery1/loading-screens/fort.dds (0x4aef94 ->
// 0x414968; PS4 slot of 0x58b5c8) and the projection 0x4adbd0 puts in the menu matrix.
// And the tip line 0x4acc34 with its per-glyph animation (section "tip", PORT-FRONTEND5.md 7.6).
// Not ported: the letterbox bars (only below 719 lines).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

#include "frontend3.h"
#include "u2mp_internal.h"

namespace u2mp {
namespace {

constexpr uint64_t fDraw2D = 0x554340, cDraw2D = 0x3fc52d;
constexpr uint64_t fTextFind = 0xcddc00, kTextDb = 0x1066660, fDcLookup = 0xa15e70;
constexpr uint64_t fItemText = 0x5b5450;      // PS3 0x4153d0
constexpr uint64_t fItemDesc = 0x5b5920;      // PS3 0x4156cc
// PS4 0x5a6000 (context, desc, page, alpha) is the full page draw 0x5d12f0 uses; not called (draw_page)
constexpr uint64_t fPageLayout = 0x5a2670;    // PS3 0x408928 (page, desc)
constexpr uint64_t kItemFade = 0x1050e58, kFadeScale = 0x104ed74, PAGE_FLAGS = 0xb0, PAGE_FADE = 0xf4;
constexpr uint64_t fGameText = 0x64abd0;      // PS3 0x49b5f0 "%s - %s - %s"
constexpr uint64_t kNgm = 0x113b760, NGM_GAME = 0x1ca0, NGM_OVERRIDES = 0x21b4;   // PS3 ngm+7220
constexpr uint64_t MENU_PAGES = 0x370, MENU_PAGE_COUNT = 200, PAGE_ID = 0x10, MENU_CTX = 0x10, MENU_17A4 = 0x17a4;
constexpr uint64_t kFrameCounter = 0x1116180 + 0x104;   // read by 0x5d1325
constexpr uint32_t kNetGames = 0x3b9a067d, kLevels = 0x0dec97a4;
constexpr uint32_t kPageLoading = 0xb9df67d6;
static_assert(sid("PAGE_MULTIPLAYER_LOADING") == kPageLoading, "page id");
// PS3 MenuColor values (0x3ebc18: AddColor(SID, 0xAARRGGBB))
constexpr uint32_t kColorMapName = 0xffcd4100;   // cac46a80
constexpr uint32_t kColorGameType = 0xffdfcf8f;  // f1758b4d

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }

struct DcTable { int32_t count; int32_t pad; uint8_t *records; };
DcTable *dc(uint32_t id) { return guest<DcTable *(U2MP_GUEST *)(uint32_t)>(fDcLookup)(id); }
const char *text(uint32_t id) {
    if (!id) return nullptr;
    return guest<const char *(U2MP_GUEST *)(void *, uint32_t)>(fTextFind)(image.at<void>(kTextDb), id);
}

// ---- the colour transform (PS3 0x420f58 .. 0x4212ec) -------------------------------------------------
struct alignas(16) Xform { float v[10]; };
Xform x_identity() {   // 0x420f58
    Xform x{};
    x.v[0] = x.v[1] = x.v[2] = x.v[3] = x.v[4] = x.v[7] = 1.0f;
    return x;
}
void x_colour(Xform &x, uint32_t argb) {   // 0x421018 (and 0x42159c with a MenuColor: same fields)
    constexpr float k = 1.0f / 255.0f;      // [0xdaf9d8]
    x.v[1] = float((argb >> 16) & 0xff) * k;
    x.v[2] = float((argb >> 8) & 0xff) * k;
    x.v[3] = float(argb & 0xff) * k;
    x.v[4] = float((argb >> 24) & 0xff) * k;
}
Xform x_compose(Xform d, const Xform &s) {   // 0x421548 copy + 0x4212ec
    for (int i : {0, 1, 2, 3, 4, 7}) d.v[i] *= s.v[i];
    for (int i : {5, 6, 8, 9}) d.v[i] += s.v[i];
    return d;
}

// ---- state (the PS3 loading object) ------------------------------------------------------------------
std::atomic<bool> showing{false};
float fade = 0;                                   // obj+0
std::chrono::steady_clock::time_point shown_at, last_frame;
bool have_frame = false;
alignas(16) char game_text[128];                  // PS3 1520(1)
const char *map_name = nullptr;
std::mutex data_mutex;
bool enabled = true, with_text = true, with_rosters = true, with_cards = true, with_perspective = true;
bool cards_ready = false;   // install checked the PS4 calls the cards use
void tip_reset();           // section "tip"

// =====================================================================================================
// cards: the PS3 particle systems of the loading object (obj+32 / +868 / +1704 / +2540, 836 bytes each:
// a count and 16 particles of 52 bytes). Every constant below is the PS3 one (TOC base r30 = 0xdba260).
// =====================================================================================================
constexpr uint64_t fSlotDraw = 0x5b4e90;   // PS3 0x414ad8(ctx, quad, slot) -> (ctx, list, quad, slot)
constexpr uint64_t fDrawQuad = 0xbdce90;   // PS3 0x8b16f0 + 0x8b1598: (draw ctx, {material, 0, 0, 0}, quad)
constexpr uint64_t kDataRoot = 0x104ad28;  // [ ] + 0x408: the data root the gallery loader 0x58b5d3 uses
// The gallery slot, relative to the menu manager (= ctx - 0x10; PS3 0x414968 uses ctx +444/+180/+699/+700,
// PS4 0x58b5c8 slot +0x268/+0x120/+0x367/+0x368, and the menu update 0x5b46dc sees them at ctx +0x258,
// +0x110 (state, 3 = loaded), +0x257, +0x358 (keep-alive frames)).
constexpr uint64_t GAL_STATE = 0x120, GAL_PATH = 0x268, GAL_PATH_END = 0x367, GAL_KEEP = 0x368;
constexpr uint64_t CTX_GAL_STATE = 0x110, CTX_GAL_LOADED = 0x128;

// PS3 0x90a900 / 0x90a56c: Marsaglia's multiply-with-carry generator, float = u32 x 2^-32.
struct Mwc {
    uint32_t x[4] = {0x2545f491u, 0x9e3779b9u, 0x7f4a7c15u, 0x6a09e667u}, c = 0x1234567u;
    uint32_t next() {
        const uint64_t sum = 2111111111ull * x[3] + 1492ull * x[2] + 1776ull * x[1] + 5115ull * x[0] + c;
        x[3] = x[2];
        x[2] = x[1];
        x[1] = x[0];
        x[0] = uint32_t(sum);
        c = uint32_t(sum >> 32);
        return x[0];
    }
    float unit() { return float(next()) * 2.3283061589829401e-10f; }   // [0xdd8... -32728]
};

struct Card {            // PS3 particle, 52 bytes
    float x, y, angle;   // +0 +4 +8
    uint8_t *material;   // +12..+20 (0x8a015c handle)
    int32_t type;        // +24: 0 own material, 1 menu slot 8 (gallery), 2 menu slot 10 (none: solid)
    float w, h;          // +28 +32
    int32_t orient;      // +36: quarter of a 2x2 sheet, -1 whole
    float r, g, b;       // +40 +44 +48
};
struct CardSystem {
    int32_t count = 0;
    Card p[16];
};
struct Camera { float x, y, angle, zoom, alpha; };   // 0x4ad124 out, 20 bytes

std::mutex cards_mutex;
CardSystem systems[4];
Mwc rnd;
float card_seed = 0;                                  // obj+12
std::chrono::steady_clock::time_point card_start;     // obj+8
bool cards_started = false;

uint8_t *card_material(uint32_t id) { return menu_net_material(id); }   // PS3 0x8a015c

// 0x4ac9f4(sys, material, {w, h}, -, orient, spread)
void spawn(CardSystem &s, uint8_t *material, float w, float h, int32_t orient, float spread) {
    if (s.count > 15) return;
    Card &p = s.p[s.count++];
    const float r1 = rnd.unit(), r2 = rnd.unit();
    p.x = r1 * 1180.0f + 50.0f;   // [-32688], [-32664]
    p.y = r2 * 620.0f + 50.0f;    // [-32656]
    const float r3 = rnd.unit();
    p.angle = spread * -0.5f + spread * r3;   // [-32648]
    p.material = material;
    p.h = h;
    p.w = w;
    p.orient = orient;
    p.type = 0;
    const float r4 = rnd.unit(), r5 = rnd.unit(), r6 = rnd.unit();
    p.b = r6 * 0.2f + 0.8f;   // [-32680], [-32672]
    p.r = r4 * 0.2f + 0.8f;
    p.g = r5 * 0.2f + 0.8f;
}

// 0x4ac7d8: four black slot-10 rects framing the 1280x720 card (centre (x0 + x1) / 2 + {640, 360})
void add_frame(CardSystem &s) {
    if (s.count > 12) return;
    struct R { float x, y, w, h; };
    static constexpr R rects[4] = {
        {640.0f, -500.0f, 5280.0f, 1000.0f},   // above
        {-1000.0f, 360.0f, 2000.0f, 720.0f},   // {-2640, -360} .. {-640, 360}
        {640.0f, 1220.0f, 5280.0f, 1000.0f},   // {-2640, 360} .. {2640, 1360}
        {2280.0f, 360.0f, 2000.0f, 720.0f},    // {640, -360} .. {2640, 360}
    };
    for (const R &r : rects) {
        Card &p = s.p[s.count++];
        p = Card{};
        p.x = r.x;
        p.y = r.y;
        p.w = r.w;
        p.h = r.h;
        p.orient = -1;
        p.type = 2;
    }
}

// 0x4ad600: start (on the first frame shown, from 0x4adaf8)
void start_cards() {
    // the PS3 material tables 0xb40630 (player photos) and 0xb40600 (map photos)
    static constexpr uint32_t kPhotos[16] = {
        sid("general/net/player-photos/drake"),      sid("general/net/player-photos/elena"),
        sid("general/net/player-photos/chloe"),      sid("general/net/player-photos/sullivan"),
        sid("general/net/player-photos/rescuer"),    sid("general/net/player-photos/soldier-1a"),
        sid("general/net/player-photos/soldier-1b"), sid("general/net/player-photos/soldier-2a"),
        sid("general/net/player-photos/soldier-2b"), sid("general/net/player-photos/soldier-3a"),
        sid("general/net/player-photos/lazarevic"),  sid("general/net/player-photos/lieutenant"),
        sid("general/net/player-photos/merc"),       sid("general/net/player-photos/jeff"),
        sid("general/net/player-photos/schafer"),    sid("general/net/player-photos/flynn")};
    static constexpr uint32_t kMaps[9] = {
        sid("general/net/map-photos/warzone"),  sid("general/net/map-photos/warzone-coop"),
        sid("general/net/map-photos/shambhala"), sid("general/net/map-photos/gompa"),
        sid("general/net/map-photos/treasure"), sid("general/net/map-photos/ice-cave"),
        sid("general/net/map-photos/village"),  sid("general/net/map-photos/village-coop"),
        sid("general/net/map-photos/train-wreck")};
    static_assert(kPhotos[0] == 0x05a8ba5fu && kPhotos[15] == 0xf69a236cu && kMaps[0] == 0x83b0526au &&
                      kMaps[8] == 0x9d998d63u, "PS3 tables 0xb40630 / 0xb40600");
    std::lock_guard<std::mutex> lock(cards_mutex);
    card_start = std::chrono::steady_clock::now();   // obj+8 = [[0xe60a00]+236]
    {
        const float r = rnd.unit();
        card_seed = r + r;                           // obj+12 = 2 x rand (0x4ad65c)
    }
    for (CardSystem &s : systems) s.count = 0;
    CardSystem &a = systems[0], &b = systems[1], &c = systems[2];
    // the gallery card (type 1) in the middle, 1280 x 720, white
    if (a.count <= 15) {
        Card &p = a.p[a.count++];
        p = Card{640.0f, 360.0f, 0.0f, nullptr, 1, 1280.0f, 720.0f, -1, 1.0f, 1.0f, 1.0f};
    }
    add_frame(a);
    spawn(a, card_material(sid("general/net/loading-screen/money")), 512.0f, 512.0f, -1, 0.78539819f);   // [-32380]
    bool used[16] = {};
    for (int i = 0; i < 10; ++i) {   // ten different player photos, 256 x 256
        uint32_t k = 0;
        for (int tries = 0;; ++tries) {
            k = rnd.next() & 15;
            if (!used[k] || tries == 99) break;
        }
        used[k] = true;
        spawn(b, card_material(kPhotos[k]), 256.0f, 256.0f, -1, 0.94247788f);   // [-32368]
    }
    std::memset(used, 0, 9);
    for (int i = 0; i < 4; ++i) {    // four different map photos, 512 x 256
        uint32_t k = 0;
        for (int tries = 0;; ++tries) {
            k = rnd.next() % 9;
            if (!used[k] || tries == 99) break;
        }
        used[k] = true;
        spawn(b, card_material(kMaps[k]), 512.0f, 256.0f, -1, 0.94247788f);
    }
    uint8_t *coins = card_material(sid("general/net/loading-screen/coins"));
    for (int i = 0; i < 12; ++i) spawn(c, coins, 100.0f, 100.0f, i & 3, 0.1f);   // [-32508]
    cards_started = true;
}

// 0x89fe2c: smoothstep (0 below e0, 1 above e1, else 3t^2 - 2t^3); 0x89fe94: window a..b, edges w
float smooth(float x, float e0, float e1) {
    if (x < e0) return 0.0f;
    if (x > e1) return 1.0f;
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * 3.0f + t * t * t * -2.0f;
}
float window(float x, float a, float b, float w) { return smooth(x, a, a + w) - smooth(x, b - w, b); }

// 0x4ad124(out, obj, idx): the two camera paths (idx 0 visible outside the window, idx 1 inside it).
// t = (now - obj+8) / 300 ticks; trig as 0xabc6ec(x, 0 sin / 1 cos), the zoom cosine in double (0xabc8c4).
Camera camera(float t, int idx) {
    Camera out{};
    const float ph = std::fmod(t * 0.07f, 1.0f);           // 0xabb130, [-32516]
    const float k = float(idx) * 2.0923009f;                // [-32524]
    const float base = k * 0.75f + (t + card_seed);         // [-32464]
    float px, py;
    const float w = window(ph, 0.5f, 1.0f, 0.1f);           // [-32752], 1.0, [-32508]
    if (idx == 0) {
        out.alpha = 1.0f - w;
        const float a = base * 0.20943953f;                 // [-32500]
        px = std::cos(a);
        py = std::sin(a);
    } else {
        out.alpha = w;
        const float a = base * 0.15f;                       // [-32492]
        const float amp = (1.0f - std::cos(a + a)) * 0.25f + 0.5f;   // [-32456]
        px = amp * std::sin(a);
        py = amp * std::cos(a * 0.5f);
    }
    out.x = px * 250.0f;                                    // [-32448]
    out.y = py * 150.0f;                                    // [-32440]
    const float c = float(std::cos(double(t) * 0.1 + double(k)));   // [-32480] = 0.1 (double)
    const float s = std::clamp(c * 0.5f + 0.5f, 0.0f, 1.0f);
    out.zoom = s * 1.25f + (1.0f - s);                      // [-32432]
    out.angle = std::sin(t * 0.1f + k) * 0.15707964f;       // [-32508], [-32472]
    return out;
}

// The projection 0x4adbd0 builds before its particles and stores (transposed) in the menu matrix
// [0xf0e830] = ctx+0x20 (restored to identity after them), row-vector convention as every PS3 matrix
// helper here (0x35780 dst = a x b):
//   camera M0 = {{-1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,-30,1}} (0x10644 from 0xaece90 / 0xaea700 /
//   0xb40670), view = M0^-1 (0x1b248); P = {{-f,0,0,0},{0,f,0,0},{0,0,1.000001,1},{0,0,-0.0001,0}},
//   f = 1 / tanf(0.024674013) (0xabbac0, [-32348]; 0xb40680, 0xb40690); tilt = rotX(0.01) (0x42e160,
//   [-32304]); G = tilt x view x P.
// A 2D primitive goes through such a matrix the way the PS3 2D library does it (0x8b0de8: v = (x - 640,
// 360 - y, z, 1) x M, then x, y scaled by 2 / 1280, 2 / 720 and divided by w): the screen point is
// (640 + X / W, 360 - Y / W). PS4 draws the quads with plain 2D corners, so the projection is done here.
struct Projector {
    float f, s, c;
    Projector() : f(1.0f / std::tan(0.024674013f)), s(std::sin(0.01f)), c(std::cos(0.01f)) {}
    // W of a screen point (the depth the GPU divides by)
    float depth(float y) const { return with_perspective ? (360.0f - y) * s + 30.0f : 1.0f; }
    void operator()(float x, float y, float &sx, float &sy) const {
        if (!with_perspective) {
            sx = x;
            sy = y;
            return;
        }
        const float xc = x - 640.0f, yc = 360.0f - y;
        // tilt: (xc, yc c, yc s); view: (-xc, yc c, yc s + 30); P: X = f xc, Y = f yc c, W = yc s + 30
        const float w = yc * s + 30.0f;
        sx = 640.0f + f * xc / w;
        sy = 360.0f - f * yc * c / w;
    }
};
const Projector &projector() {
    static const Projector p;
    return p;
}

void prepare() {   // the net game's map name (level record +4) and its description (0x64abd0)
    const DcTable *games = dc(kNetGames), *levels = dc(kLevels);
    const int16_t game = G<int16_t>(kNgm + NGM_GAME);
    std::lock_guard<std::mutex> lock(data_mutex);
    map_name = nullptr;
    game_text[0] = 0;
    if (!games || !games->records || game < 0 || game >= games->count) return;
    uint8_t *rec = games->records + 0x90 * size_t(game);
    const uint32_t level = *reinterpret_cast<const uint32_t *>(rec + 4);
    for (int32_t i = 0; levels && levels->records && i < levels->count; ++i) {
        const uint8_t *l = levels->records + 0x38 * size_t(i);
        if (*reinterpret_cast<const uint32_t *>(l) == level) map_name = text(*reinterpret_cast<const uint32_t *>(l + 4));
    }
    // PS3 0x49b5f0(ngm, buf, 128, rec, ngm+7220, 0, 1, 1); PS4 order as its caller 0x657ee2
    guest<void(U2MP_GUEST *)(void *, char *, int, void *, void *, int, int, int)>(fGameText)(
        image.at<void>(kNgm), game_text, int(sizeof game_text), rec, image.at<void>(kNgm + NGM_OVERRIDES), 0, 1, 1);
    game_text[sizeof game_text - 1] = 0;
}

// PS3 polls the flag 0xf119c5 once per frame (0x447248 -> 0x4adaf8(obj, flag)); the state functions only
// set it. LoadLevels::Exit and WaitReady::Enter run in the same frame, so PS3 never sees the screen
// hidden between them and nothing restarts (run 46 restarted the timer, the cards and the rosters'
// 3 s delay there). The hooks therefore only record the request; apply_show() runs per frame.
std::atomic<int> requested{-1};   // -1 nothing new, else 0 / 1
const char *requested_why = "";
void refresh_page(uint32_t id);   // 0x4233c4, below

void show(bool on, const char *why) {
    if (!enabled) return;
    requested_why = why;
    requested = on ? 1 : 0;
    log("loading screen: %s requested (%s)", on ? "show" : "hide", why);
}

void apply_show() {   // 0x4adaf8 (obj+4 0 -> 1: 0x4ad600 and 0x4233c4)
    const int r = requested.exchange(-1);
    if (r < 0 || showing.load() == (r == 1)) return;
    const bool on = r == 1;
    if (on) {
        prepare();
        shown_at = std::chrono::steady_clock::now();
        if (with_cards && cards_ready) start_cards();   // 0x4adb88 -> 0x4ad600
        tip_reset();                                    // 0x4adb98: obj+24 = obj+28 = 0
        refresh_page(kPageLoading);                     // 0x4adba0 -> 0x4233c4
    }
    showing = on;
    log("loading screen: %s (%s)", on ? "shown" : "hidden", requested_why);
}

// ---- the PS4 item calls ------------------------------------------------------------------------------
void *menu_ctx() { return image.at<void>(addr::menu_manager + MENU_CTX); }   // PS3 0xf0e810

// PS3 0x4153d0(ctx, xform, text, rect, mode, r8, flags, clip, byte) -> PS4 0x5b5450 (stack order as the
// widget call 0x57a8a9..0x57a8d1).
void item_text(const Xform &x, const char *str, float rx, float ry, float rw, float rh) {
    if (!with_text || !str || !*str) return;
    alignas(16) float rect[4] = {rx, ry, rw, rh};
    alignas(16) float clip[4] = {0, 0, 0, 0};
    Xform xf = x;
    guest<void(U2MP_GUEST *)(void *, void *, Xform *, const char *, float *, int, int, int, float *, int, int, int, int)>(
        fItemText)(menu_ctx(), nullptr, &xf, str, rect, 1, 1, 0, clip, 0, 0, 0, -1);
}

// PS3 0x4156cc(ctx, xform, desc, rect, mode, r8) -> PS4 0x5b5920(ctx, context, xform, desc, rect, mode, int).
void item_fill(const Xform &x, float rx, float ry, float rw, float rh) {
    alignas(16) uint8_t desc[0x2c] = {};   // type 0x1b (solid fill, renderer 0x5c0c07); PS3 desc 512(1)
    *reinterpret_cast<uint32_t *>(desc + 0x00) = 0x1b;
    *reinterpret_cast<int32_t *>(desc + 0x1c) = -1;
    *reinterpret_cast<int32_t *>(desc + 0x24) = -1;
    *reinterpret_cast<float *>(desc + 0x28) = 1.0f;
    alignas(16) float rect[4] = {rx, ry, rw, rh};
    Xform xf = x;
    guest<void(U2MP_GUEST *)(void *, void *, Xform *, void *, float *, int, int)>(fItemDesc)(menu_ctx(), nullptr, &xf, desc,
                                                                                          rect, 1, 1);
}

// The page object by id: PS3 menu+720[90] (id +12), PS4 menu+0x370[200] (id +0x10), as ShowPage 0x5cb9c0.
uint8_t *find_page(uint32_t id) {
    const uint64_t menu = addr::menu_manager;
    for (uint64_t i = 0; i < MENU_PAGE_COUNT; ++i) {
        uint8_t *p = G<uint8_t *>(menu + MENU_PAGES + 8 * i);
        if (!p) break;
        if (*reinterpret_cast<const uint32_t *>(p + PAGE_ID) == id) return p;
    }
    return nullptr;
}

// PS3 0x4233c4(menu, id): page +216 = 2, the counter the layout 0x408928 reads (positions set, not
// animated, while it is not 0) and decrements; PS4 page +0xf8 (layout 0x5a2ad4 / 0x5a337e).
constexpr uint64_t PAGE_SNAP = 0xf8, PAGE_ITEM_COUNT = 0xb4, PAGE_ITEMS = 0xb8;
std::atomic<bool> page_log_first{false}, page_log_visible{false};
void refresh_page(uint32_t id) {
    uint8_t *page = find_page(id);
    if (page) *reinterpret_cast<int32_t *>(page + PAGE_SNAP) = 2;
    log("loading screen: rosters page %08x %s%s", id, page ? "found, refreshed (+0xf8 = 2)" : "NOT FOUND",
        with_rosters ? "" : " (rosters off)");
    page_log_first = page_log_visible = true;
}

// Diagnostics: the page's items and, for list controls (kind 10, class vtable 0x1007910), the list
// object (+0x18) range the control itself clamps with (0x58c6a0: slot 0 current, 0x30 first, 0x38 last).
void log_page(uint8_t *page, const char *when) {
    const int32_t n = *reinterpret_cast<const int32_t *>(page + PAGE_ITEM_COUNT);
    uint8_t **items = *reinterpret_cast<uint8_t ***>(page + PAGE_ITEMS);
    log("loading screen: rosters page %s: %d items", when, n);
    const uint64_t kList10 = uint64_t(uintptr_t(image.base)) + 0x1007910;
    for (int32_t i = 0; i < n && i < 8 && items; ++i) {
        uint8_t *c = items[i];
        if (!c) continue;
        const uint64_t vt = *reinterpret_cast<const uint64_t *>(c);
        if (vt != kList10) {
            log("loading screen:   item %d vtable 0x%llx", i, (unsigned long long)(vt - uint64_t(uintptr_t(image.base))));
            continue;
        }
        uint8_t *list = *reinterpret_cast<uint8_t **>(c + 0x18);
        int32_t cur = -9, first = -9, last = -9;
        if (list) {
            using Get = int32_t(U2MP_GUEST *)(void *);
            const uint64_t *lvt = *reinterpret_cast<const uint64_t *const *>(list);
            cur = reinterpret_cast<Get>(lvt[0])(list);
            first = reinterpret_cast<Get>(lvt[0x30 / 8])(list);
            last = reinterpret_cast<Get>(lvt[0x38 / 8])(list);
        }
        log("loading screen:   item %d: team list (cond %d), list %p: rows %d..%d (current %d)", i,
            *reinterpret_cast<const int32_t *>(c + 0x10), static_cast<void *>(list), first, last, cur);
    }
}

// PS3 0x423410(menu, id, alpha, pos): finds the page; desc = 0x422b18(menu, xform with alpha [4]) with
// byte desc+64 = 1 and the position composed in ([8] [9]); page->update({menu, 0, frame time})
// (vtable +8); layout 0x408928; render 0x4062fc. PS4: the update is page vtable +0x10 with {menu (8),
// event (4), time (4)} (the menu update 0x5cfb06 calls the current page so); 0x5a6000(context, desc,
// page, alpha) is the page draw 0x5d12f0 uses (PS3 0x409428: layout 0x5a2670 + render). desc =
// {menu+0x10, transform, screen rect {0, 0, 1280, 720} (0xe34290), frame / 300 (0x5d132d), +0x44 (PS3
// desc+64) = 1, menu+0x17a4}.
void draw_page(uint32_t id, float alpha, float x, float y, float dt) {
    if (!with_rosters) return;
    const uint64_t menu = addr::menu_manager;
    uint8_t *page = find_page(id);
    if (!page) return;
    struct Update { void *menu; int32_t event; float time; } u{image.at<void>(menu), 0, dt};
    static_assert(offsetof(Update, event) == 8 && offsetof(Update, time) == 12, "update args");
    using UpdateFn = void(U2MP_GUEST *)(void *, Update *);
    const uint64_t *vt = *reinterpret_cast<const uint64_t *const *>(page);
    reinterpret_cast<UpdateFn>(vt[0x10 / 8])(page, &u);
    struct alignas(8) Desc {
        void *ctx;
        float x[10];          // +0x08 (the transform, unaligned here as in 0x5d12f0's frame)
        float screen[4];      // +0x30
        float time;           // +0x40
        uint8_t state12;      // +0x44
        uint8_t pad[3];
        int32_t v17a4;        // +0x48
        int32_t pad2;
    } d{};
    static_assert(offsetof(Desc, screen) == 0x30 && offsetof(Desc, time) == 0x40 && offsetof(Desc, v17a4) == 0x48, "desc");
    d.ctx = menu_ctx();
    Xform xf = x_identity();
    xf.v[4] = alpha;   // PS3 423410: alpha at context +16, position at +32 / +36
    xf.v[8] = x;
    xf.v[9] = y;
    std::memcpy(d.x, xf.v, sizeof d.x);
    d.screen[2] = 1280.0f;
    d.screen[3] = 720.0f;
    d.time = float(G<int32_t>(kFrameCounter)) * (1.0f / 300.0f);
    d.state12 = 1;   // PS3 0x4234f8: stb 1 at desc+64
    d.v17a4 = G<int32_t>(menu + MENU_17A4);
    // PS3 0x423410 does not use the full page draw 0x409428 (frame 0x405318, background 0x4156cc, button
    // prompts 0x4074a0): it calls only the layout 0x408928 and the item render 0x4062fc, so the loading
    // screen shows the team lists alone. PS4 0x5a6000 is the full draw (background fill 0x5bc290 /
    // 0x5b56d0, layout 0x5a2670, frame 0x5b8710 / 0x5bb830, prompts 0x5a4f10, then the render, page
    // vtable +0x18 = PS3 vtable +12). Only its layout and render are called here, with the two things
    // 0x5a6000 does around them: the 0.75 scale of pages with flag 0x80000 (0x5a6090) and the item fade
    // 0x1050e58 = (1 - page +0xf4) * (flag 0x800 ? 1 : [0x104ed74]) (0x5a60bb..0x5a60f5, restored after).
    const uint32_t flags = *reinterpret_cast<const uint32_t *>(page + PAGE_FLAGS);
    if (flags & 0x80000) d.x[0] *= 0.75f;
    float &item_fade = G<float>(kItemFade);
    const float saved_fade = item_fade;
    item_fade = (1.0f - *reinterpret_cast<const float *>(page + PAGE_FADE)) * ((flags & 0x800) ? 1.0f : G<float>(kFadeScale));
    guest<void(U2MP_GUEST *)(void *, void *)>(fPageLayout)(page, &d);
    using RenderFn = void(U2MP_GUEST *)(void *, void *, void *);
    reinterpret_cast<RenderFn>(vt[0x18 / 8])(page, nullptr, &d);
    item_fade = saved_fade;
    // logged on the first frame after show and on the first fully visible one
    if (page_log_first.exchange(false)) log_page(page, "updated and drawn (first frame)");
    if (alpha >= 1.0f && page_log_visible.exchange(false)) log_page(page, "updated and drawn (visible)");
}

// ---- cards: the gallery picture (0x4aef94..0x4aefdc) -------------------------------------------------
// PS3, every frame shown and while 0x49e6e0(ngm) (a valid net game): sprintf("loading-screens/%s",
// "fort.dds") and 0x414968(menu+0x10, path). PS4 has the same loader inlined at 0x58b5c8: path
// "%s/build/main/gallery1/%s" (0xe87590) with [0x104ad28]+0x408, slot +0x368 = 10 (frames to keep it),
// a new path is copied to +0x268 (+0x367 = 0) and +0x120 = 1 if it was 0. The menu update (0x5b46dc)
// loads it into ctx+0x118 (state 3) and unloads it 10 frames after the last request.
void request_gallery() {
    const DcTable *games = dc(kNetGames);
    const int16_t game = G<int16_t>(kNgm + NGM_GAME);
    if (!games || game < 0 || game >= games->count) return;   // 0x49e6e0
    const char *root = G<const char *>(kDataRoot);
    if (!root) return;
    char path[0x200];
    std::snprintf(path, sizeof path, "%s/build/main/gallery1/%s", root + 0x408, "loading-screens/fort.dds");
    uint8_t *slot = image.at<uint8_t>(addr::menu_manager);
    *reinterpret_cast<int32_t *>(slot + GAL_KEEP) = 10;
    char *cur = reinterpret_cast<char *>(slot + GAL_PATH);
    if (std::strcmp(cur, path) != 0) {
        std::strncpy(cur, path, 0x100);
        slot[GAL_PATH_END] = 0;
        int32_t &state = *reinterpret_cast<int32_t *>(slot + GAL_STATE);
        if (state == 0) state = 1;
    }
}

// ---- cards: drawing (0x4adbd0) ------------------------------------------------------------------------
struct alignas(16) Vertex {   // the PS4 menu quad vertex (0x40 bytes), PS3 0x8b16f0's 48-byte vertex
    float x, y, z, u, v;
    float pad[3];
    float color[4];           // +0x20 r g b a
    float color2[4];          // +0x30 (1, 1, 1, 1) in the renderer's 0x5b4e90 quads (case 0x1b)
};
static_assert(sizeof(Vertex) == 0x40, "menu quad vertex");
struct alignas(16) Quad {
    Vertex v[4];
    uint32_t tail;            // cleared after the vertices by the renderer (0x5bde26, 0x5c0d5d)
    uint32_t pad[3];
};

std::atomic<int> card_log_budget{8};

// One screen quad: corners in PS3 order (-w,-h) (w,-h) (w,h) (-w,h), all with the same colour.
void submit(const Card &p, const float (&sx)[4], const float (&sy)[4], const float (&u)[4], const float (&v)[4],
            const float (&rgba)[4]) {
    Quad q{};
    for (int i = 0; i < 4; ++i) {
        q.v[i].x = sx[i];
        q.v[i].y = sy[i];
        q.v[i].u = u[i];
        q.v[i].v = v[i];
        std::memcpy(q.v[i].color, rgba, sizeof q.v[i].color);
    }
    if (p.type == 0) {   // 0x8b16f0 + 0x8b1598 with the particle's material
        if (!p.material) return;
        alignas(16) uint64_t ctx[6] = {};
        alignas(16) uint64_t info[4] = {uint64_t(uintptr_t(p.material)), 0, 0, 0};
        guest<void(U2MP_GUEST *)(void *, void *, void *)>(fDrawQuad)(ctx, info, &q);
        return;
    }
    for (Vertex &vx : q.v)
        for (float &c : vx.color2) c = 1.0f;
    // 0x414ad8(menu+0x10, 8 | 10, quad): slot 8 = menu material 8 with the gallery texture (only when
    // loaded), slot 10 = no material (solid colour)
    guest<void(U2MP_GUEST *)(void *, void *, void *, int)>(fSlotDraw)(menu_ctx(), nullptr, &q, p.type == 1 ? 8 : 10);
}

// 0x4adbd0(system, camera)
void draw_system(const CardSystem &s, const Camera &cam) {
    const Projector &proj = projector();
    for (int32_t i = 0; i < s.count && i < 16; ++i) {
        const Card &p = s.p[i];
        // UVs by orientation (0x4ae0e0..0x4ae13c): v0 = f25, v1 = f26, u0 = f27, u1 = f28
        float v0 = 0.0f, v1 = 1.0f, u0 = 0.0f, u1 = 1.0f;
        switch (p.orient) {
        case 0: v0 = 0.0f, v1 = 0.5f, u0 = 0.0f, u1 = 0.5f; break;
        case 1: v0 = 0.0f, v1 = 0.5f, u0 = 0.5f, u1 = 1.0f; break;
        case 2: v0 = 0.5f, v1 = 1.0f, u0 = 0.0f, u1 = 0.5f; break;
        case 3: v0 = 0.5f, v1 = 1.0f, u0 = 0.5f, u1 = 1.0f; break;
        default: break;
        }
        if (p.type == 1) std::swap(v0, v1);   // 0x4ae484: the gallery card is flipped in v
        // the particle transform (0x4ae140..0x4ae2a8): R(p.angle) T(p + cam - centre) R(cam.angle)
        // S(cam.zoom) T(centre), applied to the corners (0x4ae2b0..0x4ae47c)
        const float dx = (cam.x + p.x) - 640.0f, dy = (cam.y + p.y) - 360.0f;
        const float pc = std::cos(p.angle), ps = std::sin(p.angle);
        const float cc = std::cos(cam.angle), cs = std::sin(cam.angle);
        auto place_flat = [&](float lx, float ly, float &ox, float &oy) {
            const float x = lx * pc - ly * ps + dx, y = lx * ps + ly * pc + dy;
            const float x2 = x * cc - y * cs, y2 = x * cs + y * cc;
            ox = x2 * cam.zoom + 640.0f;
            oy = y2 * cam.zoom + 360.0f;
        };
        auto place = [&](float lx, float ly, float &ox, float &oy) {
            float x, y;
            place_flat(lx, ly, x, y);
            proj(x, y, ox, oy);
        };
        const float hw = p.w * 0.5f, hh = p.h * 0.5f;
        const float rgba[4] = {p.r, p.g, p.b, cam.alpha};
        // PS3 projects on the GPU with perspective-correct UVs; the PS4 quad is affine, so a textured
        // card is cut into n x n cells small enough on screen for the difference not to show.
        int n = 1;
        if (p.type != 2 && with_perspective) {
            float cx[4], cy[4];
            place(-hw, -hh, cx[0], cy[0]);
            place(hw, -hh, cx[1], cy[1]);
            place(hw, hh, cx[2], cy[2]);
            place(-hw, hh, cx[3], cy[3]);
            float edge = 0;
            for (int k = 0; k < 4; ++k)
                edge = std::max(edge, std::hypot(cx[(k + 1) & 3] - cx[k], cy[(k + 1) & 3] - cy[k]));
            n = std::clamp(int(std::ceil(edge / 96.0f)), 1, 12);
        }
        for (int gy = 0; gy < n; ++gy)
            for (int gx = 0; gx < n; ++gx) {
                const float a0 = float(gx) / float(n), a1 = float(gx + 1) / float(n);
                const float b0 = float(gy) / float(n), b1 = float(gy + 1) / float(n);
                const float lx[4] = {-hw + 2 * hw * a0, -hw + 2 * hw * a1, -hw + 2 * hw * a1, -hw + 2 * hw * a0};
                const float ly[4] = {-hh + 2 * hh * b0, -hh + 2 * hh * b0, -hh + 2 * hh * b1, -hh + 2 * hh * b1};
                const float ua[4] = {a0, a1, a1, a0}, vb[4] = {b0, b0, b1, b1};
                // the cell before projection, clipped to W >= 1 (the GPU clips at its near plane, W =
                // 0.0001; what lies between projects more than 100000 units off screen)
                struct P { float x, y, u, v, w; };
                P in[4], out[8];
                for (int k = 0; k < 4; ++k) {
                    place_flat(lx[k], ly[k], in[k].x, in[k].y);
                    in[k].u = u0 + (u1 - u0) * ua[k];
                    in[k].v = v0 + (v1 - v0) * vb[k];
                    in[k].w = proj.depth(in[k].y) - 1.0f;
                }
                int m = 0;
                for (int k = 0; k < 4; ++k) {
                    const P &a = in[k], &b = in[(k + 1) & 3];
                    if (a.w >= 0) out[m++] = a;
                    if ((a.w >= 0) != (b.w >= 0)) {
                        const float f = a.w / (a.w - b.w);
                        out[m++] = P{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.u + (b.u - a.u) * f,
                                     a.v + (b.v - a.v) * f, 0.0f};
                    }
                }
                if (m < 3) continue;
                float px[8], py[8];
                for (int k = 0; k < m; ++k) proj(out[k].x, out[k].y, px[k], py[k]);
                // a fan of quads (the last one a triangle when m is odd: its fourth corner repeats the third)
                for (int k = 1; k + 1 < m; k += 2) {
                    const int i2 = k + 1, i3 = std::min(k + 2, m - 1);
                    const int idx[4] = {0, k, i2, i3};
                    float sx[4], sy[4], u[4], v[4];
                    for (int j = 0; j < 4; ++j) {
                        sx[j] = px[idx[j]];
                        sy[j] = py[idx[j]];
                        u[j] = out[idx[j]].u;
                        v[j] = out[idx[j]].v;
                    }
                    submit(p, sx, sy, u, v, rgba);
                }
            }
        if (card_log_budget.load(std::memory_order_relaxed) > 0 && p.type != 2) {
            card_log_budget.fetch_sub(1, std::memory_order_relaxed);
            const uint8_t *ctx = static_cast<const uint8_t *>(menu_ctx());
            log("loading screen: card type %d material %p %.0fx%.0f at %.0f,%.0f, %dx%d cells, gallery state %d "
                "loaded %d", p.type, static_cast<void *>(p.material), double(p.w), double(p.h), double(p.x),
                double(p.y), n, n, *reinterpret_cast<const int32_t *>(ctx + CTX_GAL_STATE), ctx[CTX_GAL_LOADED]);
        }
    }
}

// 0x4ae858..0x4ae940 and 0x4af070 / 0x4af0bc: the two cameras and the four systems drawn with each
void draw_cards(float a, bool target) {
    std::lock_guard<std::mutex> lock(cards_mutex);
    if (!cards_started) return;
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - card_start).count();
    Camera cam0 = camera(t, 0), cam1 = camera(t, 1);
    cam0.alpha *= a;
    cam1.alpha *= a;
    const float inv = 1.0f - a, sq = inv * inv;
    const float k = std::clamp(1.0f - sq, 0.0f, 1.0f);
    const float zf = (1.0f - k) * 1.2f + k;   // [-32248]
    cam1.zoom *= zf;
    cam0.zoom *= zf;
    if (!target) {   // fading out: the cards sink by 50 (1 - a)^2
        cam1.y += sq * 50.0f;
        cam0.y += sq * 50.0f;
    }
    if (cam1.alpha != 0.0f)
        for (const CardSystem &s : systems) draw_system(s, cam1);
    if (cam0.alpha != 0.0f)
        for (const CardSystem &s : systems) draw_system(s, cam0);
}

// =====================================================================================================
// tip: the tip line 0x4acc34 with its per-glyph animation (callback 0x4ad118 -> 0x4acf2c, 0x4acb44)
// =====================================================================================================
// PS3 printer 0x73cf20(struct, text) = PS4 0xcc9a70(struct, text) (paired through the converters 0x41fd18
// / 0x5bc290). PS3 calls the struct's glyph callback (+132; NULL = 0x73b2e0, the glyph draw) per character
// from the line printer 0x73c494 (0x73c6d4) with a copy of the struct holding the glyph's x / y / scale
// and character (+120); it returns the advance. PS4: float cb(ctx, glyph) from 0xccae50 (0xccb610, copy
// at -0x110, character at +0x94); NULL = [0x104af58] = 0xcc7cc0 (eboot.elf reloc), the PS4 glyph draw:
// it reads the same fields (x, y, scale +0x20/+0x24, colour +0x28, font +0x30, flags +0x34, clip
// +0x10/+0x14, matrix +0x50.., character +0x94) and returns the advance.
constexpr uint64_t fPrint = 0xcc9a70, fGlyphDraw = 0xcc7cc0, fDeferText = 0xbda9c0;
constexpr uint64_t kArena = 0x124a000, kDeferScale = 0x1050e58, kFonts = 0x144e640;
constexpr uint32_t kTips = 0x7f306129;   // {count, records}: {u32 game type, u32 generic, u32 text id}
constexpr uint64_t NG_TYPE = 0xc;        // net game record +0xc (PS4 0x5dee67; PS3 0x49e4a0: +12)
// PS4 text struct (PS3 offsets in comments)
constexpr size_t T_X = 0x00, T_Y = 0x04, T_CLIP = 0x10 /* +24 */, T_SCALE = 0x20 /* +8 */, T_COLOUR = 0x28 /* +16 */,
                 T_VALUE = 0x2c /* +20 */, T_FONT = 0x30 /* +40 */, T_FLAGS = 0x34 /* +116 */, T_MATRIX = 0x50 /* +48 */,
                 T_ONE = 0x90, T_W = 0x94 /* +120 */, T_H = 0x98 /* +124 */, T_CB = 0xa0 /* +132 */, T_SIZE = 0xb0;
// flags: 4 = measure only (PS3 mode 4; the PS4 converter always sets it), 1 = PS3 mode 1 (the converters
// map desc bit 1 to 1 and bit 2 to 8 on both), 0x10 = break at the width limit (PS4 sets it where PS3
// passes its box width, PS3 always applies the width)
constexpr uint32_t TF_MEASURE = 4, TF_MODE1 = 1, TF_WRAP = 0x10;

using GlyphFn = float(U2MP_GUEST *)(void *, uint8_t *);
struct Tip {                      // the loading object's tip fields
    int32_t index = 0;            // obj+16
    int32_t glyph = 0;            // obj+20
    int32_t time = 0, length = 0; // obj+24, obj+28 (ticks)
} tip;
bool with_tip = true, tip_ready = false;
void tip_reset() { tip.time = tip.length = 0; }
const std::chrono::steady_clock::time_point tick_base = std::chrono::steady_clock::now();
int32_t ticks() {   // [[0xe60a00]+236]: 300 per second
    return int32_t(std::chrono::duration<double>(std::chrono::steady_clock::now() - tick_base).count() * 300.0);
}

// 0x89fd8c: cosine step (0 below a, 1 above b, else (cos(pi t) - 1) x -0.5)
float ease(float x, float a, float b) {
    if (x < a) return 0.0f;
    if (x > b) return 1.0f;
    float t = std::min((x - a) / (b - a), 1.0f);
    t = std::max(t, 0.0f);
    float r = (std::cos(t * 3.14159274f) - 1.0f) * -0.5f;
    r = std::max(r, 0.0f);
    return std::min(r, 1.0f);
}

// 0x4acb44(obj, glyph i): fade (obj+0) x (in - out); in over 75 ticks, 7.5 ticks apart per glyph; out
// over the last 150 of the duration
float glyph_alpha(int32_t i) {
    const float e = float(ticks() - tip.time) * 0.013333334f + float(i) * -0.1f;   // [-32628], [-32620]
    const float d = float(tip.length) * 0.0066666668f;                              // [-32640]
    return fade * (ease(e, 0.0f, 1.0f) - ease(e, d - 1.0f, d));
}

// 0x4ad118 -> 0x4acf2c(obj, glyph): when alpha a != 0, a shadow 1 unit lower (colour word = int(128 a):
// black, alpha 128 a), then the glyph with alpha int(255 a); returns the second draw's advance.
// PS3 colours are 0xRRGGBBAA; the PS4 printer's are the same bytes swapped (the renderer swaps them for
// it at 0x5be4da..0x5be5c3), so alpha is the top byte here.
U2MP_GUEST float tip_glyph(void *ctx, uint8_t *g) {
    const GlyphFn draw = guest<GlyphFn>(fGlyphDraw);
    // measure passes (queueing a deferred text measures right away) draw nothing and must not count
    if (*reinterpret_cast<const uint32_t *>(g + T_FLAGS) & TF_MEASURE) return draw(ctx, g);
    const float a = glyph_alpha(tip.glyph);
    ++tip.glyph;
    alignas(16) uint8_t copy[T_SIZE];
    if (a != 0.0f) {
        std::memcpy(copy, g, 0xa0);
        *reinterpret_cast<float *>(copy + T_Y) += 1.0f;   // [-32612]
        *reinterpret_cast<uint32_t *>(copy + T_COLOUR) = uint32_t(int32_t(a * 128.0f)) << 24;   // [-32548]
        draw(ctx, copy);
    }
    std::memcpy(copy, g, 0xa0);
    uint32_t &c = *reinterpret_cast<uint32_t *>(copy + T_COLOUR);
    c = (c & 0x00ffffffu) | ((uint32_t(int32_t(a * 255.0f)) & 0xffu) << 24);   // [-32540]
    return draw(ctx, copy);
}

void text_struct(uint8_t *t, float w, float h, uint32_t flags, int32_t font) {
    std::memset(t, 0, T_SIZE);
    float *clip = reinterpret_cast<float *>(t + T_CLIP);
    clip[1] = clip[3] = -1.0f;                                      // no clip (PS3 +24..+36: 0, -1, 0, -1)
    float *scale = reinterpret_cast<float *>(t + T_SCALE);
    scale[0] = scale[1] = 0.75f;                                    // 0x3f400000
    *reinterpret_cast<uint32_t *>(t + T_COLOUR) = 0xffffffffu;      // PS3 -1
    *reinterpret_cast<float *>(t + T_VALUE) = 1.0f;
    *reinterpret_cast<int32_t *>(t + T_FONT) = font;                // PS3 1
    *reinterpret_cast<uint32_t *>(t + T_FLAGS) = flags;
    float *m = reinterpret_cast<float *>(t + T_MATRIX);             // PS3 0x3cf18: identity
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    *reinterpret_cast<float *>(t + T_ONE) = 1.0f;
    *reinterpret_cast<float *>(t + T_W) = w;
    *reinterpret_cast<float *>(t + T_H) = h;
}

// 0x4acc34
void draw_tip() {
    const DcTable *tbl = dc(kTips);
    if (!tbl || !tbl->records || tbl->count <= 0) return;
    const uint8_t *recs = tbl->records;
    auto rec = [&](int32_t i) { return reinterpret_cast<const uint32_t *>(recs + 12 * size_t(i)); };
    int32_t type = 0;   // 0x49e4a0: the net game's type, 0 without one
    {
        const DcTable *games = dc(kNetGames);
        const int16_t game = G<int16_t>(kNgm + NGM_GAME);
        if (games && games->records && game >= 0 && game < games->count)
            type = *reinterpret_cast<const int32_t *>(games->records + 0x90 * size_t(game) + NG_TYPE);
    }
    const int32_t now = ticks();
    if (tip.time + tip.length < now) {   // 0x4ace20: the next tip
        uint32_t r;
        {
            std::lock_guard<std::mutex> lock(cards_mutex);   // the generator is shared with the cards
            r = rnd.next();
        }
        const int32_t count = tbl->count, old = tip.index;
        tip.index = 0;
        int32_t n = int32_t(r % uint32_t(count)) + 1;
        // count n eligible records (type == current, or generic) from index 0 and take the record after
        // the n-th, wrapping; one more while that is the previous tip. (Bounded here: PS3 spins forever
        // when nothing else qualifies.)
        for (int guard = 0; guard < count * 8; ++guard) {
            const uint32_t *x = rec(tip.index);
            if (int32_t(x[0]) == type || x[1] != 0) --n;
            tip.index = tip.index + 1 < count ? tip.index + 1 : 0;
            if (n != 0) continue;
            if (tip.index == old) {
                n = 1;
                continue;
            }
            break;
        }
        const char *s = text(rec(tip.index)[2]);
        tip.time = now;                                                   // obj+24
        tip.length = int32_t(float(s ? std::strlen(s) : 0) * 75.0f);   // obj+28: 0xac0fc8, [-32564]
    }
    const char *s = text(rec(tip.index)[2]);   // 0x9294c4
    if (!s || !*s) return;
    const int32_t font = G<void *>(kFonts + 8 * 1) ? 1 : -1;   // PS3 font 1; the PS4 default if it is not loaded
    alignas(16) uint8_t t[T_SIZE];
    // measure (mode 4) in width 600; then x = (1280 - w) / 2, y = 650 - h, the measured width kept
    text_struct(t, 600.0f, 9999.0f, TF_MEASURE | TF_WRAP, font);   // [-32584] = 9999
    guest<void(U2MP_GUEST *)(uint8_t *, const char *)>(fPrint)(t, s);
    const float w = *reinterpret_cast<float *>(t + T_W), h = *reinterpret_cast<float *>(t + T_H);
    text_struct(t, w, 9999.0f, TF_MODE1 | TF_WRAP, font);
    *reinterpret_cast<float *>(t + T_X) = (1280.0f - w) * 0.5f;     // [-32556], [-32752]
    *reinterpret_cast<float *>(t + T_Y) = 650.0f - h;              // [-32576]
    *reinterpret_cast<uint64_t *>(t + T_CB) = uint64_t(uintptr_t(&tip_glyph));   // PS3 OPD 0x4ad118
    tip.glyph = 0;                                                  // obj+20
    // no draw list, as the renderer does it (0x5be5f1): +0x90 = [0x1050e58], queue the text (0xbda9c0
    // copies the struct, callback included, and the string), then the measure-only call
    *reinterpret_cast<float *>(t + T_ONE) = G<float>(kDeferScale);
    guest<void(U2MP_GUEST *)(void *, uint8_t *, const char *)>(fDeferText)(image.at<void>(kArena), t, s);
    *reinterpret_cast<uint32_t *>(t + T_FLAGS) |= TF_MEASURE;
    guest<void(U2MP_GUEST *)(uint8_t *, const char *)>(fPrint)(t, s);
}

// ---- the screen (PS3 0x4ae638) -------------------------------------------------------------------------
void draw_screen(float dt) {
    const bool target = showing.load();
    if (target && with_cards && cards_ready) request_gallery();   // 0x4aef94..0x4aefdc, every frame shown
    // fade (0x4aefe4..0x4af06c): up while shown, down after
    fade = target ? std::min(fade + dt, 1.0f) : std::max(fade - dt, 0.0f);
    if (fade <= 0) return;
    // a = (fade - 0.4) x 1.6667 clamped while fading in; the plain fade while fading out (0x4af028 ->
    // 0x4ae750 skips the remap)
    const float a = target ? std::clamp((fade - 0.4f) * 1.6666666f, 0.0f, 1.0f) : fade;   // [db2458], [db2460]
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - shown_at).count();
    Xform base = x_identity();   // 584(1): alpha a; 904(1) = copy
    base.v[4] = a;
    Xform black = x_identity();  // 664(1): 0x421018(0xff000000)
    x_colour(black, 0xff000000u);
    // 1. full-screen fill (0x4156cc, type 0x1b, rect {0, 0, 1280, 720}), transform 624(1) = base o black
    item_fill(x_compose(base, black), 0, 0, 1280, 720);
    // the card systems with both cameras (0x4ae858..0x4ae940, 0x4af070, 0x4af0bc)
    if (with_cards && cards_ready) draw_cards(a, target);
    // 2. edges, twice (0x4ae944, 0x4aea20): {0, 0, 1280, 720} and {-10, -10, 1290, 730}, colour a
    if (uint8_t *m = menu_net_material(sid("general/net/loading-screen/edge"))) {
        menu_draw_quad(m, 0, 0, 1280, 720, 0, 0, 1, 1, a, a);
        menu_draw_quad(m, -10, -10, 1290, 730, 0, 0, 1, 1, a, a);
    }
    // 3. rosters (0x4aeb60): alpha a * clamp(t - 3, 0, 1), y = 200 * (1 - clamp((t - 2) * 0.6667, 0, 1))^2
    const float slide = 1.0f - std::clamp((t - 2.0f) * 0.6666667f, 0.0f, 1.0f);
    draw_page(kPageLoading, a * std::clamp(t - 3.0f, 0.0f, 1.0f), 0, slide * slide * 200.0f, dt);
    // 4. the band (0x4aeb68): {128, 22, 1152, 278}, colour a, alpha a * 0.75
    if (uint8_t *m = menu_net_material(sid("general/net/loading-screen/title-back")))
        menu_draw_quad(m, 128, 22, 1152, 278, 0, 0, 1, 1, a * 0.75f, a);
    const char *name;
    char desc[sizeof game_text];
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        name = map_name;
        std::memcpy(desc, game_text, sizeof desc);
    }
    // 5. map name (0x4aec54..0x4aed60): base o scale 1.4 o MenuColor cac46a80 ([5] = 3.0), rect {0, 70, 1280, 100}
    Xform big = x_identity();
    big.v[0] = 1.4f;   // [db2490]
    Xform c1 = x_identity();
    x_colour(c1, kColorMapName);
    c1.v[5] = 3.0f;    // f31 = [db249c]
    item_text(x_compose(x_compose(base, big), c1), name, 0, 70, 1280, 100);
    // 6. game description (0x4aed68..0x4aee3c): base o MenuColor f1758b4d ([5] = 3.0), rect {0, 120, 1280, 100}
    Xform c2 = x_identity();
    x_colour(c2, kColorGameType);
    c2.v[5] = 3.0f;
    item_text(x_compose(base, c2), desc, 0, 120, 1280, 100);
    // 7. the tip line (0x4aee44 -> 0x4acc34); (the bars 0x415294 apply only below 719 lines)
    if (with_tip && with_text && tip_ready) draw_tip();
}

U2MP_GUEST uint64_t draw_2d(void *tables, uint64_t a, uint64_t b) {
    const uint64_t r = guest<uint64_t(U2MP_GUEST *)(void *, uint64_t, uint64_t)>(fDraw2D)(tables, a, b);
    const auto now = std::chrono::steady_clock::now();
    const float dt = have_frame ? std::min(std::chrono::duration<float>(now - last_frame).count(), 0.1f) : 0.0f;
    last_frame = now;
    have_frame = true;
    apply_show();   // the per-frame poll of 0x447248
    if (showing.load(std::memory_order_relaxed) || fade > 0) draw_screen(dt);
    return r;
}

// ---- show / hide: TaskManagerOnline state Enter / Exit ----------------------------------------------
using StateFn = uint64_t(U2MP_GUEST *)(void *);
struct Slot { uint64_t vtable_slot, fn; bool on; const char *why; };
constexpr Slot kSlots[] = {
    {0x100ac00 + 0x08, 0x5f2e50, true, "LoadLevels::Enter"},    // PS3 0x4405c4
    {0x100ac00 + 0x10, 0x5f2e30, false, "LoadLevels::Exit"},    // PS3 0x438448
    {0x100abc0 + 0x08, 0x5f2d90, true, "WaitReady::Enter"},     // PS3 0x43bd28
    {0x100abc0 + 0x10, 0x5f2830, false, "WaitReady::Exit"},     // PS3 0x443f00
    {0x100ab80 + 0x08, 0x5f23e0, false, "Playing::Enter"},
    {0x100a9c0 + 0x08, 0x5f00d0, false, "Leave::Enter"},
};
constexpr size_t kSlotCount = sizeof(kSlots) / sizeof(*kSlots);
StateFn original[kSlotCount];
template <size_t I> U2MP_GUEST uint64_t state_fn(void *state) {
    if (!kSlots[I].on) show(false, kSlots[I].why);
    const uint64_t r = original[I](state);
    if (kSlots[I].on) show(true, kSlots[I].why);
    return r;
}
template <size_t... I> constexpr std::array<void *, sizeof...(I)> make_fns(std::index_sequence<I...>) {
    return {reinterpret_cast<void *>(&state_fn<I>)...};
}

}  // namespace

void install_loading(int &installed) {
    if (const char *v = std::getenv("UNCHARTED_U2MP_LOADING"); v && std::strcmp(v, "0") == 0) {
        enabled = false;
        log("loading screen: off (UNCHARTED_U2MP_LOADING=0)");
        return;
    }
    if (const char *v = std::getenv("UNCHARTED_U2MP_LOADING_TEXT"); v && std::strcmp(v, "0") == 0) with_text = false;
    if (const char *v = std::getenv("UNCHARTED_U2MP_LOADING_ROSTERS"); v && std::strcmp(v, "0") == 0) with_rosters = false;
    if (const char *v = std::getenv("UNCHARTED_U2MP_LOADING_PARTICLES"); v && std::strcmp(v, "0") == 0) with_cards = false;
    if (const char *v = std::getenv("UNCHARTED_U2MP_LOADING_PERSPECTIVE"); v && std::strcmp(v, "0") == 0)
        with_perspective = false;
    if (const char *v = std::getenv("UNCHARTED_U2MP_LOADING_TIP"); v && std::strcmp(v, "0") == 0) with_tip = false;
    // the PS4 calls this port makes (prologue push rbp; mov rbp,rsp)
    static const unsigned char prologue[] = {0x55, 0x48, 0x89, 0xe5};
    for (uint64_t f : {fItemText, fItemDesc, fPageLayout, fGameText})
        if (!image.matches(f, prologue, sizeof prologue)) {
            log("loading screen: 0x%llx is not a function start, not installed", (unsigned long long)f);
            return;
        }
    // the calls and the gallery loader layout the cards use: 0x5b4e90 (PS3 0x414ad8), 0xbdce90, and the
    // inlined loader 0x58b5c8 (mov dword [r13+0x368], 10 / mov ecx, 0x408) whose slot fields are written
    if (with_cards) {
        static const unsigned char slot_draw[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
        static const unsigned char quad[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53};
        static const unsigned char loader[] = {0x41, 0xc7, 0x85, 0x68, 0x03, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00,
                                               0xb9, 0x08, 0x04, 0x00, 0x00};
        static const unsigned char gal_state[] = {0x41, 0x83, 0xbd, 0x10, 0x01, 0x00, 0x00, 0x02};   // 0x5b46dc
        cards_ready = image.matches(fSlotDraw, slot_draw, sizeof slot_draw) && image.matches(fDrawQuad, quad, sizeof quad) &&
                      image.matches(0x58b5c8, loader, sizeof loader) && image.matches(0x5b46dc, gal_state, sizeof gal_state);
        if (!cards_ready) log("loading screen: card calls not as expected, cards off");
    }
    // the tip: printer 0xcc9a70, glyph draw 0xcc7cc0 (the default callback: reloc of 0x104af58), deferred
    // text 0xbda9c0, and the callback call in the line printer (0xccb602: mov rdi,[rbp-0x150]; lea rsi,
    // [rbp-0x110]; call [rbp-0x148])
    if (with_tip) {
        static const unsigned char print[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
        static const unsigned char defer[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53};
        static const unsigned char cb_call[] = {0x48, 0x8b, 0xbd, 0xb0, 0xfe, 0xff, 0xff, 0x48, 0x8d, 0xb5,
                                                0xf0, 0xfe, 0xff, 0xff, 0xff, 0x95, 0xb8, 0xfe, 0xff, 0xff};
        tip_ready = image.matches(fPrint, print, sizeof print) && image.matches(fGlyphDraw, print, sizeof print) &&
                    image.matches(fDeferText, defer, sizeof defer) && image.matches(0xccb602, cb_call, sizeof cb_call);
        if (!tip_ready) log("loading screen: tip calls not as expected, tip off");
    }
    const uint64_t base = uint64_t(uintptr_t(image.base));
    // A slot may already hold another port's hook (run 42: cinema.cpp wraps WaitReady::Exit 0x100abd0);
    // then that hook is chained instead of the PS4 function. Anything else is left alone.
    int chained = 0;
    for (const Slot &s : kSlots) {
        const uint64_t cur = G<uint64_t>(s.vtable_slot);
        if (cur == base + s.fn) continue;
        if (!cur || (cur >= base && cur < base + image.size)) {
            log("loading screen: state vtable slot 0x%llx holds 0x%llx, not 0x%llx: not installed",
                (unsigned long long)s.vtable_slot, (unsigned long long)(cur >= base ? cur - base : cur),
                (unsigned long long)s.fn);
            return;
        }
        ++chained;
    }
    if (!retarget_call(cDraw2D, fDraw2D, reinterpret_cast<void *>(&draw_2d), "loading screen draw (Draw 0x3fc52d)")) return;
    static const auto fns = make_fns(std::make_index_sequence<kSlotCount>{});
    for (size_t i = 0; i < kSlotCount; ++i) {
        original[i] = reinterpret_cast<StateFn>(G<uint64_t>(kSlots[i].vtable_slot));   // PS4 function or a host hook
        G<uint64_t>(kSlots[i].vtable_slot) = uint64_t(uintptr_t(fns[i]));
    }
    log("loading screen: PS3 port installed (%zu state hooks, %d chained; text %s, rosters %s, cards %s, "
        "perspective %s, tip %s)", kSlotCount, chained, with_text ? "on" : "off", with_rosters ? "on" : "off",
        with_cards && cards_ready ? "on" : "off", with_perspective ? "on" : "off",
        with_tip && tip_ready ? "on" : "off");
    ++installed;
}

}  // namespace u2mp
