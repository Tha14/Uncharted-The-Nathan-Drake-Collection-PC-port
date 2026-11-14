// u2mp rewrite tests, gameplay group: scoreboard, rosternames, visibility, gameplay3, matchend, spawn, scripttime
// (u2mp/docs/spec/gameplay.md). Old (u2mp_old) and new (u2mp) run the same scenarios, each on a fresh fake image and
// pool (hx::clear before each side, so near_stub addresses are equal); module statics carry over between scenarios
// alike on both sides. Host clocks come from fake_clock.h (flags_gameplay.sh force-includes it into the sources).
//
// run: bash gow3_pc/tests/u2mp_rewrite/run.sh gameplay scoreboard.cpp rosternames.cpp visibility.cpp gameplay3.cpp
//      matchend.cpp spawn.cpp scripttime.cpp            (HX_VERBOSE=1 prints every record of failing scenarios)
#include "harness.h"
#define HX_NO_CLOCK_MACRO
#include "fake_clock.h"
#include "u2mp_internal.h"

#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#define DECL_MODULE                                    \
    void install_spawn(int &installed);                \
    void install_visibility(int &installed);           \
    void install_scripttime(int &installed);           \
    void install_scoreboard(int &installed);           \
    void install_rosternames(int &installed);          \
    void install_matchend(int &installed);             \
    void install_gameplay3(int &installed);            \
    void visibility_kick();
namespace u2mp_old {
DECL_MODULE
}
namespace u2mp {
void visibility_kick();
}

const uint8_t *(*gp_package_by_id)(uint32_t id) = nullptr;

namespace {

using hx::get;
using hx::put;
using hx::rec;
using hx::where;

struct Side {
    const char *tag;
    void (*spawn)(int &);
    void (*vis)(int &);
    void (*st)(int &);
    void (*sb)(int &);
    void (*rn)(int &);
    void (*me)(int &);
    void (*g3)(int &);
    void (*kick)();
};
const Side OLD{"old",
               u2mp_old::install_spawn,
               u2mp_old::install_visibility,
               u2mp_old::install_scripttime,
               u2mp_old::install_scoreboard,
               u2mp_old::install_rosternames,
               u2mp_old::install_matchend,
               u2mp_old::install_gameplay3,
               u2mp_old::visibility_kick};
const Side NEW{"new",
               u2mp::install_spawn,
               u2mp::install_visibility,
               u2mp::install_scripttime,
               u2mp::install_scoreboard,
               u2mp::install_rosternames,
               u2mp::install_matchend,
               u2mp::install_gameplay3,
               u2mp::visibility_kick};

// ---------------------------------------------------------------------------------------------------------------
// helpers

uint64_t bump = 0;
uint64_t salloc(size_t n, size_t align = 16) {
    bump = (bump + align - 1) & ~uint64_t(align - 1);
    const uint64_t o = bump;
    bump += n;
    if (bump > hx::kImageSize) {
        std::fprintf(stderr, "scratch overflow\n");
        std::exit(3);
    }
    return o;
}
uint64_t gstub(void *host) {
    const uint64_t o = salloc(16);
    hx::stub(o, host);
    return o;
}
std::string fb(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    char b[16];
    std::snprintf(b, sizeof b, "%08x", u);
    return b;
}
std::string hexd(const void *p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; i++) {
        std::snprintf(b, sizeof b, "%02x", static_cast<const uint8_t *>(p)[i]);
        s += b;
    }
    return s;
}
bool all_zero(const void *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (static_cast<const uint8_t *>(p)[i]) return false;
    return true;
}
uint64_t A(uint64_t off) { return hx::addr(off); }
void put_bytes(uint64_t off, std::initializer_list<uint8_t> b) {
    size_t i = 0;
    for (uint8_t v : b) hx::mem[off + i++] = v;
}
void put_call(uint64_t at, uint64_t target) {
    hx::mem[at] = 0xe8;
    put<int32_t>(at + 1, int32_t(int64_t(target) - int64_t(at + 5)));
}
uint64_t mk_str(const char *s) {
    const uint64_t o = salloc(std::strlen(s) + 1, 1);
    std::memcpy(hx::mem + o, s, std::strlen(s) + 1);
    return o;
}
void setenv_(const char *name, const char *value) {
    std::string s = std::string(name) + "=" + (value ? value : "");
    _putenv(s.c_str());   // "NAME=" removes it
}
void inst(void (*f)(int &), const char *what) {
    int n = 7;
    f(n);
    rec("install %s -> installed %d", what, n);
}

long long side_clock[2] = {1000000000000LL, 1000000000000LL};
void tick(long long ns) { hx::fake_clock_ns += ns; }

// host-pointer slots the modules write: compared as "holds a host pointer"
const uint64_t kHostSlots[] = {0x100a290, 0x100a2f0, 0x100ab90, 0x100ab98, 0x100aa48};
std::vector<uint8_t> norm(std::vector<uint8_t> s) {
    const uint64_t lo = uint64_t(uintptr_t(hx::mem)), hi = lo + hx::kImageSize;
    for (uint64_t o : kHostSlots) {
        uint64_t v;
        std::memcpy(&v, s.data() + o, 8);
        if (v && (v < lo || v >= hi)) {
            v = 0x484f53545054520aull;   // "host pointer"
            std::memcpy(s.data() + o, &v, 8);
        }
    }
    return s;
}

int n_scen = 0, n_pass = 0;
int g_side = 0;
std::vector<std::string> failed;
std::function<void()> reset_cfg;

