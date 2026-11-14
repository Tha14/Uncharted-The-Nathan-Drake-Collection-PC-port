// u2mp rewrite tests, group "loading": the old loading.cpp (git) and the new one (u2mp/docs/spec/loading.md) run
// through the same installs, state-hook calls and frames against the same fake game image. Every guest call is
// recorded with its arguments (floats as bit patterns, buffers as 32-bit words, host pointers by meaning) and the
// images are compared after every step. The host clock is hx::fake_clock_ns (fake_clock.h, flags_loading.sh), so the
// times, fades and card positions are exact.
//
// The module keeps process-wide state that install sets only one way (switches, "off"), so every install variant and
// every switch combination runs in its own child process (this program with the scenario name as argument). Inside
// one process old and new go through the same sequence of shows, hides, frames and tips (the random generator, the
// card log budget and the page logs are process-wide).
#define HX_NO_CLOCK_MACRO
#include "fake_clock.h"
#include "harness.h"

#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace u2mp {
void install_loading(int &installed);
}
namespace u2mp_old {
void install_loading(int &installed);
}

extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *, char *, unsigned long);

using hx::get;
using hx::put;
#define G U2MP_GUEST

namespace {

constexpr long long S = 1000000000LL, MS = 1000000LL;

// ---- game addresses (image offsets) --------------------------------------------------------------------------
constexpr uint64_t M = 0x1136640, CTX = M + 0x10;
constexpr uint64_t kSlots[6] = {0x100ac08, 0x100ac10, 0x100abc8, 0x100abd0, 0x100ab88, 0x100a9c8};
constexpr uint64_t kSlotFns[6] = {0x5f2e50, 0x5f2e30, 0x5f2d90, 0x5f2830, 0x5f23e0, 0x5f00d0};
constexpr uint64_t kNgmIndex = 0x113b760 + 0x1ca0, kDataRoot = 0x104ad28, kFrameCounter = 0x1116284;
constexpr uint64_t kItemFade = 0x1050e58, kFadeScale = 0x104ed74, kFont1 = 0x144e648;
constexpr uint32_t kNetGames = 0x3b9a067d, kLevels = 0x0dec97a4, kTips = 0x7f306129, kPageId = 0xb9df67d6;
constexpr uint32_t kEdge = 0x6732636e, kBand = 0x270e3564, kMoney = 0x9595f0a7, kCoins = 0xb4b036fa;
constexpr uint32_t kPhotos[16] = {0x05a8ba5f, 0x01ef2812, 0x01f45731, 0x87f4823f, 0xa6da247b, 0x59555d94,
                                  0x54167b4d, 0x2bbe0247, 0x26fd249e, 0xf9a7c39b, 0xeabed4cf, 0x4ea5310c,
                                  0x6477b06a, 0x3a459fd0, 0x09a8d7c7, 0xf69a236c};
constexpr uint32_t kMaps[9] = {0x83b0526a, 0x4409a1a8, 0x7c3df559, 0xed98d85e, 0x1e1227ac,
                               0x308b816c, 0x00ce50f3, 0xae5c9059, 0x9d998d63};

// ---- the test's data in the image ---------------------------------------------------------------------------
constexpr uint64_t R = 0x1600000;
constexpr uint64_t NG_TBL = R + 0x0, NG_REC = R + 0x100, LV_TBL = R + 0x1000, LV_REC = R + 0x1100;
constexpr uint64_t TIP_TBL = R + 0x2000, TIP_REC = R + 0x2100, STR = R + 0x4000;
constexpr uint64_t PAGE = R + 0x10000, OTHER_PAGE = R + 0x10400, PAGE_VT = R + 0x11000, ST_UPDATE = R + 0x11800,
                   ST_RENDER = R + 0x11900, ST_RENDER2 = R + 0x11a00, ITEMS = R + 0x12000, ITEM = R + 0x12100,
                   LIST = R + 0x13000, LIST_VT = R + 0x13100, ST_GET0 = R + 0x13800, ST_GET30 = R + 0x13900,
                   ST_GET38 = R + 0x13a00;
constexpr uint64_t DROOT = R + 0x30000, TABLES_ARG = R + 0x50000, GLYPH_STATE = R + 0x51000, HOOK_STATE = R + 0x52000;
constexpr uint64_t MAT = 0x1700000;   // materials: MAT + (id & 0xffff) * 16

unsigned fb(float f) {
    unsigned u;
    std::memcpy(&u, &f, 4);
    return u;
}
std::string words(const void *p, size_t n) {
    std::string s;
    char t[16];
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t w;
        std::memcpy(&w, static_cast<const uint8_t *>(p) + i, 4);
        std::snprintf(t, sizeof t, "%08x ", w);
        s += t;
    }
    return s;
}
int al(const void *p, int a) { return int(uintptr_t(p) % uintptr_t(a)); }
const char *W(const void *p) {   // hx::where in a ring of buffers (several per rec line)
    static std::string ring[8];
    static int i = 0;
    i = (i + 1) & 7;
    ring[i] = hx::where(p);
    return ring[i].c_str();
}

// ---- configuration the fakes answer from (same for both sides) -----------------------------------------------
std::map<uint32_t, uint64_t> dc_map;     // DC table id -> offset of the DcTable (missing: NULL)
std::map<uint32_t, uint64_t> text_map;   // text id -> offset of the string (missing: NULL)
std::set<uint32_t> null_mats;            // materials that are NULL
uint64_t hud_ret = 0x5150;
int desc_mode = 0;                       // 0: a short description, 1: 128 non-zero bytes
int update_action = 0;                   // 1: the page update changes flags/fade; 2: it changes vtable +0x18
std::set<uint32_t> tip_text_ids;

// ---- per-run transient state ---------------------------------------------------------------------------------
struct Queued {
    uint8_t st[0xb0];
    std::string s;
};
std::vector<Queued> queue;
const uint8_t *cur_glyph = nullptr;
const uint8_t *last_layout_desc = nullptr;

using CB = float(G *)(void *, uint8_t *);
using FrameFn = uint64_t(G *)(void *, uint64_t, uint64_t);
using HookFn = uint64_t(G *)(void *);

std::string ts_dump(const uint8_t *ts) {
    uint64_t cb;
    std::memcpy(&cb, ts + 0xa0, 8);
    std::string s = words(ts, 0xa0);
    s += cb ? (std::string("cb=") + W(reinterpret_cast<void *>(cb))) : std::string("cb=0");
    s += " " + words(ts + 0xa8, 8);
    return s;
}

void run_glyphs(const uint8_t *st, const char *s, const char *what) {
    uint64_t cb;
    std::memcpy(&cb, st + 0xa0, 8);
    if (!cb) {
        hx::rec("%s: no callback", what);
        return;
    }
    const size_t n = std::strlen(s);
    for (size_t i = 0; i < n; ++i) {
        alignas(16) uint8_t g[0xb0];
        std::memcpy(g, st, 0xb0);
        float x;
        std::memcpy(&x, g, 4);
        x += 10.0f * float(i);
        std::memcpy(g, &x, 4);
        const uint32_t ch = uint8_t(s[i]);
        std::memcpy(g + 0x94, &ch, 4);
        cur_glyph = g;
        const float r = reinterpret_cast<CB>(cb)(hx::mem + GLYPH_STATE, g);
        cur_glyph = nullptr;
        hx::rec("%s glyph %zu -> %08x", what, i, fb(r));
    }
}

