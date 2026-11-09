// u2mp scoreboard: the in-match scoreboard (PAGE_MULTIPLAYER_SCOREBOARD) of the PS3 net player tracker.
//
// PS3 1.10: the tracker object 0xf2e150 holds four overlay entries of 20 bytes from +8 {+0 shown (u8),
// +4 fade, +8 fade speed, +0xc dim flag (u8), +0x10 page id}: 0 = PAGE_MULTIPLAYER_SCOREBOARD f650325d,
// 1 = ROUND_RESULTS 5dad5a68 (the co-op wave screen), 2 = RESULTS 3d437466, 3 = ROUND_RESULTS. The static
// init is 0x4cc778. NetUpdate 0x447248 calls, once per frame:
//   * 0x4cbf00 (update): with the NGM state (ngm +6796) 2 = playing, in a competitive game, a press
//     toggles entry 0 (0x4cc3dc..0x4cc444: speed 2.0, 0x4cbac0(obj, 0, !shown, 1)); a script request
//     (0x6b6b3c, symbol 1a9688ee) and the co-op "all players dead" test (0x494178) show it too; other
//     states hide the entries (0x4cbf98..). Then every entry fades: fade += dt * speed * (shown ? 1 : -1),
//     clamped to 0..1 (0x4cc0d0..0x4cc1cc, dt = frame ticks x 1/75).
//   * 0x4cbac0(obj, entry, show, ...): on a change, a sound (0x30fb28 / 0x7069c8, ids d9b465e1 shown /
//     2f118f31 hidden, entry 0 only) and, when shown, 0x4233c4(menu, page id) = page +216 = 2 (positions
//     set again on the next layout).
//   * 0x4cbc10 (draw): the topmost entry with a fade draws its page through 0x423410(menu, id, 0,
//     {0, y}) with alpha fade (x the HUD alpha in states 3..5), ease = shown ? fade^2 : 1 - (1 - fade)^2,
//     y = (1 - ease) * (shown ? 20 : -20) ([db2ffc] / [db300c]); the dim flag adds 0x421b00.
//
// PS4 1.02: the object is still there (0x104f544, static data, the same layout: page ids at +0x18 /
// +0x2c / +0x40 / +0x54, speed 1.0 at +0x10), and the wave-screen natives 0x668850 / 0x668890 still
// write entry 1. The update and the draw are gone with NetUpdate (u2mp/docs/functions-frontend-net.tsv:
// 0x447248 absent; 0x4cbf00 has no PS4 code, the pairing with 0x5e8e70 in functions-match-gameplay.tsv
// is a message handler that only shares the sid 8a87d1da), so nothing ever shows entry 0: no
// scoreboard in a match. This file ports the entry-0 part of 0x4cbf00 / 0x4cbac0 / 0x4cbc10 and runs
// it once per frame from the frontend 2D draw call 0x3fc52d (the call loading.cpp wraps; chained).
//
// The button. PS3 reads it from its own pad layer; PS4 replaced that with scePad bits (the pad poll
// 0x565dd2.. stores the raw OrbisPadData buttons at 0x14b7fb0) and SELECT became the touchpad
// (0x100000), which the PC runtime reports for the touchpad click, Back/View (right half) and Tab (left
// half). UNCHARTED_U2MP_SCOREBOARD_BUTTON=<hex mask> picks another scePad bit, 0 turns the port off.
//
// Not ported: the show / hide sounds (no PS4 counterpart of 0x30fb28 found), the script request
// (symbol 1a9688ee) and the co-op all-dead show (0x494178 has no PS4 code; DcNetAllPlayersDeadP 0x6694b0
// is a script native), the dim (0x421b00; entry 0 has no dim flag: 0x104f558 = 0).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
namespace {

constexpr uint64_t fDraw2D = 0x554340, cDraw2D = 0x3fc52d;   // the per-frame 2D draw (loading.cpp wraps it)
constexpr uint64_t fPageLayout = 0x5a2670;                    // PS3 0x408928 (page, desc)
constexpr uint64_t kItemFade = 0x1050e58, kFadeScale = 0x104ed74;
constexpr uint64_t MENU_PAGES = 0x370, MENU_PAGE_COUNT = 200, PAGE_ID = 0x10, MENU_CTX = 0x10, MENU_17A4 = 0x17a4;
constexpr uint64_t PAGE_FLAGS = 0xb0, PAGE_FADE = 0xf4, PAGE_SNAP = 0xf8;
constexpr uint64_t kFrameCounter = 0x1116180 + 0x104;
constexpr uint64_t kNgm = 0x113b760, NGM_STATE = 0x1e80;      // PS3 ngm +6796
constexpr uint64_t kNetState = 0x14ce97c;                     // NET_SM
constexpr uint64_t kPadButtons = 0x14b7fb0;                   // raw OrbisPadData.buttons (pad poll 0x565de6)
constexpr uint64_t kTracker = 0x104f544;                      // PS3 0xf2e150
constexpr uint64_t E0_SHOWN = 0x08, E0_FADE = 0x0c, E0_SPEED = 0x10, E0_PAGE = 0x18;
constexpr uint32_t kPageScoreboard = 0xf650325d;
static_assert(sid("PAGE_MULTIPLAYER_SCOREBOARD") == kPageScoreboard, "page id");

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }

