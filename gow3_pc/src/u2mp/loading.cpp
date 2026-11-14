// u2mp loading: the multiplayer match loading screen.
//
// The PS4 build of Uncharted 2 dropped the screen the PS3 game shows while a match loads. This file
// draws it again in the PS4 build: a black screen, a slowly drifting table of picture cards seen
// through a slight perspective, edge overlays, the team rosters sliding in, a title band with the
// map name and game description, and a tip line whose letters fade in and out one after another.
// Six TaskManagerOnline state functions ask for the screen to be shown or hidden; the frame's HUD
// update call is redirected here so the screen is drawn once per frame on top of the menu.
//
// Reimplemented from the behaviour description u2mp/docs/spec/loading.md.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "frontend3.h"
#include "u2mp_internal.h"

// The float results must match the game's arithmetic bit for bit: no fused multiply-add.
#pragma STDC FP_CONTRACT OFF

namespace u2mp {
namespace {

// ---------------------------------------------------------------------------------------------
// Game addresses (image offsets).

constexpr uint64_t kMenu = addr::menu_manager;  // menu manager
constexpr uint64_t kDrawCtx = kMenu + 0x10;     // draw context the menu item calls take

constexpr uint64_t fHudUpdate = 0x554340;
constexpr uint64_t fDcLookup = 0xa15e70;
constexpr uint64_t fTextLookup = 0xcddc00;
constexpr uint64_t fGameDescription = 0x64abd0;
constexpr uint64_t fItemText = 0x5b5450;
constexpr uint64_t fItemDesc = 0x5b5920;
constexpr uint64_t fPageLayout = 0x5a2670;
constexpr uint64_t fSlotQuad = 0x5b4e90;
constexpr uint64_t fMaterialQuad = 0xbdce90;
constexpr uint64_t fPrintText = 0xcc9a70;
constexpr uint64_t fGlyphDraw = 0xcc7cc0;
constexpr uint64_t fQueueText = 0xbda9c0;

constexpr uint64_t kDrawCall = 0x3fc52d;

constexpr uint64_t kNetGameManager = 0x113b760;
constexpr uint64_t kNetGameIndex = kNetGameManager + 0x1ca0;
constexpr uint64_t kNetGameOverrides = kNetGameManager + 0x21b4;
constexpr uint64_t kTextDatabase = 0x1066660;
constexpr uint64_t kDataRoot = 0x104ad28;
constexpr uint64_t kFrameCounter = 0x1116180 + 0x104;
constexpr uint64_t kItemFade = 0x1050e58;
constexpr uint64_t kFadeScale = 0x104ed74;
constexpr uint64_t kFontOne = 0x144e640 + 8;
constexpr uint64_t kTextArena = 0x124a000;
constexpr uint64_t kTeamListVtable = 0x1007910;

constexpr uint64_t kPageList = kMenu + 0x370;
constexpr uint64_t kMenuValue = kMenu + 0x17a4;
constexpr uint64_t kGalleryState = kMenu + 0x120;
constexpr uint64_t kGalleryLoaded = kMenu + 0x138;
constexpr uint64_t kGalleryPath = kMenu + 0x268;
constexpr uint64_t kGalleryKeepAlive = kMenu + 0x368;

// DC tables and ids.
constexpr uint32_t kNetGamesTable = 0x3b9a067d;
constexpr uint32_t kLevelsTable = 0x0dec97a4;
constexpr uint32_t kTipsTable = 0x7f306129;
constexpr uint32_t kRosterPage = 0xb9df67d6;
static_assert(sid("PAGE_MULTIPLAYER_LOADING") == kRosterPage, "roster page id");
constexpr uint32_t kEdgeImage = 0x6732636e;
constexpr uint32_t kBandImage = 0x270e3564;
constexpr uint32_t kMoneyImage = 0x9595f0a7;
constexpr uint32_t kCoinsImage = 0xb4b036fa;

// Picture ids are derived from their names so no id table is carried around.
constexpr const char *kPlayerPhotoNames[16] = {
    "drake",      "elena",      "chloe",      "sullivan",  "rescuer",    "soldier-1a", "soldier-1b", "soldier-2a",
    "soldier-2b", "soldier-3a", "lazarevic",  "lieutenant", "merc",      "jeff",       "schafer",    "flynn"};
constexpr const char *kMapPhotoNames[9] = {"warzone", "warzone-coop", "shambhala", "gompa", "treasure",
                                           "ice-cave", "village", "village-coop", "train-wreck"};

constexpr uint32_t joined_sid(const char *prefix, const char *name) {
    // sid() continues a CRC, so hashing the prefix and then the name equals hashing the joined path.
    return sid(name, sid(prefix));
}
static_assert(joined_sid("general/net/player-photos/", "drake") == 0x05a8ba5f, "player photo id");
static_assert(joined_sid("general/net/map-photos/", "train-wreck") == 0x9d998d63, "map photo id");

uint32_t player_photo_id(uint32_t k) { return joined_sid("general/net/player-photos/", kPlayerPhotoNames[k]); }
uint32_t map_photo_id(uint32_t k) { return joined_sid("general/net/map-photos/", kMapPhotoNames[k]); }

// Float constants, checked against the bit patterns the game uses.
constexpr float kMoneySpread = 0.78539819f;
constexpr float kPhotoSpread = 0.94247788f;
constexpr float kCameraPhaseStep = 2.0923009f;
constexpr float kCamera0Turn = 0.20943953f;
constexpr float kCameraTilt = 0.15707964f;
constexpr float kHalfFov = 0.024674013f;
constexpr float kFadeInRemap = 1.6666666f;
constexpr float kRosterSlide = 0.6666667f;
constexpr float kGlyphTimeScale = 0.013333334f;
constexpr float kTipDurationScale = 0.0066666668f;
constexpr float kPi = 3.14159274f;
constexpr float kRandomScale = 2.3283061589829401e-10f;
static_assert(std::bit_cast<uint32_t>(kMoneySpread) == 0x3f490fdb);
static_assert(std::bit_cast<uint32_t>(kPhotoSpread) == 0x3f71463b);
static_assert(std::bit_cast<uint32_t>(0.1f) == 0x3dcccccd);
static_assert(std::bit_cast<uint32_t>(kCameraPhaseStep) == 0x4005e842);
static_assert(std::bit_cast<uint32_t>(kCamera0Turn) == 0x3e567751);
static_assert(std::bit_cast<uint32_t>(kCameraTilt) == 0x3e20d97c);
static_assert(std::bit_cast<uint32_t>(kHalfFov) == 0x3cca2128);
static_assert(std::bit_cast<uint32_t>(kFadeInRemap) == 0x3fd55555);
static_assert(std::bit_cast<uint32_t>(kRosterSlide) == 0x3f2aaaab);
static_assert(std::bit_cast<uint32_t>(kGlyphTimeScale) == 0x3c5a740e);
static_assert(std::bit_cast<uint32_t>(kTipDurationScale) == 0x3bda740e);
static_assert(std::bit_cast<uint32_t>(kPi) == 0x40490fdb);
static_assert(std::bit_cast<uint32_t>(kRandomScale) == 0x2f7ffffe);

// Perspective of the card table, derived once at load.
const float kProjFocal = 1.0f / std::tan(kHalfFov);
const float kTiltSin = std::sin(0.01f);
const float kTiltCos = std::cos(0.01f);

// ---------------------------------------------------------------------------------------------
// Small helpers for guest memory and guest calls.

template <typename T> T peek(uint64_t offset) {
    T v;
    std::memcpy(&v, image.base + offset, sizeof v);
    return v;
}
template <typename T> void poke(uint64_t offset, T v) { std::memcpy(image.base + offset, &v, sizeof v); }
template <typename T> T peek_abs(const uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <typename T> void poke_abs(uint8_t *p, T v) { std::memcpy(p, &v, sizeof v); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }
template <typename F> F guest_abs(uint64_t address) { return reinterpret_cast<F>(uintptr_t(address)); }
uint64_t absolute(uint64_t offset) { return uint64_t(uintptr_t(image.base)) + offset; }

// float -> int32 as the x86 truncating conversion does it: out-of-range and NaN give INT32_MIN.
int32_t trunc_i32(float v) {
    if (!(v >= -2147483648.0f && v < 2147483648.0f)) return INT32_MIN;
    return int32_t(v);
}

// A DC table: {s32 count; s32 pad; u8 *records}.
struct DcTable {
    int32_t count;
    int32_t pad;
    uint8_t *records;
};
const DcTable *dc_table(uint32_t id) {
    return static_cast<const DcTable *>(guest<void *(U2MP_GUEST *)(uint32_t)>(fDcLookup)(id));
}
// Text by id; id 0 means "no text" and is not asked of the game.
const char *text_by_id(uint32_t id) {
    if (id == 0) return nullptr;
    return guest<const char *(U2MP_GUEST *)(void *, uint32_t)>(fTextLookup)(image.base + kTextDatabase, id);
}

// ---------------------------------------------------------------------------------------------
// Clocks.

using Clock = std::chrono::steady_clock;
// The tip tick base is taken at load, not at install, so ticks count from process start.
const Clock::time_point g_tick_base = Clock::now();

float seconds_since(Clock::time_point since, Clock::time_point now) {
    return std::chrono::duration<float>(now - since).count();
}
// 300 ticks per second, the PS3 net clock rate the tip timing was written for.
int32_t ticks() {
    return int32_t(std::chrono::duration<double>(Clock::now() - g_tick_base).count() * 300.0);
}

// ---------------------------------------------------------------------------------------------
// Switches and module state.

bool g_enabled = true;
bool g_text_on = true, g_rosters_on = true, g_cards_on = true, g_perspective_on = true, g_tip_on = true;
bool g_cards_ready = false, g_tip_ready = false;
bool cards_ok() { return g_cards_on && g_cards_ready; }

enum Request : int { kNone = 0, kShow = 1, kHide = 2 };
std::atomic<int> g_pending{kNone};
std::atomic<bool> g_shown{false};
std::atomic<const char *> g_reason{""};

float g_fade = 0.0f;
Clock::time_point g_shown_at{};
bool g_have_previous_frame = false;
Clock::time_point g_previous_frame{};

// The map name and description are written by the show path and read by the draw.
std::mutex g_text_lock;
const char *g_map_name = nullptr;
alignas(16) char g_description[128] = {};

// Cards, their start time and the random generator share one lock (the tip borrows it for its
// single random step).
std::mutex g_card_lock;

struct Card {
    float x, y, angle;
    uint8_t *material;
    int type;  // 0 own material, 1 gallery picture (slot 8), 2 solid colour (slot 10)
    float w, h;
    int orientation;  // 0..3 quarter of a 2x2 sheet, -1 whole picture
    float r, g, b;
};
struct CardSystem {
    int count = 0;
    Card cards[16];
};
CardSystem g_systems[4];
bool g_cards_started = false;
float g_card_seed = 0.0f;
Clock::time_point g_card_start{};
int g_card_log_budget = 8;

// Multiply-with-carry generator, one process-wide state, never reseeded.
uint32_t g_rng_x[4] = {0x2545f491u, 0x9e3779b9u, 0x7f4a7c15u, 0x6a09e667u};
uint32_t g_rng_carry = 0x01234567u;

// Callers hold g_card_lock.
uint32_t rng_step() {
    const uint64_t sum = 2111111111ull * g_rng_x[3] + 1492ull * g_rng_x[2] + 1776ull * g_rng_x[1] +
                         5115ull * g_rng_x[0] + g_rng_carry;
    g_rng_x[3] = g_rng_x[2];
    g_rng_x[2] = g_rng_x[1];
    g_rng_x[1] = g_rng_x[0];
    g_rng_x[0] = uint32_t(sum);
    g_rng_carry = uint32_t(sum >> 32);
    return g_rng_x[0];
}
float rng_unit() { return float(rng_step()) * kRandomScale; }

// Tip state. The glyph callback reads it while the game prints the tip.
int32_t g_tip_index = 0;
int32_t g_tip_time = 0;
int32_t g_tip_length = 0;
int32_t g_glyph_counter = 0;

// Roster page logging: once on the first frame after show, once when fully visible.
bool g_log_page_first = false;
bool g_log_page_visible = false;

// ---------------------------------------------------------------------------------------------
// Colour transforms (10 floats: scale, rgba multipliers, two adds, multiplier, translation).

struct Xform {
    float v[10];
};
Xform identity_xform() {
    Xform t{};
    t.v[0] = t.v[1] = t.v[2] = t.v[3] = t.v[4] = t.v[7] = 1.0f;
    return t;
}
void set_colour(Xform &t, uint32_t argb) {
    const float k = 1.0f / 255.0f;
    t.v[1] = float((argb >> 16) & 0xff) * k;
    t.v[2] = float((argb >> 8) & 0xff) * k;
    t.v[3] = float(argb & 0xff) * k;
    t.v[4] = float(argb >> 24) * k;
}
Xform compose(const Xform &d, const Xform &s) {
    Xform out{};
    for (int i : {0, 1, 2, 3, 4, 7}) out.v[i] = d.v[i] * s.v[i];
    for (int i : {5, 6, 8, 9}) out.v[i] = d.v[i] + s.v[i];
    return out;
}

// Text inside a rect through the menu's own text item.
void draw_text(const Xform &t, const char *text, float x, float y, float w, float h) {
    if (!g_text_on || !text || !*text) return;
    alignas(16) float xf[10];
    std::memcpy(xf, t.v, sizeof xf);
    alignas(16) float rect[4] = {x, y, w, h};
    alignas(16) float clip[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    using Fn = void(U2MP_GUEST *)(void *, void *, float *, const char *, float *, int, int, int, float *, int, int,
                                  int, int);
    guest<Fn>(fItemText)(image.base + kDrawCtx, nullptr, xf, text, rect, 1, 1, 0, clip, 0, 0, 0, -1);
}

// A solid rectangle through the menu's descriptor item.
void draw_fill(const Xform &t, float x, float y, float w, float h) {
    alignas(16) uint8_t desc[0x30] = {};
    poke_abs<uint32_t>(desc + 0x00, 0x1b);
    poke_abs<int32_t>(desc + 0x1c, -1);
    poke_abs<int32_t>(desc + 0x24, -1);
    poke_abs<float>(desc + 0x28, 1.0f);
    alignas(16) float xf[10];
    std::memcpy(xf, t.v, sizeof xf);
    alignas(16) float rect[4] = {x, y, w, h};
    using Fn = void(U2MP_GUEST *)(void *, void *, float *, uint8_t *, float *, int, int);
    guest<Fn>(fItemDesc)(image.base + kDrawCtx, nullptr, xf, desc, rect, 1, 1);
}

// ---------------------------------------------------------------------------------------------
// Text preparation (on show).

void prepare_texts() {
    const DcTable *games = dc_table(kNetGamesTable);
    const DcTable *levels = dc_table(kLevelsTable);
    const int16_t index = peek<int16_t>(kNetGameIndex);

    std::lock_guard<std::mutex> lock(g_text_lock);
    g_map_name = nullptr;
    g_description[0] = 0;
    if (!games || !games->records || index < 0 || index >= games->count) return;

    uint8_t *record = games->records + 0x90 * index;
    const uint32_t level_id = peek_abs<uint32_t>(record + 4);
    if (levels && levels->records) {
        // Every match is looked up; the last one stays.
        for (int32_t i = 0; i < levels->count; ++i) {
            const uint8_t *level = levels->records + 0x38 * i;
            if (peek_abs<uint32_t>(level) == level_id) g_map_name = text_by_id(peek_abs<uint32_t>(level + 4));
        }
    }
    using Fn = void(U2MP_GUEST *)(void *, char *, int, uint8_t *, void *, int, int, int);
    guest<Fn>(fGameDescription)(image.base + kNetGameManager, g_description, 128, record,
                                image.base + kNetGameOverrides, 0, 1, 1);
    g_description[127] = 0;
}

// ---------------------------------------------------------------------------------------------
// Roster page.

uint8_t *find_page(uint32_t id) {
    for (int i = 0; i < 200; ++i) {
        uint8_t *page = reinterpret_cast<uint8_t *>(peek<uint64_t>(kPageList + 8 * uint64_t(i)));
        if (!page) break;
        if (peek_abs<uint32_t>(page + 0x10) == id) return page;
    }
    return nullptr;
}

void refresh_rosters() {
    uint8_t *page = find_page(kRosterPage);
    // Snap the items into place for the next layouts instead of animating them in from wherever.
    if (page) poke_abs<int32_t>(page + 0xf8, 2);
    log("loading screen: rosters page %08x %s%s", kRosterPage, page ? "found, refreshed (+0xf8 = 2)" : "NOT FOUND",
        g_rosters_on ? "" : " (rosters off)");
    g_log_page_first = true;
    g_log_page_visible = true;
}

void log_page(uint8_t *page, const char *when) {
    const int32_t n = peek_abs<int32_t>(page + 0xb4);
    uint8_t *items = reinterpret_cast<uint8_t *>(peek_abs<uint64_t>(page + 0xb8));
    log("loading screen: rosters page %s: %d items", when, n);
    if (!items) return;
    for (int32_t i = 0; i < n && i < 8; ++i) {
        uint8_t *item = reinterpret_cast<uint8_t *>(peek_abs<uint64_t>(items + 8 * i));
        if (!item) continue;
        const uint64_t vtable = peek_abs<uint64_t>(item);
        if (vtable != absolute(kTeamListVtable)) {
            log("loading screen:   item %d vtable 0x%llx", i, (unsigned long long)(vtable - absolute(0)));
            continue;
        }
        uint8_t *list = reinterpret_cast<uint8_t *>(peek_abs<uint64_t>(item + 0x18));
        int32_t current = -9, first = -9, last = -9;
        if (list) {
            using Getter = int32_t(U2MP_GUEST *)(uint8_t *);
            const uint8_t *lvt = reinterpret_cast<const uint8_t *>(peek_abs<uint64_t>(list));
            current = guest_abs<Getter>(peek_abs<uint64_t>(lvt + 0x00))(list);
            first = guest_abs<Getter>(peek_abs<uint64_t>(lvt + 0x30))(list);
            last = guest_abs<Getter>(peek_abs<uint64_t>(lvt + 0x38))(list);
        }
        log("loading screen:   item %d: team list (cond %d), list %p: rows %d..%d (current %d)", i,
            peek_abs<int32_t>(item + 0x10), static_cast<void *>(list), first, last, current);
    }
}

void draw_rosters(float alpha, float x, float y, float dt) {
    if (!g_rosters_on) return;
    uint8_t *page = find_page(kRosterPage);
    if (!page) return;

    const uint64_t vtable = peek_abs<uint64_t>(page);
    const uint64_t update_fn = peek_abs<uint64_t>(reinterpret_cast<uint8_t *>(vtable + 0x10));

    alignas(8) uint8_t update_args[16] = {};
    poke_abs<uint64_t>(update_args + 0, absolute(kMenu));
    poke_abs<int32_t>(update_args + 8, 0);
    poke_abs<float>(update_args + 12, dt);
    guest_abs<void(U2MP_GUEST *)(uint8_t *, uint8_t *)>(update_fn)(page, update_args);

    // The page desc: where and how the page's items are drawn this frame.
    const uint32_t flags = peek_abs<uint32_t>(page + 0xb0);
    alignas(8) uint8_t desc[0x50] = {};
    poke_abs<uint64_t>(desc + 0x00, absolute(kDrawCtx));
    Xform t = identity_xform();
    t.v[4] = alpha;
    t.v[8] = x;
    t.v[9] = y;
    if (flags & 0x80000) t.v[0] = t.v[0] * 0.75f;
    std::memcpy(desc + 0x08, t.v, sizeof t.v);
    const float viewport[4] = {0.0f, 0.0f, 1280.0f, 720.0f};
    std::memcpy(desc + 0x30, viewport, sizeof viewport);
    poke_abs<float>(desc + 0x40, float(peek<int32_t>(kFrameCounter)) * (1.0f / 300.0f));
    desc[0x44] = 1;
    poke_abs<int32_t>(desc + 0x48, peek<int32_t>(kMenuValue));

    // The items read their fade from a global; give them this page's fade for the draw only.
    const float saved_fade = peek<float>(kItemFade);
    const float scale = (flags & 0x800) ? 1.0f : peek<float>(kFadeScale);
    poke<float>(kItemFade, (1.0f - peek_abs<float>(page + 0xf4)) * scale);

    guest<void(U2MP_GUEST *)(uint8_t *, uint8_t *)>(fPageLayout)(page, desc);
    // The render entry is read only now: the update or layout may have replaced it.
    const uint64_t render_fn = peek_abs<uint64_t>(reinterpret_cast<uint8_t *>(vtable + 0x18));
    guest_abs<void(U2MP_GUEST *)(uint8_t *, void *, uint8_t *)>(render_fn)(page, nullptr, desc);

    poke<float>(kItemFade, saved_fade);

    if (g_log_page_first) {
        g_log_page_first = false;
        log_page(page, "updated and drawn (first frame)");
    }
    if (alpha >= 1.0f && g_log_page_visible) {
        g_log_page_visible = false;
        log_page(page, "updated and drawn (visible)");
    }
}

// ---------------------------------------------------------------------------------------------
// Gallery picture request (the big fort picture behind the cards).

void request_gallery() {
    const DcTable *games = dc_table(kNetGamesTable);
    const int16_t index = peek<int16_t>(kNetGameIndex);
    if (!games || index < 0 || index >= games->count) return;
    const uint64_t root = peek<uint64_t>(kDataRoot);
    if (!root) return;

    char path[512];
    std::snprintf(path, sizeof path, "%s/build/main/gallery1/%s", reinterpret_cast<const char *>(root + 0x408),
                  "loading-screens/fort.dds");
    // Keeps the menu's gallery slot from being released while we use it.
    poke<int32_t>(kGalleryKeepAlive, 10);

    char *slot_path = reinterpret_cast<char *>(image.base + kGalleryPath);
    if (std::strcmp(slot_path, path) != 0) {
        std::strncpy(slot_path, path, 0x100);
        slot_path[0xff] = 0;
        if (peek<int32_t>(kGalleryState) == 0) poke<int32_t>(kGalleryState, 1);
    }
}

// ---------------------------------------------------------------------------------------------
// Cards.

void spawn_card(CardSystem &sys, uint8_t *material, float w, float h, int orientation, float spread) {
    if (sys.count > 15) return;
    const float r1 = rng_unit();
    const float r2 = rng_unit();
    const float r3 = rng_unit();
    const float r4 = rng_unit();
    const float r5 = rng_unit();
    const float r6 = rng_unit();
    Card &c = sys.cards[sys.count++];
    c.x = r1 * 1180.0f + 50.0f;
    c.y = r2 * 620.0f + 50.0f;
    c.angle = spread * -0.5f + spread * r3;
    c.material = material;
    c.type = 0;
    c.w = w;
    c.h = h;
    c.orientation = orientation;
    c.r = r4 * 0.2f + 0.8f;
    c.g = r5 * 0.2f + 0.8f;
    c.b = r6 * 0.2f + 0.8f;
}

void add_plain_card(CardSystem &sys, int type, float x, float y, float w, float h, float rgb) {
    Card &c = sys.cards[sys.count++];
    c = Card{x, y, 0.0f, nullptr, type, w, h, -1, rgb, rgb, rgb};
}

// Black bands around the table, so cards drifting past the screen edge are hidden by the camera too.
void add_frame(CardSystem &sys) {
    if (sys.count > 12) return;
    add_plain_card(sys, 2, 640.0f, -500.0f, 5280.0f, 1000.0f, 0.0f);
    add_plain_card(sys, 2, -1000.0f, 360.0f, 2000.0f, 720.0f, 0.0f);
    add_plain_card(sys, 2, 640.0f, 1220.0f, 5280.0f, 1000.0f, 0.0f);
    add_plain_card(sys, 2, 2280.0f, 360.0f, 2000.0f, 720.0f, 0.0f);
}

void start_cards() {
    std::lock_guard<std::mutex> lock(g_card_lock);
    g_card_start = Clock::now();
    const float r = rng_unit();
    g_card_seed = r + r;
    for (CardSystem &sys : g_systems) sys.count = 0;

    CardSystem &a = g_systems[0];
    add_plain_card(a, 1, 640.0f, 360.0f, 1280.0f, 720.0f, 1.0f);
    add_frame(a);
    spawn_card(a, menu_net_material(kMoneyImage), 512.0f, 512.0f, -1, kMoneySpread);

    // Photos are picked without repeats where possible; after 100 draws the last one is taken anyway.
    CardSystem &b = g_systems[1];
    bool used[16] = {};
    for (int i = 0; i < 10; ++i) {
        uint32_t k = 0;
        for (int draw = 0; draw < 100; ++draw) {
            k = rng_step() & 15u;
            if (!used[k]) break;
        }
        used[k] = true;
        spawn_card(b, menu_net_material(player_photo_id(k)), 256.0f, 256.0f, -1, kPhotoSpread);
    }
    for (int i = 0; i < 9; ++i) used[i] = false;
    for (int i = 0; i < 4; ++i) {
        uint32_t k = 0;
        for (int draw = 0; draw < 100; ++draw) {
            k = rng_step() % 9u;
            if (!used[k]) break;
        }
        used[k] = true;
        spawn_card(b, menu_net_material(map_photo_id(k)), 512.0f, 256.0f, -1, kPhotoSpread);
    }

    CardSystem &c = g_systems[2];
    uint8_t *coins = menu_net_material(kCoinsImage);
    for (int i = 0; i < 12; ++i) spawn_card(c, coins, 100.0f, 100.0f, i & 3, 0.1f);

    g_cards_started = true;
}

float smoothstep(float x, float e0, float e1) {
    if (x < e0) return 0.0f;
    if (x > e1) return 1.0f;
    const float tt = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return (tt * tt) * 3.0f + ((tt * tt) * tt) * -2.0f;
}
// Cross-fade window between the two cameras over one 0..1 phase.
float camera_window(float x) { return smoothstep(x, 0.5f, 0.5f + 0.1f) - smoothstep(x, 1.0f - 0.1f, 1.0f); }

struct Camera {
    float x, y, zoom, angle, alpha;
};

Camera camera_at(float t, int idx) {
    Camera cam{};
    const float ph = std::fmod(t * 0.07f, 1.0f);
    const float k = float(idx) * kCameraPhaseStep;
    const float b = k * 0.75f + (t + g_card_seed);
    const float w = camera_window(ph);
    float px, py;
    if (idx == 0) {
        cam.alpha = 1.0f - w;
        const float ang = b * kCamera0Turn;
        px = std::cos(ang);
        py = std::sin(ang);
    } else {
        cam.alpha = w;
        const float ang = b * 0.15f;
        const float amp = (1.0f - std::cos(ang + ang)) * 0.25f + 0.5f;
        px = amp * std::sin(ang);
        py = amp * std::cos(ang * 0.5f);
    }
    cam.x = px * 250.0f;
    cam.y = py * 150.0f;
    const float cz = float(std::cos(double(t) * 0.1 + double(k)));
    const float s = std::clamp(cz * 0.5f + 0.5f, 0.0f, 1.0f);
    cam.zoom = s * 1.25f + (1.0f - s);
    cam.angle = std::sin(t * 0.1f + k) * kCameraTilt;
    return cam;
}

struct Point {
    float x, y;
};
struct ClipPoint {
    float x, y, u, v, d;
};

float card_depth(float y) { return g_perspective_on ? (360.0f - y) * kTiltSin + 30.0f : 1.0f; }

Point project(Point p) {
    if (!g_perspective_on) return p;
    const float xc = p.x - 640.0f;
    const float yc = 360.0f - p.y;
    const float w = yc * kTiltSin + 30.0f;
    return Point{640.0f + (kProjFocal * xc) / w, 360.0f - ((kProjFocal * yc) * kTiltCos) / w};
}

void submit_quad(const Card &card, const Point screen[4], const float u[4], const float v[4], float alpha) {
    if (card.type == 0 && !card.material) return;
    alignas(16) uint8_t quad[0x110] = {};
    const float second = card.type == 0 ? 0.0f : 1.0f;
    for (int i = 0; i < 4; ++i) {
        uint8_t *vert = quad + 0x40 * i;
        const float pos[5] = {screen[i].x, screen[i].y, 0.0f, u[i], v[i]};
        std::memcpy(vert, pos, sizeof pos);
        const float colour[8] = {card.r, card.g, card.b, alpha, second, second, second, second};
        std::memcpy(vert + 0x20, colour, sizeof colour);
    }
    if (card.type == 0) {
        alignas(16) uint8_t ctx48[48] = {};
        alignas(16) uint64_t info[4] = {uint64_t(uintptr_t(card.material)), 0, 0, 0};
        guest<void(U2MP_GUEST *)(uint8_t *, uint64_t *, uint8_t *)>(fMaterialQuad)(ctx48, info, quad);
    } else {
        guest<void(U2MP_GUEST *)(void *, void *, uint8_t *, int)>(fSlotQuad)(image.base + kDrawCtx, nullptr, quad,
                                                                             card.type == 1 ? 8 : 10);
    }
}

void draw_system(const CardSystem &sys, const Camera &cam) {
    for (int i = 0; i < sys.count && i < 16; ++i) {
        const Card &card = sys.cards[i];

        float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
        switch (card.orientation) {
        case 0: u0 = 0.0f; u1 = 0.5f; v0 = 0.0f; v1 = 0.5f; break;
        case 1: u0 = 0.5f; u1 = 1.0f; v0 = 0.0f; v1 = 0.5f; break;
        case 2: u0 = 0.0f; u1 = 0.5f; v0 = 0.5f; v1 = 1.0f; break;
        case 3: u0 = 0.5f; u1 = 1.0f; v0 = 0.5f; v1 = 1.0f; break;
        default: break;
        }
        // The gallery picture is stored upside down relative to the other materials.
        if (card.type == 1) std::swap(v0, v1);

        const float dx = (cam.x + card.x) - 640.0f;
        const float dy = (cam.y + card.y) - 360.0f;
        const float pc = std::cos(card.angle), ps = std::sin(card.angle);
        const float cc = std::cos(cam.angle), cs = std::sin(cam.angle);
        auto flat = [&](float lx, float ly) {
            const float X = (lx * pc - ly * ps) + dx;
            const float Y = (lx * ps + ly * pc) + dy;
            const float X2 = X * cc - Y * cs;
            const float Y2 = X * cs + Y * cc;
            return Point{X2 * cam.zoom + 640.0f, Y2 * cam.zoom + 360.0f};
        };

        const float hw = card.w * 0.5f, hh = card.h * 0.5f;

        // Split big cards into cells so the perspective bends them smoothly (about 96 units a cell).
        int n = 1;
        if (card.type != 2 && g_perspective_on) {
            Point corner[4] = {project(flat(-hw, -hh)), project(flat(hw, -hh)), project(flat(hw, hh)),
                               project(flat(-hw, hh))};
            float edge = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const Point &p = corner[k], &q = corner[(k + 1) & 3];
                edge = std::max(edge, std::hypot(q.x - p.x, q.y - p.y));
            }
            n = std::clamp(trunc_i32(std::ceil(edge / 96.0f)), 1, 12);
        }

        for (int gy = 0; gy < n; ++gy) {
            for (int gx = 0; gx < n; ++gx) {
                const float a0 = float(gx) / float(n), a1 = float(gx + 1) / float(n);
                const float b0 = float(gy) / float(n), b1 = float(gy + 1) / float(n);
                const float fa[4] = {a0, a1, a1, a0};
                const float fb[4] = {b0, b0, b1, b1};
                ClipPoint in[4];
                for (int k = 0; k < 4; ++k) {
                    const Point p = flat(-hw + (2.0f * hw) * fa[k], -hh + (2.0f * hh) * fb[k]);
                    in[k] = ClipPoint{p.x, p.y, u0 + (u1 - u0) * fa[k], v0 + (v1 - v0) * fb[k], card_depth(p.y) - 1.0f};
                }
                // Cut away what lies behind the near plane (depth below 1).
                ClipPoint out[8];
                int m = 0;
                for (int k = 0; k < 4; ++k) {
                    const ClipPoint &p = in[k], &q = in[(k + 1) & 3];
                    const bool p_in = p.d >= 0.0f, q_in = q.d >= 0.0f;
                    if (p_in) out[m++] = p;
                    if (p_in != q_in) {
                        const float fr = p.d / (p.d - q.d);
                        out[m++] = ClipPoint{p.x + (q.x - p.x) * fr, p.y + (q.y - p.y) * fr, p.u + (q.u - p.u) * fr,
                                             p.v + (q.v - p.v) * fr, 0.0f};
                    }
                }
                if (m < 3) continue;
                Point screen[8];
                for (int k = 0; k < m; ++k) screen[k] = project(Point{out[k].x, out[k].y});
                for (int j = 1; j + 1 < m; j += 2) {
                    const int idx[4] = {0, j, j + 1, std::min(j + 2, m - 1)};
                    Point qs[4];
                    float qu[4], qv[4];
                    for (int k = 0; k < 4; ++k) {
                        qs[k] = screen[idx[k]];
                        qu[k] = out[idx[k]].u;
                        qv[k] = out[idx[k]].v;
                    }
                    submit_quad(card, qs, qu, qv, cam.alpha);
                }
            }
        }

        if (g_card_log_budget > 0 && card.type != 2) {
            --g_card_log_budget;
            log("loading screen: card type %d material %p %.0fx%.0f at %.0f,%.0f, %dx%d cells, gallery state %d loaded %d",
                card.type, static_cast<void *>(card.material), double(card.w), double(card.h), double(card.x),
                double(card.y), n, n, peek<int32_t>(kGalleryState), int(peek<uint8_t>(kGalleryLoaded)));
        }
    }
}

void draw_cards(float a, bool target) {
    std::lock_guard<std::mutex> lock(g_card_lock);
    if (!g_cards_started) return;
    const float t = seconds_since(g_card_start, Clock::now());
    Camera cam0 = camera_at(t, 0);
    Camera cam1 = camera_at(t, 1);
    cam0.alpha = cam0.alpha * a;
    cam1.alpha = cam1.alpha * a;
    // The table zooms in while the screen fades in and sinks while it fades out.
    const float inv = 1.0f - a;
    const float sq = inv * inv;
    const float kk = std::clamp(1.0f - sq, 0.0f, 1.0f);
    const float zf = (1.0f - kk) * 1.2f + kk;
    cam1.zoom = cam1.zoom * zf;
    cam0.zoom = cam0.zoom * zf;
    if (!target) {
        cam1.y = cam1.y + sq * 50.0f;
        cam0.y = cam0.y + sq * 50.0f;
    }
    if (cam1.alpha != 0.0f)
        for (const CardSystem &sys : g_systems) draw_system(sys, cam1);
    if (cam0.alpha != 0.0f)
        for (const CardSystem &sys : g_systems) draw_system(sys, cam0);
}

// ---------------------------------------------------------------------------------------------
// Tip line.

float cosine_step(float x, float lo, float hi) {
    if (x < lo) return 0.0f;
    if (x > hi) return 1.0f;
    float tt = std::min((x - lo) / (hi - lo), 1.0f);
    tt = std::max(tt, 0.0f);
    const float r = (std::cos(tt * kPi) - 1.0f) * -0.5f;
    return std::min(std::max(r, 0.0f), 1.0f);
}

// Each letter fades in a little after the one before it and fades out the same way at the end.
float glyph_alpha(int32_t i) {
    const int32_t elapsed = int32_t(uint32_t(ticks()) - uint32_t(g_tip_time));
    const float e = float(elapsed) * kGlyphTimeScale + float(i) * -0.1f;
    const float d = float(g_tip_length) * kTipDurationScale;
    return g_fade * (cosine_step(e, 0.0f, 1.0f) - cosine_step(e, d - 1.0f, d));
}

using GlyphDrawFn = float(U2MP_GUEST *)(void *, uint8_t *);

U2MP_GUEST float tip_glyph(void *state, uint8_t *glyph) {
    // Measuring passes go straight through and do not count letters.
    if (peek_abs<uint32_t>(glyph + 0x34) & 4u) return guest<GlyphDrawFn>(fGlyphDraw)(state, glyph);

    const float alpha = glyph_alpha(g_glyph_counter);
    g_glyph_counter += 1;

    alignas(16) uint8_t copy[0xb0] = {};
    if (alpha != 0.0f) {
        // Drop shadow: one unit lower, black, at half strength.
        std::memcpy(copy, glyph, 0xa0);
        poke_abs<float>(copy + 0x04, peek_abs<float>(copy + 0x04) + 1.0f);
        poke_abs<uint32_t>(copy + 0x28, uint32_t(uint8_t(trunc_i32(alpha * 128.0f))) << 24);
        guest<GlyphDrawFn>(fGlyphDraw)(state, copy);
    }
    std::memcpy(copy, glyph, 0xa0);
    const uint32_t colour = peek_abs<uint32_t>(copy + 0x28);
    poke_abs<uint32_t>(copy + 0x28, (colour & 0x00ffffffu) | (uint32_t(uint8_t(trunc_i32(alpha * 255.0f))) << 24));
    return guest<GlyphDrawFn>(fGlyphDraw)(state, copy);
}

void fresh_text_struct(uint8_t *s, float w, float h, uint32_t flags, int32_t font) {
    std::memset(s, 0, 0xb0);
    const float clip[4] = {0.0f, -1.0f, 0.0f, -1.0f};
    std::memcpy(s + 0x10, clip, sizeof clip);
    poke_abs<float>(s + 0x20, 0.75f);
    poke_abs<float>(s + 0x24, 0.75f);
    poke_abs<uint32_t>(s + 0x28, 0xffffffffu);
    poke_abs<float>(s + 0x2c, 1.0f);
    poke_abs<int32_t>(s + 0x30, font);
    poke_abs<uint32_t>(s + 0x34, flags);
    for (int k : {0x50, 0x64, 0x78, 0x8c}) poke_abs<float>(s + k, 1.0f);
    poke_abs<float>(s + 0x90, 1.0f);
    poke_abs<float>(s + 0x94, w);
    poke_abs<float>(s + 0x98, h);
}

void draw_tip() {
    const DcTable *tips = dc_table(kTipsTable);
    if (!tips || !tips->records || tips->count <= 0) return;
    const int32_t count = tips->count;

    const DcTable *games = dc_table(kNetGamesTable);
    const int16_t index = peek<int16_t>(kNetGameIndex);
    int32_t game_type = 0;
    if (games && games->records && index >= 0 && index < games->count)
        game_type = peek_abs<int32_t>(games->records + 0x90 * index + 0xc);

    const int32_t now = ticks();
    if (int32_t(uint32_t(g_tip_time) + uint32_t(g_tip_length)) < now) {
        uint32_t r;
        {
            std::lock_guard<std::mutex> lock(g_card_lock);
            r = rng_step();
        }
        // Walk to the record after the n-th eligible one, never landing on the tip shown last.
        const int32_t old = g_tip_index;
        int32_t idx = 0;
        uint32_t n = r % uint32_t(count) + 1u;
        const int64_t limit = int64_t(count) * 8;
        for (int64_t step = 0; step < limit; ++step) {
            const uint8_t *rec = tips->records + 12 * idx;
            if (peek_abs<int32_t>(rec) == game_type || peek_abs<uint32_t>(rec + 4) != 0) n -= 1;
            idx = idx == count - 1 ? 0 : idx + 1;
            if (n == 0) {
                if (idx == old) {
                    n = 1;
                    continue;
                }
                break;
            }
        }
        g_tip_index = idx;
        const char *chosen = text_by_id(peek_abs<uint32_t>(tips->records + 12 * g_tip_index + 8));
        g_tip_time = now;
        g_tip_length = trunc_i32(float(chosen ? std::strlen(chosen) : 0) * 75.0f);
    }

    const char *s = text_by_id(peek_abs<uint32_t>(tips->records + 12 * g_tip_index + 8));
    if (!s || !*s) return;

    const int32_t font = peek<uint64_t>(kFontOne) != 0 ? 1 : -1;
    alignas(16) uint8_t text[0xb0];
    using PrintFn = void(U2MP_GUEST *)(uint8_t *, const char *);

    fresh_text_struct(text, 600.0f, 9999.0f, 4u | 0x10u, font);
    guest<PrintFn>(fPrintText)(text, s);
    const float W = peek_abs<float>(text + 0x94);
    const float H = peek_abs<float>(text + 0x98);

    fresh_text_struct(text, W, 9999.0f, 1u | 0x10u, font);
    poke_abs<float>(text + 0x00, (1280.0f - W) * 0.5f);
    poke_abs<float>(text + 0x04, 650.0f - H);
    poke_abs<uint64_t>(text + 0xa0, uint64_t(uintptr_t(&tip_glyph)));
    g_glyph_counter = 0;
    poke_abs<float>(text + 0x90, peek<float>(kItemFade));
    guest<void(U2MP_GUEST *)(void *, uint8_t *, const char *)>(fQueueText)(image.base + kTextArena, text, s);
    // A measuring pass with our callback attached; the callback lets it through uncounted.
    poke_abs<uint32_t>(text + 0x34, peek_abs<uint32_t>(text + 0x34) | 4u);
    guest<PrintFn>(fPrintText)(text, s);
}

// ---------------------------------------------------------------------------------------------
// One frame of the screen.

void draw_screen(float dt) {
    const bool target = g_shown.load();

    if (target && cards_ok()) request_gallery();

    g_fade = target ? std::min(g_fade + dt, 1.0f) : std::max(g_fade - dt, 0.0f);
    if (g_fade <= 0.0f) return;

    // Fading in, the black comes first and the content follows; fading out, everything goes together.
    const float a = target ? std::clamp((g_fade - 0.4f) * kFadeInRemap, 0.0f, 1.0f) : g_fade;
    const float t = seconds_since(g_shown_at, Clock::now());

    Xform base = identity_xform();
    base.v[4] = a;

    Xform black = identity_xform();
    set_colour(black, 0xff000000u);
    draw_fill(compose(base, black), 0.0f, 0.0f, 1280.0f, 720.0f);

    if (cards_ok()) draw_cards(a, target);

    if (uint8_t *edge = menu_net_material(kEdgeImage)) {
        menu_draw_quad(edge, 0.0f, 0.0f, 1280.0f, 720.0f, 0.0f, 0.0f, 1.0f, 1.0f, a, a);
        menu_draw_quad(edge, -10.0f, -10.0f, 1290.0f, 730.0f, 0.0f, 0.0f, 1.0f, 1.0f, a, a);
    }

    const float slide = 1.0f - std::clamp((t - 2.0f) * kRosterSlide, 0.0f, 1.0f);
    draw_rosters(a * std::clamp(t - 3.0f, 0.0f, 1.0f), 0.0f, (slide * slide) * 200.0f, dt);

    if (uint8_t *band = menu_net_material(kBandImage))
        menu_draw_quad(band, 128.0f, 22.0f, 1152.0f, 278.0f, 0.0f, 0.0f, 1.0f, 1.0f, a * 0.75f, a);

    const char *map_name;
    char description[128];
    {
        std::lock_guard<std::mutex> lock(g_text_lock);
        map_name = g_map_name;
        std::memcpy(description, g_description, sizeof description);
    }

    Xform big = identity_xform();
    big.v[0] = 1.4f;
    Xform name_colour = identity_xform();
    set_colour(name_colour, 0xffcd4100u);
    name_colour.v[5] = 3.0f;
    draw_text(compose(compose(base, big), name_colour), map_name, 0.0f, 70.0f, 1280.0f, 100.0f);

    Xform desc_colour = identity_xform();
    set_colour(desc_colour, 0xffdfcf8fu);
    desc_colour.v[5] = 3.0f;
    draw_text(compose(base, desc_colour), description, 0.0f, 120.0f, 1280.0f, 100.0f);

    if (g_tip_on && g_text_on && g_tip_ready) draw_tip();
}

// The state hooks only record a request; it is applied here once per frame, like the PS3 game's
// per-frame flag poll, so a hide and show within one frame never restart the screen.
void apply_pending() {
    const int request = g_pending.exchange(kNone);
    if (request == kNone) return;
    const bool on = request == kShow;
    if (on == g_shown.load()) return;
    if (on) {
        prepare_texts();
        g_shown_at = Clock::now();
        if (cards_ok()) start_cards();
        g_tip_time = 0;
        g_tip_length = 0;
        refresh_rosters();
    }
    g_shown.store(on);
    log("loading screen: %s (%s)", on ? "shown" : "hidden", g_reason.load());
}

U2MP_GUEST uint64_t frame_entry(void *tables, uint64_t a, uint64_t b) {
    const uint64_t result = guest<uint64_t(U2MP_GUEST *)(void *, uint64_t, uint64_t)>(fHudUpdate)(tables, a, b);

    const Clock::time_point now = Clock::now();
    float dt = 0.0f;
    if (g_have_previous_frame) dt = std::min(seconds_since(g_previous_frame, now), 0.1f);
    g_have_previous_frame = true;
    g_previous_frame = now;

    apply_pending();
    if (g_shown.load() || g_fade > 0.0f) draw_screen(dt);
    return result;
}

// ---------------------------------------------------------------------------------------------
// State hooks.

struct StateSlot {
    uint64_t slot;      // vtable slot (image offset)
    uint64_t expected;  // the game's function there
    bool show;
    const char *name;
};
constexpr StateSlot kSlots[6] = {
    {0x100ac08, 0x5f2e50, true, "LoadLevels::Enter"},  {0x100ac10, 0x5f2e30, false, "LoadLevels::Exit"},
    {0x100abc8, 0x5f2d90, true, "WaitReady::Enter"},   {0x100abd0, 0x5f2830, false, "WaitReady::Exit"},
    {0x100ab88, 0x5f23e0, false, "Playing::Enter"},    {0x100a9c8, 0x5f00d0, false, "Leave::Enter"},
};
uint64_t g_original[6] = {};

void record_request(bool on, const char *why) {
    if (!g_enabled) return;
    g_reason.store(why);
    g_pending.store(on ? kShow : kHide);
    log("loading screen: %s requested (%s)", on ? "show" : "hide", why);
}

using StateFn = uint64_t(U2MP_GUEST *)(void *);

template <int I> U2MP_GUEST uint64_t state_hook(void *state) {
    const StateFn original = guest_abs<StateFn>(g_original[I]);
    if (kSlots[I].show) {
        const uint64_t result = original(state);
        record_request(true, kSlots[I].name);
        return result;
    }
    record_request(false, kSlots[I].name);
    return original(state);
}
constexpr StateFn kHooks[6] = {&state_hook<0>, &state_hook<1>, &state_hook<2>,
                               &state_hook<3>, &state_hook<4>, &state_hook<5>};

bool switch_on(const char *name) {
    const char *v = std::getenv(name);
    return !(v && std::strcmp(v, "0") == 0);
}

}  // namespace

void install_loading(int &installed) {
    if (!switch_on("UNCHARTED_U2MP_LOADING")) {
        g_enabled = false;
        log("loading screen: off (UNCHARTED_U2MP_LOADING=0)");
        return;
    }
    // A switch turned off by an earlier install stays off.
    g_text_on = g_text_on && switch_on("UNCHARTED_U2MP_LOADING_TEXT");
    g_rosters_on = g_rosters_on && switch_on("UNCHARTED_U2MP_LOADING_ROSTERS");
    g_cards_on = g_cards_on && switch_on("UNCHARTED_U2MP_LOADING_PARTICLES");
    g_perspective_on = g_perspective_on && switch_on("UNCHARTED_U2MP_LOADING_PERSPECTIVE");
    g_tip_on = g_tip_on && switch_on("UNCHARTED_U2MP_LOADING_TIP");

    static const unsigned char kPrologue[] = {0x55, 0x48, 0x89, 0xe5};
    for (uint64_t f : {fItemText, fItemDesc, fPageLayout, fGameDescription}) {
        if (!image.matches(f, kPrologue, sizeof kPrologue)) {
            log("loading screen: 0x%llx is not a function start, not installed", (unsigned long long)f);
            return;
        }
    }

    static const unsigned char kPushFour[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
    static const unsigned char kPushThree[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53};
    if (g_cards_on) {
        // The gallery slot handling the cards rely on (keep-alive store, slot state check).
        static const unsigned char kKeepAlive[] = {0x41, 0xc7, 0x85, 0x68, 0x03, 0x00, 0x00, 0x0a,
                                                   0x00, 0x00, 0x00, 0xb9, 0x08, 0x04, 0x00, 0x00};
        static const unsigned char kStateCheck[] = {0x41, 0x83, 0xbd, 0x10, 0x01, 0x00, 0x00, 0x02};
        g_cards_ready = image.matches(fSlotQuad, kPushFour, sizeof kPushFour) &&
                        image.matches(fMaterialQuad, kPushThree, sizeof kPushThree) &&
                        image.matches(0x58b5c8, kKeepAlive, sizeof kKeepAlive) &&
                        image.matches(0x5b46dc, kStateCheck, sizeof kStateCheck);
        if (!g_cards_ready) log("loading screen: card calls not as expected, cards off");
    }
    if (g_tip_on) {
        // The printer's per-glyph callback call site the tip depends on.
        static const unsigned char kGlyphCall[] = {0x48, 0x8b, 0xbd, 0xb0, 0xfe, 0xff, 0xff, 0x48, 0x8d, 0xb5,
                                                   0xf0, 0xfe, 0xff, 0xff, 0xff, 0x95, 0xb8, 0xfe, 0xff, 0xff};
        g_tip_ready = image.matches(fPrintText, kPushFour, sizeof kPushFour) &&
                      image.matches(fGlyphDraw, kPushFour, sizeof kPushFour) &&
                      image.matches(fQueueText, kPushThree, sizeof kPushThree) &&
                      image.matches(0xccb602, kGlyphCall, sizeof kGlyphCall);
        if (!g_tip_ready) log("loading screen: tip calls not as expected, tip off");
    }

    // Another module may have wrapped a slot already; a pointer outside the image is chained.
    const uint64_t base = absolute(0);
    int chained = 0;
    for (const StateSlot &s : kSlots) {
        const uint64_t value = peek<uint64_t>(s.slot);
        if (value == base + s.expected) continue;
        if (value == 0 || (value >= base && value < base + image.size)) {
            log("loading screen: state vtable slot 0x%llx holds 0x%llx, not 0x%llx: not installed",
                (unsigned long long)s.slot, (unsigned long long)(value >= base ? value - base : value),
                (unsigned long long)s.expected);
            return;
        }
        ++chained;
    }

    if (!retarget_call(kDrawCall, fHudUpdate, reinterpret_cast<void *>(&frame_entry),
                       "loading screen draw (Draw 0x3fc52d)"))
        return;

    for (int i = 0; i < 6; ++i) {
        g_original[i] = peek<uint64_t>(kSlots[i].slot);
        poke<uint64_t>(kSlots[i].slot, uint64_t(uintptr_t(kHooks[i])));
    }

    log("loading screen: PS3 port installed (%zu state hooks, %d chained; text %s, rosters %s, cards %s, "
        "perspective %s, tip %s)",
        sizeof kSlots / sizeof kSlots[0], chained, g_text_on ? "on" : "off", g_rosters_on ? "on" : "off",
        cards_ok() ? "on" : "off", g_perspective_on ? "on" : "off", (g_tip_on && g_tip_ready) ? "on" : "off");
    installed += 1;
}

}  // namespace u2mp