// ---- guest fakes ----------------------------------------------------------------------------------------------
G uint64_t f_hud(void *tables, uint64_t a, uint64_t b) {
    hx::rec("hud_update %s %llx %llx", W(tables), (unsigned long long)a, (unsigned long long)b);
    return hud_ret;
}
G void *f_dc(uint32_t id) {
    auto it = dc_map.find(id);
    void *p = it == dc_map.end() || !it->second ? nullptr : hx::mem + it->second;
    hx::rec("dc_lookup %08x -> %s", id, W(p));
    return p;
}
G const char *f_text(void *db, uint32_t id) {
    auto it = text_map.find(id);
    const char *p = it == text_map.end() ? nullptr : reinterpret_cast<const char *>(hx::mem + it->second);
    hx::rec("text_lookup %s %08x -> %s", W(db), id, W(p));
    return p;
}
G void f_desc(void *ngm, char *buf, int n, uint8_t *rec, void *ovr, int a, int b, int c) {
    hx::rec("game_desc ngm=%s buf=%s al16=%d n=%d rec=%s ovr=%s %d %d %d", W(ngm), W(buf), al(buf, 16), n, W(rec),
            W(ovr), a, b, c);
    if (desc_mode == 1)
        for (int i = 0; i < 128; ++i) buf[i] = char('a' + i % 26);
    else
        std::snprintf(buf, size_t(n), "Elimination - %s - 10 kills", W(rec));
}
G void f_item_text(void *ctx, void *list, float *xf, const char *text, float *rect, int m, int r8, int fl, float *clip,
                   int a, int b, int c, int d) {
    hx::rec("item_text ctx=%s list=%s xf[al%d]=%s rect[al%d]=%s clip[al%d]=%s text=\"%s\" %d %d %d %d %d %d %d", W(ctx),
            W(list), al(xf, 16), words(xf, 40).c_str(), al(rect, 16), words(rect, 16).c_str(), al(clip, 16),
            words(clip, 16).c_str(), text, m, r8, fl, a, b, c, d);
}
G void f_item_desc(void *ctx, void *list, float *xf, uint8_t *desc, float *rect, int m, int x) {
    hx::rec("item_desc ctx=%s list=%s xf[al%d]=%s desc[al%d]=%s rect[al%d]=%s %d %d", W(ctx), W(list), al(xf, 16),
            words(xf, 40).c_str(), al(desc, 16), words(desc, 0x2c).c_str(), al(rect, 16), words(rect, 16).c_str(), m, x);
}
std::string desc_dump(const uint8_t *d) {
    uint64_t ctx;
    std::memcpy(&ctx, d, 8);
    return std::string("ctx=") + W(reinterpret_cast<void *>(ctx)) + " " + words(d + 8, 0x48);
}
G void f_layout(uint8_t *page, uint8_t *desc) {
    last_layout_desc = desc;
    hx::rec("page_layout %s desc[al%d]=%s item_fade=%08x", W(page), al(desc, 8), desc_dump(desc).c_str(),
            fb(get<float>(kItemFade)));
}
G void f_update(uint8_t *page, uint8_t *args) {
    uint64_t m;
    std::memcpy(&m, args, 8);
    hx::rec("page_update %s args=%s %s", W(page), W(reinterpret_cast<void *>(m)), words(args + 8, 8).c_str());
    if (update_action == 1) {
        put<uint32_t>(PAGE + 0xb0, 0x80800);
        put<float>(PAGE + 0xf4, 0.375f);
    } else if (update_action == 2) {
        put<uint64_t>(PAGE_VT + 0x18, hx::addr(ST_RENDER2));
    }
}
G void f_render(uint8_t *page, void *n, uint8_t *desc) {
    hx::rec("page_render %s %s desc=%s(%s) %s item_fade=%08x", W(page), W(n), desc == last_layout_desc ? "same" : "other",
            W(desc), desc_dump(desc).c_str(), fb(get<float>(kItemFade)));
}
G void f_render2(uint8_t *page, void *n, uint8_t *desc) {
    hx::rec("page_render2 %s %s desc=%s", W(page), W(n), desc == last_layout_desc ? "same" : "other");
}
G int32_t f_get0(uint8_t *l) {
    hx::rec("list_current %s", W(l));
    return 3;
}
G int32_t f_get30(uint8_t *l) {
    hx::rec("list_first %s", W(l));
    return 1;
}
G int32_t f_get38(uint8_t *l) {
    hx::rec("list_last %s", W(l));
    return 7;
}
G void f_slot_quad(void *ctx, void *list, uint8_t *quad, int slot) {
    hx::rec("slot_quad ctx=%s list=%s slot=%d quad[al%d]=%s", W(ctx), W(list), slot, al(quad, 16), words(quad, 0x110).c_str());
}
G void f_mat_quad(uint8_t *ctx48, uint64_t *info, uint8_t *quad) {
    hx::rec("material_quad ctx48[al%d]=%s info[al%d]=%s %llx %llx %llx quad[al%d]=%s", al(ctx48, 16), words(ctx48, 48).c_str(),
            al(info, 16), W(reinterpret_cast<void *>(info[0])), (unsigned long long)info[1], (unsigned long long)info[2],
            (unsigned long long)info[3], al(quad, 16), words(quad, 0x110).c_str());
}
G void f_print(uint8_t *ts, const char *s) {
    hx::rec("print ts[al%d]=%s text=\"%s\"", al(ts, 16), ts_dump(ts).c_str(), s);
    uint32_t flags;
    std::memcpy(&flags, ts + 0x34, 4);
    uint64_t cb;
    std::memcpy(&cb, ts + 0xa0, 8);
    if (cb) run_glyphs(ts, s, "print");
    if (flags & 4) {
        const float w = std::min(600.0f, float(std::strlen(s)) * 9.25f), h = std::strlen(s) > 40 ? 44.5f : 22.25f;
        std::memcpy(ts + 0x94, &w, 4);
        std::memcpy(ts + 0x98, &h, 4);
    }
}
G void f_queue(void *arena, uint8_t *ts, const char *s) {
    hx::rec("queue_text %s ts[al%d]=%s text=\"%s\"", W(arena), al(ts, 16), ts_dump(ts).c_str(), s);
    Queued q;
    std::memcpy(q.st, ts, 0xb0);
    q.s = s;
    queue.push_back(q);
}
G float f_glyph(void *state, uint8_t *g) {
    hx::rec("glyph_draw %s %s al16=%d %s", W(state), g == cur_glyph ? "orig" : "copy", al(g, 16), words(g, 0xa0).c_str());
    return 7.5f + float(g[0x94]) * 0.125f;
}
template <int I> G uint64_t f_orig(void *state) {
    hx::rec("original %d state=%s", I, W(state));
    return 0xa000 + I;
}
template <int I> G uint64_t f_chain(void *state) {
    hx::rec("chained %d state=%s", I, W(state));
    return 0xc000 + I;
}
void *const kOrig[6] = {(void *)&f_orig<0>, (void *)&f_orig<1>, (void *)&f_orig<2>,
                        (void *)&f_orig<3>, (void *)&f_orig<4>, (void *)&f_orig<5>};