using Draw2DFn = uint64_t(U2MP_GUEST *)(void *, uint64_t, uint64_t);
Draw2DFn previous_draw = nullptr;   // loading.cpp's wrapper (or 0x554340 itself)
uint32_t button_mask = 0x100000;    // scePad touchpad (PS3 SELECT)
uint32_t last_buttons = 0;
bool shown = false;
float fade = 0.0f, speed = 1.0f;
bool have_frame = false;
std::chrono::steady_clock::time_point last_frame;
std::atomic<bool> log_page_found{true};

uint8_t *find_page(uint32_t id) {   // menu +0x370[200], id +0x10 (as ShowPage 0x5cb9c0)
    const uint64_t menu = addr::menu_manager;
    for (uint64_t i = 0; i < MENU_PAGE_COUNT; ++i) {
        uint8_t *p = G<uint8_t *>(menu + MENU_PAGES + 8 * i);
        if (!p) break;
        if (*reinterpret_cast<const uint32_t *>(p + PAGE_ID) == id) return p;
    }
    return nullptr;
}

// PS3 0x4cbac0 for entry 0: on a change, 0x4233c4 (page +0xf8 = 2 on PS4) when shown.
void set_shown(bool on, const char *why) {
    if (on == shown) return;
    shown = on;
    G<uint8_t>(kTracker + E0_SHOWN) = on ? 1 : 0;
    if (on) {
        if (uint8_t *page = find_page(kPageScoreboard)) *reinterpret_cast<int32_t *>(page + PAGE_SNAP) = 2;
        if (log_page_found.exchange(false))
            log("scoreboard: page %08x %s", kPageScoreboard, find_page(kPageScoreboard) ? "found" : "NOT FOUND");
    }
    log("scoreboard: %s (%s)", on ? "shown" : "hidden", why);
}

// PS3 0x423410(menu, id, alpha, pos) -> the page update (vtable +0x10), layout 0x5a2670 and render
// (vtable +0x18), as loading.cpp's draw_page (see the notes there).
void draw_page(uint8_t *page, float alpha, float x, float y, float dt) {
    const uint64_t menu = addr::menu_manager;
    struct Update { void *menu; int32_t event; float time; } u{image.at<void>(menu), 0, dt};
    static_assert(offsetof(Update, event) == 8 && offsetof(Update, time) == 12, "update args");
    using UpdateFn = void(U2MP_GUEST *)(void *, Update *);
    const uint64_t *vt = *reinterpret_cast<const uint64_t *const *>(page);
    reinterpret_cast<UpdateFn>(vt[0x10 / 8])(page, &u);
    struct alignas(8) Desc {
        void *ctx;
        float x[10];
        float screen[4];
        float time;
        uint8_t state12;
        uint8_t pad[3];
        int32_t v17a4;
        int32_t pad2;
    } d{};
    static_assert(offsetof(Desc, screen) == 0x30 && offsetof(Desc, time) == 0x40 && offsetof(Desc, v17a4) == 0x48, "desc");
    d.ctx = image.at<void>(menu + MENU_CTX);
    d.x[0] = d.x[1] = d.x[2] = d.x[3] = d.x[7] = 1.0f;   // identity (PS3 0x420f58)
    d.x[4] = alpha;
    d.x[8] = x;
    d.x[9] = y;
    d.screen[2] = 1280.0f;
    d.screen[3] = 720.0f;
    d.time = float(G<int32_t>(kFrameCounter)) * (1.0f / 300.0f);
    d.state12 = 1;
    d.v17a4 = G<int32_t>(menu + MENU_17A4);
    const uint32_t flags = *reinterpret_cast<const uint32_t *>(page + PAGE_FLAGS);
    if (flags & 0x80000) d.x[0] *= 0.75f;
    float &item_fade = G<float>(kItemFade);
    const float saved_fade = item_fade;
    item_fade = (1.0f - *reinterpret_cast<const float *>(page + PAGE_FADE)) * ((flags & 0x800) ? 1.0f : G<float>(kFadeScale));
    guest<void(U2MP_GUEST *)(void *, void *)>(fPageLayout)(page, &d);
    using RenderFn = void(U2MP_GUEST *)(void *, void *, void *);
    reinterpret_cast<RenderFn>(vt[0x18 / 8])(page, nullptr, &d);
    item_fade = saved_fade;
}

