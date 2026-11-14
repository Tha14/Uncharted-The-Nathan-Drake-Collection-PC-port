// u2mp rewrite tests, group "hud": the multiplayer HUD Draw, its install and the weapon panel trigger
// (hud.cpp, spec u2mp/docs/spec/hud.md), old (git) against new, on the same fake image and frame sequences.
//
// Every guest function the module calls is a stub that enters an assembly thunk: the thunk stores rdi, rsi,
// rdx, rcx, r8, r9, rax (al of a variadic call) and xmm0..xmm7 and jumps to a host handler, which records the
// arguments: integers, float bits, V2 registers, and the bytes behind pointers (positions, colours, text widget
// copies, DrawText states, menu quad blocks, texts) together with where the pointer points (the image or a host
// temporary). The process-wide state of both modules (draw counter, resolve cache, log-once set, theater buttons,
// weapon trigger) runs through identical scenario sequences in the same order.
#include "harness.h"

#include <cmath>
#include <cstdlib>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace u2mp {
void install_hud(int &installed);
}
namespace u2mp_old {
void install_hud(int &installed);
}

// ---- register capture ---------------------------------------------------------------------------------
struct Regs {
    uint64_t gp[6];      // rdi rsi rdx rcx r8 r9
    uint64_t rax, pad;
    uint8_t xmm[8][16];
};
extern "C" {
Regs hx_regs;
}

#define HX_THUNK(n)                                                                                                    \
    extern "C" void n##_thunk();                                                                                       \
    asm(".text\n.globl " #n "_thunk\n" #n "_thunk:\n"                                                                  \
        "movq %rdi, hx_regs+0(%rip)\nmovq %rsi, hx_regs+8(%rip)\nmovq %rdx, hx_regs+16(%rip)\n"                        \
        "movq %rcx, hx_regs+24(%rip)\nmovq %r8, hx_regs+32(%rip)\nmovq %r9, hx_regs+40(%rip)\n"                        \
        "movq %rax, hx_regs+48(%rip)\n"                                                                                \
        "movdqu %xmm0, hx_regs+64(%rip)\nmovdqu %xmm1, hx_regs+80(%rip)\nmovdqu %xmm2, hx_regs+96(%rip)\n"             \
        "movdqu %xmm3, hx_regs+112(%rip)\nmovdqu %xmm4, hx_regs+128(%rip)\nmovdqu %xmm5, hx_regs+144(%rip)\n"          \
        "movdqu %xmm6, hx_regs+160(%rip)\nmovdqu %xmm7, hx_regs+176(%rip)\n"                                           \
        "jmp " #n "_handler\n");

using hx::get;
using hx::put;

namespace {

// ---- image offsets (spec section 3) -------------------------------------------------------------------
constexpr uint64_t NETSTATE = 0x14ce97c, HUDFLAGS = 0x1130068, MTAB = 0x1130070, HUDX = 0x1130348, SCORE = 0x104ae70,
                   ALPHA = 0x104ed0c, MASK = 0x104f4c4, WAVEA = 0x104f564, SPEED = 0x104f1bc, BLCOUNT = 0x113a788,
                   BLIST = 0x113a790, CINMODE = 0x113b008, FILMSTART = 0x113b030, FILMLEN = 0x113ab68,
                   LOAD0 = 0x1147544, LOAD1 = 0x1147548, PAD = 0x14b7fb0, LPCELL = 0x115bd20, LPID = 0x115bd28,
                   MSGCLOCK = 0x11161c4, C1F = 0x1116204, C1D = 0x1116210, C2F = 0x1116284, FRAMES = 0x1116384,
                   NGM = 0x113b760;
constexpr uint64_t fLookup = 0xa15e70, fText = 0x5555a0, fImage = 0x5559f0, fFind = 0xcddc00, fMaterial = 0x5558c0,
                   fQuad = 0x555c60, fMenuQuad = 0xbdce90, fProject = 0x5566b0, fDrawText = 0xcc9a70,
                   fByName = 0x555140, fTeamCol = 0x64aeb0, fMoney = 0x64cbd0, fCallback = 0x5c8840,
                   fSndInit = 0x3dd780, fSndPlay = 0x9b5c20, fCurWeapon = 0x71dec0, fShowWeapon = 0x954740,
                   fPanelDraw = 0x563a20, fHeld = 0x1000;
constexpr uint64_t SLOT_DRAW = 0x103f558, SLOT_SHOW = 0x1027030, SLOT_PANEL = 0x103f768;
constexpr uint64_t E_MSG = 0x40, E_NAMES = 0x860, E_MK = 0x1560, E_FADE = 0x1920, E_TH = 0x1928, E_CM = 0x1a90;

uint64_t P(uint64_t off) { return hx::addr(off); }
uint64_t OFF(uint64_t abs) { return abs - P(0); }
template <typename T> T at(uint64_t abs) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void *>(abs), sizeof v);
    return v;
}
uint32_t fbits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
float ffrom(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
const float NaN = std::numeric_limits<float>::quiet_NaN(), Inf = std::numeric_limits<float>::infinity();

// ---- heap in the image (deterministic per scenario) -----------------------------------------------------
uint64_t g_heap = 0x1600000;
uint64_t A(size_t n) {
    g_heap = (g_heap + 15) & ~uint64_t(15);
    const uint64_t o = g_heap;
    g_heap += n;
    return o;
}
uint64_t S(const char *s) {   // a C string in the image
    const uint64_t o = A(std::strlen(s) + 1);
    std::memcpy(hx::mem + o, s, std::strlen(s) + 1);
    return o;
}

std::string hexb(const void *p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) s += d[b[i] >> 4], s += d[b[i] & 15];
    return s;
}
std::string where(uint64_t p) { return hx::where(reinterpret_cast<const void *>(p)); }
std::string fx(uint32_t b) {
    char t[48];
    std::snprintf(t, sizeof t, "%08x(%g)", b, double(ffrom(b)));
    return t;
}
std::string xf(int i) {   // xmm i low float
    uint32_t b;
    std::memcpy(&b, hx_regs.xmm[i], 4);
    return fx(b);
}
std::string xv2(int i) {   // xmm i as a V2
    uint32_t a, b;
    std::memcpy(&a, hx_regs.xmm[i], 4);
    std::memcpy(&b, hx_regs.xmm[i] + 4, 4);
    return "(" + fx(a) + "," + fx(b) + ")";
}
std::string floats(uint64_t p, int n) {
    std::string s = "(";
    for (int i = 0; i < n; i++) s += (i ? "," : "") + fx(at<uint32_t>(p + 4 * i));
    return s + ")";
}
std::string cstr(uint64_t p) {
    if (!p) return "NULL";
    return where(p) + ":\"" + std::string(reinterpret_cast<const char *>(p)) + "\"";
}
std::string align(uint64_t p) { return (p & 15) ? "unaligned" : "a16"; }

// ---- scripted guest state ------------------------------------------------------------------------------
struct Proj {
    float pos[4], dir[4], angle, dist, scale;
    int32_t clamped;
};
struct St {
    std::map<uint32_t, uint64_t> lookup;      // id -> absolute pointer
    std::map<uint32_t, uint64_t> texts;       // text id -> absolute C string
    std::set<uint32_t> mat_fail;              // ids the material lookup never finds
    std::map<uint32_t, uint64_t> mat_alias;   // id -> absolute result
    std::map<uint64_t, int32_t> widget_index_after;   // image widget -> index the game call writes
    std::vector<Proj> proj;
    size_t proj_i = 0;
    int dt_mode = 0;   // DrawText writes: 0 size from the text, 1 NaN, 2 huge, 3 nothing
    std::map<int32_t, uint32_t> team_col;
    int32_t money = 0;
    uint64_t weapon = 0;
    std::map<uint64_t, uint64_t> held;        // object -> held object
    uint32_t byname_table = 0;
    uint64_t snd_buf = 0;
};
St st;

uint64_t material_default(uint64_t key, int32_t idx) {
    const uint32_t id = uint32_t(key), t = uint32_t(key >> 32);
    if (st.mat_fail.count(id)) return 0;
    if (auto it = st.mat_alias.find(id); it != st.mat_alias.end()) return it->second;
    if (t > 2) return 0;
    const uint64_t tbl = get<uint64_t>(MTAB + 8 * t);
    if (!tbl) return 0;
    const uint16_t cnt = at<uint16_t>(tbl + 2);
    const uint64_t ent = at<uint64_t>(tbl + 8);
    if (!ent) return 0;
    if (idx >= 0 && idx < cnt) return ent + 0x28 * uint64_t(idx);
    for (uint32_t k = 0; k < cnt; k++)
        if (at<uint32_t>(ent + 0x28 * k + 8) == id) return ent + 0x28 * k;
    return 0;
}

}  // namespace

// ---- handlers (System V, entered from the thunks) -------------------------------------------------------
#define GP(i) hx_regs.gp[i]
extern "C" {
U2MP_GUEST uint64_t lookup_handler() {
    const uint32_t id = uint32_t(GP(0));
    hx::rec("LookupPointer %08x", id);
    auto it = st.lookup.find(id);
    return it == st.lookup.end() ? 0 : it->second;
}
U2MP_GUEST void text_handler() {
    const uint64_t w = GP(0), pos = GP(1), fmt = GP(2);
    const std::string f = reinterpret_cast<const char *>(fmt);
    std::string arg;
    if (f.find('s') != std::string::npos)
        arg = cstr(GP(3));
    else
        arg = std::to_string(int32_t(GP(3)));
    const bool host = where(w) == "host";
    hx::rec("TextWidget w=%s[%s] pos=%s%s fmt=%s scale=%s al=%u arg=%s", where(w).c_str(),
            hexb(reinterpret_cast<void *>(w), host ? 0x40 : 0x2c).c_str(), where(pos).c_str(), floats(pos, 2).c_str(),
            cstr(fmt).c_str(), xf(0).c_str(), unsigned(hx_regs.rax & 0xff), arg.c_str());
}
U2MP_GUEST void image_handler() {
    const uint64_t w = GP(1), pos = GP(2);
    hx::rec("ImageWidget ctx=%llx w=%s pos=%s%s alpha=%s", (unsigned long long)GP(0), where(w).c_str(),
            where(pos).c_str(), floats(pos, 2).c_str(), xf(0).c_str());
    if (auto it = st.widget_index_after.find(w); it != st.widget_index_after.end())
        std::memcpy(reinterpret_cast<void *>(w + 0x18), &it->second, 4);
}
U2MP_GUEST uint64_t find_handler() {
    const uint32_t id = uint32_t(GP(1));
    hx::rec("TextFind db=%s id=%08x", where(GP(0)).c_str(), id);
    auto it = st.texts.find(id);
    return it == st.texts.end() ? 0 : it->second;
}
U2MP_GUEST uint64_t material_handler() {
    const uint64_t key = GP(0);
    const int32_t idx = int32_t(GP(1));
    const uint64_t r = material_default(key, idx);
    hx::rec("Material %016llx %d -> %s", (unsigned long long)key, idx, where(r).c_str());
    return r;
}
U2MP_GUEST void quad_handler() {
    const uint64_t c = GP(3);
    hx::rec("Quad ctx=%llx key=%016llx idx=%d col=%s%s%s centred=%u galpha=%u pos=%s size=%s dir=%s uv0=%s uv1=%s",
            (unsigned long long)GP(0), (unsigned long long)GP(1), int32_t(GP(2)), where(c).c_str(), align(c).c_str(),
            floats(c, 4).c_str(), unsigned(GP(4) & 0xff), unsigned(GP(5) & 0xff), xv2(0).c_str(), xv2(1).c_str(),
            xv2(2).c_str(), xv2(3).c_str(), xv2(4).c_str());
}
U2MP_GUEST void menuquad_handler() {
    const uint64_t c = GP(0), i = GP(1), v = GP(2);
    hx::rec("MenuQuad ctx[%s %s %s] info[%s %s mat=%s %s] verts[%s %s %s]", where(c).c_str(), align(c).c_str(),
            hexb(reinterpret_cast<void *>(c), 48).c_str(), where(i).c_str(), align(i).c_str(),
            where(at<uint64_t>(i)).c_str(), hexb(reinterpret_cast<void *>(i + 8), 24).c_str(), where(v).c_str(),
            align(v).c_str(), hexb(reinterpret_cast<void *>(v), 0x110).c_str());
}
U2MP_GUEST void project_handler() {
    const uint64_t out = GP(0), w = GP(1);
    hx::rec("Project out=%s%s[%s] world=%s%s%s margin=%s", where(out).c_str(), align(out).c_str(),
            hexb(reinterpret_cast<void *>(out), 0x30).c_str(), where(w).c_str(), align(w).c_str(),
            floats(w, 4).c_str(), xf(0).c_str());
    if (!st.proj.empty()) {
        const Proj &p = st.proj[st.proj_i++ % st.proj.size()];
        std::memcpy(reinterpret_cast<void *>(out), &p, 0x30);
    }
}
U2MP_GUEST void drawtext_handler() {
    const uint64_t s = GP(0), t = GP(1);
    hx::rec("DrawText state=%s%s[%s] text=%s", where(s).c_str(), align(s).c_str(),
            hexb(reinterpret_cast<void *>(s), 0xc0).c_str(), cstr(t).c_str());
    float w = 0, h = 0;
    const float sc = at<float>(s + 0x20);
    switch (st.dt_mode) {
    case 0: w = float(std::strlen(reinterpret_cast<const char *>(t))) * 13.5f * sc, h = 18.25f * sc; break;
    case 1: w = NaN, h = NaN; break;
    case 2: w = 3e38f, h = 1e30f; break;
    default: return;
    }
    std::memcpy(reinterpret_cast<void *>(s + 0x9c), &w, 4);
    std::memcpy(reinterpret_cast<void *>(s + 0x98), &h, 4);
}
struct NameRet {
    uint64_t key;
    uint64_t index;
};
U2MP_GUEST NameRet byname_handler() {
    const uint64_t n = GP(0);
    hx::rec("HandleByName %s %d", cstr(n).c_str(), int32_t(GP(1)));
    uint32_t h = 2166136261u;
    for (const char *p = reinterpret_cast<const char *>(n); *p; ++p) h = (h ^ uint8_t(*p)) * 16777619u;
    const char *name = reinterpret_cast<const char *>(n);
    const int32_t idx = name[0] == 'i' ? 2 : -1;   // names starting with 'i' come with an index
    return NameRet{uint64_t(h) | uint64_t(st.byname_table) << 32, uint64_t(uint32_t(idx)) | 0xdead00000000ull};
}
U2MP_GUEST void teamcol_handler() {
    const uint64_t out = GP(0);
    bool zero = true;
    for (int i = 0; i < 0x40; i++) zero = zero && reinterpret_cast<uint8_t *>(out)[i] == 0;
    hx::rec("TeamColours out=%s%s%s ngm=%s team=%d arg=%d", where(out).c_str(), align(out).c_str(),
            zero ? "[zero]" : "[dirty]", where(GP(1)).c_str(), int32_t(GP(2)), int32_t(GP(3)));
    auto it = st.team_col.find(int32_t(GP(2)));
    const uint32_t c = it == st.team_col.end() ? 0x00123456u + uint32_t(GP(2)) * 0x10101u : it->second;
    std::memcpy(reinterpret_cast<void *>(out + 0x10), &c, 4);
    std::memset(reinterpret_cast<void *>(out), 0x77, 4);   // junk outside +0x10
}
U2MP_GUEST int32_t money_handler() {
    hx::rec("CoopMoney %s", where(GP(0)).c_str());
    return st.money;
}
U2MP_GUEST void callback_handler() {
    hx::rec("DoCallback %s %08x %u", where(GP(0)).c_str(), uint32_t(GP(1)), uint32_t(GP(2)));
}
U2MP_GUEST void sndinit_handler() {
    const uint64_t b = GP(0);
    bool zero = true;
    for (int i = 0; i < 0x200; i++) zero = zero && reinterpret_cast<uint8_t *>(b)[i] == 0;
    hx::rec("SoundInit buf=%s%s%s id=%08x %u", where(b).c_str(), align(b).c_str(), zero ? "[zero]" : "[dirty]",
            uint32_t(GP(1)), uint32_t(GP(2)));
    st.snd_buf = b;
    std::memset(reinterpret_cast<void *>(b), 0x55, 0x20);
}
U2MP_GUEST void sndplay_handler() {
    hx::rec("SoundPlay buf=%s %u", GP(0) == st.snd_buf ? "same" : "other", uint32_t(GP(1)));
}
U2MP_GUEST uint64_t curweapon_handler() {
    hx::rec("CurrentWeapon %s", where(GP(0)).c_str());
    return st.weapon;
}
U2MP_GUEST void showweapon_handler() {
    hx::rec("ShowWeaponHud %s arg=%s", where(GP(0)).c_str(), xf(0).c_str());
}
U2MP_GUEST void paneldraw_handler() {
    hx::rec("PanelDraw %llx %llx", (unsigned long long)GP(0), (unsigned long long)GP(1));
}
U2MP_GUEST uint64_t held_handler() {
    hx::rec("Held %s %d", where(GP(0)).c_str(), int32_t(GP(1)));
    auto it = st.held.find(GP(0));
    return it == st.held.end() ? 0 : it->second;
}
}