void *const kChain[6] = {(void *)&f_chain<0>, (void *)&f_chain<1>, (void *)&f_chain<2>,
                         (void *)&f_chain<3>, (void *)&f_chain<4>, (void *)&f_chain<5>};

// ---- the two modules -----------------------------------------------------------------------------------------
struct Mod {
    const char *name;
    void (*install)(int &);
    uint64_t hooks[6] = {};
    void *frame = nullptr;
};
Mod mods[2] = {{"old", &u2mp_old::install_loading}, {"new", &u2mp::install_loading}};
std::vector<std::string> last_rec;   // the old side's record of the last step
int passed = 0, failed = 0;

std::vector<uint8_t> cmp_image() {
    std::vector<uint8_t> s = hx::snapshot();
    for (int i = 0; i < 6; ++i) {
        const uint64_t v = get<uint64_t>(kSlots[i]);
        for (const Mod &m : mods)
            if (v && v == m.hooks[i]) {
                const uint64_t tok = 0x4b4f4f4800000000ull | uint64_t(i);   // "HOOK" i
                std::memcpy(s.data() + kSlots[i], &tok, 8);
            }
    }
    return s;
}

bool step(const std::string &name, const std::function<void(Mod &)> &op) {
    std::vector<uint8_t> s0(hx::mem, hx::mem + hx::kImageSize);
    const long long c0 = hx::fake_clock_ns;
    std::vector<std::string> rec[2];
    std::vector<uint8_t> img[2];
    for (int side = 0; side < 2; ++side) {
        if (side) std::memcpy(hx::mem, s0.data(), hx::kImageSize);
        hx::fake_clock_ns = c0;
        hx::calls.clear();
        queue.clear();
        op(mods[side]);
        rec[side] = std::move(hx::calls);
        hx::calls.clear();
        img[side] = cmp_image();
    }
    const bool ok = hx::same(name.c_str(), rec[0], rec[1], &img[0], &img[1]);
    if (ok) {
        std::printf("PASS %s (%zu calls)\n", name.c_str(), rec[0].size());
        ++passed;
    } else {
        ++failed;
    }
    std::fflush(stdout);
    // HX_LOADING_DUMP=<file>: the old side's record of every step is appended there (to read what a step did)
    if (const char *d = std::getenv("HX_LOADING_DUMP"); d && *d)
        if (FILE *f = std::fopen(d, "a")) {
            std::fprintf(f, "##### %s\n", name.c_str());
            for (const std::string &l : rec[0]) std::fprintf(f, "%s\n", l.c_str());
            std::fclose(f);
        }
    last_rec = std::move(rec[0]);
    return ok;
}

bool is_hook_value(uint64_t v) {
    if (!v) return false;
    if (v >= uint64_t(uintptr_t(hx::mem)) && v < uint64_t(uintptr_t(hx::mem)) + hx::kImageSize) return false;
    for (void *c : kChain)
        if (v == uint64_t(uintptr_t(c))) return false;
    return true;
}

void op_install(Mod &m) {
    hx::note_retarget(0x3fc52d, nullptr);
    int n = 5;
    m.install(n);
    hx::rec("installed counter %d", n);
    m.frame = hx::retarget_host(0x3fc52d);
    for (int i = 0; i < 6; ++i) {
        const uint64_t v = get<uint64_t>(kSlots[i]);
        m.hooks[i] = is_hook_value(v) ? v : 0;
        hx::rec("slot %d: %s", i, m.hooks[i] ? "module hook" : (v ? W(reinterpret_cast<void *>(v)) : "0"));
    }
    hx::rec("frame entry %s", m.frame ? "retargeted" : "none");
}

void render_queue(long long delay) {
    hx::fake_clock_ns += delay;
    for (const Queued &q : queue) run_glyphs(q.st, q.s.c_str(), "render");
    queue.clear();
}

uint64_t frame(Mod &m, long long adv, long long render_delay = 0) {
    hx::fake_clock_ns += adv;
    if (!m.frame) {
        hx::rec("frame: no entry");
        return 0;
    }
    const uint64_t r = reinterpret_cast<FrameFn>(m.frame)(hx::mem + TABLES_ARG, 0x1111, 0x2222);
    hx::rec("frame -> %llx", (unsigned long long)r);
    render_queue(render_delay);
    return r;
}

void hook(Mod &m, int i) {
    if (!m.hooks[i]) {
        hx::rec("hook %d not installed", i);
        return;
    }
    const uint64_t r = reinterpret_cast<HookFn>(m.hooks[i])(hx::mem + HOOK_STATE + 0x10 * uint64_t(i));
    hx::rec("hook %d -> %llx", i, (unsigned long long)r);
}

// ---- image builders ------------------------------------------------------------------------------------------
void bytes(uint64_t off, std::initializer_list<int> b) {
    for (int v : b) hx::mem[off++] = uint8_t(v);
}

struct InstallOpt {
    int break_prologue = -1, break_card = -1, break_tip = -1;
    int slot_null = -1, slot_inimage = -1;
    std::vector<int> chained;
    bool retarget_fail = false;
};

void install_image(const InstallOpt &o) {
    const uint64_t pro[4] = {0x5b5450, 0x5b5920, 0x5a2670, 0x64abd0};
    for (int i = 0; i < 4; ++i) bytes(pro[i], {0x55, 0x48, 0x89, 0xe5});
    bytes(0x5b4e90, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54});
    bytes(0xbdce90, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53});
    bytes(0x58b5c8, {0x41, 0xc7, 0x85, 0x68, 0x03, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0xb9, 0x08, 0x04, 0x00, 0x00});
    bytes(0x5b46dc, {0x41, 0x83, 0xbd, 0x10, 0x01, 0x00, 0x00, 0x02});
    bytes(0xcc9a70, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54});
    bytes(0xcc7cc0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54});
    bytes(0xbda9c0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53});
    bytes(0xccb602, {0x48, 0x8b, 0xbd, 0xb0, 0xfe, 0xff, 0xff, 0x48, 0x8d, 0xb5, 0xf0, 0xfe, 0xff, 0xff, 0xff, 0x95, 0xb8,
                     0xfe, 0xff, 0xff});
    if (o.break_prologue >= 0) hx::mem[pro[o.break_prologue] + 3] = 0x90;
    const uint64_t card[4] = {0x5b4e90 + 11, 0xbdce90 + 10, 0x58b5c8 + 15, 0x5b46dc + 7};
    if (o.break_card >= 0) hx::mem[card[o.break_card]] ^= 0xff;
    const uint64_t tip[4] = {0xcc9a70 + 11, 0xcc7cc0 + 11, 0xbda9c0 + 10, 0xccb602 + 19};
    if (o.break_tip >= 0) hx::mem[tip[o.break_tip]] ^= 0xff;
    for (int i = 0; i < 6; ++i) {
        hx::stub(kSlotFns[i], kOrig[i]);
        put<uint64_t>(kSlots[i], hx::addr(kSlotFns[i]));
    }
    for (int i : o.chained) put<uint64_t>(kSlots[i], uint64_t(uintptr_t(kChain[i])));
    if (o.slot_null >= 0) put<uint64_t>(kSlots[o.slot_null], 0);
    if (o.slot_inimage >= 0) put<uint64_t>(kSlots[o.slot_inimage], hx::addr(0x123456));
    if (o.retarget_fail) hx::fail_offsets.push_back(0x3fc52d);
}