void scenario(const char *name, const std::function<void(const Side &)> &body) {
    std::vector<std::string> r[2];
    std::vector<uint8_t> im[2];
    for (int k = 0; k < 2; k++) {
        const Side &s = k ? NEW : OLD;
        g_side = k;
        hx::clear();
        hx::bind_both();
        bump = 0x1600000;
        hx::fake_clock_ns = side_clock[k];
        if (reset_cfg) reset_cfg();
        setenv_("UNCHARTED_U2MP_SCOREBOARD_BUTTON", nullptr);
        setenv_("UNCHARTED_U2MP_TIME_LIMIT", nullptr);
        body(s);
        hx::bind_both();
        side_clock[k] = hx::fake_clock_ns;
        r[k] = hx::calls;
        im[k] = norm(hx::snapshot());
    }
    n_scen++;
    const bool ok = hx::same(name, r[0], r[1], &im[0], &im[1]);
    if (ok) n_pass++;
    else failed.push_back(name);
    std::printf("%s %s (%zu records)\n", ok ? "PASS" : "FAIL", name, r[0].size());
    if ((!ok && std::getenv("HX_VERBOSE")) || std::getenv("HX_DUMP")) {
        for (int k = 0; k < 2; k++) {
            std::printf("  --- %s\n", k ? "new" : "old");
            for (const auto &l : r[k]) std::printf("    %s\n", l.c_str());
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// shared game addresses
constexpr uint64_t kNgm = 0x113b760, kNetSm = 0x14ce97c, kNgmState = 0x113d5e0, kGame = 0x113d400;
constexpr uint64_t kTracker0 = 0x113b770, kTrackerStride = 0x2c0;
constexpr uint64_t kLookup = 0xa15e70;
constexpr uint32_t kNetGamesId = 0x3b9a067d, kLevelsId = 0x0dec97a4;

// LookupPointer: id -> image offset (0 = NULL)
std::map<uint32_t, uint64_t> lookups;
U2MP_GUEST const uint8_t *h_lookup(uint32_t id) {
    rec("guest LookupPointer %08x", id);
    auto it = lookups.find(id);
    return it == lookups.end() || !it->second ? nullptr : hx::mem + it->second;
}
// package_by_id: id -> image offset
std::map<uint32_t, uint64_t> packages;
const uint8_t *pkg_by_id(uint32_t id) {
    auto it = packages.find(id);
    return it == packages.end() || !it->second ? nullptr : hx::mem + it->second;
}

// =================================================================================================================
// scoreboard

constexpr uint64_t kSbCall = 0x3fc52d, kSbDraw = 0x554340, kSbLayout = 0x5a2670, kSbTracker = 0x104f544;
constexpr uint64_t kMenu = 0x1136640, kPages = kMenu + 0x370, kItemFade = 0x1050e58, kFadeScale = 0x104ed74;
constexpr uint64_t kButtons = 0x14b7fb0, kMenuFrames = 0x1116284;

struct SbCfg {
    int draw_n = 0;
    bool mutate_in_update = false;
    uint64_t alt_vt = 0, alt_render = 0;   // for the mutation
    void *layout_desc = nullptr;
} sb;
using H3 = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t);

U2MP_GUEST uint64_t h_draw2d(uint64_t a, uint64_t b, uint64_t c) {
    rec("guest draw2d %llx %llx %llx", (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
    return 0xd2d0000ull + uint64_t(++sb.draw_n);
}
U2MP_GUEST uint64_t h_draw2d_chained(uint64_t a, uint64_t b, uint64_t c) {
    rec("guest draw2d(chained) %llx %llx %llx", (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
    return 0xc4a1000ull + uint64_t(++sb.draw_n);
}
std::string desc_str(const uint8_t *d) {
    void *ctx;
    std::memcpy(&ctx, d, 8);
    return "ctx=" + where(ctx) + " " + hexd(d + 8, 0x48) + (uintptr_t(d) % 8 ? " MISALIGNED" : "");
}
U2MP_GUEST void h_page_update(uint8_t *page, uint8_t *args) {
    uint64_t m;
    int32_t e;
    float dt;
    std::memcpy(&m, args, 8);
    std::memcpy(&e, args + 8, 4);
    std::memcpy(&dt, args + 12, 4);
    rec("guest page.update %s menu=%s ev=%d dt=%s", where(page).c_str(), where((void *)m).c_str(), e, fb(dt).c_str());
    if (sb.mutate_in_update) {
        const uint64_t vt = get<uint64_t>(uint64_t(page - hx::mem));
        put<uint64_t>(vt - uint64_t(uintptr_t(hx::mem)) + 0x18, A(sb.alt_render));   // slot 3 of the page's vtable
        put<uint64_t>(uint64_t(page - hx::mem), A(sb.alt_vt));                       // the page's vptr
    }
}
U2MP_GUEST void h_layout(uint8_t *page, uint8_t *desc) {
    rec("guest layout %s desc{%s} itemfade=%s", where(page).c_str(), desc_str(desc).c_str(),
        fb(get<float>(kItemFade)).c_str());
    sb.layout_desc = desc;
    const float marker = 7.5f;
    std::memcpy(desc + 0x30, &marker, 4);   // the layout writes into the descriptor
    desc[0x4c] = 0x5a;
}
void render_common(const char *which, uint8_t *page, void *arg, uint8_t *desc) {
    rec("guest %s %s arg=%s same_desc=%d desc{%s} itemfade=%s", which, where(page).c_str(), where(arg).c_str(),
        desc == sb.layout_desc, desc_str(desc).c_str(), fb(get<float>(kItemFade)).c_str());
}
U2MP_GUEST void h_render(uint8_t *page, void *arg, uint8_t *desc) { render_common("render", page, arg, desc); }
U2MP_GUEST void h_render2(uint8_t *page, void *arg, uint8_t *desc) { render_common("render2", page, arg, desc); }
U2MP_GUEST void h_render3(uint8_t *page, void *arg, uint8_t *desc) { render_common("render3", page, arg, desc); }

void sb_setup(uint64_t target = kSbDraw) {
    put_bytes(kSbLayout, {0x55, 0x48, 0x89, 0xe5});
    put_bytes(kSbDraw, {0x55, 0x48, 0x89, 0xe5});
    put<uint32_t>(kSbTracker + 0x18, 0xf650325d);
    put<float>(kSbTracker + 0x10, 1.0f);
    put_call(kSbCall, target);
}
// after the install: the guest functions become stubs
H3 sb_after(uint64_t target = kSbDraw) {
    hx::stub(kSbDraw, (void *)h_draw2d);
    if (target != kSbDraw) hx::stub(target, (void *)h_draw2d_chained);
    hx::stub(kSbLayout, (void *)h_layout);
    return (H3)hx::retarget_host(kSbCall);
}
H3 sb_install(const Side &s, const char *env, uint64_t target = kSbDraw) {
    sb_setup(target);
    if (env) setenv_("UNCHARTED_U2MP_SCOREBOARD_BUTTON", env);
    inst(s.sb, "scoreboard");
    return sb_after(target);
}
uint64_t sb_vtable(void *update, void *render) {
    const uint64_t vt = salloc(0x20, 8);
    put<uint64_t>(vt + 0x10, A(gstub(update)));
    put<uint64_t>(vt + 0x18, A(gstub(render)));
    return vt;
}
uint64_t sb_page(uint32_t id, uint32_t flags = 0, float pfade = 0.25f) {
    const uint64_t p = salloc(0x100, 16);
    put<uint64_t>(p, A(sb_vtable((void *)h_page_update, (void *)h_render)));
    put<uint32_t>(p + 0x10, id);
    put<uint32_t>(p + 0xb0, flags);
    put<float>(p + 0xf4, pfade);
    put<int32_t>(p + 0xf8, 0);
    return p;
}
void sb_pages(const std::vector<uint64_t> &list) {
    for (size_t i = 0; i < list.size(); i++) put<uint64_t>(kPages + 8 * i, list[i] ? A(list[i]) : 0);
}
void sb_state(int netsm, int ngmstate) {
    put<int32_t>(kNetSm, netsm);
    put<int32_t>(kNgmState, ngmstate);
    put<int32_t>(kMenuFrames, 12345);
    put<int32_t>(kMenu + 0x17a4, -77);
    put<float>(kFadeScale, 0.8f);
    put<float>(kItemFade, 0.3f);
}
void sb_frame(H3 h, long long dns, uint32_t buttons, uint64_t page_for_snap = 0) {
    tick(dns);
    put<uint32_t>(kButtons, buttons);
    sb.layout_desc = nullptr;
    const uint64_t r = h(0x1111, 0x2222, 0x3333);
    rec("H -> %llx; tracker shown=%u fade=%s speed=%s itemfade=%s snap=%d", (unsigned long long)r,
        get<uint8_t>(kSbTracker + 8), fb(get<float>(kSbTracker + 0xc)).c_str(), fb(get<float>(kSbTracker + 0x10)).c_str(),
        fb(get<float>(kItemFade)).c_str(), page_for_snap ? get<int32_t>(page_for_snap + 0xf8) : -999);
}
constexpr long long ms50 = 50000000LL;
constexpr uint32_t kSbId = 0xf650325d, kTouch = 0x100000;

void scoreboard_tests() {
    // install
    scenario("scoreboard install env unset", [](const Side &s) { sb_install(s, nullptr); });
    scenario("scoreboard install env 0", [](const Side &s) { sb_install(s, "0"); });
    scenario("scoreboard install env zz (0, off)", [](const Side &s) { sb_install(s, "zz"); });
    scenario("scoreboard install env unset after 0 (mask stays 0)", [](const Side &s) { sb_install(s, nullptr); });
    scenario("scoreboard install env 0x200", [](const Side &s) { sb_install(s, "0x200"); });
    scenario("scoreboard install env overflow / junk", [](const Side &s) {
        sb_install(s, "1ffffffffff");
        sb_install(s, "12qz");
    });
    scenario("scoreboard install bad layout prologue", [](const Side &s) {
        sb_setup();
        hx::mem[kSbLayout + 3] = 0xe4;
        inst(s.sb, "scoreboard");
    });
    scenario("scoreboard install bad 2D draw prologue", [](const Side &s) {
        sb_setup();
        hx::mem[kSbDraw] = 0x90;
        inst(s.sb, "scoreboard");
    });
    scenario("scoreboard install tracker page id wrong", [](const Side &s) {
        sb_setup();
        put<uint32_t>(kSbTracker + 0x18, 0xf650325c);
        inst(s.sb, "scoreboard");
    });
    scenario("scoreboard install tracker speed wrong", [](const Side &s) {
        sb_setup();
        put<float>(kSbTracker + 0x10, 2.0f);
        inst(s.sb, "scoreboard");
    });
    scenario("scoreboard install no E8", [](const Side &s) {
        sb_setup();
        hx::mem[kSbCall] = 0xe9;
        inst(s.sb, "scoreboard");
    });
    scenario("scoreboard install call site not executable", [](const Side &s) {
        sb_setup();
        hx::noexec.push_back({kSbCall + 4, 1});
        inst(s.sb, "scoreboard");
    });
    scenario("scoreboard install retarget fails", [](const Side &s) {
        sb_setup();
        hx::fail_offsets.push_back(kSbCall);
        inst(s.sb, "scoreboard");
    });
    scenario("scoreboard install chained (call reaches 0x700000)", [](const Side &s) {
        H3 h = sb_install(s, "100000", 0x700000);
        sb_state(1, 0);
        sb_frame(h, 0, 0);   // the very first H call: dt 0, not playing
    });
    scenario("scoreboard install chained (call reaches before the image start)", [](const Side &s) {
        sb_setup();
        put<int32_t>(kSbCall + 1, int32_t(-0x3fc532 - 0x100));   // target = image - 0x100
        inst(s.sb, "scoreboard");
    });
    // per frame
    scenario("scoreboard frames: not playing (NET_SM 1, NGM state 3), press ignored", [](const Side &s) {
        H3 h = sb_install(s, "100000");
        sb_state(1, 2);
        sb_frame(h, ms50, kTouch);
        sb_state(5, 3);
        sb_frame(h, ms50, 0);
        sb_frame(h, ms50, kTouch);
    });
    scenario("scoreboard frames: playing, no press; press with page absent (first show: NOT FOUND log)",
             [](const Side &s) {
                 H3 h = sb_install(s, "100000");
                 sb_state(2, 2);
                 sb_frame(h, ms50, 0);
                 sb_frame(h, ms50, 0x20);           // bit outside the mask
                 sb_frame(h, ms50, 0x20 | kTouch);  // rising edge
                 sb_frame(h, ms50, kTouch);         // held
                 sb_frame(h, ms50, kTouch | 0x8);   // held + other bit
             });
    scenario("scoreboard frames: page behind a NULL, then present (flags 0)", [](const Side &s) {
        H3 h = sb_install(s, "100000");
        sb_state(2, 2);
        const uint64_t other = sb_page(0x1234), p = sb_page(kSbId, 0);
        sb_pages({other, 0, p});
        sb_frame(h, ms50, kTouch, p);
        sb_pages({other, sb_page(0x99), sb_page(0x98), p});
        sb_frame(h, ms50, kTouch, p);
        sb_frame(h, 20000000LL, kTouch, p);
    });
    scenario("scoreboard frames: flags 0x80000 / 0x800 / both, fade reaches 1", [](const Side &s) {
        H3 h = sb_install(s, "100000");
        sb_state(2, 2);
        const uint64_t p1 = sb_page(kSbId, 0x80000, 0.5f);
        sb_pages({p1});
        sb_frame(h, ms50, kTouch, p1);
        put<uint32_t>(p1 + 0xb0, 0x800);
        sb_frame(h, ms50, kTouch, p1);
        put<uint32_t>(p1 + 0xb0, 0x80800);
        put<float>(p1 + 0xf4, -1.5f);
        sb_frame(h, 400000000LL, kTouch, p1);   // capped at 0.1
        sb_frame(h, 400000000LL, kTouch, p1);
        sb_frame(h, 400000000LL, kTouch, p1);   // fade clamps at 1
    });
    scenario("scoreboard frames: press again hides, fade out draws with y < 0, until 0", [](const Side &s) {
        H3 h = sb_install(s, "100000");
        sb_state(3, 2);
        const uint64_t p = sb_page(kSbId);
        sb_pages({p});
        sb_frame(h, ms50, 0, p);
        sb_frame(h, ms50, kTouch, p);   // hide
        for (int i = 0; i < 8; i++) sb_frame(h, 25000000LL, kTouch, p);
        sb_frame(h, ms50, 0, p);
    });
    scenario("scoreboard frames: show, then leave playing while shown (hidden, fade keeps drawing)", [](const Side &s) {
        H3 h = sb_install(s, "100000");
        sb_state(2, 2);
        const uint64_t p = sb_page(kSbId);
        sb_pages({p});
        sb_frame(h, ms50, kTouch, p);   // show (second show: no page log)
        for (int i = 0; i < 3; i++) sb_frame(h, ms50, 0, p);
        sb_state(2, 4);
        for (int i = 0; i < 4; i++) sb_frame(h, ms50, kTouch, p);
        sb_state(1, 2);
        for (int i = 0; i < 6; i++) sb_frame(h, ms50, 0, p);
    });
    scenario("scoreboard frames: 200-entry list (found at 199; at 200 not found)", [](const Side &s) {
        H3 h = sb_install(s, "100000");
        sb_state(2, 2);
        std::vector<uint64_t> list;
        const uint64_t filler = sb_page(0x77);
        for (int i = 0; i < 200; i++) list.push_back(filler);
        const uint64_t p = sb_page(kSbId);
        list[199] = p;
        sb_pages(list);
        sb_frame(h, ms50, kTouch, p);
        sb_frame(h, ms50, 0, p);
        list[199] = filler;
        list.push_back(p);   // index 200: past the list
        sb_pages(list);
        sb_frame(h, ms50, 0, p);
        sb_frame(h, ms50, kTouch, p);   // hide
        sb_frame(h, 400000000LL, 0, p);
        sb_frame(h, 400000000LL, 0, p);
    });
    scenario("scoreboard frames: vptr and slot 3 changed during the update", [](const Side &s) {
        H3 h = sb_install(s, "100000");
        sb_state(2, 2);
        const uint64_t p = sb_page(kSbId, 0x80000);
        sb.alt_vt = sb_vtable((void *)h_page_update, (void *)h_render3);
        sb.alt_render = gstub((void *)h_render2);
        sb_pages({p});
        sb.mutate_in_update = true;
        sb_frame(h, ms50, kTouch, p);
        sb_frame(h, ms50, 0, p);
        sb.mutate_in_update = false;
    });
    scenario("scoreboard frames: env 0x200 mask, touchpad ignored, 0x200 toggles", [](const Side &s) {
        H3 h = sb_install(s, "0x200");
        sb_state(2, 2);
        const uint64_t p = sb_page(kSbId);
        sb_pages({p});
        sb_frame(h, ms50, kTouch, p);
        sb_frame(h, ms50, kTouch | 0x200, p);
        sb_frame(h, ms50, 0, p);
        sb_frame(h, ms50, 0x200, p);
        sb_frame(h, 400000000LL, 0, p);
        sb_frame(h, 400000000LL, 0, p);
    });
    scenario("scoreboard frames: equal timing in ns not exact in float (33333333 ns, 0.0999999 s)", [](const Side &s) {
        H3 h = sb_install(s, "100000");
        sb_state(2, 2);
        const uint64_t p = sb_page(kSbId);
        sb_pages({p});
        sb_frame(h, 33333333LL, kTouch, p);
        sb_frame(h, 16666667LL, 0, p);
        sb_frame(h, 99999999LL, 0, p);
        sb_frame(h, 123456789LL, kTouch, p);
        sb_frame(h, 7, 0, p);
        for (int i = 0; i < 8; i++) sb_frame(h, 33333333LL, 0, p);
    });
    // last on purpose: when dt differs in its last bit the fade statics differ afterwards
    scenario("scoreboard frames: dt rounding for 16777223 ns and 40000001 ns (> 2^24 ns)", [](const Side &s) {
        H3 h = sb_install(s, "100000");
        sb_state(2, 2);
        const uint64_t p = sb_page(kSbId);
        sb_pages({p});
        sb_frame(h, 16777223LL, kTouch, p);
        sb_frame(h, 40000001LL, 0, p);
    });
}

// =================================================================================================================
// rosternames

constexpr uint64_t kTeamVt = 0x100a280, kRowBuild = 0x5b0f70, kCellText = 0x5b1e90;
constexpr uint64_t kTable1 = 0x1147bd8, kTable2 = 0x114d2ec;
using H6 = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

U2MP_GUEST uint64_t h_row_build(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    rec("guest row_build %llx %llx %llx %llx %llx %llx", (unsigned long long)a, (unsigned long long)b, (unsigned long long)c,
        (unsigned long long)d, (unsigned long long)e, (unsigned long long)f);
    return 0x40e0b111dull;
}
U2MP_GUEST uint64_t h_cell_text(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    rec("guest cell_text %llx %llx %llx %llx %llx %llx", (unsigned long long)a, (unsigned long long)b, (unsigned long long)c,
        (unsigned long long)d, (unsigned long long)e, (unsigned long long)f);
    return 0xce11ull;
}
void rn_setup() {
    put_bytes(0x5b24fc, {0x4b, 0x8d, 0xb4, 0x3c, 0x18, 0x39, 0x00, 0x00});
    put_bytes(0x66c998, {0x49, 0x8d, 0xbc, 0x1d, 0x20, 0x39, 0x00, 0x00});
    put<uint64_t>(kTeamVt + 0x10, A(kRowBuild));
    put<uint64_t>(kTeamVt + 0x70, A(kCellText));
}
struct RnHooks {
    H6 row = nullptr, cell = nullptr;
};
RnHooks rn_install(const Side &s) {
    rn_setup();
    inst(s.rn, "rosternames");
    hx::stub(kRowBuild, (void *)h_row_build);
    hx::stub(kCellText, (void *)h_cell_text);
    RnHooks h;
    const uint64_t lo = A(0), hi = A(hx::kImageSize);
    uint64_t v = get<uint64_t>(kTeamVt + 0x10);
    if (v < lo || v >= hi) h.row = (H6)v;
    v = get<uint64_t>(kTeamVt + 0x70);
    if (v < lo || v >= hi) h.cell = (H6)v;
    rec("hooks row=%d cell=%d", h.row != nullptr, h.cell != nullptr);
    return h;
}
void rn_name(uint64_t off, const char *s, size_t field) {
    std::memset(hx::mem + off, 0, field);
    std::memcpy(hx::mem + off, s, std::min(std::strlen(s), field));
}
void rn_dump() {
    for (int i = 0; i < 10; i++) {
        const uint64_t t = kTracker0 + kTrackerStride * i;
        rec("tracker %d display %s", i, hexd(hx::mem + t + 0x60, 0x41).c_str());
    }
    for (uint64_t tb : {kTable1, kTable2})
        for (int i = 0; i < 12; i++) rec("table %llx rec %d display %s", (unsigned long long)tb, i, hexd(hx::mem + tb + 0x80 * i + 0x3918, 0x21).c_str());
}
void rnc(H6 h, const char *what) {
    if (!h) {
        rec("no %s hook", what);
        return;
    }
    const uint64_t r = h(0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6);
    rec("%s returned %llx", what, (unsigned long long)r);
}
const char k64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";   // 64 chars

void rosternames_tests() {
    scenario("rosternames install NULL image", [](const Side &s) {
        rn_setup();
        hx::set_image_both(nullptr, 0);
        inst(s.rn, "rosternames");
    });
    scenario("rosternames install cell-text bytes differ", [](const Side &s) {
        rn_setup();
        hx::mem[0x5b24fc + 7] = 1;
        inst(s.rn, "rosternames");
    });
    scenario("rosternames install NetStatGame bytes differ", [](const Side &s) {
        rn_setup();
        hx::mem[0x66c998] = 0x48;
        inst(s.rn, "rosternames");
    });
    scenario("rosternames install slot 2 wrong, slot 14 right", [](const Side &s) {
        rn_setup();
        put<uint64_t>(kTeamVt + 0x10, A(kRowBuild + 1));
        inst(s.rn, "rosternames");
    });
    scenario("rosternames install slot 2 right, slot 14 wrong", [](const Side &s) {
        rn_setup();
        put<uint64_t>(kTeamVt + 0x70, kCellText);   // offset, not absolute
        inst(s.rn, "rosternames");
    });
    scenario("rosternames install slot 14 past the image size", [](const Side &s) {
        rn_setup();
        hx::set_image_both(hx::mem, kTeamVt + 0x70 + 7);
        inst(s.rn, "rosternames");
    });
    scenario("rosternames install slot 2 past the image size", [](const Side &s) {
        rn_setup();
        hx::set_image_both(hx::mem, kTeamVt + 0x10 + 7);
        inst(s.rn, "rosternames");
    });
    scenario("rosternames install both (+2), hooks pass args, nothing to fill", [](const Side &s) {
        RnHooks h = rn_install(s);
        rnc(h.row, "row build");
        rnc(h.cell, "cell text");
    });
    scenario("rosternames fill trackers: empty nick, display set, 63 / 64 chars", [](const Side &s) {
        RnHooks h = rn_install(s);
        const uint64_t t = kTracker0;
        rn_name(t + 0x20, "alpha", 0x40);                               // 0: filled
        rn_name(t + kTrackerStride + 0x20, "", 0x40);                  // 1: empty nick
        rn_name(t + 2 * kTrackerStride + 0x20, "bravo", 0x40);         // 2: display already set
        rn_name(t + 2 * kTrackerStride + 0x60, "keep", 0x40);
        std::memcpy(hx::mem + t + 3 * kTrackerStride + 0x20, k64, 63);   // 3: 63 chars + 0
        std::memcpy(hx::mem + t + 4 * kTrackerStride + 0x20, k64, 64);   // 4: 64 chars running to the display
        std::memcpy(hx::mem + t + 9 * kTrackerStride + 0x20, k64, 64);   // 9: 64 chars, display byte 0 is 0
        hx::mem[t + 9 * kTrackerStride + 0x60] = 0;
        hx::mem[t + 9 * kTrackerStride + 0x61] = 'Q';
        hx::mem[t + 9 * kTrackerStride + 0x80] = 'R';
        rnc(h.row, "row build");
        rn_dump();
        rnc(h.cell, "cell text");   // all filled already: no log
    });
    scenario("rosternames fill records: 31 / 32 chars, counts 0, 3, 10, 12, both tables", [](const Side &s) {
        RnHooks h = rn_install(s);
        put<uint32_t>(kTable1, 3);
        put<uint32_t>(kTable2, 0);
        for (int i = 0; i < 12; i++) {
            std::memcpy(hx::mem + kTable1 + 0x80 * i + 0x38f8, k64 + i, i == 1 ? 31 : 32);
            std::memcpy(hx::mem + kTable2 + 0x80 * i + 0x38f8, k64 + 20 + i, 31);
        }
        rn_name(kTable1 + 0x80 * 2 + 0x3918, "set", 0x40);
        rnc(h.cell, "cell text");
        put<uint32_t>(kTable2, 12);
        put<uint32_t>(kTable1, 10);
        rnc(h.row, "row build");
        rn_dump();
        put<uint32_t>(kTable2, 0xffffffff);
        rnc(h.row, "row build");
    });
    scenario("rosternames log limit: nine filling calls, non-filling calls", [](const Side &s) {
        RnHooks h = rn_install(s);
        for (int k = 0; k < 9; k++) {
            rn_name(kTracker0 + kTrackerStride * (k % 10) + 0x20, "nick", 0x40);
            rn_name(kTracker0 + kTrackerStride * (k % 10) + 0x60, "", 0x40);
            if (k == 4) rnc(h.cell, "cell text (nothing new)");
            rnc((k & 1) ? h.cell : h.row, "hook");
        }
        rn_dump();
    });
}

// =================================================================================================================
// visibility

constexpr uint64_t kProbeVt = 0x1004180, kCollision = 0x1127dd0, kCamera = 0x104e820, kFilter = 0x151f048;
constexpr uint64_t kKickA = 0x151f058, kKickB = 0x151f060, kKickC = 0x151f068, kProbeFrame = 0x11161a8;
constexpr uint64_t kPosFn = 0x665f70, kViewFn = 0x5566b0, kExitSlot = 0x100ab90, kExitFn = 0x5f2350;
const std::pair<unsigned, uint64_t> kProbeSlots[] = {{1, 0x4f1dc0}, {2, 0x4f2130}, {5, 0x4f2490}, {7, 0x4f25b0},
                                                     {9, 0x4f26c0}, {12, 0x4f2850}, {32, 0x4f2600}};

struct VisCfg {
    float pos[10][4] = {};
    int32_t reject[10] = {};
    uint8_t active = 1;
    int cur = -1;
    uint64_t exit_ret = 0xe1e1;
} vis;

int tracker_index(const uint8_t *p) {
    const int64_t d = p - (hx::mem + kTracker0);
    return d >= 0 && d % kTrackerStride == 0 ? int(d / kTrackerStride) : -1;
}
std::string v4(const float *v) { return fb(v[0]) + "," + fb(v[1]) + "," + fb(v[2]) + "," + fb(v[3]); }
U2MP_GUEST uint64_t h_pos(float *out, uint8_t *tracker) {
    vis.cur = tracker_index(tracker);
    rec("guest player_pos tracker=%s out_al16=%d out_zero=%d", where(tracker).c_str(), int(uintptr_t(out) % 16 == 0),
        all_zero(out, 16));
    if (vis.cur >= 0) std::memcpy(out, vis.pos[vis.cur], 16);
    return 0xdeadbeef;
}
U2MP_GUEST uint64_t h_view(uint8_t *view, float f, const float *target) {
    rec("guest view_test f=%s target=%s view_al16=%d view_zero=%d", fb(f).c_str(), v4(target).c_str(),
        int(uintptr_t(view) % 16 == 0), all_zero(view, 0x80));
    const int32_t r = vis.cur >= 0 ? vis.reject[vis.cur] : 0;
    std::memcpy(view + 0x2c, &r, 4);
    return 0x77;
}
U2MP_GUEST void h_init(uint8_t *probe, uint32_t a, uint32_t b, uint32_t c, uint32_t d, void *buf, uint32_t size) {
    rec("guest probe.Init %s %u %u %u %u %s %u", where(probe).c_str(), a, b, c, d, where(buf).c_str(), size);
}
U2MP_GUEST void h_raydir(uint8_t *probe, uint32_t z, const float *start, const float *delta, float len) {
    rec("guest probe.SetRayDir %s %u start=%s delta=%s len=%s", where(probe).c_str(), z, v4(start).c_str(),
        v4(delta).c_str(), fb(len).c_str());
}
U2MP_GUEST void h_radius(uint8_t *probe, uint32_t z, float r) {
    rec("guest probe.SetRadius %s %u %s", where(probe).c_str(), z, fb(r).c_str());
}
U2MP_GUEST void h_filter(uint8_t *probe, uint32_t a, uint32_t b, uint32_t c, const uint64_t *obj) {
    rec("guest probe.SetFilter %s %u %u %u obj=%llx (ptr %s)", where(probe).c_str(), a, b, c, (unsigned long long)*obj,
        where(obj).c_str());
}
U2MP_GUEST void h_kick(uint8_t *probe, uint64_t a, uint32_t b, uint64_t c, uint32_t d, uint32_t e) {
    rec("guest probe.Kick %s %llx %x %llx %u %u", where(probe).c_str(), (unsigned long long)a, b, (unsigned long long)c, d, e);
}
U2MP_GUEST void h_release(uint8_t *probe) { rec("guest probe.Release %s", where(probe).c_str()); }
U2MP_GUEST uint8_t h_isactive(uint8_t *probe) {
    rec("guest probe.IsActive %s -> %u", where(probe).c_str(), vis.active);
    return vis.active;
}
U2MP_GUEST void h_init2(uint8_t *probe, uint32_t, uint32_t, uint32_t, uint32_t, void *, uint32_t) {
    rec("guest probe.Init(replaced entry) %s", where(probe).c_str());
}
U2MP_GUEST uint64_t h_exit(void *state) {
    rec("guest Playing::Exit %s", where(state).c_str());
    return vis.exit_ret;
}

void vis_setup() {
    for (const auto &[slot, fn] : kProbeSlots) put<uint64_t>(kProbeVt + 8 * slot, A(fn));
    put_bytes(kPosFn, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00,
                       0x00, 0x49, 0x89, 0xfc});
    put_bytes(kViewFn, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x89, 0xfb});
    put_bytes(0x66618c, {0x4c, 0x8d, 0xb0, 0xb0, 0x01, 0x00, 0x00, 0x48, 0x63, 0x90, 0xd4, 0x01, 0x00, 0x00});
    put_bytes(0x662797, {0x48, 0x8d, 0x05, 0xe2, 0x19, 0x9a, 0x00, 0x48, 0x89, 0x83, 0xb0, 0x01, 0x00, 0x00});
    put_bytes(0x63e813, {0xe8, 0x48, 0x79, 0x02, 0x00});
    put<uint64_t>(kExitSlot, A(kExitFn));
}
void vis_after() {
    void *hosts[] = {(void *)h_init, (void *)h_kick, (void *)h_release, (void *)h_raydir,
                     (void *)h_filter, (void *)h_isactive, (void *)h_radius};
    int i = 0;
    for (const auto &[slot, fn] : kProbeSlots) hx::stub(fn, hosts[i++]);
    hx::stub(kPosFn, (void *)h_pos);
    hx::stub(kViewFn, (void *)h_view);
    hx::stub(kExitFn, (void *)h_exit);
}
using HExit = uint64_t(U2MP_GUEST *)(void *);
HExit vis_install(const Side &s) {
    vis_setup();
    inst(s.vis, "visibility");
    vis_after();
    const uint64_t v = get<uint64_t>(kExitSlot);
    if (v >= A(0) && v < A(hx::kImageSize)) return nullptr;
    return (HExit)v;
}
uint64_t tr(int i) { return kTracker0 + kTrackerStride * i; }
// a tracker: used, net id, excluded, probe vptr ok
void vis_tracker(int i, bool used, uint32_t id, bool excluded = false, bool probe_ok = true) {
    const uint64_t t = tr(i);
    hx::mem[t + 0x20] = used ? 1 : 0;
    put<uint32_t>(t + 0x120, id);
    hx::mem[t + 0x26c] = excluded ? 1 : 0;
    put<uint64_t>(t + 0x1b0, probe_ok ? A(kProbeVt) : A(kProbeVt + 8));
    for (int k = 0; k < 4; k++) vis.pos[i][k] = float(i * 10 + k) + 0.375f;
}
void vis_world(int netsm = 3, bool world = true, int32_t frame = 100, int local = 0) {
    put<int32_t>(kNetSm, netsm);
    put<uint64_t>(kCollision, world ? 0x5151ull : 0);
    put<int32_t>(kProbeFrame, frame);
    put<uint64_t>(kNgm, local >= 0 ? A(tr(local)) : 0);
    const float cam[4] = {1.5f, -2.25f, 3.125f, 1.0f};
    std::memcpy(hx::mem + kCamera, cam, 16);
    put<uint64_t>(kFilter, 0xf117e70bull);
    put<uint64_t>(kKickA, 0xaaaa000000000001ull);
    put<uint32_t>(kKickB, 0xbbbb0002u);
    put<uint64_t>(kKickC, 0xcccc000000000003ull);
}
void kickf(const Side &s, const char *what) {
    rec("-- kick %s", what);
    s.kick();
}
void exitf(HExit h, const char *what) {
    rec("-- exit %s", what);
    if (!h) {
        rec("no exit hook");
        return;
    }
    const uint64_t r = h(hx::mem + 0x1650000);
    rec("exit returned %llx", (unsigned long long)r);
}

void visibility_tests() {
    scenario("visibility kick before any install (not ready)", [](const Side &s) {
        vis_setup();
        vis_after();
        vis_world();
        vis_tracker(0, true, 1);
        vis_tracker(1, true, 2);
        kickf(s, "not ready");
    });
    for (const auto &[slot, fn] : kProbeSlots) {
        static std::vector<std::string> names;
        names.push_back("visibility install probe slot " + std::to_string(slot) + " wrong");
        const unsigned sl = slot;
        scenario(names.back().c_str(), [sl](const Side &s) {
            vis_setup();
            put<uint64_t>(kProbeVt + 8 * sl, 0);
            if (sl == 9) put<uint64_t>(kProbeVt + 8 * 32, 0);   // first failing slot only
            inst(s.vis, "visibility");
        });
    }
    scenario("visibility install probe slot 32 past the image size", [](const Side &s) {
        vis_setup();
        hx::set_image_both(hx::mem, kProbeVt + 8 * 32 + 7);
        inst(s.vis, "visibility");
    });
    {
        static const uint64_t check_at[] = {kPosFn + 20, kViewFn + 13, 0x66618c + 5, 0x662797 + 2, 0x63e813 + 4};
        for (int k = 0; k < 5; k++) {
            static std::vector<std::string> names;
            names.push_back("visibility install check bytes differ #" + std::to_string(k));
            const uint64_t at = check_at[k];
            scenario(names.back().c_str(), [at](const Side &s) {
                vis_setup();
                hx::mem[at] ^= 0xff;
                inst(s.vis, "visibility");
            });
        }
    }
    scenario("visibility install image too small for the kick arguments (silent)", [](const Side &s) {
        vis_setup();
        hx::set_image_both(hx::mem, kKickC + 7);
        inst(s.vis, "visibility");
    });
    scenario("visibility install Exit slot wrong", [](const Side &s) {
        vis_setup();
        put<uint64_t>(kExitSlot, A(kExitFn + 0x10));
        inst(s.vis, "visibility");
        vis_after();
        vis_world();
        vis_tracker(1, true, 2);
        kickf(s, "still not ready");
    });
    scenario("visibility kick: initial exit frame -1 equals frame -1; gates", [](const Side &s) {
        HExit h = vis_install(s);
        (void)h;
        vis_tracker(0, true, 1);
        vis_tracker(1, true, 2);
        vis_world(3, true, -1);
        kickf(s, "frame -1");
        vis_world(1, true, 5);
        kickf(s, "NET_SM 1");
        vis_world(3, false, 5);
        kickf(s, "no collision world");
        vis_world(3, true, 5, -1);
        kickf(s, "no local tracker");
    });
    scenario("visibility kick: remote players, unused, excluded, local id, wrong vptr, view rejected", [](const Side &s) {
        vis_install(s);
        vis_world(2, true, 10, 0);
        vis_tracker(0, true, 100);              // local
        vis_tracker(1, true, 101);              // remote: kicked (first kick log)
        vis_tracker(2, false, 102);             // unused
        vis_tracker(3, true, 103, true);        // excluded
        vis_tracker(4, true, 100);              // same net id as local
        vis_tracker(5, true, 105, false, false);   // wrong vptr
        vis_tracker(6, true, 106);              // view rejected
        vis.reject[6] = -3;
        vis_tracker(7, true, 107);              // kicked
        vis.pos[7][3] = -0.0f;
        kickf(s, "frame 10");
        put<int32_t>(kProbeFrame, 11);
        kickf(s, "frame 11 (no first-kick log)");
    });
    scenario("visibility kick: earlier kicked trackers become unused / excluded / local (release rules)", [](const Side &s) {
        vis_install(s);
        vis_world(2, true, 20, 0);
        vis_tracker(0, true, 100);
        vis_tracker(1, false, 101);   // kicked before: release, IsActive 1
        vis.active = 1;
        kickf(s, "1 unused (active)");
        vis_tracker(7, true, 107, true);   // kicked before: excluded, IsActive 0
        vis.active = 0;
        kickf(s, "7 excluded (inactive)");
        vis_tracker(1, true, 101);
        vis_tracker(7, true, 107);
        vis.active = 1;
        kickf(s, "1 and 7 kicked again");
        vis_tracker(1, true, 100);                 // now the local id
        vis_tracker(7, false, 107, false, false);  // unused + vptr wrong: flag cleared, no release calls
        kickf(s, "1 local id, 7 unused with wrong vptr");
        kickf(s, "nothing kicked");
    });
    scenario("visibility kick: view rejected after an earlier kick keeps the flag; slot entry read at call time",
             [](const Side &s) {
                 vis_install(s);
                 vis_world(2, true, 30, 0);
                 vis_tracker(0, true, 100);
                 vis_tracker(3, true, 103);
                 kickf(s, "3 kicked");
                 vis.reject[3] = 1;
                 kickf(s, "3 rejected (flag stays)");
                 vis.reject[3] = 0;
                 put<uint64_t>(kProbeVt + 8, A(gstub((void *)h_init2)));   // Init entry replaced after install
                 kickf(s, "3 kicked with replaced Init entry");
                 vis_tracker(3, false, 103);
                 vis.active = 1;
                 kickf(s, "3 unused: released");
             });
    scenario("visibility exit: kicked trackers released with the world; same-frame kick skipped; next frame kicks",
             [](const Side &s) {
                 HExit h = vis_install(s);
                 vis_world(2, true, 40, 0);
                 vis_tracker(0, true, 100);
                 vis_tracker(2, true, 102);
                 vis_tracker(5, true, 105);
                 vis_tracker(8, true, 108);
                 kickf(s, "2, 5, 8 kicked");
                 put<uint64_t>(tr(5) + 0x1b0, A(kProbeVt + 16));   // vptr wrong at release: counted, no calls
                 vis.active = 0;
                 put<int32_t>(kProbeFrame, 41);
                 vis.exit_ret = 0x1234567890abcdefull;
                 exitf(h, "with world");
                 kickf(s, "same frame as Exit");
                 exitf(h, "none kicked (no log)");
                 put<int32_t>(kProbeFrame, 42);
                 put<uint64_t>(tr(5) + 0x1b0, A(kProbeVt));
                 kickf(s, "next frame");
                 put<uint64_t>(kCollision, 0);
                 put<int32_t>(kProbeFrame, -7);
                 exitf(h, "without world (flags cleared, nothing counted)");
                 put<uint64_t>(kCollision, 0x99);
                 put<int32_t>(kProbeFrame, -6);
                 kickf(s, "after exit without world");
                 vis.active = 1;
                 exitf(h, "with world, IsActive 1");
             });
}

// =================================================================================================================
// gameplay3

constexpr uint64_t kG3Vt = 0x100aa40, kG3Slot = 0x100aa48, kG3Enter = 0x5f0ea0, kG3Patch = 0x5f0b50, kG3Back = 0x5f0b67;
constexpr uint64_t kNetClock = 0x113a040, kMenuState = 0x1137654, kEvInit = 0xccf830, kEvFn = 0x5cd5c0;
constexpr uint64_t kScale = 0xe62288;

void *g3_event = nullptr;
U2MP_GUEST void h_ev_init(uint8_t *ev, uint32_t a, uint64_t b) {
    rec("guest event_init al16=%d zero=%d %u %llx", int(uintptr_t(ev) % 16 == 0), all_zero(ev, 0x100), a, (unsigned long long)b);
    g3_event = ev;
    for (int i = 0; i < 16; i++) ev[i] = uint8_t(0xa0 + i);
}
U2MP_GUEST void h_ev(void *menu, uint8_t *ev, uint32_t code) {
    rec("guest menu_event menu=%s same_event=%d ev=%s code=%u state=%u arg=%u fade=%s", where(menu).c_str(), ev == g3_event,
        hexd(ev, 16).c_str(), code, get<uint32_t>(kMenuState), get<uint32_t>(kMenuState + 4),
        fb(get<float>(kMenuState + 8)).c_str());
}
U2MP_GUEST void h_enter(void *state) {
    rec("guest ShowResults::Enter(0x5f0ea0) state=%s NET_SM=%d", where(state).c_str(), get<int32_t>(kNetSm));
}

void g3_setup() {
    put_bytes(0x5f0e90, {0x48, 0x8b, 0x47, 0x08, 0xc7, 0x80, 0x48, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0xc3});
    put_bytes(0x5f1264, {0x48, 0x8d, 0x1d, 0xd5, 0x53, 0xb4, 0x00, 0x8b, 0x83, 0x14, 0x10, 0x00, 0x00, 0x83, 0xf8, 0x06,
                         0x77, 0x0a, 0xb9, 0x61, 0x00, 0x00, 0x00});
    put_bytes(0x5f0af0, {0xc7, 0x05, 0x82, 0xde, 0xed, 0x00, 0x1e, 0x00, 0x00, 0x00});
    put_call(0x5f128e, kEvInit);
    put_call(0x5f129e, kEvFn);
    put_bytes(kG3Patch, {0x48, 0x8d, 0x05, 0xe9, 0x94, 0xb4, 0x00, 0x48, 0x8b, 0x00, 0xc5, 0xfa, 0x2a, 0x40, 0x0c, 0xc5,
                         0xfa, 0x59, 0x0d, 0x21, 0x17, 0x87, 0x00});
    put<uint32_t>(kScale, 0xBB5A740Eu);
    put<uint64_t>(kG3Slot, A(kG3Enter));
    put_bytes(kG3Back, {0xc5, 0xf9, 0x7e, 0xc8, 0xc3});   // vmovd eax,xmm1; ret: the patched path returns xmm1
}
using HEnter = void(U2MP_GUEST *)(void *);
HEnter g3_after() {
    hx::stub(kG3Enter, (void *)h_enter);
    hx::stub(kEvInit, (void *)h_ev_init);
    hx::stub(kEvFn, (void *)h_ev);
    const uint64_t v = get<uint64_t>(kG3Slot);
    return v >= A(0) && v < A(hx::kImageSize) ? nullptr : (HEnter)v;
}
uint8_t *pool_now() { return hx::exec_alloc(0); }
// the patch: stub bytes (host addresses by meaning) and the result of running the patched code path
void g3_check_patch(const char *what) {
    if (hx::mem[kG3Patch] != 0xe9) {
        rec("%s: no patch (%s)", what, hexd(hx::mem + kG3Patch, 5).c_str());
        return;
    }
    const uint8_t *stub = hx::mem + kG3Patch + 5 + get<int32_t>(kG3Patch + 1);
    std::string s = hexd(stub, 51);
    uint64_t clock, block, back;
    std::memcpy(&clock, stub + 2, 8);
    std::memcpy(&block, stub + 20, 8);
    std::memcpy(&back, stub + 43, 8);
    for (int i : {2, 20, 43}) s.replace(size_t(2 * i), 16, "................");
    const float *b = reinterpret_cast<const float *>(uintptr_t(block));
    rec("%s: stub %s at %s; clock %s; block al16=%d {%s,%s,%s,%s}; back %s", what, s.c_str(), where(stub).c_str(),
        where((void *)clock).c_str(), int(block % 16 == 0), fb(b[0]).c_str(), fb(b[1]).c_str(), fb(b[2]).c_str(),
        fb(b[3]).c_str(), where((void *)back).c_str());
}
void g3_run(int32_t frames, const char *what) {
    if (hx::mem[kG3Patch] != 0xe9) return;
    const uint64_t clk = salloc(16);
    put<int32_t>(clk + 0xc, frames);
    put<uint64_t>(kNetClock, A(clk));
    const uint32_t r = reinterpret_cast<uint32_t (*)()>(hx::mem + kG3Patch)();
    float f;
    std::memcpy(&f, &r, 4);
    rec("%s: patched update at frames %d -> xmm1 %08x (%g)", what, frames, r, double(f));
}
void g3_enter(HEnter h, uint32_t menu_state, int64_t frames /* <-1e12 = NULL clock */, const char *what) {
    rec("-- enter %s", what);
    put<uint32_t>(kMenuState, menu_state);
    put<uint32_t>(kMenuState + 4, 0x44);
    put<float>(kMenuState + 8, 0.25f);
    put<int32_t>(kNetSm, 25);
    if (frames < -1000000000000LL) put<uint64_t>(kNetClock, 0);
    else {
        const uint64_t clk = salloc(16);
        put<int32_t>(clk + 0xc, int32_t(frames));
        put<uint64_t>(kNetClock, A(clk));
    }
    g3_event = nullptr;
    if (!h) {
        rec("no enter hook");
        return;
    }
    h(hx::mem + 0x1660000);
    rec("after enter: NET_SM=%d state=%u arg=%u fade=%s", get<int32_t>(kNetSm), get<uint32_t>(kMenuState),
        get<uint32_t>(kMenuState + 4), fb(get<float>(kMenuState + 8)).c_str());
}
void g3_inst(const Side &s) {
    uint8_t *p0 = pool_now();
    inst(s.g3, "gameplay3");
    rec("near_stub bytes taken: %lld", (long long)(pool_now() - p0));
}

void gameplay3_tests() {
    scenario("gameplay3 install NULL image", [](const Side &s) {
        g3_setup();
        hx::set_image_both(nullptr, 0);
        g3_inst(s);
    });
    scenario("gameplay3 install image too small (silent)", [](const Side &s) {
        g3_setup();
        hx::set_image_both(hx::mem, kG3Vt + 0x1f);
        g3_inst(s);
    });
    scenario("gameplay3 install image too small for NET_SM (silent)", [](const Side &s) {
        g3_setup();
        hx::set_image_both(hx::mem, kNetSm + 3);
        g3_inst(s);
    });
    const uint64_t anchors[] = {0x5f0e90 + 14, 0x5f1264 + 22, 0x5f0af0 + 6};
    for (int k = 0; k < 3; k++) {
        static std::vector<std::string> names;
        names.push_back("gameplay3 install anchor differs #" + std::to_string(k));
        const uint64_t at = anchors[k];
        scenario(names.back().c_str(), [at](const Side &s) {
            g3_setup();
            hx::mem[at] ^= 1;
            g3_inst(s);
        });
    }
    scenario("gameplay3 install event-init call target wrong", [](const Side &s) {
        g3_setup();
        put_call(0x5f128e, kEvInit + 1);
        g3_inst(s);
    });
    scenario("gameplay3 install menu-event call not E8", [](const Side &s) {
        g3_setup();
        hx::mem[0x5f129e] = 0xe9;
        g3_inst(s);
    });
    scenario("gameplay3 install countdown bytes differ (Enter still +1)", [](const Side &s) {
        g3_setup();
        hx::mem[kG3Patch + 22] = 0x01;
        g3_inst(s);
        g3_check_patch("patch");
    });
    scenario("gameplay3 install scale constant differs", [](const Side &s) {
        g3_setup();
        put<float>(kScale, -1.0f / 299.0f);
        g3_inst(s);
    });
    scenario("gameplay3 install scale constant -0 vs +0 style (compare as float)", [](const Side &s) {
        g3_setup();
        put<uint32_t>(kScale, 0x3B5A740Eu);   // +1/300
        g3_inst(s);
    });
    scenario("gameplay3 install no stub memory", [](const Side &s) {
        g3_setup();
        hx::exec_left = 0;
        g3_inst(s);
    });
    scenario("gameplay3 install Enter slot wrong (countdown +1)", [](const Side &s) {
        g3_setup();
        put<uint64_t>(kG3Slot, A(kG3Enter + 1));
        g3_inst(s);
        g3_check_patch("patch");
        g3_run(600, "before any Enter");
    });
    scenario("gameplay3 install everything (+2); stub; Enter sequence; countdown", [](const Side &s) {
        g3_setup();
        g3_inst(s);
        HEnter h = g3_after();
        g3_check_patch("patch");
        g3_run(0, "end from earlier");
        g3_enter(h, 0, 300, "menu 0, frames 300");
        g3_run(300, "after Enter");
        g3_run(-900, "after Enter, negative frames");
        g3_check_patch("patch after Enter");
    });
    scenario("gameplay3 Enter: menu states and clocks", [](const Side &s) {
        g3_setup();
        g3_inst(s);
        HEnter h = g3_after();
        g3_enter(h, 5, 1, "menu 5");
        g3_enter(h, 6, -2, "menu 6");
        g3_enter(h, 1, 7, "menu 1");
        g3_enter(h, 4, -1000000LL - 1, "menu 4, negative frames");
        g3_enter(h, 7, 123456789, "menu 7, big frames");
        g3_enter(h, 0xffffffffu, -2000000000000LL, "menu ffffffff, NULL clock");
        g3_enter(h, 2, 2147483647LL, "menu 2, INT_MAX frames");
        g3_run(1000, "after the Enters");
    });
}

// =================================================================================================================
// matchend

constexpr uint64_t kMeSlot = 0x100ab98, kMeOrig = 0x5f14d0, kRealFrames = 0x1116384, kScorePtr = 0x104ae70;
constexpr uint64_t kDebugOff = 0x104f4f8, kCash = 0x11475d0;
using HUpd = uint64_t(U2MP_GUEST *)(void *);
U2MP_GUEST uint64_t h_session_update(void *state) {
    rec("guest session Playing::Update %s timelimit=%s", where(state).c_str(), fb(get<float>(kNgm + 0x21b8)).c_str());
    return 0x5e55105ull;
}
void me_state() {
    put<int32_t>(kRealFrames, 90000);
    put<int32_t>(kNgm + 0x1d5c, 30000);
    put<int32_t>(kNgm + 0x1d60, 3000);
    put<int32_t>(kNgm + 0x1c9c, 2);
    put<int16_t>(kGame, 1);
    put<int32_t>(kNgm + 0x1d74, 1);
    put<int32_t>(kNgm + 0x21c0, 3);
    put<int32_t>(kNgm + 0x1e80, 2);
    put<int32_t>(kNgm + 0x1e84, 0);
    put<int32_t>(kNgm + 0x1e88, 0);
    put<int32_t>(kNgm + 0x21b4, 50);
    put<float>(kNgm + 0x21b8, 600.0f);
    put<int32_t>(kNetSm, 20);
    put<int32_t>(kCash, 12345);
    // *net-games*: 3 records, game types 4, 7, -2
    const uint64_t recs = salloc(0x90 * 3);
    for (int i = 0; i < 3; i++) put<int32_t>(recs + 0x90 * i + 0xc, i == 0 ? 4 : i == 1 ? 7 : -2);
    const uint64_t t = salloc(16);
    put<int32_t>(t, 3);
    put<uint64_t>(t + 8, A(recs));
    lookups[kNetGamesId] = t;
    const uint64_t sc = salloc(0x3dee + 0x34 * 10, 16);
    for (int i = 0; i < 10; i++) put<uint16_t>(sc + 0x3dee + 0x34 * i, uint16_t(i == 1 ? 65535 : 10 * i + 3));
    put<uint64_t>(kScorePtr, A(sc));
}
HUpd me_install(const Side &s, const char *env, bool wrapped = true) {
    hx::stub(kLookup, (void *)h_lookup);
    if (wrapped) put<uint64_t>(kMeSlot, A(gstub((void *)h_session_update)));
    if (env) setenv_("UNCHARTED_U2MP_TIME_LIMIT", env);
    inst(s.me, "matchend");
    const uint64_t v = get<uint64_t>(kMeSlot);
    return v >= A(0) && v < A(hx::kImageSize) ? nullptr : (HUpd)v;
}
void upd(HUpd h, long long dms, const char *what) {
    rec("-- update %s", what);
    tick(dms * 1000000LL);
    if (!h) {
        rec("no update hook");
        return;
    }
    const uint64_t r = h(hx::mem + 0x1670000);
    rec("update returned %llx; timelimit=%s", (unsigned long long)r, fb(get<float>(kNgm + 0x21b8)).c_str());
}

void matchend_tests() {
    scenario("matchend install NULL image", [](const Side &s) {
        hx::set_image_both(nullptr, 0);
        inst(s.me, "matchend");
    });
    scenario("matchend install image too small", [](const Side &s) {
        put<uint64_t>(kMeSlot, A(0x1234));
        hx::set_image_both(hx::mem, kMeSlot + 7);
        setenv_("UNCHARTED_U2MP_TIME_LIMIT", "30");   // not even read
        inst(s.me, "matchend");
    });
    scenario("matchend install slot 0", [](const Side &s) { me_install(s, nullptr, false); });
    scenario("matchend install slot = 0x5f14d0", [](const Side &s) {
        put<uint64_t>(kMeSlot, A(kMeOrig));
        me_install(s, nullptr, false);
    });
    scenario("matchend install slot = 0x5f14d0 as offset (not absolute: wrapped)", [](const Side &s) {
        put<uint64_t>(kMeSlot, kMeOrig);
        me_install(s, nullptr, false);
    });
    for (const char *env : {"abc", "5", "4000", "9.99", "nan", "-30"}) {
        static std::vector<std::string> names;
        names.push_back(std::string("matchend install env ") + env + " (ignored)");
        scenario(names.back().c_str(), [env](const Side &s) {
            me_state();
            HUpd h = me_install(s, env);
            upd(h, 0, "first");
        });
    }
    scenario("matchend first update without test limit: logs (end state changed), visibility_kick after update",
             [](const Side &s) {
                 me_state();
                 HUpd h = me_install(s, nullptr);
                 // visibility ready, one remote tracker: its guest calls come after the original update
                 vis_setup();
                 inst(s.vis, "visibility");
                 vis_after();
                 hx::stub(kLookup, (void *)h_lookup);
                 vis_world(20, true, 500, 0);
                 vis_tracker(0, true, 1);
                 vis_tracker(4, true, 5);
                 upd(h, 1, "first");
                 upd(h, 100, "within 10 s");
                 upd(h, 9898, "just before 10 s");
                 upd(h, 1, "at 10 s");
                 upd(h, 5000, "within");
                 put<int32_t>(kNgm + 0x1e84, 1);
                 upd(h, 1, "end A changed");
                 put<int32_t>(kNgm + 0x1e88, 2);
                 upd(h, 1, "end B changed");
                 put<int32_t>(kNgm + 0x1e80, 3);
                 upd(h, 1, "state changed");
                 upd(h, 9999, "9999 ms after the change");
                 upd(h, 1, "10000 ms after the change");
             });
    scenario("matchend env 10 (used) and limit handling", [](const Side &s) {
        me_state();
        HUpd h = me_install(s, "10");
        upd(h, 20000, "limit 600 -> 10");
        upd(h, 20000, "limit already 10");
        put<float>(kNgm + 0x21b8, -0.0f);
        upd(h, 20000, "limit -0 -> 10");
    });
    scenario("matchend env 3600 (used)", [](const Side &s) {
        me_state();
        HUpd h = me_install(s, "3600");
        upd(h, 20000, "600 -> 3600");
    });
    scenario("matchend env 30 then a second install with 5", [](const Side &s) {
        me_state();
        HUpd h = me_install(s, "30");
        upd(h, 20000, "-> 30");
        h = me_install(s, "5");
        put<float>(kNgm + 0x21b8, 45.5f);
        upd(h, 20000, "45.5 -> test limit");
    });
    scenario("matchend log contents: lookup NULL, game -1, game >= count, records NULL, teams 0/3/9, no score table, debug, "
             "negative / wrapping frame differences",
             [](const Side &s) {
                 me_state();
                 HUpd h = me_install(s, nullptr);
                 auto change = [](int v) { put<int32_t>(kNgm + 0x1e84, v); };
                 upd(h, 20000, "base (type 7)");
                 put<int16_t>(kGame, 0);
                 put<int32_t>(kNgm + 0x1c9c, 3);
                 change(10);
                 upd(h, 1, "game 0, teams 3");
                 put<int16_t>(kGame, 2);
                 put<int32_t>(kNgm + 0x1c9c, 9);
                 change(11);
                 upd(h, 1, "game 2 (type -2), teams 9");
                 put<int16_t>(kGame, -1);
                 put<int32_t>(kNgm + 0x1c9c, 0);
                 change(12);
                 upd(h, 1, "game -1, teams 0");
                 put<int16_t>(kGame, 3);
                 put<int32_t>(kNgm + 0x1c9c, -4);
                 change(13);
                 upd(h, 1, "game == count, teams -4");
                 put<int16_t>(kGame, 1);
                 put<uint64_t>(lookups[kNetGamesId] + 8, 0);
                 put<int32_t>(kNgm + 0x1c9c, 1);
                 put<uint64_t>(kScorePtr, 0);
                 change(14);
                 upd(h, 1, "records NULL, score table NULL");
                 lookups[kNetGamesId] = 0;
                 hx::mem[kDebugOff] = 1;
                 put<int32_t>(kRealFrames, -5);
                 put<int32_t>(kNgm + 0x1d5c, 2147483600);
                 put<int32_t>(kNgm + 0x1d60, -2147483600);
                 put<float>(kNgm + 0x21b8, 1e10f);
                 change(15);
                 upd(h, 1, "lookup NULL, debug, wrapping differences");
                 put<int32_t>(kRealFrames, 100);
                 put<int32_t>(kNgm + 0x1d5c, 400);
                 put<float>(kNgm + 0x21b8, 299.5f);
                 change(-1);
                 put<int32_t>(kNgm + 0x1e88, -1);
                 put<int32_t>(kNgm + 0x1e80, -1);
                 upd(h, 1, "all -1, negative difference");
             });
}

// =================================================================================================================
// spawn

constexpr uint64_t kSpCall = 0x662fd0, kPkgTable = 0x1130390;
using HSp = const uint8_t *(U2MP_GUEST *)(void *, uint32_t, int32_t);

struct Tag {
    uint32_t key;
    int32_t value;
    uint16_t type = 1;
    bool null_value = false, null_data = false;
};
uint64_t mk_tags(const std::vector<Tag> &tags, int32_t count = -1, bool null_entries = false) {
    const uint64_t list = salloc(16);
    const uint64_t entries = salloc(16 * std::max<size_t>(tags.size(), 1));
    for (size_t i = 0; i < tags.size(); i++) {
        put<uint32_t>(entries + 16 * i, tags[i].key);
        if (tags[i].null_value) continue;
        const uint64_t v = salloc(16), d = salloc(4, 4);
        put<uint16_t>(v, tags[i].type);
        put<uint16_t>(v + 2, 4);
        put<int32_t>(d, tags[i].value);
        if (!tags[i].null_data) put<uint64_t>(v + 8, A(d));
        put<uint64_t>(entries + 16 * i + 8, A(v));
    }
    put<uint64_t>(list, null_entries ? 0 : A(entries));
    put<int32_t>(list + 8, count >= 0 ? count : int32_t(tags.size()));
    return list;
}
constexpr uint32_t kOnline = 0x5bfae217, kStart = 0x33364361;
uint64_t mk_spawner(const char *name, uint32_t id, uint64_t tags = 0) {
    const uint64_t sp = salloc(0x90);
    if (name) put<uint64_t>(sp + 0x20, A(mk_str(name)));
    put<uint32_t>(sp + 0x28, id);
    if (tags) put<uint64_t>(sp + 0x68, A(tags));
    put<float>(sp + 0x1c, 1.0f);
    return sp;
}
uint64_t online(const char *name, uint32_t id, bool start) {
    return mk_spawner(name, id, mk_tags({{kOnline, 1}, {kStart, start ? 1 : 0}}));
}
struct GroupSpec {
    std::vector<uint64_t> all;   // 0 = NULL entry
    int32_t count = -1;          // -1 = all.size()
    bool null_all = false;
    std::vector<std::pair<uint32_t, uint64_t>> sorted;   // id, spawner offset (0 = NULL)
    int32_t sorted_count = -1;
    bool null_sorted = false;
};
uint64_t mk_group(const GroupSpec &g) {
    const uint64_t grp = salloc(0x28);
    const uint64_t all = salloc(8 * std::max<size_t>(g.all.size(), 1));
    for (size_t i = 0; i < g.all.size(); i++) put<uint64_t>(all + 8 * i, g.all[i] ? A(g.all[i]) : 0);
    const uint64_t srt = salloc(16 * std::max<size_t>(g.sorted.size(), 1));
    for (size_t i = 0; i < g.sorted.size(); i++) {
        put<uint32_t>(srt + 16 * i, g.sorted[i].first);
        put<uint64_t>(srt + 16 * i + 8, g.sorted[i].second ? A(g.sorted[i].second) : 0);
    }
    put<uint64_t>(grp + 8, g.null_all ? 0 : A(all));
    put<int32_t>(grp + 0x10, g.count >= 0 ? g.count : int32_t(g.all.size()));
    put<uint64_t>(grp + 0x18, g.null_sorted ? 0 : A(srt));
    put<int32_t>(grp + 0x20, g.sorted_count >= 0 ? g.sorted_count : int32_t(g.sorted.size()));
    return grp;
}
uint64_t mk_pkg(uint64_t group) {
    const uint64_t p = salloc(0x9b0);
    if (group) put<uint64_t>(p + 0x9a0, A(group));
    return p;
}
uint64_t pkg_with(const std::vector<uint64_t> &sps) {   // all = sps, sorted = their ids sorted
    GroupSpec g;
    g.all = sps;
    std::vector<std::pair<uint32_t, uint64_t>> v;
    for (uint64_t s : sps)
        if (s) v.push_back({get<uint32_t>(s + 0x28), s});
    std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first < b.first; });
    g.sorted = v;
    return mk_pkg(mk_group(g));
}
// package table: buckets of node package offsets (0 = node with a NULL package)
struct TableSpec {
    std::vector<std::vector<uint64_t>> buckets;
    int32_t count = -1;
    bool null_buckets = false;
    int end_after = -1;            // bucket 0: the chain reaches the end marker after this many nodes (rest hidden)
};
void mk_table(const TableSpec &t) {
    const size_t n = t.buckets.size();
    const uint64_t arr = salloc(8 * (n + 1));
    const uint64_t end = salloc(0x20);
    put<uint64_t>(arr + 8 * n, A(end));
    for (size_t b = 0; b < n; b++) {
        uint64_t next = 0;
        std::vector<uint64_t> nodes;
        for (size_t i = 0; i < t.buckets[b].size(); i++) nodes.push_back(salloc(0x20));
        for (size_t i = nodes.size(); i-- > 0;) {
            put<uint64_t>(nodes[i], next);
            put<uint32_t>(nodes[i] + 0x10, uint32_t(0x9000 + b * 100 + i));
            if (t.buckets[b][i]) put<uint64_t>(nodes[i] + 0x18, A(t.buckets[b][i]));
            next = A(nodes[i]);
            if (b == 0 && int(i) == t.end_after) {   // end marker before node i: nodes i.. hang off the marker
                put<uint64_t>(end, next);
                next = A(end);
            }
        }
        put<uint64_t>(arr + 8 * b, next);
    }
    put<uint64_t>(kPkgTable + 0x10, t.null_buckets ? 0 : A(arr));
    put<int32_t>(kPkgTable + 0x4c, t.count >= 0 ? t.count : int32_t(n));
}
void mk_buckets(const std::vector<std::vector<uint64_t>> &b) {
    TableSpec t;
    t.buckets = b;
    mk_table(t);
}
struct Level {
    uint32_t id;
    const char *name;   // nullptr = NULL
    uint32_t pkg;
};
// net-games records (level ids), levels; game index
void mk_levels(const std::vector<uint32_t> &games, const std::vector<Level> &levels, int16_t game, bool null_games_recs = false,
               bool null_level_recs = false, int32_t games_count = -1) {
    const uint64_t gr = salloc(0x90 * std::max<size_t>(games.size(), 1));
    for (size_t i = 0; i < games.size(); i++) put<uint32_t>(gr + 0x90 * i + 4, games[i]);
    const uint64_t gt = salloc(16);
    put<int32_t>(gt, games_count >= 0 ? games_count : int32_t(games.size()));
    if (!null_games_recs) put<uint64_t>(gt + 8, A(gr));
    const uint64_t lr = salloc(0x38 * std::max<size_t>(levels.size(), 1));
    for (size_t i = 0; i < levels.size(); i++) {
        put<uint32_t>(lr + 0x38 * i, levels[i].id);
        if (levels[i].name) put<uint64_t>(lr + 0x38 * i + 0x18, A(mk_str(levels[i].name)));
        put<uint32_t>(lr + 0x38 * i + 0x20, levels[i].pkg);
    }
    const uint64_t lt = salloc(16);
    put<int32_t>(lt, int32_t(levels.size()));
    if (!null_level_recs) put<uint64_t>(lt + 8, A(lr));
    lookups[kNetGamesId] = gt;
    lookups[kLevelsId] = lt;
    put<int16_t>(kGame, game);
}
void sp_bytes() {
    put_bytes(0x662fc9, {0x48, 0x8d, 0x3d});
    put<int32_t>(0x662fcc, int32_t(kNgm - kSpCall));
    put_call(kSpCall, 0x644130);
    put_bytes(0x662fd5, {0x4c, 0x8b, 0x88, 0xa0, 0x09, 0x00, 0x00});
}
HSp sp_install(const Side &s) {
    sp_bytes();
    hx::stub(kLookup, (void *)h_lookup);
    inst(s.spawn, "spawn");
    return (HSp)hx::retarget_host(kSpCall);
}
const uint8_t *last_own[2] = {nullptr, nullptr};

std::string describe_spawner(const uint8_t *sp) {
    if (sp >= hx::mem && sp < hx::mem + hx::kImageSize) return where(sp);
    // the own origin spawner: by content
    uint8_t copy[0x90];
    std::memcpy(copy, sp, 0x90);
    const char *name;
    std::memcpy(&name, sp + 0x20, 8);
    std::memset(copy + 0x20, 0, 8);
    return std::string("own spawner{") + hexd(copy, 0x90) + " name=" + (name ? name : "NULL") + "}";
}
void spc(HSp h, uint32_t id, int32_t mode, const char *what) {
    rec("-- spawn %s: id %08x mode %d", what, id, mode);
    if (!h) {
        rec("no hook");
        return;
    }
    const uint8_t *r = h(hx::mem + kNgm, id, mode);
    if (!r) {
        rec("result NULL");
        return;
    }
    if (r >= hx::mem && r < hx::mem + hx::kImageSize) {
        rec("result %s", where(r).c_str());
        return;
    }
    // the own package: all zero but +0x9a0 -> own group {sorted -> entry, count 1}
    uint8_t copy[0x9b0];
    std::memcpy(copy, r, sizeof copy);
    uint64_t grp;
    std::memcpy(&grp, copy + 0x9a0, 8);
    std::memset(copy + 0x9a0, 0, 8);
    const uint8_t *g = reinterpret_cast<const uint8_t *>(uintptr_t(grp));
    uint8_t gc[0x28];
    std::memcpy(gc, g, sizeof gc);
    uint64_t ent;
    std::memcpy(&ent, gc + 0x18, 8);
    std::memset(gc + 0x18, 0, 8);
    const uint8_t *e = reinterpret_cast<const uint8_t *>(uintptr_t(ent));
    uint32_t eid, pad;
    std::memcpy(&eid, e, 4);
    std::memcpy(&pad, e + 4, 4);
    const uint8_t *sp;
    std::memcpy(&sp, e + 8, 8);
    const bool same_pkg = last_own[g_side] == nullptr || last_own[g_side] == r;
    last_own[g_side] = r;
    rec("result own package (rest zero=%d, reused=%d, group rest=%s, entry id %08x pad %x, spawner %s)",
        all_zero(copy, sizeof copy), same_pkg, hexd(gc, 0x28).c_str(), eid, pad, describe_spawner(sp).c_str());
}

void spawn_tests() {
    scenario("spawn install lea bytes differ", [](const Side &s) {
        sp_bytes();
        hx::mem[0x662fcb] = 0x35;
        inst(s.spawn, "spawn");
    });
    scenario("spawn install lea target wrong", [](const Side &s) {
        sp_bytes();
        put<int32_t>(0x662fcc, int32_t(kNgm - kSpCall + 8));
        inst(s.spawn, "spawn");
    });
    scenario("spawn install group read bytes differ", [](const Side &s) {
        sp_bytes();
        hx::mem[0x662fd5 + 4] = 0x0a;
        inst(s.spawn, "spawn");
    });
    scenario("spawn install retarget_call fails", [](const Side &s) {
        sp_bytes();
        hx::fail_offsets.push_back(kSpCall);
        inst(s.spawn, "spawn");
    });
    scenario("spawn install success", [](const Side &s) { sp_install(s); });

    // level record failures (each falls back; no table: origin)
    scenario("spawn level: net-games lookup NULL -> origin", [](const Side &s) {
        HSp h = sp_install(s);
        mk_levels({0x10}, {{0x10, "lvl-pak", 0x500}}, 0);
        lookups[kNetGamesId] = 0;
        spc(h, 0x11, 0, "a");
        spc(h, 0x11, 1, "same id again (no spawner log)");
    });
    scenario("spawn level: levels lookup NULL / records NULL / game -1 / game >= count / no match / NULL name", [](const Side &s) {
        HSp h = sp_install(s);
        mk_levels({0x10}, {{0x10, "lvl-pak", 0x500}}, 0);
        lookups[kLevelsId] = 0;
        spc(h, 0x12, 0, "levels NULL");
        mk_levels({0x10}, {{0x10, "lvl-pak", 0x500}}, 0, true);
        spc(h, 0x13, 0, "net-games records NULL");
        mk_levels({0x10}, {{0x10, "lvl-pak", 0x500}}, 0, false, true);
        spc(h, 0x14, 0, "levels records NULL");
        mk_levels({0x10}, {{0x10, "lvl-pak", 0x500}}, -1);
        spc(h, 0x15, 0, "game -1");
        mk_levels({0x10}, {{0x10, "lvl-pak", 0x500}}, 1);
        spc(h, 0x16, 0, "game == count");
        mk_levels({0x10, 0x20}, {{0x10, "lvl-pak", 0x500}}, 1, false, false, 1);
        spc(h, 0x17, 0, "game >= count (count 1)");
        mk_levels({0x10}, {{0x11, "lvl-pak", 0x500}, {0x12, "x", 1}}, 0);
        spc(h, 0x18, 0, "no matching level");
        mk_levels({0x10}, {{0x10, nullptr, 0x500}, {0x10, "second-pak", 0x501}}, 0);
        spc(h, 0x19, 0, "first match NULL name");
    });
    scenario("spawn level names: gompa-multi-pak (missing), again, -pak, abc, empty", [](const Side &s) {
        HSp h = sp_install(s);
        gp_package_by_id = pkg_by_id;
        mk_levels({0x10, 0x20, 0x30, 0x40}, {{0x10, "gompa-multi-pak", 0x501}, {0x20, "-pak", 0x502}, {0x30, "abc", 0x503}, {0x40, "", 0x504}}, 0);
        spc(h, 0x21, 0, "gompa-multi-pak");
        spc(h, 0x22, 1, "same level again");
        put<int16_t>(kGame, 1);
        spc(h, 0x23, 0, "-pak");
        put<int16_t>(kGame, 2);
        packages[0x503] = pkg_with({mk_spawner("pickup-x", 0x77)});
        spc(h, 0x24, 0, "abc (pak loaded, no id, no online)");
        put<int16_t>(kGame, 3);
        spc(h, 0x25, 0, "empty name");
        put<int16_t>(kGame, 0);
        spc(h, 0x26, 0, "gompa again after empty (empty did not change the last name)");
    });
    scenario("spawn found: in pak, in map package only, in another package only, NULL-spawner entry", [](const Side &s) {
        HSp h = sp_install(s);
        gp_package_by_id = pkg_by_id;
        mk_levels({0x10}, {{0x10, "dock-pak", 0x601}}, 0);
        const uint64_t a = mk_spawner("online-a", 0x100), b = mk_spawner("online-b", 0x200), c = mk_spawner("c", 0x300);
        packages[0x601] = pkg_with({a, 0, c});
        packages[u2mp::sid("dock")] = pkg_with({b});
        GroupSpec gn;
        gn.all = {a};
        gn.sorted = {{0x50, a}, {0x400, 0}};
        const uint64_t nullent = mk_pkg(mk_group(gn));
        const uint64_t other = pkg_with({mk_spawner("other", 0x400), mk_spawner("other2", 0x401)});
        mk_buckets({{0, nullent}, {}, {other}});
        spc(h, 0x100, 0, "in pak");
        spc(h, 0x300, 1, "in pak (last)");
        spc(h, 0x200, 0, "map only");
        spc(h, 0x400, 0, "other package only (NULL spawner entry skipped)");
        spc(h, 0x401, 0, "other package");
        spc(h, 0x50, 0, "table package with id 0x50");
    });
    scenario("spawn lower bound on duplicates / unsorted entries; counts 0 and 100001; NULL sorted", [](const Side &s) {
        HSp h = sp_install(s);
        gp_package_by_id = pkg_by_id;
        mk_levels({0x10}, {{0x10, "u-pak", 0x701}}, 0);
        const uint64_t s1 = mk_spawner("s1", 1), s2 = mk_spawner("s2", 2), s3 = mk_spawner("s3", 3), s4 = mk_spawner("s4", 4);
        GroupSpec g;
        g.all = {s1, s2, s3, s4};
        g.sorted = {{5, s1}, {3, s2}, {9, s3}, {9, s4}, {12, s1}, {2, s2}, {20, s3}};
        packages[0x701] = mk_pkg(mk_group(g));
        GroupSpec z = g;
        z.sorted_count = 0;
        GroupSpec big = g;
        big.sorted_count = 100001;
        GroupSpec ns = g;
        ns.null_sorted = true;
        GroupSpec ok;
        ok.all = {s1};
        ok.sorted = {{77, s2}};
        mk_buckets({{mk_pkg(mk_group(z)), mk_pkg(mk_group(big)), mk_pkg(mk_group(ns)), mk_pkg(0)}, {mk_pkg(mk_group(ok))}});
        for (uint32_t id : {5u, 3u, 9u, 12u, 2u, 20u, 4u, 0u, 21u, 77u}) {
            char b[32];
            std::snprintf(b, sizeof b, "id %u", id);
            spc(h, id, 1, b);
        }
    });
    scenario("spawn fallback a: online start spawners in pak, mode 0 rotating; mode 1; no starts", [](const Side &s) {
        HSp h = sp_install(s);
        gp_package_by_id = pkg_by_id;
        mk_levels({0x10}, {{0x10, "fb-pak", 0x801}}, 0);
        const uint64_t o1 = online("online-1", 0xa1, true), o2 = online("online-2", 0xa2, false),
                       o3 = online("online-3", 0xa3, true), o4 = mk_spawner("plain", 0xa4),
                       o5 = online("online-5", 0xa5, true);
        packages[0x801] = pkg_with({o1, 0, o2, o3, o4, o5});
        for (int i = 0; i < 4; i++) spc(h, 0xf00 + i, 0, "mode 0 starts");
        for (int i = 0; i < 4; i++) spc(h, 0xf10 + i, 1, "mode 1 online");
        spc(h, 0xf10 + 3, 2, "mode 2 same id (no log)");
        packages[0x801] = pkg_with({o2, o4, online("online-6", 0xa6, false)});
        for (int i = 0; i < 3; i++) spc(h, 0xf20 + i, 0, "mode 0 without starts");
    });
    scenario("spawn fallback b/c/d: loaded package online, map prefixes, all[0], origin", [](const Side &s) {
        HSp h = sp_install(s);
        gp_package_by_id = pkg_by_id;
        mk_levels({0x10}, {{0x10, "fbx-pak", 0x901}}, 0);   // pak missing
        const uint64_t t1 = pkg_with({mk_spawner("x", 1)});                              // no online
        const uint64_t t2 = pkg_with({online("o-b1", 0xb1, false), online("o-b2", 0xb2, true)});
        mk_buckets({{t1}, {0, t2}});
        for (int i = 0; i < 3; i++) spc(h, 0xb00 + i, 0, "b: loaded package (mode 0)");
        spc(h, 0xb10, 1, "b: loaded package (mode 1)");
        mk_buckets({{t1}});
        const uint64_t cp = mk_spawner("continue-player-1", 0xc1), on = mk_spawner("online-x", 0xc2),
                       npc = mk_spawner("npc-guard", 0xc3), pk = mk_spawner("pickup-ammo", 0xc4),
                       pk2 = mk_spawner("pickup-gun", 0xc5), nn = mk_spawner(nullptr, 0xc6), q = mk_spawner("?", 0xc7);
        packages[u2mp::sid("fbx")] = pkg_with({cp, on, npc, pk, nn, pk2});
        for (int i = 0; i < 3; i++) spc(h, 0xc00 + i, 0, "c: pickup-");
        packages[u2mp::sid("fbx")] = pkg_with({cp, on, npc, nn, on});
        spc(h, 0xc10, 0, "c: npc-");
        packages[u2mp::sid("fbx")] = pkg_with({cp, nn, on, cp, on});
        for (int i = 0; i < 2; i++) spc(h, 0xc20 + i, 0, "c: online-");
        packages[u2mp::sid("fbx")] = pkg_with({nn, cp, q});
        spc(h, 0xc30, 0, "c: continue-player");
        packages[u2mp::sid("fbx")] = pkg_with({q, nn});
        spc(h, 0xc40, 0, "c: none matches: all[0]");
        packages[u2mp::sid("fbx")] = pkg_with({0, q});
        spc(h, 0xd00, 0, "d: all[0] NULL -> origin");
        GroupSpec g;
        g.all = {q};
        g.count = 0;
        packages[u2mp::sid("fbx")] = mk_pkg(mk_group(g));
        spc(h, 0xd01, 0, "d: map group count 0");
        g.count = 100001;
        packages[u2mp::sid("fbx")] = mk_pkg(mk_group(g));
        spc(h, 0xd02, 0, "d: map group count 100001");
        g.count = 1;
        g.null_all = true;
        packages[u2mp::sid("fbx")] = mk_pkg(mk_group(g));
        spc(h, 0xd03, 0, "d: map all NULL");
        packages[u2mp::sid("fbx")] = mk_pkg(0);
        spc(h, 0xd04, 0, "d: map group NULL");
        packages.erase(u2mp::sid("fbx"));
        spc(h, 0xd05, 0, "d: no map package");
        spc(h, 0xd05, 1, "d: same id (no log)");
    });
    scenario("spawn tags: missing key, wrong type, NULL value, NULL data, value 2, count 4097/0, NULL entries/list",
             [](const Side &s) {
                 HSp h = sp_install(s);
                 gp_package_by_id = pkg_by_id;
                 mk_levels({0x10}, {{0x10, "tg-pak", 0xa01}}, 0);
                 std::vector<uint64_t> sps;
                 sps.push_back(mk_spawner("t-missing", 0xe1, mk_tags({{kStart, 1}})));
                 sps.push_back(mk_spawner("t-type2", 0xe2, mk_tags({{kOnline, 1, 2}, {kStart, 1}})));
                 sps.push_back(mk_spawner("t-nullval", 0xe3, mk_tags({{kOnline, 1, 1, true}, {kStart, 1}})));
                 sps.push_back(mk_spawner("t-nulldata", 0xe4, mk_tags({{kOnline, 1, 1, false, true}, {kStart, 1}})));
                 sps.push_back(mk_spawner("t-val2", 0xe5, mk_tags({{kOnline, 2}, {kStart, 1}})));
                 sps.push_back(mk_spawner("t-4097", 0xe6, mk_tags({{kOnline, 1}, {kStart, 1}}, 4097)));
                 sps.push_back(mk_spawner("t-0", 0xe7, mk_tags({{kOnline, 1}, {kStart, 1}}, 0)));
                 sps.push_back(mk_spawner("t-nullent", 0xe8, mk_tags({{kOnline, 1}, {kStart, 1}}, -1, true)));
                 sps.push_back(mk_spawner("t-nolist", 0xe9));
                 sps.push_back(mk_spawner("t-dupkey", 0xea, mk_tags({{kOnline, 0}, {kOnline, 1}, {kStart, 1}})));
                 packages[0xa01] = pkg_with(sps);
                 mk_buckets({});
                 spc(h, 0xe00, 0, "no candidate in pak: map pick (no map) -> origin");
                 sps.push_back(mk_spawner("t-ok-nostart", 0xeb, mk_tags({{kStart, 0}, {kOnline, 1}})));
                 sps.push_back(mk_spawner("t-ok-4096", 0xec, mk_tags({{kOnline, 1}, {kStart, 1}, {kStart, 0}}, 3)));
                 packages[0xa01] = pkg_with(sps);
                 for (int i = 0; i < 3; i++) spc(h, 0xe10 + i, 0, "mode 0");
                 for (int i = 0; i < 3; i++) spc(h, 0xe20 + i, 1, "mode 1");
             });
    scenario("spawn package walk: end marker, NULL package nodes, bucket count bounds, chain > 4096 nodes", [](const Side &s) {
        HSp h = sp_install(s);
        gp_package_by_id = pkg_by_id;
        lookups.clear();
        const uint64_t hidden = pkg_with({mk_spawner("hidden", 0x55)});
        const uint64_t vis1 = pkg_with({mk_spawner("vis", 0x66)});
        TableSpec t;
        t.buckets = {{vis1, 0, hidden}, {0}};
        t.end_after = 2;   // vis1, NULL, end marker -> hidden
        mk_table(t);
        spc(h, 0x66, 0, "visible");
        spc(h, 0x55, 0, "behind the end marker");
        t.count = 0;
        mk_table(t);
        spc(h, 0x66, 1, "count 0");
        t.count = (1 << 20) + 1;
        mk_table(t);
        spc(h, 0x66, 1, "count 2^20+1");
        t.count = -1;
        t.null_buckets = true;
        mk_table(t);
        spc(h, 0x66, 1, "NULL buckets");
        TableSpec lng;
        std::vector<uint64_t> chain(4097, 0);
        chain[4095] = vis1;
        chain[4096] = hidden;
        lng.buckets = {chain, {pkg_with({mk_spawner("b1", 0x77)})}};
        mk_table(lng);
        spc(h, 0x66, 0, "node 4096 visited");
        spc(h, 0x55, 0, "node 4097 not visited");
        spc(h, 0x77, 0, "next bucket still walked");
    });
}

// =================================================================================================================
// scripttime

const uint8_t kStExpected[86] = {
    0xF6, 0x80, 0x68, 0x01, 0x00, 0x00, 0x08, 0x74, 0x37, 0x48, 0x8B, 0x05, 0x15, 0x67, 0xB2, 0x00, 0x48, 0x85,
    0xC0, 0x74, 0x27, 0x48, 0x8D, 0x3D, 0x0A, 0x20, 0x48, 0x00, 0x48, 0x8D, 0x35, 0xA4, 0xDA, 0x45, 0x00, 0x48,
    0x8D, 0x15, 0xDA, 0xDA, 0x45, 0x00, 0x48, 0x8D, 0x0D, 0xF0, 0xCA, 0x3A, 0x00, 0x41, 0xB8, 0x9B, 0x02, 0x00,
    0x00, 0x45, 0x31, 0xC9, 0xFF, 0xD0, 0xCD, 0x41, 0x5D, 0xC3, 0x48, 0x8D, 0x05, 0xE6, 0xE8, 0x6C, 0x00, 0xC5,
    0xFA, 0x2A, 0x40, 0x50, 0xC5, 0xFA, 0x59, 0x05, 0xF5, 0xFC, 0x41, 0x00, 0x5D, 0xC3};
void st_setup() {
    std::memcpy(hx::mem + 0xa47853, kStExpected, 86);
    put<uint32_t>(0xe6759c, 0x3B5A740Eu);
}
void st_show() {
    rec("bytes a47853..a478a9: %s", hexd(hx::mem + 0xa47853, 86).c_str());
    if (hx::mem[0xa47863] != 0xc5) return;
    // run the patched net-timer path: push rbp; jmp 0xa4785c (returns through pop rbp; ret at 0xa478a7)
    const uint64_t entry = salloc(16);
    hx::mem[entry] = 0x55;
    hx::mem[entry + 1] = 0xe9;
    put<int32_t>(entry + 2, int32_t(int64_t(0xa4785c) - int64_t(entry + 6)));
    for (int32_t d : {0, 5, -3, 300, 2147483647}) {
        put<int32_t>(0x1116390, d);
        const float r = reinterpret_cast<float (*)()>(hx::mem + entry)();
        rec("net-timer delta %d -> %s", d, fb(r).c_str());
    }
}
void scripttime_tests() {
    scenario("scripttime bytes match (+1)", [](const Side &s) {
        st_setup();
        inst(s.st, "scripttime");
        st_show();
    });
    for (int at : {0, 9, 42, 85}) {
        static std::vector<std::string> names;
        names.push_back("scripttime byte " + std::to_string(at) + " differs");
        scenario(names.back().c_str(), [at](const Side &s) {
            st_setup();
            hx::mem[0xa47853 + at] ^= 0x10;
            inst(s.st, "scripttime");
            st_show();
        });
    }
    scenario("scripttime region not executable", [](const Side &s) {
        st_setup();
        hx::noexec.push_back({0xa478a8, 1});
        inst(s.st, "scripttime");
        st_show();
    });
    scenario("scripttime installed twice (second sees patched bytes)", [](const Side &s) {
        st_setup();
        inst(s.st, "scripttime");
        inst(s.st, "scripttime");
    });
}

}  // namespace

int main() {
    hx::init();
    reset_cfg = [] {
        sb = SbCfg{};
        vis = VisCfg{};
        lookups.clear();
        packages.clear();
        gp_package_by_id = pkg_by_id;
        g3_event = nullptr;
    };

    scripttime_tests();
    rosternames_tests();
    gameplay3_tests();
    visibility_tests();
    matchend_tests();
    spawn_tests();
    scoreboard_tests();
    std::printf("\n%d scenarios, %d passed, %d failed\n", n_scen, n_pass, n_scen - n_pass);
    for (const auto &f : failed) std::printf("  failed: %s\n", f.c_str());
    return n_scen == n_pass ? 0 : 1;
}
