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
// PS3 0x423410: PS4 page object menu+0x370[], page draw 0x5a6000 as 0x5d12f0 calls it, with the PS3
// slide-in and fade-in), the title-back band (0x4aeb68), the map name (0x4aec24, MenuColor cac46a80
// = 0xffcd4100, scale 1.4) and the game description (0x4aed68, PS3 0x49b5f0 = PS4 0x64abd0, MenuColor
// f1758b4d = 0xffdfcf8f). Not ported (PORT-FRONTEND5.md): the gallery picture panels 0x4adbd0 with
// their motion 0x4ad124 (fort.dds in 3D perspective), the coins / money particles 0x4ad600 and the tip
// line 0x4acc34; the letterbox bars (only below 719 lines) are not needed.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
constexpr uint64_t fPageDraw = 0x5a6000;      // (context, desc, page, alpha), as 0x5d12f0 calls it
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
bool enabled = true, with_text = true, with_rosters = true;

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

void show(bool on, const char *why) {   // 0x4adaf8
    if (!enabled || showing.load() == on) return;
    if (on) {
        prepare();
        shown_at = std::chrono::steady_clock::now();
    }
    showing = on;
    log("loading screen: %s (%s)", on ? "shown" : "hidden", why);
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

// PS3 0x423410(menu, id, alpha, pos): the page object (PS4 menu+0x370[i], id +0x10) drawn as 0x5d12f0
// draws the top page: 0x5a6000(context, desc, page, alpha), desc = {menu+0x10, transform, screen rect
// {0, 0, 1280, 720} (0xe34290), frame time / 300 (0x5d132d), menu state 1..2, menu+0x17a4}.
void draw_page(uint32_t id, float alpha, float x, float y) {
    if (!with_rosters) return;
    const uint64_t menu = addr::menu_manager;
    void *page = nullptr;
    for (uint64_t i = 0; i < MENU_PAGE_COUNT && !page; ++i) {
        void *p = G<void *>(menu + MENU_PAGES + 8 * i);
        if (!p) break;
        if (*reinterpret_cast<const uint32_t *>(static_cast<uint8_t *>(p) + PAGE_ID) == id) page = p;
    }
    if (!page) return;
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
    const int32_t st = G<int32_t>(menu);
    d.state12 = uint8_t(uint32_t(st - 1) < 2);
    d.v17a4 = G<int32_t>(menu + MENU_17A4);
    guest<void(U2MP_GUEST *)(void *, void *, void *, float)>(fPageDraw)(nullptr, &d, page, alpha);
}

// ---- the screen (PS3 0x4ae638) -------------------------------------------------------------------------
void draw_screen(float dt) {
    // fade (0x4aefe4..0x4af06c): up while shown, down after
    fade = showing.load() ? std::min(fade + dt, 1.0f) : std::max(fade - dt, 0.0f);
    if (fade <= 0) return;
    const float a = std::clamp((fade - 0.4f) * 1.6666666f, 0.0f, 1.0f);   // [db2458], [db2460]
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - shown_at).count();
    Xform base = x_identity();   // 584(1): alpha a; 904(1) = copy
    base.v[4] = a;
    Xform black = x_identity();  // 664(1): 0x421018(0xff000000)
    x_colour(black, 0xff000000u);
    // 1. full-screen fill (0x4156cc, type 0x1b, rect {0, 0, 1280, 720}), transform 624(1) = base o black
    item_fill(x_compose(base, black), 0, 0, 1280, 720);
    // (the gallery panels 0x4adbd0 x4 are not ported)
    // 2. edges, twice (0x4ae944, 0x4aea20): {0, 0, 1280, 720} and {-10, -10, 1290, 730}, colour a
    if (uint8_t *m = menu_net_material(sid("general/net/loading-screen/edge"))) {
        menu_draw_quad(m, 0, 0, 1280, 720, 0, 0, 1, 1, a, a);
        menu_draw_quad(m, -10, -10, 1290, 730, 0, 0, 1, 1, a, a);
    }
    // 3. rosters (0x4aeb60): alpha a * clamp(t - 3, 0, 1), y = 200 * (1 - clamp((t - 2) * 0.6667, 0, 1))^2
    const float slide = 1.0f - std::clamp((t - 2.0f) * 0.6666667f, 0.0f, 1.0f);
    draw_page(kPageLoading, a * std::clamp(t - 3.0f, 0.0f, 1.0f), 0, slide * slide * 200.0f);
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
    // (the tip line 0x4acc34 and the coin particles 0x4ad600 are not ported; the bars 0x415294 apply
    // only below 719 lines)
}

U2MP_GUEST uint64_t draw_2d(void *tables, uint64_t a, uint64_t b) {
    const uint64_t r = guest<uint64_t(U2MP_GUEST *)(void *, uint64_t, uint64_t)>(fDraw2D)(tables, a, b);
    const auto now = std::chrono::steady_clock::now();
    const float dt = have_frame ? std::min(std::chrono::duration<float>(now - last_frame).count(), 0.1f) : 0.0f;
    last_frame = now;
    have_frame = true;
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
    // the PS4 calls this port makes (prologue push rbp; mov rbp,rsp)
    static const unsigned char prologue[] = {0x55, 0x48, 0x89, 0xe5};
    for (uint64_t f : {fItemText, fItemDesc, fPageDraw, fGameText})
        if (!image.matches(f, prologue, sizeof prologue)) {
            log("loading screen: 0x%llx is not a function start, not installed", (unsigned long long)f);
            return;
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
    log("loading screen: PS3 port installed (%zu state hooks, %d chained; text %s, rosters %s)", kSlotCount, chained,
        with_text ? "on" : "off", with_rosters ? "on" : "off");
    ++installed;
}

}  // namespace u2mp