void stubs_runtime() {
    hx::stub(0x554340, (void *)&f_hud);
    hx::stub(0xa15e70, (void *)&f_dc);
    hx::stub(0xcddc00, (void *)&f_text);
    hx::stub(0x64abd0, (void *)&f_desc);
    hx::stub(0x5b5450, (void *)&f_item_text);
    hx::stub(0x5b5920, (void *)&f_item_desc);
    hx::stub(0x5a2670, (void *)&f_layout);
    hx::stub(0x5b4e90, (void *)&f_slot_quad);
    hx::stub(0xbdce90, (void *)&f_mat_quad);
    hx::stub(0xcc9a70, (void *)&f_print);
    hx::stub(0xcc7cc0, (void *)&f_glyph);
    hx::stub(0xbda9c0, (void *)&f_queue);
    hx::stub_ptr(PAGE_VT + 0x10, ST_UPDATE, (void *)&f_update);
    hx::stub_ptr(PAGE_VT + 0x18, ST_RENDER, (void *)&f_render);
    hx::stub(ST_RENDER2, (void *)&f_render2);
    hx::stub_ptr(LIST_VT + 0x00, ST_GET0, (void *)&f_get0);
    hx::stub_ptr(LIST_VT + 0x30, ST_GET30, (void *)&f_get30);
    hx::stub_ptr(LIST_VT + 0x38, ST_GET38, (void *)&f_get38);
}

int str_slot = 0;
void put_text(uint32_t id, const std::string &s) {
    const uint64_t off = STR + 0x200 * uint64_t(str_slot++);
    std::memcpy(hx::mem + off, s.c_str(), s.size() + 1);
    text_map[id] = off;
}

void set_table(uint64_t tbl, int32_t count, uint64_t recs) {
    put<int32_t>(tbl, count);
    put<int32_t>(tbl + 4, 0x7777);
    put<uint64_t>(tbl + 8, recs ? hx::addr(recs) : 0);
}

struct TipRec {
    int32_t type;
    uint32_t generic, text;
};
void set_tips(const std::vector<TipRec> &t, int32_t count = -1) {
    set_table(TIP_TBL, count < 0 ? int32_t(t.size()) : count, TIP_REC);
    for (size_t i = 0; i < t.size(); ++i) {
        put<int32_t>(TIP_REC + 12 * i, t[i].type);
        put<uint32_t>(TIP_REC + 12 * i + 4, t[i].generic);
        put<uint32_t>(TIP_REC + 12 * i + 8, t[i].text);
    }
}

void set_netgames(int16_t index) {
    set_table(NG_TBL, 3, NG_REC);
    for (int i = 0; i < 3; ++i) {
        put<uint32_t>(NG_REC + 0x90 * i + 4, 0x2000 + i);
        put<int32_t>(NG_REC + 0x90 * i + 0xc, 5 + i);
    }
    put<int16_t>(kNgmIndex, index);
}

struct Lv {
    uint32_t level, name;
};
void set_levels(const std::vector<Lv> &l) {
    set_table(LV_TBL, int32_t(l.size()), LV_REC);
    for (size_t i = 0; i < l.size(); ++i) {
        put<uint32_t>(LV_REC + 0x38 * i, l[i].level);
        put<uint32_t>(LV_REC + 0x38 * i + 4, l[i].name);
    }
}

void set_pages(bool found, bool null_before = false) {
    for (int i = 0; i < 4; ++i) put<uint64_t>(M + 0x370 + 8 * i, 0);
    put<uint64_t>(M + 0x370, hx::addr(OTHER_PAGE));
    if (found && !null_before) put<uint64_t>(M + 0x378, hx::addr(PAGE));
    if (null_before) put<uint64_t>(M + 0x380, hx::addr(PAGE));   // after the first NULL: not reached
}

void set_items(int n, bool items_null) {
    put<int32_t>(PAGE + 0xb4, n);
    put<uint64_t>(PAGE + 0xb8, items_null ? 0 : hx::addr(ITEMS));
}

void setup_world() {
    stubs_runtime();
    dc_map = {{kNetGames, NG_TBL}, {kLevels, LV_TBL}, {kTips, TIP_TBL}};
    set_netgames(1);
    put_text(0x51, "Village");
    put_text(0x52, "Plaza");
    put_text(0x53, "Lost City");
    set_levels({{0x2000, 0x52}, {0x2001, 0x51}, {0x2002, 0x53}, {0x3000, 0x52}});
    // tips: short texts so they change every second or two
    put_text(0x101, "Use cover often");
    put_text(0x102, "Reload!");
    put_text(0x103, "Treasure wins games");
    put_text(0x104, "Climb");
    put_text(0x105, "Medals unlock boosters and new skins");
    put_text(0x106, "Aim high");
    tip_text_ids = {0x101, 0x102, 0x103, 0x104, 0x105, 0x106};
    set_tips({{6, 0, 0x101}, {2, 0, 0x102}, {2, 1, 0x103}, {6, 0, 0x104}, {9, 0, 0x105}, {6, 0, 0x106}});
    // the roster page and its items
    put<uint32_t>(OTHER_PAGE + 0x10, 0x12345678);
    put<uint64_t>(PAGE, hx::addr(PAGE_VT));
    put<uint32_t>(PAGE + 0x10, kPageId);
    put<uint32_t>(PAGE + 0xb0, 0);
    put<float>(PAGE + 0xf4, 0.25f);
    set_pages(true);
    set_items(10, false);
    const uint64_t list_kind = hx::addr(0x1007910);
    for (int i = 0; i < 10; ++i) {
        const uint64_t it = ITEM + 0x40 * uint64_t(i);
        put<uint64_t>(ITEMS + 8 * i, hx::addr(it));
        put<int32_t>(it + 0x10, 100 + i);
        put<uint64_t>(it, list_kind);
        put<uint64_t>(it + 0x18, hx::addr(LIST));
    }
    put<uint64_t>(ITEMS + 8 * 1, 0);                      // a NULL item
    put<uint64_t>(ITEM + 0x40 * 2, hx::addr(0x1234560));  // another kind of item
    put<uint64_t>(ITEM + 0x40 * 3 + 0x18, 0);             // a team list without its list
    put<uint64_t>(ITEM + 0x40 * 5, 0x10);                 // a vtable below the image (unsigned wrap in the log)
    put<uint64_t>(LIST, hx::addr(LIST_VT));
    put<int32_t>(kFrameCounter, 12345);
    put<int32_t>(M + 0x17a4, 77);
    put<float>(kItemFade, 0.8125f);
    put<float>(kFadeScale, 0.625f);
    put<uint64_t>(kFont1, hx::addr(R + 0x60000));
    put<uint64_t>(kDataRoot, hx::addr(DROOT));
    std::strcpy(reinterpret_cast<char *>(hx::mem + DROOT + 0x408), "/app0/data");
    std::memset(hx::mem + M + 0x268, 'Z', 0x100);   // a stale gallery path without NUL
    put<int32_t>(M + 0x120, 0);
    hx::mem[M + 0x138] = 1;
}