// PS3 0x4cbf00 (entry 0) and 0x4cbc10, once per frame.
void update_and_draw(float dt) {
    const bool playing = G<int32_t>(kNetState) > 1 && G<int32_t>(kNgm + NGM_STATE) == 2;
    const uint32_t buttons = G<uint32_t>(kPadButtons);
    const bool pressed = (buttons & ~last_buttons & button_mask) != 0;
    last_buttons = buttons;
    if (!playing) {
        set_shown(false, "not playing");   // 0x4cbf98: other states hide the entries
    } else if (pressed) {
        speed = 2.0f;                      // 0x4cc414: 2.0 at +0x10
        G<float>(kTracker + E0_SPEED) = speed;
        set_shown(!shown, "button");
    }
    // the fade (0x4cc0d0..0x4cc130): dt x speed, up while shown, down after, 0..1
    fade = std::clamp(fade + (shown ? dt : -dt) * speed, 0.0f, 1.0f);
    G<float>(kTracker + E0_FADE) = fade;
    if (fade <= 0.0f) return;
    uint8_t *page = find_page(kPageScoreboard);
    if (!page) return;
    // the draw (0x4cbe80..0x4cbed4): alpha fade, slide of 20 units down in / up out
    const float ease = shown ? fade * fade : 1.0f - (1.0f - fade) * (1.0f - fade);
    draw_page(page, fade, 0.0f, (1.0f - ease) * (shown ? 20.0f : -20.0f), dt);
}

U2MP_GUEST uint64_t draw_2d(void *tables, uint64_t a, uint64_t b) {
    const uint64_t r = previous_draw(tables, a, b);
    const auto now = std::chrono::steady_clock::now();
    const float dt = have_frame ? std::min(std::chrono::duration<float>(now - last_frame).count(), 0.1f) : 0.0f;
    last_frame = now;
    have_frame = true;
    update_and_draw(dt);
    return r;
}

}  // namespace

void install_scoreboard(int &installed) {
    if (const char *v = std::getenv("UNCHARTED_U2MP_SCOREBOARD_BUTTON")) {
        button_mask = uint32_t(std::strtoul(v, nullptr, 16));
        if (!button_mask) {
            log("scoreboard: off (UNCHARTED_U2MP_SCOREBOARD_BUTTON=0)");
            return;
        }
    }
    static const unsigned char prologue[] = {0x55, 0x48, 0x89, 0xe5};
    if (!image.matches(fPageLayout, prologue, sizeof prologue) || !image.matches(fDraw2D, prologue, sizeof prologue)) {
        log("scoreboard: page layout / 2D draw not as expected, not installed");
        return;
    }
    // the tracker object: entry 0's page id and speed as the PS3 init 0x4cc778 sets them
    if (G<uint32_t>(kTracker + E0_PAGE) != kPageScoreboard || G<float>(kTracker + E0_SPEED) != 1.0f) {
        log("scoreboard: tracker 0x%llx is not as expected, not installed", (unsigned long long)kTracker);
        return;
    }
    // Chain the call 0x3fc52d: it calls 0x554340, or a hook installed earlier (loading.cpp).
    if (!image.executable(cDraw2D, 5) || image.base[cDraw2D] != 0xe8) {
        log("scoreboard: 0x%llx is not a call, not installed", (unsigned long long)cDraw2D);
        return;
    }
    int32_t rel = 0;
    std::memcpy(&rel, image.base + cDraw2D + 1, 4);
    const unsigned char *current = image.base + cDraw2D + 5 + rel;
    const uint64_t current_offset = uint64_t(uintptr_t(current)) - uint64_t(uintptr_t(image.base));
    previous_draw = reinterpret_cast<Draw2DFn>(const_cast<unsigned char *>(current));
    if (!retarget_call(cDraw2D, current_offset, reinterpret_cast<void *>(&draw_2d),
                       current_offset == fDraw2D ? "scoreboard: 2D draw" : "scoreboard: 2D draw (chained)"))
        return;
    ++installed;
    log("scoreboard: PAGE_MULTIPLAYER_SCOREBOARD port installed (button mask 0x%x; PS3 0x4cbf00 / 0x4cbc10)",
        button_mask);
}

}  // namespace u2mp
