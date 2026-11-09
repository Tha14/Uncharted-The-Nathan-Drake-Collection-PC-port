// u2mp loading: the multiplayer loading screen. Notes and the PS3 trace: u2mp/docs/PORT-FRONTEND4.md.
//
// PS3 1.10: the per-frame net display 0x447248 (from the frame draw 0x76c80, whose PS4 pair is Draw
// 0x3fc0b0) runs, while the byte 0xf119c5 is set, the loading screen object 0xf1ff04: 0x4adaf8
// (show / hide; on show 0x4ad600 starts the coin animation and 0x4233c4 resets the lists of
// PAGE_MULTIPLAYER_LOADING) and 0x4ae638 (draw: the band general/net/loading-screen/title-back with
// its edges .../edge, the net game's map picture (level record 0x491f38 +0xc), the map name and the
// game type (TextDb 0x9294c4), a tip, the team rosters of PAGE_MULTIPLAYER_LOADING drawn in place
// (0x423410), the money / coins animation .../money, .../coins). 0xf119c5 is set by LoadLevels::Enter
// (0x4405c4) and WaitReady::Enter (0x43bd28) and cleared by LoadLevels::Exit (0x438448) and
// WaitReady::Exit (0x443f00). PS4 lost all of it (none of these ids occurs in its code); its net.pak
// still has the four materials.
//
// PC: the same show / hide points on the PS4 TaskManagerOnline states (vtables 0x100ac00 LoadLevels,
// 0x100abc0 WaitReady: [dtor, Enter, Exit, Update]; session.cpp wraps only the Update slots), plus
// Playing::Enter and Leave::Enter as a safety off. The screen is drawn after the frame's 2D / menu
// pass in Draw (the call 0x3fc52d -> 0x554340), into the deferred 2D arena: textured quads with a
// NULL draw list (menuimages.cpp menu_draw_quad) and text through the menu renderer's own text call
// 0xbda9c0(arena 0x124a000, style, text). The style (0xa0 bytes: +0 x, +4 y, +0x10..+0x1c clip,
// +0x20/+0x24 scale, +0x28 colour, +0x30 font by language, +0x34 flags, +0x50.. transform, +0x94
// wrap width, +0x98 limit) is copied from the renderer's last text (0x5be615) and positioned here.
// Layout: PS3's elements at fixed places (the PS3 camera-like motion 0x4ad124 and the coin
// animation 0x4ad600 are not traced; the rosters are not drawn).
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

#include "frontend3.h"
#include "u2mp_internal.h"