// the parent: every scenario process
struct Proc {
    const char *name;
    std::vector<std::pair<const char *, const char *>> env;
    std::function<void()> run;
};

const char *kVars[6] = {"UNCHARTED_U2MP_LOADING", "UNCHARTED_U2MP_LOADING_TEXT", "UNCHARTED_U2MP_LOADING_ROSTERS",
                        "UNCHARTED_U2MP_LOADING_PARTICLES", "UNCHARTED_U2MP_LOADING_PERSPECTIVE",
                        "UNCHARTED_U2MP_LOADING_TIP"};

void base() {
    hx::init();
    hx::clear();
    hx::bind_both();
    hx::fake_clock_ns = 10 * S;
}

void install_only(const char *name, const InstallOpt &o, bool call_hooks) {
    base();
    install_image(o);
    step(name, op_install);
    if (!call_hooks) return;
    for (int i = 0; i < 6; ++i)
        step(std::string(name) + ": state hook " + std::to_string(i) + " (S10 result passed through)",
             [i](Mod &m) { hook(m, i); });
}

// install with every check passing, then the runtime stubs and the world
void install_runtime(const char *name) {
    base();
    install_image({});
    step(name, op_install);
    setup_world();
}

void frames(Mod &m, int n, long long adv, long long render_delay = 0) {
    for (int i = 0; i < n; ++i) frame(m, adv, render_delay);
}

// a whole show cycle: show request, the apply frame, the fade in, rosters visible, hide and fade out
void cycle(const std::string &name, int show_slot = 0, int hide_slot = 1) {
    step(name + ": show request + apply frame", [&](Mod &m) {
        hook(m, show_slot);
        frame(m, 100 * MS);
    });
    step(name + ": fade in and frames", [&](Mod &m) {
        frames(m, 10, 100 * MS, 30 * MS);
        frame(m, 3600 * MS, 50 * MS);
    });
    step(name + ": hide and fade out", [&](Mod &m) {
        hook(m, hide_slot);
        frames(m, 12, 100 * MS);
    });
}