HX_THUNK(lookup)
HX_THUNK(text)
HX_THUNK(image)
HX_THUNK(find)
HX_THUNK(material)
HX_THUNK(quad)
HX_THUNK(menuquad)
HX_THUNK(project)
HX_THUNK(drawtext)
HX_THUNK(byname)
HX_THUNK(teamcol)
HX_THUNK(money)
HX_THUNK(callback)
HX_THUNK(sndinit)
HX_THUNK(sndplay)
HX_THUNK(curweapon)
HX_THUNK(showweapon)
HX_THUNK(paneldraw)
HX_THUNK(held)

namespace {

using DrawFn = void(U2MP_GUEST *)(void *, void *);
using ShowFn = void(U2MP_GUEST *)(void *, float);
DrawFn g_draw[2] = {};
std::vector<uint64_t> g_icons;
uint64_t g_show_probe[2] = {}, g_panel_probe[2] = {};

void base_stubs() {
    hx::stub(fLookup, (void *)lookup_thunk);
    hx::stub(fText, (void *)text_thunk);
    hx::stub(fImage, (void *)image_thunk);
    hx::stub(fFind, (void *)find_thunk);
    hx::stub(fMaterial, (void *)material_thunk);
    hx::stub(fQuad, (void *)quad_thunk);
    hx::stub(fMenuQuad, (void *)menuquad_thunk);
    hx::stub(fProject, (void *)project_thunk);
    hx::stub(fDrawText, (void *)drawtext_thunk);
    hx::stub(fByName, (void *)byname_thunk);
    hx::stub(fTeamCol, (void *)teamcol_thunk);
    hx::stub(fMoney, (void *)money_thunk);
    hx::stub(fCallback, (void *)callback_thunk);
    hx::stub(fSndInit, (void *)sndinit_thunk);
    hx::stub(fSndPlay, (void *)sndplay_thunk);
    hx::stub(fCurWeapon, (void *)curweapon_thunk);
    hx::stub(fShowWeapon, (void *)showweapon_thunk);
    hx::stub(fPanelDraw, (void *)paneldraw_thunk);
    hx::stub(fHeld, (void *)held_thunk);
}

void setenv_(const char *n, const char *v) {
#ifdef _WIN32
    _putenv_s(n, v ? v : "");
#else
    if (v) setenv(n, v, 1); else unsetenv(n);
#endif
}

// ---- scenario runner ------------------------------------------------------------------------------------
int g_scen = 0, g_pass = 0;
std::vector<std::string> g_fail_names;
std::function<void(int)> g_post;   // after each side's body: normalise per-side host values in the image

void run(const std::string &name, const std::function<void()> &setup, const std::function<void(int)> &body) {
    if (std::getenv("HX_HUD_TRACE")) std::printf("RUN %s\n", name.c_str()), std::fflush(stdout);
    hx::clear();
    hx::bind_both();
    g_heap = 0x1600000;
    st = St{};
    base_stubs();
    setup();
    std::vector<uint8_t> raw(hx::mem, hx::mem + hx::kImageSize);
    const St saved = st;
    const uint64_t heap_saved = g_heap;
    hx::calls.clear();
    body(0);
    if (g_post) g_post(0);
    const auto a = hx::calls;
    if (const char *d = std::getenv("HX_HUD_DUMP"); d && name.rfind(d, 0) == 0)   // print the old record
        for (const auto &c : a) std::printf("  %s\n", c.c_str());
    const auto ia = hx::snapshot();
    std::memcpy(hx::mem, raw.data(), hx::kImageSize);
    st = saved;
    g_heap = heap_saved;
    hx::bind_both();
    hx::calls.clear();
    body(1);
    if (g_post) g_post(1);
    const auto b = hx::calls;
    const auto ib = hx::snapshot();
    g_post = nullptr;
    ++g_scen;
    if (hx::same(name.c_str(), a, b, &ia, &ib)) {
        ++g_pass;
        std::printf("PASS %s (%zu calls)\n", name.c_str(), a.size());
    } else {
        g_fail_names.push_back(name);
    }
    std::fflush(stdout);
}

// ---- install image ------------------------------------------------------------------------------------
struct Bytes {
    uint64_t off;
    std::vector<uint8_t> b;
};
std::vector<uint8_t> hexv(const char *s) {
    std::vector<uint8_t> v;
    for (const char *p = s; *p;) {
        if (*p == ' ') { ++p; continue; }
        v.push_back(uint8_t(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
        p += 2;
    }
    return v;
}
const std::vector<Bytes> &checks1() {
    static const std::vector<Bytes> v = {
        {0x5555a0, hexv("55 48 89 e5 41 56 53 48 81 ec 10 02 00 00 84 c0")},
        {0x5559f0, hexv("55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec")},
        {0x55aab2, hexv("48 8d 05 97 4a ae 00")},
        {0x55ab94, hexv("c7 43 08 00 20 00 00")},
        {0x555c60, hexv("55 48 89 e5 41 57 41 56 53 48 81 ec b8 02 00 00")},
        {0x5566b0, hexv("55 48 89 e5 41 56 53 48 83 ec 30 48 89 fb c5 78")},
        {0xbdce90, hexv("55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 40 49")},
        {0xcc9a70, hexv("55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 58 12 00 00")},
        {0x5557e7, hexv("48 8d bd 30 ff ff ff 48 89 de e8 7a 42 77 00")},
        {0x55d557, hexv("48 89 83 a8 0c 00 00 89 93 b0 0c 00 00")},
        {0x55b8d2, hexv("49 63 8f e0 14 00 00")},
        {0x55b8e3, hexv("48 8d 1c 89 48 c1 e3 05")},
        {0x55b918, hexv("49 89 8c 1f 60 08 00 00")},
        {0x55b953, hexv("41 c7 84 1f 6c 08 00 00 03 00 00 00")},
        {0x55c0e1, hexv("88 84 1e f8 08 00 00")},
        {0x55c0e8, hexv("49 8b 85 b0 02 00 00 48 89 84 1e f0 08 00 00")},
        {0x55c08f, hexv("41 8b 85 28 01 00 00 89 84 1e fc 08 00 00")},
        {0x8921bb, hexv("c6 80 0c 04 00 00 01")},
        {0x555340, hexv("48 8b bf 20 03 00 00 c5 f8 28 09 4c 8d 97 60 15")},
        {0x55ab69, hexv("48 8d bb 60 15 00 00")},
        {0x55ab81, hexv("48 8d bb 90 1a 00 00")},
        {0x649bc1, hexv("41 89 87 10 1b 00 00")},
    };
    return v;
}
const std::vector<Bytes> &checks2() {
    static const std::vector<Bytes> v = {
        {0xbdce90, hexv("55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 40 49")},
        {0x64cbd0, hexv("55 48 89 e5 41 56 53 48 89 fb bf 7d 06 9a 3b e8")},
        {0x662de6, hexv("41 89 9c 86 a0 02 00 00")},
        {0x5f61a4, hexv("47 39 74 ac 4c")},
        {0x5e7ddd, hexv("83 b9 78 06 00 00 00")},
        {0x55c515, hexv("8a 96 9c 20 00 00")},
        {0x560e38, hexv("c5 fa 7e 80 40 03 00 00")},
        {0x66885f, hexv("48 8d 05 de 6c 9e 00 c6 40 1c 00")},
        {0x669844, hexv("48 8d 05 15 1f ad 00 44 8b 80 d0 1d 00 00")},
        {0x66985e, hexv("48 05 88 1d 00 00")},
        {0x66988c, hexv("83 bf 50 02 00 00 ff")},
        {0x67235e, hexv("8b 93 34 02 00 00")},
        {0x6722a8, hexv("8b 93 38 02 00 00")},
        {0x6724cc, hexv("8b 83 58 02 00 00 85 c0 74 47 c5 fa 2a 83 68 02 00 00")},
        {0x67251d, hexv("8b 83 5c 02 00 00")},
        {0x67237f, hexv("44 8b b5 48 fe ff ff")},
        {0x64aeb0, hexv("55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18")},
        {0x6237ab, hexv("48 89 8a c0 1c 00 00 89 82 c8 1c 00 00")},
        {0x669a77, hexv("8b 83 40 04 00 00 41 3b 86 20 01 00 00")},
    };
    return v;
}
const std::vector<Bytes> &checks_weapon() {
    static const std::vector<Bytes> v = {
        {0x954740, hexv("55 48 89 e5 48 83 ec 10 c5 fa 11 45 fc be fa 20 bd 2b")},
        {0x95477a, hexv("83 08 08")},
        {0x563a20, hexv("55 48 89 e5 41 57 41 56 41 55 41 54")},
        {0x71dec0, hexv("48 63 8f b8 04 00 00")},
    };
    return v;
}
void install_image() {
    for (const auto *l : {&checks1(), &checks2(), &checks_weapon()})
        for (const Bytes &c : *l) std::memcpy(hx::mem + c.off, c.b.data(), c.b.size());
    const uint8_t ret[] = {0xc3, 0x66, 0x66, 0x66, 0x66};
    std::memcpy(hx::mem + 0x55c5b0, ret, 5);
    put<uint64_t>(SLOT_DRAW, P(0x55c5b0));
    put<uint64_t>(SLOT_SHOW, P(fShowWeapon));
    put<uint64_t>(SLOT_PANEL, P(fPanelDraw));
}
bool host_ptr(uint64_t v) { return v > 0x100000 && (v < P(0) || v >= P(hx::kImageSize)); }
// the slots hold host functions after install: note them, and normalise them for the image comparison
void install_post(int which) {
    for (uint64_t s : {SLOT_DRAW, SLOT_SHOW, SLOT_PANEL}) {
        const uint64_t v = get<uint64_t>(s);
        if (host_ptr(v)) {
            hx::rec("slot 0x%llx -> host function", (unsigned long long)s);
            if (s == SLOT_DRAW) g_draw[which] = reinterpret_cast<DrawFn>(v);
            if (s == SLOT_SHOW) g_show_probe[which] = v;
            if (s == SLOT_PANEL) g_panel_probe[which] = v;
            put<uint64_t>(s, 0xfeedfacecafebeefull);
        }
    }
}
void install_body(int which) {
    int n = 7;
    if (which == 0)
        u2mp_old::install_hud(n);
    else
        u2mp::install_hud(n);
    hx::rec("installed %d", n);
}
void install_scn(const std::string &name, const std::function<void()> &mod, const char *env_hud = nullptr,
                 const char *env_weapon = nullptr) {
    setenv_("UNCHARTED_U2MP_HUD", env_hud);
    setenv_("UNCHARTED_U2MP_WEAPON_HUD", env_weapon);
    run(name,
        [&] {
            install_image();
            if (mod) mod();
        },
        [](int w) {
            install_body(w);
            install_post(w);
        });
    setenv_("UNCHARTED_U2MP_HUD", nullptr);
    setenv_("UNCHARTED_U2MP_WEAPON_HUD", nullptr);
}

// ---- the game world -----------------------------------------------------------------------------------
struct World {
    uint64_t fb, set, rec, element, ng, ngrecs, score, t[3], tent[3];
    uint64_t w[0x100];   // widget per set field
    uint64_t terr[5], tcell[5];
    uint64_t flag, flagcell, tags, tagent, tagval, tagbool;
    uint64_t chr[10], chrcell[10], hobj[10], hvt[10];
    uint64_t boosters, brecs, blist[6];
    uint64_t wave, waveent;
    uint64_t player, pcell, pvt, ctrl, cvt;
} W;

uint64_t ngm(uint64_t f) { return NGM + f; }
uint64_t trk(int i) { return NGM + 0x10 + 0x2c0 * uint64_t(i); }
uint64_t el(uint64_t f) { return W.element + f; }
void flags(uint32_t f) { put<uint32_t>(W.fb, f); }
void gtype(int32_t t) { put<int32_t>(W.ngrecs + 0xc, t); }

const uint32_t T1IDS[] = {0x12ffe6ae, 0x68951e57, 0x2fb98f98, 0xbda0f568, 0xf1b63553, 0xffa98ec5, 0x50000001,
                          0xa0000001, 0xa0000002, 0xa0000003, 0xa0000004, 0xa0000005, 0xa0000006, 0xb0000001,
                          0xb0000002, 0xb0000003, 0xb0000004, 0x4240ef2e};
void mat_table(int t, const std::vector<uint32_t> &ids) {
    W.t[t] = A(16);
    W.tent[t] = A(0x28 * ids.size() + 8);
    put<uint16_t>(W.t[t] + 2, uint16_t(ids.size()));
    put<uint64_t>(W.t[t] + 8, P(W.tent[t]));
    for (size_t k = 0; k < ids.size(); k++) {
        put<uint32_t>(W.tent[t] + 0x28 * k + 8, ids[k]);
        put<uint32_t>(W.tent[t] + 0x28 * k, 0x1000 + uint32_t(k));
    }
    put<uint64_t>(MTAB + 8 * t, P(W.t[t]));
}
uint64_t text_widget(int k) {
    const uint64_t o = A(0x30);
    const float v[] = {10.0f + k, 20.0f + k, 100.0f, 30.0f};
    std::memcpy(hx::mem + o, v, 16);
    put<uint32_t>(o + 0x10, uint32_t(k % 3));
    put<float>(o + 0x14, 0.8f + 0.01f * k);
    put<float>(o + 0x18, 0.25f), put<float>(o + 0x1c, 0.5f), put<float>(o + 0x20, 0.75f), put<float>(o + 0x24, 0.9f);
    put<uint32_t>(o + 0x28, 5);
    put<uint32_t>(o + 0x2c, 0xabababab);   // past 0x2c: never copied
    return o;
}
uint64_t image_widget(const char *name, bool net, uint32_t id, int32_t index, float x, float y) {
    const uint64_t o = A(0x30);
    put<uint64_t>(o, name ? P(S(name)) : 0);
    put<uint32_t>(o + 0x10, net ? 0x4240ef2e : 0x11223344);
    put<uint32_t>(o + 0x14, id);
    put<int32_t>(o + 0x18, index);
    put<float>(o + 0x24, x), put<float>(o + 0x28, y);
    return o;
}
void handle(uint64_t off, uint32_t id, uint32_t table, int32_t index = -1) {
    put<uint32_t>(off, id), put<uint32_t>(off + 4, table), put<int32_t>(off + 8, index);
}
void live(uint64_t slot_field, uint64_t id_field, uint64_t cell, uint64_t obj, uint32_t id) {
    put<uint64_t>(cell, P(obj));
    put<uint32_t>(obj + 0x44, id);
    put<uint64_t>(slot_field, P(cell));
    put<uint32_t>(id_field, id);
}

// the basic world every Draw scenario starts from: a multiplayer match of `type` with HUD flags `f`
void world(int32_t type, uint32_t f) {
    put<int32_t>(NETSTATE, 2);
    W.fb = A(0x410);
    put<uint64_t>(HUDFLAGS, P(W.fb));
    flags(f);
    std::vector<uint32_t> t0;
    for (uint32_t k = 0; k < 0x20; k++) t0.push_back(0x11110001 + k);
    for (uint32_t k = 0; k < 11; k++) t0.push_back(0x22220000 + k), t0.push_back(0x22230000 + k);
    mat_table(0, t0);
    mat_table(1, std::vector<uint32_t>(std::begin(T1IDS), std::end(T1IDS)));
    mat_table(2, {0x33330001, 0x33330002, 0x33330003, 0x33330004, 0x33330005});
    W.rec = A(0x100);
    W.set = A(0x100);
    put<uint64_t>(W.rec + 0x90, P(W.set));
    st.lookup[0x9353ed16] = P(W.rec);
    int k = 0;
    for (uint64_t fld : {0x08, 0x10, 0x30, 0x38, 0x40, 0x48, 0x78, 0xc8}) {
        W.w[fld] = text_widget(k++);
        put<uint64_t>(W.set + fld, P(W.w[fld]));
    }
    W.w[0x18] = image_widget(nullptr, false, 0, -1, 1, 2);
    W.w[0x20] = image_widget("general/net/carry-own", true, 0x12ffe6ae, 3, 3, 4);
    W.w[0x28] = image_widget("general/net/carry-other", false, 0x11110002, -1, 5, 6);
    W.w[0x58] = image_widget("general/net/score-frame", false, 0x11110001, -1, 7, 8);
    W.w[0x60] = image_widget("general/net/turf-frame", true, 0x99999999, -1, 100.5f, 600.25f);
    W.w[0xb8] = image_widget("general/net/message-back", false, 0x11110003, -1, 9, 10);
    W.w[0xd0] = image_widget("general/net/coop-back", true, 0x68951e57, -1, 11, 12);
    for (uint64_t fld : {0x18, 0x20, 0x28, 0x58, 0x60, 0xb8, 0xd0}) put<uint64_t>(W.set + fld, P(W.w[fld]));
    put<float>(W.set + 0x68, 12.5f), put<float>(W.set + 0x6c, -3.25f), put<float>(W.set + 0x70, 300.0f),
        put<float>(W.set + 0x74, 40.0f);
    put<float>(W.set + 0xc0, 640.0f), put<float>(W.set + 0xc4, 85.0f);
    // net games
    W.ng = A(16);
    W.ngrecs = A(0x90 * 3);
    put<int32_t>(W.ng, 3);
    put<uint64_t>(W.ng + 8, P(W.ngrecs));
    st.lookup[0x3b9a067d] = P(W.ng);
    put<int16_t>(ngm(0x1ca0), 0);
    gtype(type);
    put<int32_t>(W.ngrecs + 0x90 + 0xc, 99);
    // trackers: ids 100 + i, teams i % 2, local = tracker 0
    for (int i = 0; i < 10; i++) {
        put<int32_t>(trk(i) + 0x120, 100 + i);
        put<int32_t>(trk(i) + 0x134, i % 2);
    }
    put<uint64_t>(ngm(0), P(trk(0)));
    // scores
    W.score = A(0x3dee + 0x34 * 8 + 8);
    put<uint64_t>(SCORE, P(W.score));
    for (int t = 0; t < 8; t++) put<uint16_t>(W.score + 0x3dee + 0x34 * t, uint16_t(1000 * t + 17));
    put<int32_t>(ngm(0x21b4), 50);
    put<float>(ngm(0x21b8), 600.0f);
    put<int32_t>(FRAMES, 30000);
    put<int32_t>(ngm(0x1d5c), 12000);
    put<int32_t>(ngm(0x1d60), 6000);
    put<int32_t>(ngm(0x1d70), 2);
    put<float>(ALPHA, 0.875f);
    st.texts[0xd38a7d18] = P(S("SCORE LIMIT"));
    st.texts[0xe1167dfa] = P(S("ROUND"));
    st.texts[0x9b5b3c66] = P(S("x"));
    W.element = A(0x1c00);
    put<float>(el(E_FADE), 0.8f);
}

// ---- parts of the world ---------------------------------------------------------------------------------
void msg(int i, const char *t1, uint32_t c1, const char *t2, uint32_t c2, float start, float end, uint32_t fl,
         int32_t count, uint64_t icon = 0) {
    const uint64_t e = el(E_MSG) + 8 + 0x80 * uint64_t(i);
    std::memset(hx::mem + e, 0, 0x80);
    std::strncpy(reinterpret_cast<char *>(hx::mem + e), t1, 48);
    put<uint32_t>(e + 0x30, c1);
    std::strncpy(reinterpret_cast<char *>(hx::mem + e + 0x34), t2, 48);
    put<uint32_t>(e + 0x64, c2);
    put<uint64_t>(e + 0x68, icon ? P(icon) : 0);
    put<float>(e + 0x70, start), put<float>(e + 0x74, end);
    put<uint32_t>(e + 0x78, fl);
    put<int32_t>(e + 0x7c, count);
}
uint64_t icon_rec(const char *name, float sx, float sy, float div, float mul) {
    const uint64_t o = A(0x40);
    put<uint64_t>(o + 0x10, name ? P(S(name)) : 0);
    put<float>(o + 0x18, sx), put<float>(o + 0x1c, sy), put<float>(o + 0x28, div), put<float>(o + 0x30, mul);
    return o;
}
void messages_default() {
    put<int32_t>(MSGCLOCK, 300 * 100);   // now = 100 s
    const uint64_t ic = icon_rec("hud/weapon/ak47", 128, 64, 2.0f, 3.0f);
    msg(0, "Alpha", 0x00ff0000, "Bravo", 0x0000ff00, 99.9f, 106.0f, 8, 0, ic);           // appearing
    msg(1, "Charlie killed", 0x00123456, "", 0, 90.0f, 100.2f, 4, 3, 0);                 // vanishing, blue badge
    msg(2, "", 0, "Delta", 0x00abcdef, 95.0f, 105.0f, 2, 2, ic);                           // red badge
    put<int32_t>(el(E_MSG), 3);
}
void name_entry(int i, float x, float y, float s, int32_t side, int32_t kind, float col_a, int32_t status, float fill,
                const char *name, float name_a, uint64_t emblem, uint8_t talk, int32_t rank) {
    const uint64_t e = el(E_NAMES) + 0xa0 * uint64_t(i);
    std::memset(hx::mem + e, 0, 0xa0);
    put<float>(e, x), put<float>(e + 4, y), put<float>(e + 8, s);
    put<int32_t>(e + 0xc, side), put<int32_t>(e + 0x10, kind);
    put<float>(e + 0x20, 0.2f), put<float>(e + 0x24, 0.4f), put<float>(e + 0x28, 0.6f), put<float>(e + 0x2c, col_a);
    put<int32_t>(e + 0x30, status);
    put<float>(e + 0x34, fill);
    std::strncpy(reinterpret_cast<char *>(hx::mem + e + 0x38), name, 64);
    put<float>(e + 0x80, 0.9f), put<float>(e + 0x84, 0.8f), put<float>(e + 0x88, 0.7f), put<float>(e + 0x8c, name_a);
    put<uint64_t>(e + 0x90, emblem);
    hx::mem[e + 0x98] = talk;
    put<int32_t>(e + 0x9c, rank);
}
void names_handles() {
    const uint64_t n = el(E_NAMES);
    handle(n + 0xca8, 0xa0000001, 1);
    handle(n + 0xcb4, 0xa0000002, 1);
    handle(n + 0xcc0, 0xa0000003, 1);
    handle(n + 0xccc, 0xa0000004, 1);
    handle(n + 0xcd8, 0xa0000005, 1);
    handle(n + 0xce4, 0xa0000006, 1);
    handle(n + 0xcf0, 0x68951e57, 1);
}
void names_default() {
    names_handles();
    name_entry(0, 640, 400, 1.0f, 0, 1, 0.9f, 0, 0, "Nate", 1.0f, 0, 0, 10);
    name_entry(1, 1270, 300, 0.75f, 2, 0, 0.8f, 1, 0.2f, "Sully", 0.9f, 0, 1, 70);
    name_entry(2, 100, 500, 1.25f, 3, 2, 0.7f, 2, 0.6f, "Chloe", 0.6f, 0x1234, 1, 30);
    put<int32_t>(el(E_NAMES) + 0xc80, 3);
    put<int32_t>(C1F, 12345);
}
void marker_types() {
    const uint64_t m = el(E_MK);
    for (uint32_t t = 0; t < 11; t++) {
        const uint64_t r = m + 0x210 + 0x24 * t;
        handle(r, 0x22220000 + t, 0);
        if (t % 2 == 0) handle(r + 0xc, 0x22230000 + t, 0);
        put<float>(r + 0x18, 1.0f + 0.125f * float(t));
        hx::mem[r + 0x1c] = t == 3;
        put<int32_t>(r + 0x20, t == 4 ? 7 : -1);
    }
    handle(m + 0x39c, 0xa0000001, 1);
    handle(m + 0x3a8, 0x68951e57, 1);
}
void marker(int i, uint32_t type, float fade, float target, float fill, int32_t last, uint32_t col = 0xc0ff8040,
            uint32_t ring = 0xff20a0f0) {
    const uint64_t e = el(E_MK) + 0x10 + 0x40 * uint64_t(i);
    put<uint32_t>(e + 4, type);
    put<float>(e + 0x10, 1.5f * i), put<float>(e + 0x14, 2.0f), put<float>(e + 0x18, -3.0f), put<float>(e + 0x1c, 1.0f);
    put<uint32_t>(e + 0x20, col), put<uint32_t>(e + 0x24, ring);
    put<float>(e + 0x2c, fade), put<float>(e + 0x30, target), put<float>(e + 0x34, fill);
    put<int32_t>(e + 0x3c, last);
}
Proj proj(float x, float y, float dx, float dy, float angle, float scale, int32_t clamped) {
    Proj p{};
    p.pos[0] = x, p.pos[1] = y, p.pos[2] = 0.5f, p.pos[3] = 1;
    p.dir[0] = dx, p.dir[1] = dy;
    p.angle = angle, p.dist = 12, p.scale = scale, p.clamped = clamped;
    return p;
}
void markers_default() {
    marker_types();
    put<int32_t>(C1F, 12345), put<int32_t>(C1D, 5);
    put<uint32_t>(MASK, 0xffffffff);
    marker(0, 4, 0.5f, 1.0f, 0.5f, 12300);
    marker(1, 6, 1.0f, 1.0f, 0.0f, 12340);
    marker(2, 3, 0.2f, 0.0f, 1.0f, 12345);
    put<int32_t>(el(E_MK), 3);
    st.proj = {proj(300, 200, 0.6f, -0.8f, 0.9f, 1.25f, 1), proj(800, 100, 0, 1, 2.5f, 0.75f, 0),
               proj(-20, 900, -1, 0, 3.5f, 2.0f, 1)};
}
void theater_default(int ncam = 4) {
    put<uint8_t>(ngm(0x209c), 1);
    const uint64_t th = el(E_TH);
    const uint32_t ids[] = {0x3cf5deed, 0x11111111, 0xe8184ce8, 0xff502160, 0x806c9824, 0x887c7d6e, 0x22222222,
                            0x33333333, 0x44444444, 0x55555555, 0x66666666};
    for (int c = 0; c < 11; c++) {
        const uint64_t cam = th + 0xc + 0x18 * uint64_t(c);
        if (cam + 0x18 > th + 0xfc) break;
        put<uint32_t>(cam, ids[c]);
        put<uint32_t>(cam + 4, 0xaaaa0000 + c);
        handle(cam + 8, 0x33330001 + (c % 5), 2);
        put<uint32_t>(cam + 0x14, 0xcafe0000 + c);
    }
    put<int32_t>(th + 0xfc, ncam);
    for (int c = 0; c < 10; c += 2) st.texts[0xcafe0000 + c] = P(S(c == 0 ? "Free camera" : "Follow camera"));
    put<int32_t>(CINMODE, 1);
    put<int32_t>(FILMLEN, 125);
    put<int32_t>(FILMSTART, 30000 - 300 * 61 - 150);
    put<float>(SPEED, 2.0f);
}
void territories() {
    for (int j = 0; j < 5; j++) {
        W.terr[j] = A(0x270);
        W.tcell[j] = A(8);
        live(ngm(0x1d80) + 0x10 * j, ngm(0x1d88) + 0x10 * j, W.tcell[j], W.terr[j], 0x7000 + j);
        put<int32_t>(W.terr[j] + 0x250, 5 - j);   // listed in reverse
        put<int32_t>(W.terr[j] + 0x234, j == 0 ? -1 : j % 2);
        put<int32_t>(W.terr[j] + 0x238, -1);
        put<int32_t>(W.terr[j] + 0x268, 10), put<int32_t>(W.terr[j] + 0x26c, 8);
    }
    put<uint32_t>(ngm(0x1dd0), 5);
    st.team_col[0] = 0x40ff2020, st.team_col[1] = 0x802020ff;
}
void carry_flag(uint32_t owner_id, bool live_handle) {
    W.flag = A(0x450);
    W.flagcell = A(8);
    put<uint32_t>(W.flag + 0x30, 0x310f465e);
    put<int32_t>(W.flag + 0x440, int32_t(owner_id));
    if (live_handle) live(ngm(0x1cc0), ngm(0x1cc8), W.flagcell, W.flag, 0x6000);
    // blink tag table: 2 entries, the tag second
    W.tags = A(16), W.tagent = A(0x20), W.tagval = A(16), W.tagbool = A(4);
    put<uint64_t>(W.flag + 0x68, P(W.tags));
    put<uint64_t>(W.tags, P(W.tagent));
    put<uint32_t>(W.tags + 8, 2);
    put<uint32_t>(W.tagent, 0x1234);
    put<uint32_t>(W.tagent + 0x10, 0xe9a3dd67);
    put<uint64_t>(W.tagent + 0x18, P(W.tagval));
    put<uint16_t>(W.tagval, 1);
    put<uint64_t>(W.tagval + 8, P(W.tagbool));
    put<uint32_t>(W.tagbool, 1);
}
void characters() {   // trackers' characters, held objects through vtable slot +0xe8 (stub fHeld)
    for (int i = 0; i < 10; i++) {
        W.chr[i] = A(0x2930), W.chrcell[i] = A(8), W.hobj[i] = A(16), W.hvt[i] = A(0xf0);
        live(trk(i) + 0x10, trk(i) + 0x18, W.chrcell[i], W.chr[i], 0x5000 + i);
        put<uint64_t>(W.chr[i] + 0x2928, P(W.hobj[i]));
        put<uint64_t>(W.hobj[i], P(W.hvt[i]));
        put<uint64_t>(W.hvt[i] + 0xe8, P(fHeld));
    }
}
void boosters_world() {
    W.brecs = A(0x20 * 4);
    W.boosters = A(16);
    put<int32_t>(W.boosters, 4);
    put<uint64_t>(W.boosters + 8, P(W.brecs));
    st.lookup[0x916e03b0] = P(W.boosters);
    const uint32_t sids[] = {0xb0000001, 0xb0000002, 0xb0000003, 0xbbbbbbbb};
    const uint32_t ids[] = {0x1001, 0x05454b23, 0x01845694, 0x1004};
    for (int i = 0; i < 4; i++) put<uint32_t>(W.brecs + 0x20 * i + 8, sids[i]), put<uint32_t>(W.brecs + 0x20 * i + 0xc, ids[i]);
    put<uint32_t>(BLCOUNT, 6);
    for (int i = 0; i < 6; i++) {
        W.blist[i] = A(0x20);
        put<uint32_t>(W.blist[i] + 8, 0xb0000001 + i);
        put<uint32_t>(W.blist[i] + 0xc, i == 4 ? 0x05454b23u : i == 5 ? 0x01845694u : 0x2001u + i);
        put<uint64_t>(BLIST + 8 * i, i == 2 ? 0 : P(W.blist[i]));
    }
    put<uint32_t>(LOAD0, 0x2001), put<uint32_t>(LOAD1, 0x2004);
    put<int32_t>(trk(0) + 0x2a0, 0), put<int32_t>(trk(0) + 0x2a4, 3);
    put<float>(HUDX, 33.5f);
}
void wave_world() {
    put<uint32_t>(W.ngrecs + 0x7c, 0x77770001);
    W.wave = A(16), W.waveent = A(12 * 5);
    put<int32_t>(W.wave, 5);
    put<uint64_t>(W.wave + 8, P(W.waveent));
    const int32_t e[5][3] = {{1, 0, 5}, {1, 30, 6}, {2, 45, 7}, {2, 90, 8}, {4, 200, 9}};
    for (int k = 0; k < 5; k++)
        for (int j = 0; j < 3; j++) put<int32_t>(W.waveent + 12 * k + 4 * j, e[k][j]);
    st.lookup[0x77770001] = P(W.wave);
    put<int32_t>(ngm(0x1d6c), 1);   // wave 2
    put<int32_t>(ngm(0x1d64), 30000 - 300 * 60);
    put<int32_t>(ngm(0x1d68), 0);
    st.money = 3;
}
void centre(const char *text, int32_t start, uint32_t colour) {
    std::memset(hx::mem + el(E_CM), 0, 0x90);
    std::strncpy(reinterpret_cast<char *>(hx::mem + el(E_CM)), text, 128);
    put<int32_t>(el(E_CM) + 0x80, start);
    put<uint32_t>(el(E_CM) + 0x84, colour);
    put<int32_t>(el(E_CM) + 0x88, 77);
}
void rich(int32_t type, uint32_t f) {
    world(type, f);
    messages_default();
    names_default();
    markers_default();
    theater_default();
    territories();
    characters();
    carry_flag(103, true);
    boosters_world();
    wave_world();
    put<int32_t>(C2F, 1000);
    centre("Capture the flag!", 1000 - 150, 0xc0ffe080);
}

void draw(int which, int frames = 1, const std::function<void(int)> &per_frame = nullptr, uint64_t ctx = 0) {
    for (int f = 0; f < frames; f++) {
        if (per_frame) per_frame(f);
        hx::rec("-- frame %d", f);
        g_draw[which](reinterpret_cast<void *>(P(W.element)), reinterpret_cast<void *>(ctx));
    }
}
void draw_scn(const std::string &name, const std::function<void()> &setup, int frames = 1,
              const std::function<void(int)> &per_frame = nullptr, uint64_t ctx = 0) {
    run(name, setup, [=](int w) { draw(w, frames, per_frame, ctx); });
}

// ---- weapon trigger world -------------------------------------------------------------------------------
void player_world(bool probe_slot) {
    W.player = A(0x2770), W.pcell = A(8), W.pvt = A(0x268), W.ctrl = A(16), W.cvt = A(0x2f8);
    put<uint64_t>(W.pcell, P(W.player));
    put<uint32_t>(W.player + 0x44, 0x4242);
    put<uint64_t>(LPCELL, P(W.pcell));
    put<uint32_t>(LPID, 0x4242);
    put<uint64_t>(W.player, P(W.pvt));
    put<uint64_t>(W.pvt + 0x260, P(fCurWeapon));
    put<uint64_t>(W.player + 0x2768, P(W.ctrl));
    put<uint64_t>(W.ctrl, P(W.cvt));
    put<uint64_t>(W.cvt + 0x2f0, probe_slot ? 0 : P(fShowWeapon));   // probe: set per side in the body
}

}  // namespace

int main() {
    std::set_terminate([] {
        std::printf("terminate; last calls:\n");
        for (size_t i = hx::calls.size() > 6 ? hx::calls.size() - 6 : 0; i < hx::calls.size(); i++)
            std::printf("  %.300s\n", hx::calls[i].c_str());
        std::fflush(stdout);
        std::abort();
    });
    hx::init();
    hx::bind_both();
    setenv_("UNCHARTED_U2MP_HUD", nullptr);
    setenv_("UNCHARTED_U2MP_WEAPON_HUD", nullptr);
    setenv_("UNCHARTED_U2MP_NO_THEATER", nullptr);

    // ================= install (trigger stays off) =================
    run("install/base NULL", [] { install_image(); },
        [](int w) {
            hx::set_image_both(nullptr, 0);
            install_body(w);
            hx::bind_both();
        });
    install_scn("install/UNCHARTED_U2MP_HUD=0", nullptr, "0");
    install_scn("install/UNCHARTED_U2MP_HUD=1 (not 0)", [] { hx::mem[0x5555a0] ^= 1; }, "1");
    for (size_t i = 0; i < checks1().size(); i++) {
        const uint64_t off = checks1()[i].off + checks1()[i].b.size() - 1;
        install_scn("install/check1 #" + std::to_string(i) + " broken", [off] { hx::mem[off] ^= 0x80; });
    }
    install_scn("install/stub not ret", [] { hx::mem[0x55c5b0 + 4] = 0x90; });
    for (size_t i = 0; i < checks2().size(); i++) {
        if (checks2()[i].off == 0xbdce90) continue;   // also in the first table
        const uint64_t off = checks2()[i].off + (i % checks2()[i].b.size());
        install_scn("install/group byte #" + std::to_string(i) + " broken", [off] { hx::mem[off] ^= 0x01; });
    }
    run("install/image too small (team score slot)", [] { install_image(); },
        [](int w) {
            hx::set_image_both(hx::mem, 0x104ae70 + 7);
            install_body(w);
            hx::bind_both();
            install_post(w);
        });
    run("install/image too small (vtable)", [] { install_image(); },
        [](int w) {
            hx::set_image_both(hx::mem, 0x103f550 + 0xf);
            install_body(w);
            hx::bind_both();
            install_post(w);
        });
    install_scn("install/slot not the stub", [] { put<uint64_t>(SLOT_DRAW, P(0x55c5b1)); });
    install_scn("install/UNCHARTED_U2MP_WEAPON_HUD=0", nullptr, nullptr, "0");
    for (size_t i = 0; i < checks_weapon().size(); i++) {
        const uint64_t off = checks_weapon()[i].off + checks_weapon()[i].b.size() - 1;
        install_scn("install/weapon bytes #" + std::to_string(i) + " broken", [off] { hx::mem[off] ^= 0x40; });
    }
    if (!g_draw[0] || !g_draw[1]) {
        std::printf("no Draw installed, stopping\n");
        return 1;
    }

    // ================= draw gates (trigger off) =================
    draw_scn("gate/net state 1", [] { rich(0, 0xffffffff); put<int32_t>(NETSTATE, 1); });
    draw_scn("gate/net state negative", [] { rich(0, 0xffffffff); put<int32_t>(NETSTATE, -5); });
    draw_scn("gate/flags pointer 0", [] { rich(0, 0xffffffff); put<uint64_t>(HUDFLAGS, 0); });
    draw_scn("gate/settings NULL", [] { rich(0, 0xffffffff); st.lookup.erase(0x9353ed16); });
    draw_scn("gate/settings set NULL", [] { rich(0, 0xffffffff); put<uint64_t>(W.rec + 0x90, 0); });
    draw_scn("gate/local 0", [] { rich(0, 0xffffffff); put<uint64_t>(ngm(0), 0); });
    draw_scn("gate/net games NULL", [] { rich(0, 0xffffffff); st.lookup.erase(0x3b9a067d); });
    draw_scn("gate/net game index -1", [] { rich(0, 0xffffffff); put<int16_t>(ngm(0x1ca0), -1); });
    draw_scn("gate/net game index = count", [] { rich(0, 0xffffffff); put<int16_t>(ngm(0x1ca0), 3); });
    draw_scn("gate/net game index 0x7fff", [] { rich(0, 0xffffffff); put<int16_t>(ngm(0x1ca0), 0x7fff); });
    draw_scn("gate/net game records NULL", [] { rich(0, 0xffffffff); put<uint64_t>(W.ng + 8, 0); });
    draw_scn("gate/net game index 1 (type 99)", [] { rich(0, 0xffffffff); put<int16_t>(ngm(0x1ca0), 1); });
    // draw counter without the trigger: 1, 2, ..., 600
    draw_scn("counter/draws to 600 (no trigger)", [] { world(0, 0); }, 600, nullptr, 0x5150);

    // ================= install with the trigger =================
    install_scn("install/all good (trigger on)", nullptr);
    install_scn("install/show slot other value", [] { put<uint64_t>(SLOT_SHOW, P(0x954741)); });
    install_scn("install/panel slot other value", [] { put<uint64_t>(SLOT_PANEL, 0); });
    install_scn("install/both slots other values", [] { put<uint64_t>(SLOT_SHOW, 1), put<uint64_t>(SLOT_PANEL, 2); });
    install_scn("install/UNCHARTED_U2MP_WEAPON_HUD=00", nullptr, nullptr, "00");
    draw_scn("counter/draws to 1200 (trigger on)", [] { world(1, 0); }, 600, nullptr, 0);

    // ================= every type with all flags, and with each flag cleared =================
    const int32_t types[] = {0, 1, 2, 3, 4, 7, 8, 9, 10, 11, 12, -1};
    const uint32_t bits[] = {0x10, 0x8000, 0x20000, 0x40000, 0x80000, 0x100000};
    for (int32_t t : types) {
        draw_scn("types/type " + std::to_string(t) + " all flags", [t] { rich(t, 0xffffffff); }, 2);
        for (uint32_t b : bits) {
            char n[64];
            std::snprintf(n, sizeof n, "types/type %d without 0x%x", t, b);
            draw_scn(n, [t, b] { rich(t, 0x1f8010 & ~b); }, 2);
        }
        draw_scn("types/type " + std::to_string(t) + " only HUD flags off", [t] { rich(t, 0); }, 1);
    }
    draw_scn("types/draw context passed on", [] { rich(9, 0xffffffff); }, 1, nullptr, 0xc0ffee);
    draw_scn("types/no theater env set", [] {
        rich(0, 0xffffffff);
    }, 1, [](int) { setenv_("UNCHARTED_U2MP_NO_THEATER", "1"); });
    setenv_("UNCHARTED_U2MP_NO_THEATER", nullptr);
    draw_scn("types/no theater env empty-ish (unset)", [] { rich(0, 0xffffffff); }, 1);

    // ================= score panel =================
    for (int team : {0, 1, 5, 9, -1}) {
        draw_scn("score/team " + std::to_string(team), [team] {
            world(0, 0x40000);
            put<int32_t>(trk(0) + 0x134, team);
        });
    }
    draw_scn("score/score table 0", [] { world(0, 0x40000); put<uint64_t>(SCORE, 0); });
    draw_scn("score/type 10 round + match start", [] { world(10, 0x40000); put<int32_t>(ngm(0x1d70), 0x7fffffff); });
    draw_scn("score/label not found", [] { world(0, 0x40000); st.texts.clear(); });
    draw_scn("score/label widget NULL", [] { world(10, 0x40000); put<uint64_t>(W.set + 0x38, 0); });
    for (uint64_t fld : {0x08, 0x10, 0x30, 0x38, 0x40, 0x48, 0x58}) {
        char n[64];
        std::snprintf(n, sizeof n, "score/widget +0x%llx NULL", (unsigned long long)fld);
        draw_scn(n, [fld] { world(2, 0x40000); put<uint64_t>(W.set + fld, 0); });
    }
    const float limits[] = {0.0f, -5.0f, 600.0f, 0.001f, 0.004f, 7e6f, 1e10f, NaN, Inf, -Inf, 3.5f};
    for (float lim : limits) {
        char n[64];
        std::snprintf(n, sizeof n, "score/time limit %g", double(lim));
        draw_scn(n, [lim] { world(1, 0x40000); put<float>(ngm(0x21b8), lim); });
    }
    draw_scn("score/elapsed beyond the limit", [] {
        world(1, 0x40000);
        put<float>(ngm(0x21b8), 60.0f);
        put<int32_t>(ngm(0x1d5c), 30000 - 300 * 3725);
    });
    draw_scn("score/frame counter wrap", [] {
        world(10, 0x40000);
        put<int32_t>(FRAMES, int32_t(0x80000010u));
        put<int32_t>(ngm(0x1d60), 0x7ffffff0);
    });
    draw_scn("score/frame advance", [] { world(0, 0x40000); }, 5,
             [](int f) { put<int32_t>(FRAMES, 30000 + 290 * f); });

    // ================= carrier icon =================
    auto carrier_base = [](uint32_t owner, bool live_handle) {
        world(0, 0x40000);
        characters();
        carry_flag(owner, live_handle);
    };
    draw_scn("carrier/live handle, carrier other team", [=] { carrier_base(103, true); });
    draw_scn("carrier/live handle, carrier own team", [=] { carrier_base(102, true); });
    draw_scn("carrier/live handle, no carrier", [=] { carrier_base(55, true); });
    draw_scn("carrier/dead handle id mismatch, no held", [=] {
        carrier_base(103, true);
        put<uint32_t>(ngm(0x1cc8), 0x6001);
    });
    draw_scn("carrier/held flag at tracker 3", [=] {
        carrier_base(103, false);
        st.held[P(W.hobj[1])] = P(W.tagent);   // wrong type first
        st.held[P(W.hobj[3])] = P(W.flag);
    });
    draw_scn("carrier/held: wrong type, vtable 0, slot 0, char dead", [=] {
        carrier_base(103, false);
        st.held[P(W.hobj[0])] = P(W.tagent);
        put<uint64_t>(W.hobj[1], 0);
        put<uint64_t>(W.hvt[2] + 0xe8, 0);
        put<uint64_t>(W.chr[3] + 0x2928, 0);
        put<uint32_t>(W.chr[4] + 0x44, 1);
        put<uint64_t>(trk(5) + 0x10, 0);
        put<uint32_t>(trk(6) + 0x18, 0);
        put<uint64_t>(W.chrcell[7], 0);
        st.held[P(W.hobj[9])] = P(W.flag);
    });
    draw_scn("carrier/handle slot 0 and id 0", [=] {
        carrier_base(103, true);
        put<uint64_t>(ngm(0x1cc0), 0);
        put<uint32_t>(ngm(0x1cc8), 0);
    });
    draw_scn("carrier/flag object pointer 0", [=] { carrier_base(103, true); put<uint64_t>(W.flagcell, 0); });
    draw_scn("carrier/blink frames 0..1200", [=] { carrier_base(103, true); }, 12,
             [](int f) { put<int32_t>(FRAMES, 30000 + 100 * f + (f % 3)); });
    draw_scn("carrier/blink frames negative", [=] { carrier_base(103, true); }, 6,
             [](int f) { put<int32_t>(FRAMES, -1000 + 140 * f); });
    draw_scn("carrier/blink frames huge", [=] { carrier_base(103, true); }, 4,
             [](int f) { put<int32_t>(FRAMES, 0x7fffff00 - 150 * f); });
    auto blink_case = [&](const char *n, const std::function<void()> &mod) {
        draw_scn(std::string("carrier/blink ") + n, [=] { carrier_base(103, true); mod(); }, 2,
                 [](int f) { put<int32_t>(FRAMES, 30000 + 160 * f); });
    };
    blink_case("no carrier", [] { put<int32_t>(W.flag + 0x440, 7); });
    blink_case("tag value type 2", [] { put<uint16_t>(W.tagval, 2); });
    blink_case("tag bool 0", [] { put<uint32_t>(W.tagbool, 0); });
    blink_case("tag bool pointer 0", [] { put<uint64_t>(W.tagval + 8, 0); });
    blink_case("tag value pointer 0", [] { put<uint64_t>(W.tagent + 0x18, 0); });
    blink_case("count 0", [] { put<uint32_t>(W.tags + 8, 0); });
    blink_case("count 4097", [] { put<uint32_t>(W.tags + 8, 4097); });
    blink_case("count 1 (tag absent)", [] { put<uint32_t>(W.tags + 8, 1); });
    blink_case("table 0", [] { put<uint64_t>(W.flag + 0x68, 0); });
    blink_case("entries 0", [] { put<uint64_t>(W.tags, 0); });
    blink_case("tag twice, first false", [] {
        put<uint32_t>(W.tagent, 0xe9a3dd67);
        put<uint64_t>(W.tagent + 8, 0);
    });
    draw_scn("carrier/types 1 and 2 have none", [=] { carrier_base(103, true); gtype(1); });

    // ================= Turf War =================
    auto turf = [](int team) {
        world(7, 0x40000);
        put<int32_t>(trk(0) + 0x134, team);
        territories();
    };
    draw_scn("turf/local team 0", [=] { turf(0); });
    draw_scn("turf/local team 1 (reversed)", [=] { turf(1); });
    draw_scn("turf/local team 2", [=] { turf(2); });
    draw_scn("turf/missing, id mismatch, slot 0, count 3", [=] {
        turf(0);
        put<uint32_t>(W.terr[1] + 0x44, 1);
        put<uint64_t>(ngm(0x1d80) + 0x20, 0);
        put<uint32_t>(ngm(0x1dd0), 4);
        put<uint64_t>(W.tcell[3], 0);
    });
    draw_scn("turf/count 9 (limited to 5)", [=] { turf(1); put<uint32_t>(ngm(0x1dd0), 9); });
    draw_scn("turf/count 0", [=] { turf(1); put<uint32_t>(ngm(0x1dd0), 0); });
    draw_scn("turf/capture progress cases", [=] {
        turf(0);
        // j0 index 5: capturing, gain end with 0.5 progress
        put<int32_t>(W.terr[0] + 0x238, 1);
        put<int32_t>(W.terr[0] + 0x258, 30000 + 1500);
        // j1 index 4: gain 0.995+
        put<int32_t>(W.terr[1] + 0x238, 0);
        put<int32_t>(W.terr[1] + 0x258, 30000 - 15);
        // j2 index 3: loss
        put<int32_t>(W.terr[2] + 0x238, 1);
        put<int32_t>(W.terr[2] + 0x25c, 30000 + 600);
        // j3 index 2: neither end set: progress 0
        put<int32_t>(W.terr[3] + 0x238, 0);
        // j4 index 1: zero length
        put<int32_t>(W.terr[4] + 0x238, 1);
        put<int32_t>(W.terr[4] + 0x258, 30000);
        put<int32_t>(W.terr[4] + 0x268, 0);
    }, 3, [](int f) { put<int32_t>(FRAMES, 30000 + 450 * f); });
    draw_scn("turf/zero length loss, huge and negative lengths", [=] {
        turf(1);
        for (int j = 0; j < 5; j++) put<int32_t>(W.terr[j] + 0x238, j % 2);
        put<int32_t>(W.terr[0] + 0x25c, 29000), put<int32_t>(W.terr[0] + 0x26c, 0);
        put<int32_t>(W.terr[1] + 0x258, 29000), put<int32_t>(W.terr[1] + 0x268, -4);
        put<int32_t>(W.terr[2] + 0x258, 29000), put<int32_t>(W.terr[2] + 0x268, 0x7fffffff);
        put<int32_t>(W.terr[3] + 0x25c, 31000), put<int32_t>(W.terr[3] + 0x26c, 20000000);
        put<int32_t>(W.terr[4] + 0x258, 29999), put<int32_t>(W.terr[4] + 0x268, 1);
    });
    draw_scn("turf/tw NULL", [=] { turf(0); put<uint64_t>(W.set + 0x78, 0); });
    draw_scn("turf/img NULL", [=] { turf(0); put<uint64_t>(W.set + 0x60, 0); });
    draw_scn("turf/NaN rect", [=] {
        turf(0);
        put<float>(W.set + 0x70, NaN), put<float>(W.set + 0x74, Inf);
        put<float>(W.w[0x60] + 0x24, -Inf);
    });
    draw_scn("turf/owner colours with alpha bits", [=] {
        turf(0);
        st.team_col[0] = 0x00000000, st.team_col[1] = 0xffffffff;
        put<int32_t>(W.terr[2] + 0x234, 7);
        put<int32_t>(W.terr[2] + 0x238, 7);
        put<int32_t>(W.terr[2] + 0x258, 30100);
    });

    // ================= co-op panel =================
    auto coop = [](uint32_t f) {
        world(9, f);
        wave_world();
    };
    draw_scn("coop/flag off", [=] { coop(0); });
    draw_scn("coop/flag on", [=] { coop(0x8000); });
    draw_scn("coop/type 4 flag on", [=] { coop(0x8000); gtype(4); });
    for (float wa : {0.5f, NaN, -0.0f, 1e-30f}) {
        char n[64];
        std::snprintf(n, sizeof n, "coop/wave alpha %g", double(wa));
        draw_scn(n, [=] { coop(0x8000); put<float>(WAVEA, wa); });
    }
    draw_scn("coop/txt NULL", [=] { coop(0x8000); put<uint64_t>(W.set + 0xc8, 0); });
    draw_scn("coop/back image NULL", [=] { coop(0x8000); put<uint64_t>(W.set + 0xd0, 0); });
    draw_scn("coop/wave table id 0", [=] { coop(0x8000); put<uint32_t>(W.ngrecs + 0x7c, 0); });
    draw_scn("coop/wave table missing", [=] { coop(0x8000); st.lookup.erase(0x77770001); });
    draw_scn("coop/wave entries NULL", [=] { coop(0x8000); put<uint64_t>(W.wave + 8, 0); });
    draw_scn("coop/wave found at entry 2..3 over time", [=] { coop(0x8000); }, 8,
             [](int f) { put<int32_t>(FRAMES, 30000 + 300 * 20 * f); });
    draw_scn("coop/wave greater than all", [=] { coop(0x8000); put<int32_t>(ngm(0x1d6c), 9); });
    draw_scn("coop/wave smaller (0)", [=] { coop(0x8000); put<int32_t>(ngm(0x1d6c), -1); });
    draw_scn("coop/wave 3 (gap)", [=] { coop(0x8000); put<int32_t>(ngm(0x1d6c), 2); });
    draw_scn("coop/wave 1 entry 0 (prev = itself)", [=] {
        coop(0x8000);
        put<int32_t>(ngm(0x1d6c), 0);
        put<int32_t>(W.waveent + 4, -10);
    });
    draw_scn("coop/end > start", [=] {
        coop(0x8000);
        put<int32_t>(ngm(0x1d68), 30000 - 300 * 10);
    });
    draw_scn("coop/end <= start", [=] {
        coop(0x8000);
        put<int32_t>(ngm(0x1d68), 30000 - 300 * 61);
    });
    draw_scn("coop/equal times (division by zero)", [=] {
        coop(0x8000);
        put<int32_t>(W.waveent + 12 * 3 + 4, 45);
        put<int32_t>(ngm(0x1d64), 30000 - 300 * 45);
    });
    draw_scn("coop/money negative, colour NaN", [=] {
        coop(0x8000);
        st.money = -7;
        put<float>(W.w[0xc8] + 0x24, NaN);
    });

    // ================= boosters =================
    auto bw = [](int32_t type, uint32_t f) {
        world(type, f);
        boosters_world();
        put<uint8_t>(ngm(0x209c), 1);
        put<int32_t>(CINMODE, 2);
    };
    draw_scn("boosters/film + loadout", [=] { bw(0, 0x40010); });
    for (auto ix : std::vector<std::pair<int32_t, int32_t>>{{-1, 0}, {0, -1}, {1, 2}, {4, 3}, {2, 1}, {3, 0x7fffffff}}) {
        char n[64];
        std::snprintf(n, sizeof n, "boosters/film indices %d %d", ix.first, ix.second);
        draw_scn(n, [=] {
            bw(0, 0x40000);
            put<int32_t>(trk(0) + 0x2a0, ix.first), put<int32_t>(trk(0) + 0x2a4, ix.second);
        });
    }
    draw_scn("boosters/film table NULL", [=] { bw(0, 0x40000); st.lookup.erase(0x916e03b0); });
    draw_scn("boosters/film records NULL", [=] { bw(0, 0x40000); put<uint64_t>(W.boosters + 8, 0); });
    draw_scn("boosters/film cinema mode 0", [=] { bw(0, 0x40000); put<int32_t>(CINMODE, 0); });
    draw_scn("boosters/film not playing", [=] { bw(0, 0x40000); put<uint8_t>(ngm(0x209c), 0); });
    for (auto ids : std::vector<std::pair<uint32_t, uint32_t>>{
             {0, 0x2001}, {0x9999, 0x2004}, {0x2001, 0x2001}, {0x05454b23, 0x2004}, {0x2001, 0x01845694},
             {0x01845694, 0x05454b23}, {0x2003, 0x2004}, {0x2006, 0}}) {
        char n[64];
        std::snprintf(n, sizeof n, "boosters/loadout %08x %08x", ids.first, ids.second);
        draw_scn(n, [=] {
            bw(0, 0x10);
            put<uint32_t>(LOAD0, ids.first), put<uint32_t>(LOAD1, ids.second);
        });
    }
    draw_scn("boosters/list count 70", [=] { bw(0, 0x10); put<uint32_t>(BLCOUNT, 70); put<uint32_t>(LOAD0, 0x77); });
    draw_scn("boosters/list count 3", [=] { bw(0, 0x10); put<uint32_t>(BLCOUNT, 3); });
    for (int32_t t : {0, 9}) {
        for (int32_t off : {0, 1}) {
            for (int32_t r6c : {0, 1}) {
                char n[80];
                std::snprintf(n, sizeof n, "boosters/allowed type %d off %d rec+0x6c %d", t, off, r6c);
                draw_scn(n, [=] {
                    bw(t, 0x10);
                    put<int32_t>(ngm(0x1e80), off);
                    put<int32_t>(W.ngrecs + 0x6c, r6c);
                });
            }
        }
    }
    draw_scn("boosters/fade 0 (no quads)", [=] { bw(0, 0x40010); put<float>(el(E_FADE), 0.0f); });
    draw_scn("boosters/fade NaN", [=] { bw(0, 0x40010); put<float>(el(E_FADE), NaN); });
    draw_scn("boosters/hud x NaN", [=] { bw(0, 0x40010); put<float>(HUDX, NaN); });

    // ================= theater =================
    auto th = [](int ncam, int32_t mode) {
        world(0, 0x100000);
        theater_default(ncam);
        put<int32_t>(CINMODE, mode);
    };
    draw_scn("theater/cams 0", [=] { th(0, 1); });
    draw_scn("theater/cams 11", [=] { th(11, 1); });
    draw_scn("theater/cams -3", [=] { th(-3, 1); });
    for (int32_t mode : {0, 1, 2, 3}) {
        draw_scn("theater/mode " + std::to_string(mode) + " 10 cams", [=] { th(10, mode); }, 2);
    }
    draw_scn("theater/n = 0 (only never-shown cams)", [=] {
        th(1, 1);
        put<uint32_t>(el(E_TH) + 0xc, 0xe8184ce8);
    }, 2);
    // L1 / R1 presses with edge detection; release ids; cross
    draw_scn("theater/selector script", [=] { th(6, 1); }, 24, [](int f) {
        static const uint32_t pad[] = {0, 0x800, 0x800, 0, 0x800, 0, 0x800, 0, 0x800, 0, 0x800, 0, 0x800, 0,
                                       0x400, 0, 0x400, 0xc00, 0, 0x4000, 0x4000, 0, 0x4800, 0x400};
        put<uint32_t>(PAD, pad[f]);
    });
    draw_scn("theater/cross held with pending release", [=] {
        th(4, 1);
        put<uint32_t>(el(E_TH) + 8, 0xbeef);
    }, 4, [](int f) { put<uint32_t>(PAD, f < 2 ? 0x4400 : 0x400); });
    draw_scn("theater/cross pressed while fading in", [=] { th(4, 2); }, 12,
             [](int f) { put<uint32_t>(PAD, (f % 2) ? 0x4000 : 0); });
    draw_scn("theater/cross pressed with sel beyond n", [=] {
        th(3, 1);
        put<int32_t>(el(E_TH), 9);
        put<float>(el(E_TH) + 0x154, 0.95f);
    }, 2, [](int f) { put<uint32_t>(PAD, f ? 0x4000 : 0); });
    draw_scn("theater/sel negative", [=] { th(3, 1); put<int32_t>(el(E_TH), -4); }, 2);
    for (int32_t len : {0, 125, -5, 30}) {
        draw_scn("theater/length " + std::to_string(len), [=] { th(4, 1); put<int32_t>(FILMLEN, len); }, 1);
    }
    draw_scn("theater/negative position", [=] { th(4, 1); put<int32_t>(FILMSTART, 30000 + 9000); });
    draw_scn("theater/position at whole seconds", [=] { th(4, 1); put<int32_t>(FILMSTART, 0); put<int32_t>(FILMLEN, 100000); }, 40,
             [](int f) { put<int32_t>(FRAMES, 300 * (1 + f * 37)); });
    draw_scn("theater/position wrap", [=] {
        th(4, 1);
        put<int32_t>(FRAMES, int32_t(0x80000100u));
        put<int32_t>(FILMSTART, 0x7fffff00);
    });
    for (float sp : {1.0f, 2.0f, 0.0f, 150.0f, 0.25f, 99.99f, NaN, -1.0f}) {
        char n[64];
        std::snprintf(n, sizeof n, "theater/speed %g", double(sp));
        draw_scn(n, [=] { th(4, 1); put<float>(SPEED, sp); });
    }
    draw_scn("theater/square not resolved", [=] { th(4, 1); st.mat_fail.insert(0x12ffe6ae); }, 2);
    draw_scn("theater/smooth and fade NaN", [=] {
        th(4, 1);
        put<float>(el(E_TH) + 4, NaN), put<float>(el(E_TH) + 0x154, NaN);
    }, 2);
    draw_scn("theater/no name text", [=] { th(4, 1); st.texts.clear(); });
    draw_scn("theater/env NO_THEATER", [=] { th(4, 1); }, 1, [](int) { setenv_("UNCHARTED_U2MP_NO_THEATER", "x"); });
    setenv_("UNCHARTED_U2MP_NO_THEATER", nullptr);
    draw_scn("theater/flag but no film", [=] { th(4, 1); put<uint8_t>(ngm(0x209c), 0); });

    // ================= message list =================
    auto ml = [](int count) {
        world(0, 0x80000);
        messages_default();
        put<int32_t>(el(E_MSG), count);
    };
    for (int c : {0, 17, -1, 1, 3}) draw_scn("messages/count " + std::to_string(c), [=] { ml(c); });
    draw_scn("messages/count 16 mixed", [=] {
        ml(16);
        const uint64_t ics[] = {icon_rec("hud/weapon/pistol", 64, 64, 0, 0), icon_rec("", 64, 64, 1, 1),
                                icon_rec(nullptr, 64, 64, 1, 1), icon_rec("hud/weapon/big", 2000, 64, 1, 1),
                                icon_rec("hud/weapon/zero", 0, 64, 1, 1), icon_rec("hud/weapon/neg", 64, -1, 1, 1),
                                icon_rec("hud/weapon/limit", 1024, 1023.9f, 1, 1),
                                icon_rec("hud/weapon/ratio4", 64, 32, 1, 4), icon_rec("hud/weapon/ratio3.9", 64, 32, 1, 3.9f),
                                icon_rec("idx/weapon", 64, 32, 2, 1), icon_rec("hud/weapon/nan", NaN, 32, 1, 1),
                                icon_rec("hud/weapon/divnan", 64, 32, NaN, 1)};
        for (int i = 3; i < 16; i++) {
            char t1[64], t2[64];
            std::snprintf(t1, sizeof t1, i == 7 ? "A very long message text number %d that is too wide" : "Kill %d", i);
            std::snprintf(t2, sizeof t2, i % 4 == 0 ? "" : "Victim %d", i);
            msg(i, i == 9 ? "" : t1, 0x00010203 * i, t2, 0x00302010 * i, 100.0f - 0.2f * i, 100.0f + 0.3f * i,
                uint32_t(i % 16), i % 5 - 1, i - 3 < 12 ? ics[i - 3] : 0);
        }
    }, 3, [](int f) { put<int32_t>(MSGCLOCK, 30000 + 60 * f); });
    draw_scn("messages/badge square not resolved", [=] { ml(3); st.mat_fail.insert(0x12ffe6ae); });
    draw_scn("messages/badge prefix missing", [=] { ml(3); st.texts.erase(0x9b5b3c66); });
    draw_scn("messages/badge long prefix", [=] { ml(3); st.texts[0x9b5b3c66] = P(S("PrefixThatIsLongerThanTwenty")); });
    draw_scn("messages/back NULL", [=] { ml(3); put<uint64_t>(W.set + 0xb8, 0); });
    draw_scn("messages/headshot not resolved", [=] { ml(3); st.mat_fail.insert(0x2fb98f98); });
    draw_scn("messages/DrawText NaN sizes", [=] { ml(3); st.dt_mode = 1; });
    draw_scn("messages/DrawText huge sizes", [=] { ml(3); st.dt_mode = 2; });
    draw_scn("messages/DrawText writes nothing", [=] { ml(3); st.dt_mode = 3; });
    draw_scn("messages/hud alpha 0", [=] { ml(3); put<float>(ALPHA, 0.0f); });
    draw_scn("messages/times NaN", [=] {
        ml(3);
        put<float>(el(E_MSG) + 8 + 0x70, NaN), put<float>(el(E_MSG) + 8 + 0x80 + 0x74, Inf);
    });
    draw_scn("messages/48-char texts", [=] {
        ml(1);
        std::memset(hx::mem + el(E_MSG) + 8, 'M', 48);
        std::memset(hx::mem + el(E_MSG) + 8 + 0x34, 'N', 48);
    });

    // ================= names =================
    auto nm = [](int count) {
        world(0, 0x20000);
        names_default();
        put<int32_t>(el(E_NAMES) + 0xc80, count);
    };
    for (int c : {0, -2, 3}) draw_scn("names/count " + std::to_string(c), [=] { nm(c); });
    draw_scn("names/count 21", [=] {
        nm(21);
        for (int i = 0; i < 21; i++) {
            char n[32];
            std::snprintf(n, sizeof n, i % 7 == 6 ? "" : "Player%02d", i);
            name_entry(i, -100.0f + 80.0f * i, 50.0f * (i % 9), 0.5f + 0.05f * i, (i % 9 == 8) ? 7 : i % 4, i % 4 == 3 ? 5 : i % 3,
                       (i % 5) * 0.25f, i % 3, (i % 4) * 0.2f, n, (i % 6) * 0.2f, (i % 5 == 0) ? 0x99 : 0,
                       uint8_t(i % 2), 50 + i * 3);
        }
    }, 4, [](int f) { put<int32_t>(C1F, 12345 + 23 * f); });
    for (int side : {0, 1, 2, 3, 7, -1}) {
        for (int status : {0, 1, 2}) {
            char n[64];
            std::snprintf(n, sizeof n, "names/side %d status %d", side, status);
            draw_scn(n, [=] {
                nm(4);
                name_entry(0, 500, 300, 1.0f, side, 1, 0.9f, status, 0.5f, "Nate", 1.0f, 0, 1, 10);
                name_entry(1, 500, 300, 1.0f, side, 0, 0.9f, status, 0.1f, "Elena", 0.0f, 0, 1, 70);
                name_entry(2, 500, 300, 1.0f, side, 2, 0.9f, status, 0.1f, "", 1.0f, 0, 0, 10);
                name_entry(3, 500, 300, 1.0f, side, 5, 0.9f, status, 0.1f, "Drake", 1.0f, 0, 0, 10);
            }, 2, [](int f) { put<int32_t>(C1F, 12345 + 50 * f); });
        }
    }
    draw_scn("names/status 1 button on", [=] {
        nm(1);
        name_entry(0, 500, 300, 1.0f, 3, 1, 0.9f, 1, 0.1f, "Nate", 1.0f, 0, 0, 10);
        hx::mem[W.fb + 0x40c] = 1;
    }, 3, [](int f) { put<int32_t>(C1F, 100 * f); });
    draw_scn("names/fill < 0.25 both blink phases", [=] {
        nm(1);
        name_entry(0, 500, 300, 1.0f, 3, 1, 0.9f, 2, 0.2f, "Nate", 1.0f, 0, 0, 10);
    }, 6, [](int f) { put<int32_t>(C1F, 30 * f); });
    for (int rank : {58, 59, 63, 64, 100, -1, 0x7fffffff}) {
        draw_scn("names/rank " + std::to_string(rank), [=] {
            nm(2);
            name_entry(0, 500, 300, 1.0f, 0, 1, 0.9f, 0, 0, "Nate", 0.5f, 0, 0, rank);
            name_entry(1, 500, 300, 1.0f, 0, 1, 0.9f, 0, 0, "Sully", 0.5f, 0x55, 1, rank);
        });
    }
    draw_scn("names/left clamping", [=] {
        nm(4);
        name_entry(0, -500, 300, 1.0f, 2, 1, 0.9f, 0, 0, "Left", 1.0f, 0, 1, 10);
        name_entry(1, 5000, 300, 1.0f, 1, 1, 0.9f, 0, 0, "Right", 1.0f, 0, 1, 100);
        name_entry(2, 640, 300, 30.0f, 2, 1, 0.9f, 0, 0, "Huge", 1.0f, 0, 1, 100);
        name_entry(3, 130, 300, 1.0f, 2, 1, 0.9f, 0, 0, "Edge", 1.0f, 0, 0, 10);
    });
    draw_scn("names/float edges", [=] {
        nm(4);
        name_entry(0, NaN, NaN, 1.0f, 0, 1, NaN, 1, NaN, "NaN", NaN, 0, 1, 70);
        name_entry(1, Inf, -Inf, Inf, 2, 2, Inf, 2, Inf, "Inf", Inf, 0, 1, 70);
        name_entry(2, 1e30f, 1e30f, 1e20f, 1, 0, -1.0f, 2, -5.0f, "Huge", 2.0f, 0, 1, 70);
        name_entry(3, 500, 300, -1.0f, 3, 1, 0.5f, 1, 2.0f, "NegScale", 0.5f, 0, 1, 70);
        put<float>(el(E_NAMES) + 0x20, -0.5f);   // colour component negative
    }, 2, [](int f) { put<int32_t>(C1F, 12345 + 7 * f); });
    draw_scn("names/status 1 without button byte", [=] {
        nm(1);
        name_entry(0, 500, 300, 1.0f, 2, 1, 0.9f, 1, 0.1f, "Nate", 1.0f, 0, 0, 10);
    }, 1, [](int) {});   // flags pointer 0 stops the whole Draw: covered by gates

    // ================= markers =================
    auto mk = [](int count) {
        world(0, 0x20000);
        markers_default();
        put<int32_t>(el(E_MK), count);
    };
    for (int c : {0, -1, 3}) draw_scn("markers/count " + std::to_string(c), [=] { mk(c); });
    draw_scn("markers/count 9 all types", [=] {
        mk(9);
        const uint32_t tys[] = {32, 1, 11, 4, 5, 7, 10, 2, 0};
        for (int i = 0; i < 9; i++) marker(i, tys[i], 0.1f * i, i % 2 ? 1.0f : 0.0f, i % 3 ? 0.5f : 0.0f, 12345 - 10 * i);
        put<uint32_t>(MASK, ~(1u << 1));
    }, 5, [](int f) { put<int32_t>(C1F, 12345 + 20 * f), put<int32_t>(C1D, 3 + f * 10); });
    draw_scn("markers/stale entry", [=] {
        mk(2);
        marker(0, 4, 0.7f, 1.0f, 0.5f, 12345 - 301);
        marker(1, 4, 0.7f, 1.0f, 0.5f, 12345 - 300);
    });
    draw_scn("markers/fade in, out, reach target", [=] {
        mk(4);
        marker(0, 4, 0.0f, 1.0f, 0, 12345);
        marker(1, 4, 1.0f, 0.0f, 0, 12345);
        marker(2, 6, 0.98f, 1.0f, 0, 12345);
        marker(3, 8, 0.02f, 0.0f, 0, 12345);
    }, 6, [](int f) {
        put<int32_t>(C1F, 12345 + f);
        // re-arm the targets the way the game's Update would
        put<float>(el(E_MK) + 0x10 + 0x30, 1.0f);
        put<float>(el(E_MK) + 0x10 + 0x80 + 0x30, f < 3 ? 1.0f : 0.5f);
        for (int i = 0; i < 4; i++) put<int32_t>(el(E_MK) + 0x10 + 0x40 * i + 0x3c, 12345 + f);
    });
    draw_scn("markers/float edges", [=] {
        mk(5);
        marker(0, 4, NaN, 1.0f, NaN, 12345);
        marker(1, 5, Inf, 1.0f, -0.5f, 12345);
        marker(2, 6, 0.5f, NaN, 1.5f, 12345);
        marker(3, 9, 0.5f, 1.0f, 2.0f, 12345, 0x00000000, 0x00ffffff);
        marker(4, 10, 0.5f, 1.0f, 0.5f, 12345, 0xffffffff, 0xffffffff);
        st.proj = {proj(NaN, NaN, NaN, NaN, NaN, NaN, 1), proj(Inf, -Inf, 1, 0, 0, Inf, 1),
                   proj(1e30f, 1e30f, 1, 1, 1e30f, 1e20f, 5), proj(10, 10, 0, 0, 0, 0, 0),
                   proj(10, 10, 0, 0, 0, -2.0f, 1)};
    }, 2);
    draw_scn("markers/huge delta and frame wrap", [=] {
        mk(3);
        put<int32_t>(C1F, int32_t(0x80000005u));
        for (int i = 0; i < 3; i++) put<int32_t>(el(E_MK) + 0x10 + 0x40 * i + 0x3c, 0x7ffffffe);
        put<int32_t>(C1D, 0x7fffffff);
    });
    draw_scn("markers/handles unresolved, label set, dark", [=] {
        mk(3);
        st.mat_fail.insert(0x22220004);
        st.mat_fail.insert(0x22230004);
        marker(0, 4, 1.0f, 1.0f, 0.25f, 12345);
        marker(1, 3, 1.0f, 1.0f, 0.25f, 12345);
        marker(2, 0, 1.0f, 1.0f, 0.25f, 12345);
    });

    // ================= centre message =================
    auto cm = [](const char *text, int32_t start) {
        world(0, 0);
        put<int32_t>(C2F, 10000);
        centre(text, start, 0xc0ffe080);
    };
    draw_scn("centre/empty", [=] { cm("", 0); });
    for (int32_t d : {-100, 0, 30, 75, 300, 700, 742, 750, 800, 900}) {
        draw_scn("centre/ascii dt " + std::to_string(d), [=] { cm("Hello", 10000 - d); });
    }
    draw_scn("centre/utf8 over time", [=] { cm("\xc3\x9c" "ber \xe2\x82\xac" "100 \xf0\x9f\x98\x80", 10000); }, 20,
             [](int f) { put<int32_t>(C2F, 10000 + 41 * f); });
    draw_scn("centre/128 chars no NUL", [=] {
        cm("", 9000);
        std::memset(hx::mem + el(E_CM), 'Z', 128);
    }, 2);
    draw_scn("centre/clock wrap", [=] { cm("Wrap", 0x7fffff00); put<int32_t>(C2F, int32_t(0x80000010u)); });
    draw_scn("centre/hud alpha NaN", [=] { cm("Alpha", 9950); put<float>(ALPHA, NaN); });
    for (float ha : {1.5f, 2.0f, -0.5f, Inf}) {
        char n[64];
        std::snprintf(n, sizeof n, "centre/hud alpha %g", double(ha));
        draw_scn(n, [=] { cm("Alpha", 9950); put<float>(ALPHA, ha); });
        std::snprintf(n, sizeof n, "types/rich world, hud alpha %g", double(ha));
        draw_scn(n, [=] { rich(0, 0xffffffff); put<float>(ALPHA, ha); }, 2);
    }
    draw_scn("names/arrow colour NaN / out of range components", [=] {
        world(0, 0x20000);
        names_default();
        put<int32_t>(el(E_NAMES) + 0xc80, 3);
        for (int i = 0; i < 3; i++) {
            const uint64_t e = el(E_NAMES) + 0xa0 * i;
            const float c[3][4] = {{NaN, 0.5f, NaN, 0.4f}, {2.0f, -1.0f, 0.999f, NaN}, {0.5f, NaN, 1.5f, 0.3f}};
            std::memcpy(hx::mem + e + 0x20, c[i], 16);
        }
    });

    // ================= resolve cache and logs =================
    // log-once keys of image widgets (table | 0x100) and of resolves (table) are different keys
    draw_scn("cache/widget and resolve log keys", [] {
        world(0, 0x60000);
        names_default();
        put<int32_t>(el(E_NAMES) + 0xc80, 1);
        put<uint32_t>(W.w[0x58] + 0x14, 0x5a5a0001);   // table 0 widget, fails
        put<uint32_t>(W.w[0x58] + 0x10, 0x4240ef2e);   // net: table 1
        handle(el(E_NAMES) + 0xca8, 0x5a5a0001, 1);    // same id, table 1, as a resolve
        handle(el(E_NAMES) + 0xcb4, 0x5a5a0002, 0);
        put<uint32_t>(W.w[0x28] + 0x14, 0x5a5a0002);   // reverse order happens on frame 2 (carrier)
        put<int32_t>(W.w[0x28] + 0x18, -1);
    }, 2);
    // the round-robin replacement: 90 keys used in an overlapping pattern, so hits depend on what was evicted
    draw_scn("cache/eviction order", [] {
        world(0, 0x80000);
        put<int32_t>(MSGCLOCK, 30000);
        for (int i = 0; i < 16; i++) msg(i, "", 0, "", 0, 99.0f, 107.0f, 0, 0, 0);
        put<int32_t>(el(E_MSG), 16);
        g_icons.clear();
        for (int k = 0; k < 90; k++) {
            char n[32];
            std::snprintf(n, sizeof n, "e/%d", k);
            g_icons.push_back(icon_rec(n, 32, 16, 1, 1));
        }
        st.mat_alias[0x22220005] = 0;
    }, 24, [](int f) {
        for (int i = 0; i < 16; i++)
            put<uint64_t>(el(E_MSG) + 8 + 0x80 * i + 0x68, P(g_icons[(f * 7 + i * 3) % 90]));
    });
    // more than 64 distinct handles and more than 96 failing ids, over several frames, with table changes
    draw_scn("cache/many weapon icons", [] {
        world(0, 0x80000);
        put<int32_t>(MSGCLOCK, 30000);
        for (int i = 0; i < 16; i++) msg(i, "K", 0x00ffffff, "V", 0x00ffffff, 99.0f, 107.0f, 0, 0, 0);
        put<int32_t>(el(E_MSG), 16);
    }, 14, [](int f) {
        for (int i = 0; i < 16; i++) {
            char n[48];
            std::snprintf(n, sizeof n, "w/f%d/i%d", f % 10, i);
            const uint64_t ic = icon_rec(n, 32, 16, 1, 1);
            put<uint64_t>(el(E_MSG) + 8 + 0x80 * i + 0x68, P(ic));
        }
        st.byname_table = f % 3;   // tables 0, 1, 2: most ids are not in them
        if (f == 6) put<uint16_t>(W.t[1] + 2, 5);       // count change: re-resolve
        if (f == 9) put<uint64_t>(MTAB + 8, P(W.t[2]));  // pointer change
        if (f == 11) put<uint64_t>(MTAB, 0);             // table 0 NULL: no Material call
    });
    draw_scn("cache/alias inside and outside the table", [] {
        world(0, 0x20000);
        names_default();
        name_entry(0, 500, 300, 1.0f, 0, 1, 0.9f, 1, 0.5f, "A", 1.0f, 0, 1, 64);
        put<int32_t>(el(E_NAMES) + 0xc80, 1);
        st.mat_alias[0xa0000001] = P(W.tent[1] + 0x28 * 6);    // PS4 id 0x50000001
        st.mat_alias[0xa0000004] = P(A(0x30));                 // outside the table
        st.mat_alias[0xa0000006] = P(W.tent[1] + 0x28 * 2 + 4); // inside, not at an entry start
        st.mat_alias[0xffa98ec5] = P(W.tent[1] + 0x28 * 18);   // one past the end
    }, 3);
    draw_scn("cache/table 1 entries NULL", [] {
        world(0, 0x20000);
        names_default();
        put<uint64_t>(W.t[1] + 8, 0);
        st.mat_alias[0xa0000001] = P(W.tent[1] + 0x28 * 6);
    }, 2);
    draw_scn("cache/table 3 handle", [] {
        world(0, 0x20000);
        names_default();
        handle(el(E_NAMES) + 0xca8, 0xa0000001, 3);
        handle(el(E_NAMES) + 0xcb4, 0xa0000002, 0xffffffff);
    }, 2);
    draw_scn("cache/id 0 handles", [] {
        world(0, 0x20000);
        names_default();
        handle(el(E_NAMES) + 0xca8, 0, 1);
        handle(el(E_NAMES) + 0xce4, 0, 1);
        handle(el(E_NAMES) + 0xcf0, 0, 1);
    }, 2);
    draw_scn("cache/explicit indices", [] {
        world(0, 0x20000);
        names_default();
        handle(el(E_NAMES) + 0xca8, 0xa0000001, 1, 4);
        handle(el(E_NAMES) + 0xcb4, 0xa0000002, 1, 400);
        handle(el(E_NAMES) + 0xce4, 0xa0000006, 1, 0x7fffffff);
    }, 2);
    // image widgets: index >= 0 after the call, id 0 with / without name, success, failure
    draw_scn("cache/image widgets", [] {
        world(0, 0x40000);
        characters();
        carry_flag(55, true);   // no carrier: widget +0x18 (id 0, no name)
        st.widget_index_after[P(W.w[0x58])] = 2;
    }, 3);
    draw_scn("cache/image widgets failing", [] {
        world(0, 0xc0000);
        messages_default();
        st.mat_fail.insert(0x11110001);
        st.mat_fail.insert(0x11110003);
        put<uint64_t>(W.w[0xb8], 0);   // no name
        put<uint64_t>(MTAB, 0);
    }, 2);
    draw_scn("cache/image widget id 0 with name", [] {
        world(0, 0x40000);
        put<uint32_t>(W.w[0x58] + 0x14, 0);
    }, 2);
    // 100+ distinct failing ids: marker icons over many frames
    draw_scn("cache/log set overflow", [] {
        world(0, 0x20000);
        markers_default();
        put<int32_t>(el(E_MK), 8);
        st.proj = {proj(300, 200, 0.6f, -0.8f, 0.9f, 1.25f, 0)};
    }, 16, [](int f) {
        for (int i = 0; i < 8; i++) {
            marker(i, uint32_t(i % 4), 1.0f, 1.0f, 0.0f, get<int32_t>(C1F));
        }
        for (uint32_t t = 0; t < 11; t++) {
            const uint64_t r = el(E_MK) + 0x210 + 0x24 * t;
            handle(r, 0x9000000 + f * 64 + t, t % 3);
            handle(r + 0xc, 0x9100000 + f * 64 + t, 1);
        }
    });
    draw_scn("cache/after overflow: old ids again", [] {
        world(0, 0x20000);
        markers_default();
        put<int32_t>(el(E_MK), 4);
        for (uint32_t t = 0; t < 11; t++) handle(el(E_MK) + 0x210 + 0x24 * t, 0x9000000 + t, t % 3);
        st.mat_fail.insert(0x22220000 + 99);
    }, 2);
    draw_scn("cache/new failing widget after overflow", [] {
        world(0, 0x40000);
        put<uint32_t>(W.w[0x58] + 0x14, 0x5eed);
    }, 1);

    // ================= ring meter =================
    for (float fill : {0.0f, 0.5f, 1.0f, 1.5f, -0.5f, NaN, Inf, 1e-7f}) {
        for (float scale : {0.4f, 1.0f, 2.5f}) {
            char n[64];
            std::snprintf(n, sizeof n, "ring/fill %g scale %g", double(fill), double(scale));
            draw_scn(n, [=] {
                world(0, 0x20000);
                markers_default();
                put<int32_t>(el(E_MK), 1);
                marker(0, 1, 1.0f, 1.0f, fill, 12345);
                st.proj = {proj(300, 200, 0, 0, 0, scale / 1.75f, 0)};
            });
        }
    }
    draw_scn("ring/hud alpha 0", [] {
        world(0, 0x20000);
        markers_default();
        put<float>(ALPHA, 0.0f);
        put<int32_t>(el(E_MK), 1);
        marker(0, 1, 1.0f, 1.0f, 0.5f, 12345);
    });
    draw_scn("ring/ring colour alpha 0", [] {
        world(0, 0x20000);
        markers_default();
        put<int32_t>(el(E_MK), 1);
        marker(0, 1, 1.0f, 1.0f, 0.5f, 12345, 0xffffffff, 0x00ffffff);
    });
    draw_scn("ring/material NULL at draw time", [] {
        world(0, 0x20000);
        markers_default();
        put<int32_t>(el(E_MK), 1);
        marker(0, 1, 1.0f, 1.0f, 0.5f, 12345);
        st.mat_alias[0x68951e57] = P(A(0x30));   // resolves (outside the table)
    }, 2, [](int f) { if (f == 1) st.mat_fail.insert(0x68951e57); });   // cached ok, then Material NULL

    // ================= weapon trigger =================
    auto wt = [](bool probe_slot) {
        world(0, 0);
        player_world(probe_slot);
    };
    draw_scn("weapon/no local player", [] { world(0, 0); });
    draw_scn("weapon/cell 0, id 0, object 0, id mismatch", [=] { wt(false); }, 4, [](int f) {
        if (f == 0) put<uint64_t>(LPCELL, 0);
        if (f == 1) put<uint64_t>(LPCELL, P(W.pcell)), put<uint32_t>(LPID, 0);
        if (f == 2) put<uint32_t>(LPID, 0x4242), put<uint64_t>(W.pcell, 0);
        if (f == 3) put<uint64_t>(W.pcell, P(W.player)), put<uint32_t>(W.player + 0x44, 1);
    });
    draw_scn("weapon/first weapon, window, switch, NULL, expiry", [=] { wt(false); }, 14, [](int f) {
        static const uint64_t weapons[] = {0, 0x111, 0x111, 0x111, 0x222, 0x222, 0, 0, 0x222, 0x222, 0x222, 0x333,
                                           0x333, 0x333};
        static const int32_t fr[] = {100, 200, 600, 1099, 1100, 1500, 1600, 1700, 1800, 2699, 2700, 2700, 3500, 3600};
        st.weapon = weapons[f];
        put<int32_t>(FRAMES, fr[f]);
    });
    draw_scn("weapon/frame counter wrap", [=] { wt(false); }, 6, [](int f) {
        st.weapon = f < 3 ? 0x777 : 0x888;
        put<int32_t>(FRAMES, int32_t(0x7ffffc00u + 400u * uint32_t(f)));
    });
    // the probe in the controller slot (per side) and the game calling it from 7 return addresses; the slot and
    // the trampolines hold per-side host addresses: cleared before the image comparison
    g_post = [](int) {
        std::memset(hx::mem + 0x3000, 0, 0x100);
        put<uint64_t>(W.cvt + 0x2f0, 0);
    };
    run("weapon/probe in slot, 7 callers, panel draws, periodic log", [] {
        world(0, 0);
        player_world(true);
        st.weapon = 0x999;
    }, [](int w) {
        put<uint64_t>(W.cvt + 0x2f0, g_show_probe[w]);
        for (int k = 0; k < 7; k++) {
            uint8_t *t = hx::mem + 0x3000 + 0x20 * k;
            const uint8_t pre[] = {0x48, 0x83, 0xec, 0x08, 0x49, 0xbb};
            std::memcpy(t, pre, 6);
            std::memcpy(t + 6, &g_show_probe[w], 8);
            const uint8_t post[] = {0x41, 0xff, 0xd3, 0x48, 0x83, 0xc4, 0x08, 0xc3};
            std::memcpy(t + 14, post, 8);
        }
        static const int order[] = {0, 0, 1, 2, 1, 3, 4, 5, 6, 0, 6};
        for (int k = 0; k < 11; k++) {
            reinterpret_cast<ShowFn>(P(0x3000 + 0x20 * order[k]))(reinterpret_cast<void *>(P(W.ctrl)), 1.5f + k);
            reinterpret_cast<DrawFn>(g_panel_probe[w])(reinterpret_cast<void *>(P(0x1234)), reinterpret_cast<void *>(0x99));
        }
        draw(w, 600, [](int f) { put<int32_t>(FRAMES, 5000 + f); });
    });
    draw_scn("weapon/slot +0x260 wrong (warn once)", [=] { wt(false); put<uint64_t>(W.pvt + 0x260, P(0x71dec1)); }, 3);
    draw_scn("weapon/vtable 0 and ctrl 0", [=] { wt(false); put<uint64_t>(W.player, 0); put<uint64_t>(W.player + 0x2768, 0); }, 2);
    draw_scn("weapon/after warning: valid again", [=] { wt(false); st.weapon = 0x5555; }, 2);

    std::printf("\n%d scenarios, %d passed, %d failed\n", g_scen, g_pass, g_scen - g_pass);
    for (const auto &n : g_fail_names) std::printf("  failed: %s\n", n.c_str());
    return hx::failures ? 1 : 0;
}