namespace u2mp {
namespace {

constexpr uint64_t kArena = 0x124a000;
constexpr uint64_t fTextDraw = 0xbda9c0, cRendererText = 0x5be615;
constexpr uint64_t fDraw2D = 0x554340, cDraw2D = 0x3fc52d;
constexpr uint64_t fTextFind = 0xcddc00, kTextDb = 0x1066660, fDcLookup = 0xa15e70;
constexpr uint64_t kNgm = 0x113b760, NGM_GAME = 0x1ca0;
constexpr uint32_t kNetGames = 0x3b9a067d, kLevels = 0x0dec97a4, kGameTypes = 0xf985c7b6, kTips = 0xb25ab071;
constexpr size_t kStyleSize = 0xa0;
constexpr float kScreenW = 1920.0f, kScreenH = 1080.0f;

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }
template <typename T> T &F(uint8_t *p, size_t off) { return *reinterpret_cast<T *>(p + off); }

struct DcTable { int32_t count; int32_t pad; uint8_t *records; };
DcTable *dc(uint32_t id) { return guest<DcTable *(U2MP_GUEST *)(uint32_t)>(fDcLookup)(id); }
const char *text(uint32_t id) {
    if (!id) return nullptr;
    return guest<const char *(U2MP_GUEST *)(void *, uint32_t)>(fTextFind)(image.at<void>(kTextDb), id);
}

// ---- what is shown ----------------------------------------------------------------------------------
std::atomic<bool> showing{false};
std::chrono::steady_clock::time_point shown_at;
struct Screen {
    uint32_t map_image = 0;
    const char *map_name = nullptr, *game_type = nullptr, *tip = nullptr;
};
Screen screen;
std::mutex screen_mutex;
bool enabled = true, with_text = true;

void prepare() {   // the net game's map, name and game type, and a tip (PS3 0x4ae638 data)
    Screen s;
    const DcTable *games = dc(kNetGames), *levels = dc(kLevels), *types = dc(kGameTypes), *tips = dc(kTips);
    const int16_t game = G<int16_t>(kNgm + NGM_GAME);
    if (games && games->records && game >= 0 && game < games->count) {
        const uint8_t *g = games->records + 0x90 * size_t(game);
        const uint32_t level = *reinterpret_cast<const uint32_t *>(g + 4);
        const int32_t type = *reinterpret_cast<const int32_t *>(g + 0xc);
        for (int32_t i = 0; levels && levels->records && i < levels->count; ++i) {
            const uint8_t *l = levels->records + 0x38 * size_t(i);
            if (*reinterpret_cast<const uint32_t *>(l) != level) continue;
            s.map_name = text(*reinterpret_cast<const uint32_t *>(l + 4));   // as the vote rows 0x65b0c6
            s.map_image = *reinterpret_cast<const uint32_t *>(l + 0xc);
            break;
        }
        if (types && types->records && type >= 0 && type < types->count)
            s.game_type = text(*reinterpret_cast<const uint32_t *>(types->records + 0x44 * size_t(type)));
    }
    if (tips && tips->records && tips->count > 0)   // 0x14-byte records, text at +0xc (status 0x65acd9)
        s.tip = text(*reinterpret_cast<const uint32_t *>(tips->records + 0x14 * size_t(std::rand() % tips->count) + 0xc));
    std::lock_guard<std::mutex> lock(screen_mutex);
    screen = s;
}

void show(bool on, const char *why) {
    if (!enabled || showing.load() == on) return;
    if (on) {
        prepare();
        shown_at = std::chrono::steady_clock::now();
    }
    showing = on;
    log("loading screen: %s (%s)", on ? "shown" : "hidden", why);
}

// ---- text: the renderer's style, captured -----------------------------------------------------------
alignas(16) uint8_t style_template[kStyleSize];
std::atomic<bool> have_style{false};
std::mutex style_mutex;

U2MP_GUEST uint64_t renderer_text(void *arena, uint8_t *style, const char *str) {
    if (style && !have_style.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(style_mutex);
        std::memcpy(style_template, style, kStyleSize);
        have_style = true;
        log("loading screen: text style captured (font %08x, scale %.2f)", F<uint32_t>(style_template, 0x30),
            double(F<float>(style_template, 0x20)));
    }
    return guest<uint64_t(U2MP_GUEST *)(void *, uint8_t *, const char *)>(fTextDraw)(arena, style, str);
}

void draw_text(float x, float y, float scale, float wrap, const char *str) {
    if (!with_text || !str || !*str || !have_style.load()) return;
    alignas(16) uint8_t st[kStyleSize];
    {
        std::lock_guard<std::mutex> lock(style_mutex);
        std::memcpy(st, style_template, kStyleSize);
    }
    F<float>(st, 0x00) = x;
    F<float>(st, 0x04) = y;
    F<float>(st, 0x08) = 0;
    F<float>(st, 0x0c) = 0;
    F<float>(st, 0x10) = 0;          // clip: the whole screen
    F<float>(st, 0x14) = kScreenW;
    F<float>(st, 0x18) = 0;
    F<float>(st, 0x1c) = kScreenH;
    F<float>(st, 0x20) = scale;
    F<float>(st, 0x24) = scale;
    F<uint32_t>(st, 0x28) = 0xffffffffu;
    F<uint32_t>(st, 0x34) &= ~0x4u;  // "already drawn" (set after 0xbda9c0, 0x5be61a)
    F<float>(st, 0x94) = wrap;
    F<float>(st, 0x98) = 99999.0f;
    guest<uint64_t(U2MP_GUEST *)(void *, uint8_t *, const char *)>(fTextDraw)(image.at<void>(kArena), st, str);
}

// ---- the screen -------------------------------------------------------------------------------------
void draw_screen() {
    Screen s;
    {
        std::lock_guard<std::mutex> lock(screen_mutex);
        s = screen;
    }
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - shown_at).count();
    const float alpha = t < 0.3f ? t / 0.3f : 1.0f;
    // the map picture (PS3 type 0x22 crop u 0.0898..0.9102), the band, its edge, the coins
    if (uint8_t *m = menu_net_material(s.map_image)) menu_draw_quad(m, 320, 50, 1280, 700, 0.08984375f, 0, 0.91015625f, 1, alpha);
    if (uint8_t *m = menu_net_material(sid("general/net/loading-screen/title-back")))
        menu_draw_quad(m, 0, 740, kScreenW, 340, 0, 0, 1, 1, alpha);
    if (uint8_t *m = menu_net_material(sid("general/net/loading-screen/edge")))
        menu_draw_quad(m, 0, 730, kScreenW, 24, 0, 0, 1, 1, alpha);
    if (uint8_t *m = menu_net_material(sid("general/net/loading-screen/coins")))
        menu_draw_quad(m, 1560, 800, 240, 240, 0, 0, 1, 1, alpha);
    draw_text(360, 790, 1.4f, 1100, s.map_name);
    draw_text(360, 870, 1.0f, 1100, s.game_type);
    draw_text(360, 950, 0.8f, 1100, s.tip);
}

U2MP_GUEST uint64_t draw_2d(void *tables, uint64_t a, uint64_t b) {
    const uint64_t r = guest<uint64_t(U2MP_GUEST *)(void *, uint64_t, uint64_t)>(fDraw2D)(tables, a, b);
    if (showing.load(std::memory_order_relaxed)) draw_screen();
    return r;
}

// ---- show / hide: TaskManagerOnline state Enter / Exit ----------------------------------------------
using StateFn = uint64_t(U2MP_GUEST *)(void *);
struct Slot { uint64_t vtable_slot, fn; bool on; const char *why; };
constexpr Slot kSlots[] = {
    {0x100ac00 + 0x08, 0x5f2e50, true, "LoadLevels::Enter"},    // PS3 0x4405c4
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
    if (with_text && !retarget_call(cRendererText, fTextDraw, reinterpret_cast<void *>(&renderer_text), "menu text style capture"))
        with_text = false;
    static const auto fns = make_fns(std::make_index_sequence<kSlotCount>{});
    for (size_t i = 0; i < kSlotCount; ++i) {
        original[i] = reinterpret_cast<StateFn>(G<uint64_t>(kSlots[i].vtable_slot));   // PS4 function or a host hook
        G<uint64_t>(kSlots[i].vtable_slot) = uint64_t(uintptr_t(fns[i]));
    }
    log("loading screen: installed (%zu state hooks, %d chained to an earlier hook%s)", kSlotCount, chained,
        with_text ? ", text" : ", no text");
    ++installed;
}

}  // namespace u2mp