// ---- the main sequence (all switches on) ----------------------------------------------------------------------
void run_main() {
    install_runtime("S05/S07 install: all slots original, all switches on");
    // some photo materials NULL from the start: they are spawned and logged (within the 8-card budget), not drawn
    for (int k = 0; k < 16; k += 3) null_mats.insert(kPhotos[k]);
    null_mats.insert(kMaps[2]);
    step("S08/S10 show hook LoadLevels::Enter: original first, then the request log", [](Mod &m) { hook(m, 0); });
    long long t_show = 0;
    step("S08/S14 first frame: show applied (texts, cards, tip reset, roster refresh, shown log); dt 0: gallery only",
         [&](Mod &m) { frame(m, 0); });
    t_show = hx::fake_clock_ns;
    step("S15 fade in, fade <= 0.4: a = 0 (S27 no card drawn yet)", [](Mod &m) { frames(m, 3, 100 * MS); });
    step("S15 dt capped at 0.1", [](Mod &m) { frame(m, 500 * MS); });
    step("S15 fade 0.6 / 0.7 (a remapped); S27 first card logs; S22 NULL photos logged, not drawn",
         [](Mod &m) { frames(m, 2, 100 * MS, 20 * MS); });
    step("S15 fade to 1", [](Mod &m) { frames(m, 3, 100 * MS, 40 * MS); });
    step("S19/S20 rosters slide and fade in (t 2.5, 3.5, 4.2, 4.6): first-frame and visible page logs", [&](Mod &m) {
        hx::fake_clock_ns = t_show + 2500 * MS - 100 * MS;
        frame(m, 100 * MS);
        frame(m, 1000 * MS, 10 * MS);
        frame(m, 700 * MS);
        frame(m, 400 * MS, 300 * MS);
    });
    const long long cam_t[4] = {5000, 7900, 10000, 13600};
    const char *cam_name[4] = {"S24 t 5: camera 0 only (camera 1 alpha 0 skips its pass)", "S24 t 7.9: both cameras",
                               "S24 t 10: camera 1 only", "S24 t 13.6: both cameras"};
    for (int i = 0; i < 4; ++i)
        step(cam_name[i], [&](Mod &m) {
            hx::fake_clock_ns = t_show + cam_t[i] * MS - 16 * MS;
            frame(m, 16 * MS, 100 * MS);
        });
    step("S28 gallery: same path again (only the keep-alive written)", [](Mod &m) {
        put<int32_t>(M + 0x368, 0);
        frame(m, 16 * MS);
    });
    step("S28 gallery: new path while state 2 (copied, state left alone)", [](Mod &m) {
        put<int32_t>(M + 0x120, 2);
        std::strcpy(reinterpret_cast<char *>(hx::mem + DROOT + 0x408), "/app0/other-data");
        frame(m, 16 * MS);
    });
    step("S28 gallery: path longer than 255 (strncpy without NUL, +0x367 = 0, copied every frame)", [](Mod &m) {
        put<int32_t>(M + 0x120, 0);
        std::string longroot(300, 'r');
        std::strcpy(reinterpret_cast<char *>(hx::mem + DROOT + 0x408), longroot.c_str());
        frame(m, 16 * MS);
        put<int32_t>(M + 0x120, 3);
        frame(m, 16 * MS);
    });
    step("S28 gallery: path longer than 511 (snprintf truncation)", [](Mod &m) {
        std::string longroot(600, 'q');
        std::strcpy(reinterpret_cast<char *>(hx::mem + DROOT + 0x408), longroot.c_str());
        frame(m, 16 * MS);
    });
    step("S28 gallery: data root NULL (nothing written)", [](Mod &m) {
        put<int32_t>(M + 0x368, 0);
        put<uint64_t>(kDataRoot, 0);
        frame(m, 16 * MS);
    });
    put<uint64_t>(kDataRoot, hx::addr(DROOT));
    std::strcpy(reinterpret_cast<char *>(hx::mem + DROOT + 0x408), "/app0/data");
    null_mats.insert(kEdge);
    null_mats.insert(kBand);
    step("S16 edge and band materials missing (quads skipped)", [](Mod &m) { frame(m, 16 * MS); });
    null_mats.erase(kEdge);
    null_mats.erase(kBand);
    const uint32_t flag_sets[3] = {0x80000, 0x800, 0x80800};
    for (uint32_t f : flag_sets) {
        put<uint32_t>(PAGE + 0xb0, f);
        step("S19 page flags " + std::to_string(f) + " (desc [0] x 0.75, item fade value, restored after)",
             [](Mod &m) { frame(m, 16 * MS); });
    }
    put<uint32_t>(PAGE + 0xb0, 0);
    update_action = 1;
    step("S19 flags and page fade changed by the page update (read after it)", [](Mod &m) { frame(m, 16 * MS); });
    update_action = 0;
    put<uint32_t>(PAGE + 0xb0, 0);
    put<float>(PAGE + 0xf4, 0.25f);
    put<uint64_t>(kFont1, 0);
    step("S31 font pointer NULL (font -1)", [](Mod &m) { frame(m, 2000 * MS, 5 * MS); });
    put<uint64_t>(kFont1, hx::addr(R + 0x60000));
    step("S30/S32 tips rotating with the cards (shared generator), glyph callbacks at several times", [](Mod &m) {
        for (int i = 0; i < 12; ++i) frame(m, 700 * MS, (i % 4) * 90 * MS);
    });
    step("S09 hide then show in one frame while shown: no restart, no applied log", [](Mod &m) {
        hook(m, 1);
        hook(m, 2);
        frame(m, 16 * MS);
    });
    step("S09 show while shown: consumed silently", [](Mod &m) {
        hook(m, 0);
        frame(m, 16 * MS);
    });
    step("S08 hide hook WaitReady::Exit: request log before the original", [](Mod &m) { hook(m, 3); });
    step("S08 hidden log on the next frame", [](Mod &m) { frame(m, 16 * MS); });
    step("S25/S15 fading out: a = fade, cards sink by sq x 50, drawing stops at fade 0",
         [](Mod &m) { frames(m, 12, 100 * MS, 10 * MS); });
    step("S09 hide while hidden (Playing::Enter, Leave::Enter): consumed silently", [](Mod &m) {
        hook(m, 4);
        hook(m, 5);
        frame(m, 16 * MS);
        frame(m, 16 * MS);
    });
    // another full cycle with the WaitReady pair, all data present
    null_mats.clear();
    cycle("S08/S22 second show (WaitReady::Enter / WaitReady::Exit), all materials present", 2, 3);
    // text preparation edge cases (S11, S12, S13)
    dc_map.erase(kNetGames);
    cycle("S11 no net games table (no description, no map name; gallery stops; game type 0)");
    dc_map[kNetGames] = NG_TBL;
    put<uint64_t>(NG_TBL + 8, 0);
    cycle("S11 net games records NULL (gallery request still made)");
    set_netgames(-1);
    cycle("S11 net game index -1");
    set_netgames(3);
    cycle("S11 net game index = count");
    set_netgames(1);
    dc_map.erase(kLevels);
    cycle("S12 levels table NULL (description only)");
    dc_map[kLevels] = LV_TBL;
    put<uint64_t>(LV_TBL + 8, 0);
    cycle("S12 levels records NULL");
    set_levels({{0x2000, 0x52}, {0x2002, 0x53}});
    cycle("S12 no matching level (S18 empty map name skipped)");
    set_levels({{0x2001, 0x52}, {0x2000, 0x53}, {0x2001, 0x51}});
    cycle("S12 two matching levels (text called for each, last wins)");
    set_levels({{0x2001, 0x52}, {0x2001, 0}});
    cycle("S12 last match has name id 0 (no text call, map name NULL)");
    set_levels({{0x2001, 0x51}});
    desc_mode = 1;
    cycle("S13 description of 128 non-zero bytes (byte 127 forced to 0)");
    desc_mode = 0;
    null_mats = {kMoney, kCoins, kMaps[0], kMaps[1], kMaps[3], kPhotos[0], kPhotos[1], kEdge};
    cycle("S16/S22 money, coins, map and player photos and the edge missing");
    null_mats.clear();
    set_pages(false);
    cycle("S19 roster page not found (NOT FOUND log, no page calls)");
    set_pages(true, true);
    cycle("S19 page list ends at the first NULL (page after it not found)");
    set_pages(true);
    set_items(10, true);
    cycle("S20 page items pointer NULL (count logged only)");
    set_items(3, false);
    cycle("S20 three items (NULL item skipped, other vtable, list item)");
    set_items(10, false);
    update_action = 2;
    cycle("edge (not in the spec list): the page update replaces page vtable +0x18");
    update_action = 0;
    put<uint64_t>(PAGE_VT + 0x18, hx::addr(ST_RENDER));
    hud_ret = 0xfeedface12345678ull;
    step("S10 per-frame entry returns the HUD update result while hidden", [](Mod &m) { frames(m, 2, 16 * MS); });
}

// ---- the tip process (cards off: the tip is the generator's only user, so the choices are checked against an
// own model of the spec) ----------------------------------------------------------------------------------------
struct TipModel {
    uint32_t x[4] = {0x2545f491u, 0x9e3779b9u, 0x7f4a7c15u, 0x6a09e667u}, c = 0x01234567u;
    int32_t index = 0;
    int landed_on_previous = 0, limit_reached = 0, choices = 0, mismatches = 0;
    uint32_t next() {
        const uint64_t s = 2111111111ull * x[3] + 1492ull * x[2] + 1776ull * x[1] + 5115ull * x[0] + c;
        x[3] = x[2], x[2] = x[1], x[1] = x[0], x[0] = uint32_t(s), c = uint32_t(s >> 32);
        return x[0];
    }
} model;

// the tip records as the model sees them
std::vector<TipRec> tips_now;
int32_t game_type_now = 6;

// After a step: each choice shows as two identical tip text lookups in a row (choice, then draw).
void check_choices(const std::string &name) {
    int seen = 0, mism = 0;
    for (size_t i = 0; i + 1 < last_rec.size(); ++i) {
        unsigned id = 0;
        if (std::sscanf(last_rec[i].c_str(), "text_lookup %*s %x", &id) != 1 || !tip_text_ids.count(id)) continue;
        if (last_rec[i + 1].compare(0, last_rec[i].find(" -> "), last_rec[i], 0, last_rec[i].find(" -> ")) != 0) continue;
        // a choice: run the spec's algorithm
        const uint32_t r = model.next();
        const int32_t count = int32_t(tips_now.size()), old = model.index;
        int32_t idx = 0, n = int32_t(r % uint32_t(count)) + 1;
        bool done = false;
        for (int g = 0; g < count * 8; ++g) {
            const TipRec &t = tips_now[size_t(idx)];
            if (t.type == game_type_now || t.generic) --n;
            idx = idx + 1 < count ? idx + 1 : 0;
            if (n != 0) continue;
            if (idx == old) {
                n = 1;
                ++model.landed_on_previous;
                continue;
            }
            done = true;
            break;
        }
        if (!done) ++model.limit_reached;
        model.index = idx;
        ++model.choices;
        ++seen;
        if (tips_now[size_t(idx)].text != id) ++mism;
        ++i;
    }
    model.mismatches += mism;
    std::printf("     %s: %d tip choices, %d differ from the spec model (model totals: landed on previous %d, "
                "repeat limit %d)\n",
                name.c_str(), seen, mism, model.landed_on_previous, model.limit_reached);
}

void run_tips() {
    install_runtime("S04/S07 install with UNCHARTED_U2MP_LOADING_PARTICLES=0 (cards off, tip on)");
    put<int32_t>(NG_REC + 0x90 * 1 + 0xc, 6);
    tips_now = {{6, 0, 0x101}, {2, 0, 0x102}, {2, 1, 0x103}, {6, 0, 0x104}, {9, 0, 0x105}, {6, 0, 0x106}};
    set_tips(tips_now);
    step("tips: show request + apply frame", [](Mod &m) {
        hook(m, 0);
        frame(m, 0);
    });
    step("S30/S32 mixed table (eligible, generic, other types): fade in, rotations, glyph alpha 0 / > 0", [](Mod &m) {
        for (int i = 0; i < 30; ++i) frame(m, 450 * MS, (i % 5) * 70 * MS);
    });
    check_choices("mixed table");
    tips_now = {{6, 0, 0x101}, {6, 0, 0x102}, {6, 0, 0x103}, {6, 0, 0x104}};
    set_tips(tips_now);
    step("S30 all four eligible: many rotations (landing on the previous tip)", [](Mod &m) {
        for (int i = 0; i < 60; ++i) frame(m, 1500 * MS, 40 * MS);
    });
    check_choices("all eligible");
    tips_now = {{6, 0, 0x101}, {1, 0, 0x102}, {2, 0, 0x103}};
    set_tips(tips_now);
    step("S30 one eligible record only (repeat limit reached every other choice)", [](Mod &m) {
        for (int i = 0; i < 12; ++i) frame(m, 2500 * MS, 40 * MS);
    });
    check_choices("one eligible");
    put_text(0x110, "A");
    put_text(0x111, "B");
    tip_text_ids.insert(0x110);
    tip_text_ids.insert(0x111);
    tips_now = {{6, 0, 0x110}, {6, 0, 0x111}};
    set_tips(tips_now);
    step("S32 one-character tips: negative glyph alpha (low 8 bits of negative ints)", [](Mod &m) {
        for (int i = 0; i < 16; ++i) frame(m, 300 * MS, (i % 8) * 15 * MS);
    });
    check_choices("one-character tips");
    put<uint64_t>(kFont1, 0);
    step("S31 font NULL then set again", [](Mod &m) {
        frame(m, 400 * MS, 10 * MS);
        put<uint64_t>(kFont1, hx::addr(R + 0x60000));
        frame(m, 400 * MS, 10 * MS);
    });
    check_choices("font");
    tip_text_ids.insert(0x120);
    tip_text_ids.insert(0x121);
    put_text(0x121, "");
    tips_now = {{6, 0, 0x120}, {6, 0, 0x121}};
    set_tips(tips_now);
    step("S29 tip text missing / empty: nothing drawn, length 0 (a new choice every frame)",
         [](Mod &m) { frames(m, 8, 300 * MS); });
    check_choices("missing / empty texts");
    dc_map.erase(kTips);
    step("S29 tips table missing", [](Mod &m) { frames(m, 3, 300 * MS); });
    dc_map[kTips] = TIP_TBL;
    set_tips(tips_now, 0);
    step("S29 tips table empty (count 0)", [](Mod &m) { frames(m, 2, 300 * MS); });
    set_tips(tips_now, -3);
    step("S29 tips table count negative", [](Mod &m) { frames(m, 2, 300 * MS); });
    set_table(TIP_TBL, 2, 0);
    step("S29 tips records NULL", [](Mod &m) { frames(m, 2, 300 * MS); });
    tips_now = {{6, 0, 0x101}, {2, 0, 0x102}, {2, 1, 0x103}, {6, 0, 0x104}, {9, 0, 0x105}, {6, 0, 0x106}};
    set_tips(tips_now);
    dc_map.erase(kNetGames);
    game_type_now = 0;
    step("S30 no net game (game type 0: only generic records eligible)", [](Mod &m) { frames(m, 12, 1200 * MS, 30 * MS); });
    check_choices("no net game");
    step("tips: hide and fade out (glyph alpha follows the raw fade)", [](Mod &m) {
        hook(m, 1);
        for (int i = 0; i < 12; ++i) frame(m, 100 * MS, 20 * MS);
    });
    std::printf("     tip model: %d choices, %d mismatches, landed on previous %d, repeat limit %d\n", model.choices,
                model.mismatches, model.landed_on_previous, model.limit_reached);
    if (model.mismatches) ++failed;
}

// a few cycles under a switch combination
void run_switch(const char *name) {
    install_runtime(name);
    step(std::string(name) + ": show + frames", [](Mod &m) {
        hook(m, 0);
        frame(m, 0);
        frames(m, 10, 100 * MS, 20 * MS);
        frame(m, 4000 * MS, 50 * MS);
        frames(m, 4, 900 * MS, 30 * MS);
    });
    step(std::string(name) + ": hide + fade out", [](Mod &m) {
        hook(m, 3);
        frames(m, 12, 100 * MS);
    });
}

std::vector<Proc> procs() {
    std::vector<Proc> p;
    p.push_back({"S01 UNCHARTED_U2MP_LOADING=0", {{"UNCHARTED_U2MP_LOADING", "0"}},
                 [] { install_only("S01 UNCHARTED_U2MP_LOADING=0: off log, nothing patched", {}, true); }});
    for (int i = 0; i < 4; ++i) {
        const std::string n = "S02 prologue " + std::to_string(i) + " broken";
        p.push_back({strdup(n.c_str()), {}, [i, n] {
                         InstallOpt o;
                         o.break_prologue = i;
                         install_only(n.c_str(), o, false);
                     }});
    }
    for (int i = 0; i < 4; ++i) {
        const std::string n = "S03 card pattern " + std::to_string(i) + " broken";
        p.push_back({strdup(n.c_str()), {}, [i, n] {
                         InstallOpt o;
                         o.break_card = i;
                         install_only(n.c_str(), o, false);
                     }});
    }
    p.push_back({"S03 PARTICLES=0", {{"UNCHARTED_U2MP_LOADING_PARTICLES", "0"}},
                 [] { install_only("S03 UNCHARTED_U2MP_LOADING_PARTICLES=0 with a broken card pattern: no card log", [] {
                          InstallOpt o;
                          o.break_card = 2;
                          return o;
                      }(), false); }});
    for (int i = 0; i < 4; ++i) {
        const std::string n = "S04 tip pattern " + std::to_string(i) + " broken";
        p.push_back({strdup(n.c_str()), {}, [i, n] {
                         InstallOpt o;
                         o.break_tip = i;
                         install_only(n.c_str(), o, false);
                     }});
    }
    p.push_back({"S04 TIP=0", {{"UNCHARTED_U2MP_LOADING_TIP", "0"}},
                 [] { install_only("S04 UNCHARTED_U2MP_LOADING_TIP=0 with a broken tip pattern: no tip log", [] {
                          InstallOpt o;
                          o.break_tip = 1;
                          return o;
                      }(), false); }});
    p.push_back({"S04 TEXT=0 tip check", {{"UNCHARTED_U2MP_LOADING_TEXT", "0"}},
                 [] { install_only("S04 tip patterns checked although TEXT=0", [] {
                          InstallOpt o;
                          o.break_tip = 3;
                          return o;
                      }(), false); }});
    for (int i = 0; i < 6; ++i) {
        const std::string n = "S05 slot " + std::to_string(i) + " NULL";
        p.push_back({strdup(n.c_str()), {}, [i, n] {
                         InstallOpt o;
                         o.slot_null = i;
                         install_only(n.c_str(), o, true);
                     }});
    }
    p.push_back({"S05 slot in image", {}, [] {
                     InstallOpt o;
                     o.slot_inimage = 2;
                     o.chained = {0};   // chained slots before it are fine, then the in-image one stops the install
                     install_only("S05 slot 2 elsewhere in the image (after a chained slot 0)", o, true);
                 }});
    p.push_back({"S05 chained", {}, [] {
                     InstallOpt o;
                     o.chained = {3, 5};
                     install_only("S05 two host pointers chained (WaitReady::Exit, Leave::Enter)", o, true);
                 }});
    p.push_back({"S05 all chained", {}, [] {
                     InstallOpt o;
                     o.chained = {0, 1, 2, 3, 4, 5};
                     install_only("S05 all six slots chained", o, true);
                 }});
    p.push_back({"S06 retarget fails", {}, [] {
                     InstallOpt o;
                     o.retarget_fail = true;
                     o.chained = {3};
                     install_only("S06 retarget of 0x3fc52d fails: slots unchanged, no install log", o, true);
                 }});
    p.push_back({"S07 TEXT=0", {{"UNCHARTED_U2MP_LOADING_TEXT", "0"}}, [] { install_only("S07 TEXT=0", {}, false); }});
    p.push_back({"S07 ROSTERS=0", {{"UNCHARTED_U2MP_LOADING_ROSTERS", "0"}}, [] { install_only("S07 ROSTERS=0", {}, false); }});
    p.push_back({"S07 PERSPECTIVE=0", {{"UNCHARTED_U2MP_LOADING_PERSPECTIVE", "0"}},
                 [] { install_only("S07 PERSPECTIVE=0", {}, false); }});
    p.push_back({"S07 all off", {{"UNCHARTED_U2MP_LOADING_TEXT", "0"}, {"UNCHARTED_U2MP_LOADING_ROSTERS", "0"},
                                {"UNCHARTED_U2MP_LOADING_PERSPECTIVE", "0"}, {"UNCHARTED_U2MP_LOADING_PARTICLES", "0"},
                                {"UNCHARTED_U2MP_LOADING_TIP", "0"}},
                 [] { install_only("S07 all switches 0", {}, false); }});
    p.push_back({"S07 values not exactly 0", {{"UNCHARTED_U2MP_LOADING", "00"}, {"UNCHARTED_U2MP_LOADING_TEXT", "1"},
                                             {"UNCHARTED_U2MP_LOADING_ROSTERS", "0 "}, {"UNCHARTED_U2MP_LOADING_TIP", "off"}},
                 [] { install_only("S07 switch values other than exactly \"0\" leave it on", {}, false); }});
    p.push_back({"main", {}, run_main});
    p.push_back({"tips", {{"UNCHARTED_U2MP_LOADING_PARTICLES", "0"}}, run_tips});
    p.push_back({"S17 TEXT=0", {{"UNCHARTED_U2MP_LOADING_TEXT", "0"}},
                 [] { run_switch("S17 TEXT=0: fill, cards, edges, rosters, band; no texts, no tip"); }});
    p.push_back({"S21 ROSTERS=0", {{"UNCHARTED_U2MP_LOADING_ROSTERS", "0"}},
                 [] { run_switch("S21 ROSTERS=0: refresh log ' (rosters off)', no page calls"); }});
    p.push_back({"S26 PERSPECTIVE=0", {{"UNCHARTED_U2MP_LOADING_PERSPECTIVE", "0"}},
                 [] { run_switch("S26 PERSPECTIVE=0: one quad per card, flat placement"); }});
    p.push_back({"TIP=0 runtime", {{"UNCHARTED_U2MP_LOADING_TIP", "0"}}, [] { run_switch("TIP=0: no tip drawn"); }});
    p.push_back({"tip not ready runtime", {}, [] {
                     base();
                     InstallOpt o;
                     o.break_tip = 0;
                     install_image(o);
                     step("tip calls not as expected: install", op_install);
                     setup_world();
                     step("tip calls not as expected: frames without tip", [](Mod &m) {
                         hook(m, 0);
                         frame(m, 0);
                         frames(m, 10, 100 * MS);
                     });
                 }});
    p.push_back({"cards not ready runtime", {}, [] {
                     base();
                     InstallOpt o;
                     o.break_card = 1;
                     install_image(o);
                     step("card calls not as expected: install", op_install);
                     setup_world();
                     step("card calls not as expected: frames without cards or gallery request", [](Mod &m) {
                         hook(m, 0);
                         frame(m, 0);
                         frames(m, 10, 100 * MS, 10 * MS);
                     });
                 }});
    return p;
}

}  // namespace

namespace hxl {
uint8_t *material(uint32_t id) {
    if (null_mats.count(id)) return nullptr;
    return hx::mem + MAT + uint64_t(id & 0xffff) * 16;
}
}  // namespace hxl

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    std::vector<Proc> all = procs();
    if (argc > 1) {
        for (const Proc &p : all)
            if (p.name == std::string(argv[1])) {
                for (const char *v : kVars) _putenv_s(v, "");
                for (const auto &[k, v] : p.env) _putenv_s(k, v);
                p.run();
                std::printf("RESULT %d %d\n", passed, failed);
                std::fflush(stdout);
                return failed ? 1 : 0;
            }
        std::printf("no scenario %s\n", argv[1]);
        return 2;
    }
    char exe[1024];
    GetModuleFileNameA(nullptr, exe, sizeof exe);
    int tp = 0, tf = 0, crashed = 0;
    for (const Proc &p : all) {
        const std::string cmd = std::string("\"\"") + exe + "\" \"" + p.name + "\"\"";
        std::printf("== %s\n", p.name);
        std::fflush(stdout);
        FILE *f = _popen(cmd.c_str(), "r");
        char line[8192];
        bool got = false;
        while (f && std::fgets(line, sizeof line, f)) {
            int a, b;
            if (std::sscanf(line, "RESULT %d %d", &a, &b) == 2) {
                tp += a, tf += b, got = true;
                continue;
            }
            std::fputs(line, stdout);
        }
        if (f) _pclose(f);
        if (!got) {
            std::printf("FAIL %s: the process did not finish (crash?)\n", p.name);
            ++crashed;
        }
        std::fflush(stdout);
    }
    std::printf("\nloading: %d steps passed, %d failed, %d processes crashed\n", tp, tf, crashed);
    return tf || crashed ? 1 : 0;
}
