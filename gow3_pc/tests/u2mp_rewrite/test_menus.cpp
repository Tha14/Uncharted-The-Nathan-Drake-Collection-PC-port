// u2mp rewrite tests, group "menus": menus.cpp and menuimages.cpp, old (git) against new, on the same fake image.
// Spec: u2mp/docs/spec/menus.md (section 3 lists the scenarios). Run:
//   bash gow3_pc/tests/u2mp_rewrite/run.sh menus menus.cpp menuimages.cpp
//
// Both files keep process-lifetime state (the playlists once built, extra_net_package's decision, log budgets,
// caches), so: old and new run every scenario in the same order (their states move in lockstep), and the cases that
// need a fresh process (other playlist builds, other extra-package outcomes) run in child processes of this program
// (argv[1] = mode). Host pointers that differ between old and new (text buffers, playlist records, wrapper
// addresses in the query table) are compared by content or masked; guest calls record their arguments, floats as bits.
#include "deps_menus.h"
#include "harness.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace hx::mn {
State s;
}

namespace u2mp_old {
void install_menus(int &installed);
void install_menuimages(int &installed);
int32_t playlist_pick_type();
bool playlist_game_types(std::vector<int32_t> &types);
const char *extra_net_package();
uint8_t *menu_net_material(uint32_t id);
void menu_draw_quad(uint8_t *material, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                    float alpha, float rgb);
}  // namespace u2mp_old
namespace u2mp {
void install_menus(int &installed);
void install_menuimages(int &installed);
int32_t playlist_pick_type();
bool playlist_game_types(std::vector<int32_t> &types);
const char *extra_net_package();
uint8_t *menu_net_material(uint32_t id);
void menu_draw_quad(uint8_t *material, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                    float alpha, float rgb);
}  // namespace u2mp

namespace {

using hx::addr;
using hx::get;
using hx::mem;
using hx::put;
using hx::rec;
using hx::mn::s;

struct Api {
    void (*install_menus)(int &);
    void (*install_menuimages)(int &);
    int32_t (*pick)();
    bool (*types)(std::vector<int32_t> &);
    const char *(*extra)();
    uint8_t *(*net_material)(uint32_t);
    void (*draw_quad)(uint8_t *, float, float, float, float, float, float, float, float, float, float);
};
const Api A_old{u2mp_old::install_menus, u2mp_old::install_menuimages, u2mp_old::playlist_pick_type,
                u2mp_old::playlist_game_types, u2mp_old::extra_net_package, u2mp_old::menu_net_material,
                u2mp_old::menu_draw_quad};
const Api A_new{u2mp::install_menus, u2mp::install_menuimages, u2mp::playlist_pick_type, u2mp::playlist_game_types,
                u2mp::extra_net_package, u2mp::menu_net_material, u2mp::menu_draw_quad};

constexpr uint32_t sid(const char *p) {
    uint32_t crc = 0;
    for (; *p; ++p) {
        crc ^= uint32_t(uint8_t(*p)) << 24;
        for (int i = 0; i < 8; ++i) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

uint32_t fbits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
float bitsf(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

std::string hex(const void *p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string r;
    const auto *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; ++i) {
        r += d[b[i] >> 4];
        r += d[b[i] & 15];
    }
    return r;
}

std::string quote(const char *p) {
    if (!p) return "NULL";
    std::string r = "\"";
    for (; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c >= 32 && c < 127 && c != '"' && c != '\\')
            r += char(c);
        else {
            char b[8];
            std::snprintf(b, sizeof b, "\\x%02x", c);
            r += b;
        }
    }
    return r + "\"";
}

bool in_img(uint64_t v) { return v >= addr(0) && v < addr(hx::kImageSize); }
std::string w(const void *p) { return hx::where(p); }

// ---- game facts -------------------------------------------------------------------------------------
constexpr uint64_t T = 0x105e370, NI = 0x104f1c8, NGM = 0x113b760, NPD = 0x11474f8, NETSM = 0x14ce97c,
                   TEXTDB = 0x1066660;
constexpr uint64_t fStart = 0x655c40, fPanel = 0x65e890, fStatus = 0x659a90, fPlaylist = 0x6562d0,
                   fMet = 0x65bcd0, fVolume = 0x65f3f0, fFriends = 0x65bbb0, fJoin = 0x659590;
constexpr uint64_t fTextFind = 0xcddc00, fDc = 0xa15e70, fSetNetGame = 0x63dc30;
constexpr uint32_t kTraceConds[12] = {105, 77, 78, 134, 40, 41, 80, 82, 135, 108, 109, 110};
constexpr uint64_t TRACE_FN = 0x1600000;   // + 0x40 * k

// test areas (inside the fake image)
constexpr uint64_t Q = 0x1700000;
constexpr uint64_t GAMES = 0x1710000, GAMES_REC = 0x1711000, TYPES = 0x1720000, TYPES_REC = 0x1721000;
constexpr uint64_t NT = 0x1730000, NE = 0x1731000, NN = 0x1740000;   // net table, entries, names
constexpr uint64_t MT = 0x1750000, ME = 0x1751000, MN = 0x1754000;   // menu table
constexpr uint64_t HT = 0x1758000, HE = 0x1758100;                   // hud table
constexpr uint64_t PK = 0x1760000, XT = 0x1761000, XE = 0x1762000, XN = 0x1763000;   // net-u2mp package
constexpr uint64_t BOOST = 0x1770000, BOOST_REC = 0x1770100, MAPS = 0x1772000, MAPS_REC = 0x1772100;
constexpr uint64_t GPL = 0x1774000;   // guest playlist records for the 0x24 / 0x25 drawing
constexpr uint64_t FR = 0x1782000, DESC = 0x1784000, LIST = 0x1785000;
constexpr uint64_t LAUNCH = 0x1790000, CONV_CALLER = 0x1790100, CONV_CALLER_RA = 0x1790110;
constexpr uint64_t COUT = 0x17a0000, CSRC = 0x17a0100, CBOX = 0x17a0200, CCOL = 0x17a0300, SOUT = 0x17a0400;

// ---- scripted guest functions ------------------------------------------------------------------------
std::map<uint32_t, std::string> g_texts;   // TextFind answers (absent: NULL)
std::map<uint32_t, uint64_t> g_dc;         // DcLookup answers (image offsets; absent or 0: NULL)
std::function<void(uint32_t, uint8_t *)> g_orig_hook;
float g_tex_w = 512.0f, g_tex_h = 256.0f;   // what 0x5bbd90 measures
float g_conv_w = 320.0f, g_conv_h = 160.0f;
uint64_t g_conv_mat = 0;                    // image offset written at out+0x48 by the converter (0: NULL)

U2MP_GUEST const char *text_find(void *db, uint32_t id) {
    auto it = g_texts.find(id);
    rec("TextFind %s %08x -> %s", w(db).c_str(), id, it == g_texts.end() ? "NULL" : quote(it->second.c_str()).c_str());
    return it == g_texts.end() ? nullptr : it->second.c_str();
}
U2MP_GUEST void *dc_lookup(uint32_t id) {
    auto it = g_dc.find(id);
    const uint64_t off = it == g_dc.end() ? 0 : it->second;
    rec("DcLookup %08x -> %s", id, off ? w(mem + off).c_str() : "null");
    return off ? mem + off : nullptr;
}
U2MP_GUEST void set_net_game(void *ngm, int32_t index, const void *overrides, int32_t n) {
    rec("SetNetGame %s %d %s %d", w(ngm).c_str(), index, w(overrides).c_str(), n);
}
template <uint32_t C> U2MP_GUEST void orig_q(uint8_t *q) {
    rec("orig cond %u: op %u +4 %d +8 %u +0x54 %u ni+0x10 %d ni+0x298 %s", C, get<uint32_t>(q - mem),
        get<int32_t>(q - mem + 4), get<uint32_t>(q - mem + 8), get<uint32_t>(q - mem + 0x54), get<int32_t>(NI + 0x10),
        hex(mem + NI + 0x298, 18).c_str());
    if (g_orig_hook) g_orig_hook(C, q);
}
template <int K> U2MP_GUEST void trace_orig(uint8_t *q) {
    rec("trace orig %d: op %u +4 %d", K, get<uint32_t>(q - mem), get<int32_t>(q - mem + 4));
    put<int32_t>(q - mem + 4, 1000 + K);
    put<uint32_t>(q - mem + 8, uint32_t(K));
    put<uint64_t>(q - mem + 0x40, addr(0x1234 + K));
    put<uint32_t>(q - mem + 0x54, 77);
}
template <size_t... K> void stub_traces(std::index_sequence<K...>) {
    ((hx::stub(TRACE_FN + 0x40 * K, reinterpret_cast<void *>(&trace_orig<int(K)>))), ...);
}

U2MP_GUEST float *size_fn(float *out, uint8_t *m) {
    rec("ImageSize %s %s", w(out).c_str(), w(m).c_str());
    if (out) {
        out[0] = g_tex_w;
        out[1] = g_tex_h;
    }
    return out;
}
U2MP_GUEST uint64_t conv_orig(uint8_t *out, const uint8_t *src, uint64_t a2, const float *box, const void *colour,
                              uint32_t a5, float scale) {
    rec("Convert %s %s %llx %s %s %u %08x", w(out).c_str(), w(src).c_str(), (unsigned long long)a2, w(box).c_str(),
        w(colour).c_str(), a5, fbits(scale));
    if (out) {
        std::memcpy(out + 0x3c, &g_conv_w, 4);
        std::memcpy(out + 0x40, &g_conv_h, 4);
        const uint64_t m = g_conv_mat ? addr(g_conv_mat) : 0;
        std::memcpy(out + 0x48, &m, 8);
    }
    return 0x1122334455667788ull;
}
U2MP_GUEST void draw_rec(void *ctx, void *info, void *verts) {
    const auto *c = static_cast<const uint64_t *>(ctx);
    const auto *i = static_cast<const uint64_t *>(info);
    rec("DrawQuad ctx%%16=%d list %s %s | info%%16=%d mat %s %s | verts%%16=%d %s", int(uintptr_t(ctx) % 16),
        c[0] ? w(reinterpret_cast<void *>(c[0])).c_str() : "0", hex(c + 1, 40).c_str(), int(uintptr_t(info) % 16),
        w(reinterpret_cast<void *>(i[0])).c_str(), hex(i + 1, 24).c_str(), int(uintptr_t(verts) % 16),
        hex(verts, 0x110).c_str());
}

// ---- runner -----------------------------------------------------------------------------------------
std::vector<std::pair<std::string, bool>> results;

void reset_script() {
    s = hx::mn::State{};
    g_texts.clear();
    g_dc.clear();
    g_orig_hook = nullptr;
    g_tex_w = 512.0f, g_tex_h = 256.0f, g_conv_w = 320.0f, g_conv_h = 160.0f, g_conv_mat = 0;
}

void rec_playlists() {
    for (int k = 0; k < 32; ++k) {
        const uint64_t v = get<uint64_t>(NGM + 0x20a0 + 8 * k);
        if (v && !in_img(v)) rec("ngm playlist[%d] = %s", k, hex(reinterpret_cast<void *>(v), 0x6c).c_str());
    }
    rec("ngm playlist count %u", get<uint32_t>(NGM + 0x21a0));
}

void finish() {
    rec_playlists();
    // stub memory: the converter trampoline and the renderer stub (its host function address masked)
    uint8_t pool[0x80];
    std::memcpy(pool, mem + hx::kImageSize, sizeof pool);
    const int64_t target = int64_t(0x5c2ec4) + get<int32_t>(0x5c2ec4 + 4 * (0x1e - 4));
    const int64_t at = target - int64_t(hx::kImageSize);
    if (at >= 0 && at + 31 <= int64_t(sizeof pool)) std::memset(pool + at + 23, 0xEE, 8);
    rec("pool %s", hex(pool, sizeof pool).c_str());
}

std::vector<uint8_t> masked_snapshot() {
    std::vector<uint8_t> v = hx::snapshot();
    auto mask = [&](uint64_t off) {
        const uint64_t p = get<uint64_t>(off);
        if (p && !in_img(p)) std::memset(v.data() + off, 0xEE, 8);
    };
    for (int k = 0; k < 32; ++k) mask(NGM + 0x20a0 + 8 * k);
    for (int e = 0; e < 132; ++e) mask(T + 0x18 * e + 8);
    return v;
}

void run(const std::string &name, const std::function<void(const Api &)> &body) {
    std::vector<std::string> c[2];
    std::vector<uint8_t> img[2];
    for (int side = 0; side < 2; ++side) {
        hx::clear();
        hx::bind_both();
        reset_script();
        body(side == 0 ? A_old : A_new);
        finish();
        c[side] = hx::calls;
        img[side] = masked_snapshot();
    }
    const bool ok = hx::same(name.c_str(), c[0], c[1], &img[0], &img[1]);
    if (ok) std::printf("PASS %s (%zu records)\n", name.c_str(), c[0].size());
    const char *dump = std::getenv("MENUS_DUMP");
    if (dump && (name == dump || std::string(dump) == "all"))
        for (int side = 0; side < 2; ++side) {
            std::printf("---- %s %s\n", name.c_str(), side ? "new" : "old");
            for (auto &l : c[side]) std::printf("  %s\n", l.c_str());
        }
    std::fflush(stdout);
    results.push_back({name, ok});
}

// ---- menus image ---------------------------------------------------------------------------------------
enum { E50 = 0, E133, E49, E55, E39, E104, E36, E3, ETRACE, ETERM = ETRACE + 12 };

void entry(int i, uint32_t cond, uint64_t fn_off) {
    put<uint32_t>(T + 0x18 * i, cond);
    put<uint32_t>(T + 0x18 * i + 4, 0xabcd0000u + i);
    put<uint64_t>(T + 0x18 * i + 8, fn_off ? addr(fn_off) : 0);
    put<uint64_t>(T + 0x18 * i + 0x10, 0x5000 + i);
}

void menus_image() {
    entry(E50, 50, fStart);
    entry(E133, 133, fPanel);
    entry(E49, 49, fStatus);
    entry(E55, 55, fPlaylist);
    entry(E39, 39, fMet);
    entry(E104, 104, fVolume);
    entry(E36, 36, fFriends);
    entry(E3, 3, fJoin);
    for (int k = 0; k < 12; ++k) entry(ETRACE + k, kTraceConds[k], TRACE_FN + 0x40 * (k == 10 ? 9 : k));   // 108, 109 share
    hx::stub(fStart, reinterpret_cast<void *>(&orig_q<50>));
    hx::stub(fPanel, reinterpret_cast<void *>(&orig_q<133>));
    hx::stub(fStatus, reinterpret_cast<void *>(&orig_q<49>));
    hx::stub(fPlaylist, reinterpret_cast<void *>(&orig_q<55>));
    hx::stub(fMet, reinterpret_cast<void *>(&orig_q<39>));
    hx::stub(fVolume, reinterpret_cast<void *>(&orig_q<104>));
    static const uint8_t ret[] = {0xc3, 0x66, 0x66, 0x66, 0x66};
    std::memcpy(mem + fFriends, ret, 5);
    std::memcpy(mem + fJoin, ret, 5);
    stub_traces(std::make_index_sequence<12>());
    hx::stub(fTextFind, reinterpret_cast<void *>(&text_find));
    hx::stub(fDc, reinterpret_cast<void *>(&dc_lookup));
    hx::stub(fSetNetGame, reinterpret_cast<void *>(&set_net_game));
    put<int32_t>(NETSM, 2);
}

void inst_menus(const Api &a) {
    int n = 100;
    a.install_menus(n);
    rec("installed %d", n);
}
void menus_ready(const Api &a) {
    menus_image();
    inst_menus(a);
}

using QueryFn = void(U2MP_GUEST *)(uint8_t *);

// One query call as the game makes it: through the table entry `e`.
void ask(int e, uint32_t op, int32_t v = 0, uint32_t t = 0, uint32_t col = 0) {
    uint8_t *q = mem + Q;
    std::memset(q, 0xc1, 0x80);
    put<uint32_t>(Q, op);
    put<int32_t>(Q + 4, v);
    put<uint32_t>(Q + 8, t);
    put<uint32_t>(Q + 0x0c, 0x5a5a5a5a);
    put<uint64_t>(Q + 0x40, 0);
    put<uint32_t>(Q + 0x54, col);
    rec("ask entry %d op %u v %d t %u col %u", e, op, v, t, col);
    const uint64_t fn = get<uint64_t>(T + 0x18 * e + 8);
    reinterpret_cast<QueryFn>(uintptr_t(fn))(q);
    const uint64_t p = get<uint64_t>(Q + 0x40);
    std::string text = p ? quote(reinterpret_cast<const char *>(p)) : "null";
    if (p && in_img(p)) text += " @" + w(reinterpret_cast<void *>(p));
    put<uint64_t>(Q + 0x40, 0);
    rec("  -> q %s text %s", hex(q, 0x60).c_str(), text.c_str());
}
// every plain-list op, rows -1..n+1, column 1
void list_sweep(int e, int rows) {
    ask(e, 1);
    for (int r = -1; r <= rows + 1; ++r) ask(e, 2, r);
    ask(e, 2, 0, 0, 1);
    ask(e, 3, 0);
    ask(e, 4);
    ask(e, 5);
    ask(e, 6);
    ask(e, 7);
    ask(e, 9, 2);
    ask(e, 4);
    ask(e, 9, -2);
    ask(e, 4);
    ask(e, 9, 1000);
    ask(e, 4);
    ask(e, 8);
    ask(e, 4);
    ask(e, 0);
    ask(e, 10, 5);
}

// ---- playlists data ---------------------------------------------------------------------------------
struct TypeRec {
    uint32_t name, desc, image;
};
void make_types(const std::vector<TypeRec> &t) {
    put<int32_t>(TYPES, int32_t(t.size()));
    put<int32_t>(TYPES + 4, 0x7777);
    put<uint64_t>(TYPES + 8, addr(TYPES_REC));
    for (size_t i = 0; i < t.size(); ++i) {
        const uint64_t r = TYPES_REC + 0x44 * i;
        put<uint32_t>(r, t[i].name);
        put<uint32_t>(r + 4, t[i].desc);
        put<uint32_t>(r + 8, t[i].image);
    }
    g_dc[0xf985c7b6] = TYPES;
}
void make_games(const std::vector<int32_t> &types) {
    put<int32_t>(GAMES, int32_t(types.size()));
    put<uint64_t>(GAMES + 8, addr(GAMES_REC));
    for (size_t i = 0; i < types.size(); ++i) put<int32_t>(GAMES_REC + 0x90 * i + 0x0c, types[i]);
    g_dc[0x3b9a067d] = GAMES;
}
// the PS4 game types, by their name texts, plus Machinima and an objective-picture name
const std::vector<TypeRec> kFullTypes = [] {
    const uint32_t names[] = {0xa30341e7, 0x8dfdede9, 0x3d9470a5, 0x0544e6f6, 0x13f76fcc, 0x71ede2dc,
                              0xf625a024, 0x3b8de400, 0x5136a578, 0x2ee192ab, 0x00248bb5, 0xe9995f4c,
                              0x2959eca4, 0xb81a7c18, 0x5614c769, 0x8f86cb5f};
    std::vector<TypeRec> v;
    for (uint32_t i = 0; i < 16; ++i) v.push_back({names[i], 0xde500000u + i, 0x1a000000u + i});
    return v;
}();
void full_playlist_data() {
    make_types(kFullTypes);
    std::vector<int32_t> g;
    for (int i = 0; i < 16; ++i) g.push_back(i);
    g.push_back(99);
    g.push_back(-1);
    g.push_back(0);
    g.push_back(7);
    make_games(g);
}
void partial_playlist_data() {
    std::vector<TypeRec> t;
    for (int i = 0; i < 12; ++i) t.push_back(kFullTypes[size_t(i)]);   // no survival types, no Machinima
    t.push_back({0xa30341e7, 0xd0000012, 0x12});   // duplicate name texts on later records
    t.push_back({0x8dfdede9, 0xd0000013, 0x13});
    t.push_back({0x3b8de400, 0xd0000014, 0x14});
    t.push_back({0x00248bb5, 0xd0000015, 0x15});
    make_types(t);
    // game types: 0..15, then out of range and repeats; dlc refuses the plunder (2) and coop arena (9) games and the
    // first gold rush (10)
    make_games({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 2, -5, 1000, 9, 15});
    s.dlc_off = {2, 9, 10, 17, 20};
}
void fallback_playlist_data() {
    make_types({{0x8f86cb5f, 0xd1, 0x700},
                {0x11111111, 0xd2, 0},
                {0x5614c769, 0xd3, 0x701},
                {0x4bf30e73, 0xd4, 0x700},
                {0x22222222, 0xd5, 0x55320001},
                {0x33333333, 0xd6, 0x900},
                {0x44444444, 0xd7, 0x901}});
    make_games({0, 1, 2, 3, 4, 5, 99, -1, 0, 3, 1});
    s.dlc_off = {5};
}
void fallback_cap_data() {
    std::vector<TypeRec> t;
    std::vector<int32_t> g;
    for (uint32_t i = 0; i < 40; ++i) {
        t.push_back({0x10000 + i, 0xd000 + i, 0x2000 + i});
        g.push_back(int32_t(39 - i));
    }
    make_types(t);
    make_games(g);
}

// ---- menuimages image ----------------------------------------------------------------------------------
const uint8_t kConvHead[18] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                               0x54, 0x53, 0x48, 0x81, 0xec, 0xe0, 0x00, 0x00, 0x00};
// undo the prologue the trampoline ran (add rsp,0xe0; pop rbx; pop r12; pop r14; pop r15; pop rbp), then conv_orig
const uint8_t kConvUndo[15] = {0x48, 0x81, 0xc4, 0xe0, 0x00, 0x00, 0x00, 0x5b, 0x41, 0x5c, 0x41, 0x5e, 0x41, 0x5f, 0x5d};
const uint8_t kLookupHead[16] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x89,
                                 0xf3, 0x48, 0x89, 0xf8, 0x48, 0xc1, 0xe8, 0x20};
const uint8_t kDispatch[36] = {0x83, 0xc0, 0xfc, 0x83, 0xf8, 0x25, 0x0f, 0x87, 0x53, 0x42, 0x00, 0x00,
                               0x4c, 0x8d, 0xbd, 0x30, 0xe0, 0xff, 0xff, 0x49, 0xba, 0x00, 0x00, 0x00,
                               0x00, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x0d, 0x4e, 0x56, 0x00, 0x00};
const uint8_t kExit[7] = {0x48, 0x8b, 0x05, 0x48, 0x91, 0xa8, 0x00};
// after the exit's 7 bytes: the launcher's epilogue (add rsp,8; pop r15..rbp; ret)
const uint8_t kLaunchEpi[] = {0x48, 0x83, 0xc4, 0x08, 0x41, 0x5f, 0x41, 0x5e, 0x41,
                              0x5d, 0x41, 0x5c, 0x5b, 0x5d, 0xc3};
const uint8_t kQuadHead[16] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                               0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x40, 0x49};

// launcher(desc, rbp, ebx, r14d, r12d, r13d): sets the renderer's registers and jumps through the jump table
void write_launcher() {
    std::vector<uint8_t> c = {0x55, 0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,   // push rbp..r15
                              0x48, 0x83, 0xec, 0x08,                                       // sub rsp,8
                              0x49, 0x89, 0xfb,                                             // mov r11,rdi
                              0x48, 0x89, 0xf5,                                             // mov rbp,rsi
                              0x89, 0xd3,                                                   // mov ebx,edx
                              0x41, 0x89, 0xce,                                             // mov r14d,ecx
                              0x45, 0x89, 0xc4,                                             // mov r12d,r8d
                              0x45, 0x89, 0xcd,                                             // mov r13d,r9d
                              0x41, 0x8b, 0x03,                                             // mov eax,[r11]
                              0x83, 0xe8, 0x04,                                             // sub eax,4
                              0x48, 0xb9};                                                  // movabs rcx,
    const uint64_t table = addr(0x5c2ec4);
    for (int i = 0; i < 8; ++i) c.push_back(uint8_t(table >> (8 * i)));
    const uint8_t tail[] = {0x48, 0x63, 0x04, 0x81,   // movsxd rax,[rcx+rax*4]
                            0x48, 0x01, 0xc8,         // add rax,rcx
                            0xff, 0xe0};              // jmp rax
    c.insert(c.end(), tail, tail + sizeof tail);
    std::memcpy(mem + LAUNCH, c.data(), c.size());
    // converter caller inside the image: sub rsp,8; movabs rax,B+0x5bc290; call rax; add rsp,8; ret
    std::vector<uint8_t> k = {0x48, 0x83, 0xec, 0x08, 0x48, 0xb8};
    const uint64_t conv = addr(0x5bc290);
    for (int i = 0; i < 8; ++i) k.push_back(uint8_t(conv >> (8 * i)));
    const uint8_t t2[] = {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x08, 0xc3};
    k.insert(k.end(), t2, t2 + sizeof t2);
    std::memcpy(mem + CONV_CALLER, k.data(), k.size());
}

// a material table: header {u16 0xbeef, u16 count, pad, entries}; names "#1".."#3" break the name chain at that step
void make_material_table(uint64_t hdr, uint64_t entries, uint64_t names, const std::vector<std::pair<uint32_t, const char *>> &e,
                         uint16_t count_override = 0xffff) {
    put<uint16_t>(hdr, 0xbeef);
    put<uint16_t>(hdr + 2, count_override != 0xffff ? count_override : uint16_t(e.size()));
    put<uint32_t>(hdr + 4, 0x12345678);
    put<uint64_t>(hdr + 8, addr(entries));
    for (size_t j = 0; j < e.size(); ++j) {
        const uint64_t en = entries + 0x28 * j;
        std::memset(mem + en, 0x30 + int(j), 0x28);
        put<uint32_t>(en + 8, e[j].first);
        const char *n = e[j].second;
        if (!n) {
            put<uint64_t>(en, 0);
            continue;
        }
        const uint64_t p1 = names + 0xc0 * j, p2 = p1 + 0x20, p3 = p1 + 0x28, str = p1 + 0x30;
        put<uint64_t>(en, addr(p1));
        put<uint64_t>(p1 + 0x10, std::strcmp(n, "#1") ? addr(p2) : 0);
        put<uint64_t>(p2, std::strcmp(n, "#2") ? addr(p3) : 0);
        put<uint64_t>(p3, std::strcmp(n, "#3") ? addr(str) : 0);
        if (n[0] != '#') std::snprintf(reinterpret_cast<char *>(mem + str), 0x90, "%s", n);
    }
}

const uint32_t kPicDm = sid("general/net/playlists/deathmatch"), kPicObj = sid("general/net/playlists/objective"),
               kRank = sid("general/net/rank-badges/default"), kRank2 = sid("general/net/rank-badges/default2"),
               kBoostSpeed = sid("general/net/boosters/speed"), kLab = sid("general/net/playlists/the-lab"),
               kMapLong = sid("general/net/maps/village-with-a-very-long-name-that-goes-past-sixty-characters-for-sure");

void net_tables() {
    make_material_table(NT, NE, NN,
                        {{0x1000, "general/net/playlists:deathmatch,{0123-4567},shader"},
                         {0x1001, "general/net/playlists:objective"},
                         {0x1002, nullptr},
                         {0x1003, "#1"},
                         {0x1004, "general/net/rank-badges:default,x"},
                         {0x1005, "general/net/rank-badges:default2"},
                         {0x1006, "general/net/boosters:speed"},
                         {0x1007, "general/net/maps:village-with-a-very-long-name-that-goes-past-sixty-characters-for-sure,g"},
                         {0x1008, "#2"},
                         {0x1009, "#3"}});
    make_material_table(MT, ME, MN,
                        {{0x3000, "menu/set:item0"},
                         {0x3001, "menu/set:item1,"},
                         {0x3002, "menu/set:item2"},
                         {0x3003, "menu/set:item3"},
                         {0x3004, "menu/set:item4"},
                         {0x3005, nullptr},
                         {0x3006, "menu/set:item6"},
                         {0x3007, "menu:x:y,z"}});
    make_material_table(HT, HE, 0x1759000, {{0x4000, "hud:a"}, {0x4001, "hud:b"}});
    put<uint64_t>(0x1130008 + 0x68, addr(HT));
    put<uint64_t>(0x1130008 + 0x70, addr(NT));
    put<uint64_t>(0x1130008 + 0x78, addr(MT));
    // net-u2mp package, logged in
    put<int32_t>(PK + 0x30, 7);
    put<uint64_t>(PK + 0x940, addr(XT));
    make_material_table(XT, XE, XN, {{0x2000, "general/net/x:a"}, {kLab, "general/net/playlists/the-lab"}, {0x2002, nullptr}});
    s.package = PK;
}

void mi_image() {
    std::memcpy(mem + 0x5bc290, kConvHead, 18);
    std::memcpy(mem + 0x5bc2a2, kConvUndo, sizeof kConvUndo);
    hx::stub(0x5bc2a2 + sizeof kConvUndo, reinterpret_cast<void *>(&conv_orig));
    std::memcpy(mem + 0x5558c0, kLookupHead, 16);
    std::memcpy(mem + 0x5bd852, kDispatch, 36);
    std::memcpy(mem + 0x5c1ab1, kExit, 7);
    std::memcpy(mem + 0x5c1ab8, kLaunchEpi, sizeof kLaunchEpi);
    std::memcpy(mem + 0xbdce90, kQuadHead, 16);
    for (uint32_t t = 4; t <= 0x29; ++t) put<int32_t>(0x5c2ec4 + 4 * (t - 4), int32_t(0x5c1ab1 - 0x5c2ec4));
    hx::stub(0x5bbd90, reinterpret_cast<void *>(&size_fn));
    hx::stub(fDc, reinterpret_cast<void *>(&dc_lookup));
    write_launcher();
    net_tables();
}
void mi_install(const Api &a) {
    int n = 0;
    a.install_menuimages(n);
    rec("installed %d", n);
}
void mi_ready(const Api &a) {
    mi_image();
    mi_install(a);
    hx::stub(0xbdce90, reinterpret_cast<void *>(&draw_rec));
}

using LookupFn = uint8_t *(U2MP_GUEST *)(uint64_t, int32_t);
void look(uint32_t sel, uint32_t id, int32_t index) {
    uint8_t *r = reinterpret_cast<LookupFn>(addr(0x5558c0))((uint64_t(sel) << 32) | id, index);
    rec("lookup sel %u id %08x index %d -> %s", sel, id, index, w(r).c_str());
}

using SizeFn = float *(U2MP_GUEST *)(float *, uint8_t *);
void size_call(uint64_t site, float *out, uint8_t *m) {
    auto fn = reinterpret_cast<SizeFn>(hx::retarget_host(site));
    if (!fn) {
        rec("size site 0x%llx not retargeted", (unsigned long long)site);
        return;
    }
    float *r = fn(out, m);
    rec("size 0x%llx -> %s out %s", (unsigned long long)site, w(r).c_str(), out ? hex(out, 8).c_str() : "-");
}

using ConvFn = uint64_t(U2MP_GUEST *)(uint8_t *, const uint8_t *, uint64_t, const float *, const void *, uint32_t, float);
__attribute__((noinline)) uint64_t conv_outside(uint8_t *out, const uint8_t *src, const float *box, float scale) {
    const uint64_t r = reinterpret_cast<ConvFn>(addr(0x5bc290))(out, src, 0xa2a2a2a2a2ull, box, mem + CCOL, 0x55, scale);
    asm volatile("" ::: "memory");
    return r;
}
uint64_t conv_inside(uint8_t *out, const uint8_t *src, const float *box, float scale) {
    return reinterpret_cast<ConvFn>(addr(CONV_CALLER))(out, src, 0xa2a2a2a2a2ull, box, mem + CCOL, 0x55, scale);
}
void conv_src(uint32_t type, uint32_t id, uint32_t sel) {
    put<uint32_t>(CSRC, type);
    put<uint32_t>(CSRC + 0x14, id);
    put<uint32_t>(CSRC + 0x18, sel);
}

using LaunchFn = void(U2MP_GUEST *)(void *desc, void *rbp, uint32_t ebx, uint32_t r14d, uint32_t r12d, uint32_t r13d);
struct DrawIn {
    uint32_t type, value;
    float x = 100, y = 50, wd = 64, h = 80, alpha = 0.5f;
    uint32_t ebx = 0x12ab34cd, r14 = 0x0000ff00, r12 = 0xffffff80, r13 = 0x00004000;
};
void draw(const DrawIn &d) {
    put<uint32_t>(DESC, d.type);
    put<uint32_t>(DESC + 0x10, d.value);
    put<float>(DESC + 0x50, d.alpha);
    put<uint64_t>(FR, addr(LIST));
    put<float>(FR - 0x10, d.x);
    put<float>(FR - 0x0c, d.y);
    put<float>(FR - 0x08, d.wd);
    put<float>(FR - 0x04, d.h);
    rec("draw type 0x%x value %08x x %08x y %08x w %08x h %08x alpha %08x regs %08x %08x %08x %08x", d.type, d.value,
        fbits(d.x), fbits(d.y), fbits(d.wd), fbits(d.h), fbits(d.alpha), d.ebx, d.r14, d.r12, d.r13);
    reinterpret_cast<LaunchFn>(addr(LAUNCH))(mem + DESC, mem + FR + 0x1fe0, d.ebx, d.r14, d.r12, d.r13);
    rec("  draw returned");
}

void draw_tables() {
    // boosters: 0 speed (net table by PS3 name), 1 / 2 placeholders, 3 an id the net table has directly
    put<int32_t>(BOOST, 4);
    put<uint64_t>(BOOST + 8, addr(BOOST_REC));
    const uint32_t bimg[4] = {kBoostSpeed, 0x1001, 0x1002, 0x1003}, bkey[4] = {1, 0x05454b23, 0x01845694, 2};
    for (int i = 0; i < 4; ++i) {
        put<uint32_t>(BOOST_REC + 0x20 * i + 8, bimg[i]);
        put<uint32_t>(BOOST_REC + 0x20 * i + 0xc, bkey[i]);
    }
    g_dc[0x916e03b0] = BOOST;
    put<int32_t>(MAPS, 3);
    put<uint64_t>(MAPS + 8, addr(MAPS_REC));
    const uint32_t mid[3] = {0x500, 0x501, 0x502}, mimg[3] = {0x1000, kMapLong, 0x9999};
    for (int i = 0; i < 3; ++i) {
        put<uint32_t>(MAPS_REC + 0x38 * i, mid[i]);
        put<uint32_t>(MAPS_REC + 0x38 * i + 0xc, mimg[i]);
    }
    g_dc[0x0dec97a4] = MAPS;
    // guest playlist records: [0] id 0x77 deathmatch picture, [1] NULL, [2] id 0x78 picture 0x1001
    put<uint32_t>(NGM + 0x21a0, 3);
    put<uint64_t>(NGM + 0x20a0, addr(GPL));
    put<uint64_t>(NGM + 0x20a8, 0);
    put<uint64_t>(NGM + 0x20b0, addr(GPL + 0x80));
    put<uint32_t>(GPL, 0x77);
    put<uint32_t>(GPL + 0x10, kPicDm);
    put<uint32_t>(GPL + 0x80, 0x78);
    put<uint32_t>(GPL + 0x90, 0x1001);
}

void setenv_scale(const char *v) {
    std::string e = std::string("UNCHARTED_U2MP_NET_IMAGE_SCALE=") + (v ? v : "");
    static std::vector<std::string> keep;   // _putenv keeps the pointer on some CRTs
    keep.push_back(e);
    _putenv(keep.back().c_str());
}

// ---- extra package: runtime_file_translate (one C symbol for both builds) --------------------------------
int g_translate = 0;   // 0 regular file, 1 fails, 2 a directory, 3 a missing file
std::string g_file, g_dir, g_missing;
}  // namespace

extern "C" int runtime_file_translate(const char *guest, char *out, size_t size) {
    rec("runtime_file_translate %s %zu (mode %d)", guest, size, g_translate);
    const std::string &p = g_translate == 0 ? g_file : g_translate == 2 ? g_dir : g_missing;
    if (g_translate == 1) return -1;
    std::snprintf(out, size, "%s", p.c_str());
    return 0;
}

namespace {

// ======================================================================================================
// menus scenarios
// ======================================================================================================
void menus_install_scenarios() {
    run("1 install: good table", [](const Api &a) { menus_ready(a); });
    run("2a install: first cond not 50", [](const Api &a) {
        menus_image();
        put<uint32_t>(T, 51);
        inst_menus(a);
    });
    run("2b install: first fn not 0x655c40", [](const Api &a) {
        menus_image();
        put<uint64_t>(T + 8, addr(0x655c41));
        inst_menus(a);
    });
    run("2c install: image too small", [](const Api &a) {
        menus_image();
        hx::set_image_both(mem, T + 132 * 0x18 - 1);
        inst_menus(a);
    });
    run("2d install: image exactly the table end", [](const Api &a) {
        menus_image();
        hx::set_image_both(mem, T + 132 * 0x18);
        inst_menus(a);
    });
    const uint64_t helpers[3] = {fTextFind, fSetNetGame, fDc};
    for (int i = 0; i < 3; ++i)
        run("3." + std::to_string(i) + " install: helper not executable", [&, i](const Api &a) {
            menus_image();
            hx::noexec.push_back({helpers[i] + 15, 1});
            inst_menus(a);
        });
    run("3.3 install: wrapped function not executable (PLAYLIST)", [](const Api &a) {
        menus_image();
        hx::noexec.push_back({fPlaylist, 1});
        inst_menus(a);
    });
    run("4a install: cond 55 first entry wrong, later right", [](const Api &a) {
        menus_image();
        put<uint64_t>(T + 0x18 * E55 + 8, addr(0x123456));
        entry(ETERM, 55, fPlaylist);   // later entry, before the terminator
        inst_menus(a);
    });
    run("4b install: cond 39 only after the terminator", [](const Api &a) {
        menus_image();
        entry(E39, 999, 0x777777);
        entry(ETERM + 1, 39, fMet);   // ETERM is the terminator
        inst_menus(a);
    });
    run("4c install: terminator before every wrap", [](const Api &a) {
        menus_image();
        put<uint64_t>(T + 0x18 * E133 + 8, 0);
        inst_menus(a);
    });
    run("5a install: LIST_FRIENDS stub bytes differ", [](const Api &a) {
        menus_image();
        mem[fFriends + 4] = 0x90;
        inst_menus(a);
    });
    run("5b install: join list entry not pointing at the stub", [](const Api &a) {
        menus_image();
        put<uint64_t>(T + 0x18 * E3 + 8, addr(fJoin + 0x10));
        inst_menus(a);
    });
    run("5c install: replace_function fails for the join list", [](const Api &a) {
        menus_image();
        hx::fail_offsets.push_back(fJoin);
        inst_menus(a);
    });
    run("5d install: cond 36 entry missing", [](const Api &a) {
        menus_image();
        put<uint32_t>(T + 0x18 * E36, 37);
        inst_menus(a);
    });
    run("6a trace: after terminator, fn 0 skipped, 13 calls", [](const Api &a) {
        menus_image();
        // move cond 105 after the terminator; cond 77 gets a fn-0 entry before its real one; cond 78 only fn 0
        put<uint32_t>(T + 0x18 * ETRACE, 999);
        entry(ETERM + 2, 105, TRACE_FN);
        entry(ETERM + 3, 77, 0);
        put<uint32_t>(T + 0x18 * (ETRACE + 1), 1077);
        entry(ETERM + 4, 77, TRACE_FN + 0x40);
        put<uint32_t>(T + 0x18 * (ETRACE + 2), 1078);
        entry(ETERM + 5, 78, 0);
        inst_menus(a);
        for (int i = 0; i < 13; ++i) ask(ETERM + 2, 1, i);
        ask(ETERM + 4, 2, 3);
        ask(ETRACE + 2, 2, 3);   // cond 1078 entry: not traced, the original directly
        ask(ETRACE + 9, 4, 7);   // 108 and 109 share one original, each its own tracer
        ask(ETRACE + 10, 4, 8);
        for (int i = 0; i < 12; ++i) ask(ETRACE + 3, 9, -i);   // 134: 12 lines
        ask(ETRACE + 3, 9, 99);
    });
}

void menus_query_scenarios() {
    run("7 cond 133: local names", [](const Api &a) {
        menus_ready(a);
        const char *names[] = {"", "abcde", "0123456789abcdef", "0123456789abcdefGHIJ"};
        for (const char *n : names) {
            std::memset(mem + NI + 0x290, 0xff, 0x20);
            s.local_name = n;
            ask(E133, 1);
            ask(E133, 4, 3);
        }
        s.local_name = std::string("ab\0cd", 5);
        ask(E133, 2, 1);
    });
    run("8 cond 49: member count seen, restored", [](const Api &a) {
        menus_ready(a);
        put<int32_t>(NI + 0x10, 1234);
        s.members = 3;
        g_orig_hook = [](uint32_t, uint8_t *) { put<int32_t>(NI + 0x14, get<int32_t>(NI + 0x10)); };
        ask(E49, 1);
        s.members = -7;
        ask(E49, 2, 0);
        rec("ni+0x10 %d", get<int32_t>(NI + 0x10));
    });
    run("9a cond 36 online: no friends", [](const Api &a) {
        menus_ready(a);
        s.online = true;
        list_sweep(E36, 2);
    });
    run("9b cond 36 online: 3 friends, 2 online", [](const Api &a) {
        menus_ready(a);
        s.online = true;
        s.friends = 3;
        s.friend_on = {1, 0, 1};
        s.friend_rows = {"alice - online", "bob - offline", "carol - in game"};
        list_sweep(E36, 4);
    });
    run("9c cond 36 online: friend_count negative", [](const Api &a) {
        menus_ready(a);
        s.online = true;
        s.friends = -1;
        s.friend_rows = {"neg row"};
        list_sweep(E36, 2);
    });
    run("9d cond 36 online: long friend count title", [](const Api &a) {
        menus_ready(a);
        s.online = true;
        s.friends = 1000;
        s.friend_on = {1, 1, 1};
        ask(E36, 1);
    });
    run("10a cond 36 LAN: counts, rows, leader", [](const Api &a) {
        menus_ready(a);
        g_texts[0xba8db526] = "[A] players in party";
        g_texts[0xac182308] = "Party Leader";
        for (int count : {0, 1, 3, 17, -2}) {
            s.members = count;
            s.member_ok.assign(20, 1);
            s.member_id.clear();
            s.member_name.clear();
            for (int r = 0; r < 20; ++r) {
                s.member_id.push_back(r == 0 ? 1u : uint32_t(r + 1));
                s.member_name.push_back("player" + std::to_string(r));
            }
            s.member_ok[2] = 0;
            list_sweep(E36, count < 0 ? 1 : count);
        }
    });
    run("10b cond 36 LAN: leader text NULL, long names, embedded NUL", [](const Api &a) {
        menus_ready(a);
        g_texts[0xba8db526] = "[A] / [A] players";
        s.members = 4;
        s.member_ok = {1, 1, 1, 1};
        s.member_id = {1, 1, 2, 1};
        s.member_name = {"lead", std::string(120, 'n'), std::string("ab\0cd", 5), std::string(93, 'x')};
        list_sweep(E36, 4);
        g_texts[0xac182308] = "Party Leader";
        for (int r = 0; r < 4; ++r) ask(E36, 2, r);
    });
    run("10c cond 36 LAN: title without [A], long title, no text", [](const Api &a) {
        menus_ready(a);
        s.members = 12;
        g_texts[0xba8db526] = "no marker here";
        ask(E36, 1);
        g_texts[0xba8db526] = std::string(90, 'T') + "[A]" + std::string(20, 'U');
        ask(E36, 1);
        g_texts[0xba8db526] = std::string(130, 'L');
        ask(E36, 1);
        g_texts[0xba8db526] = "[A";
        ask(E36, 1);
        g_texts.erase(0xba8db526);
        ask(E36, 1);
        s.members = -2147483647 - 1;
        g_texts[0xba8db526] = "[A][A]";
        ask(E36, 1);
    });
    run("11 cond 39: players met", [](const Api &a) {
        menus_ready(a);
        ask(E39, 2, 0);   // empty: reload every call
        ask(E39, 4);
        ask(E39, 5);
        s.recent = {"one", "two", "three"};
        ask(E39, 7);   // reloads now (empty before)
        s.recent = {"x"};
        list_sweep(E39, 3);   // op 8 inside reloads
        s.recent = {};
        ask(E39, 8);
        ask(E39, 2, 0);
        ask(E39, 4);
    });
    run("12 cond 3: join list", [](const Api &a) {
        menus_ready(a);
        s.join_rows = -1;
        list_sweep(E3, 1);
        s.join_rows = -7;
        ask(E3, 1);
        s.join_rows = 2;
        s.join_title = "Online games (2)";
        s.join_row_texts = {"room A", "room B"};
        list_sweep(E3, 2);
        s.join_rows = 0;
        list_sweep(E3, 0);
    });
    run("13a cond 104: NET_SM and normal volumes", [](const Api &a) {
        menus_ready(a);
        for (int sm : {0, 1, 2, -5}) {
            put<int32_t>(NETSM, sm);
            s.volume = 0.5f;
            ask(E104, 4, 77);
            ask(E104, 9, 7);
            ask(E104, 5);
            ask(E104, 1);
        }
        put<int32_t>(NETSM, 2);
        for (float v : {0.0f, 0.5f, 1.0f, 1.04f, -0.1f, 2.0f, 0.975f, 0.97499996f, -0.0f, 1e-30f, 100.0f, 1.0e8f}) {
            s.volume = v;
            ask(E104, 4);
        }
        for (int v : {0, 7, 20, -3, 2147483647, -2147483647 - 1, 16777217}) ask(E104, 9, v);
    });
    run("13b cond 104: NaN and negative volumes", [](const Api &a) {
        menus_ready(a);
        for (float v : {std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::quiet_NaN(),
                        -std::numeric_limits<float>::infinity(), -1e30f, -1e9f, -0.025f, -0.026f}) {
            s.volume = v;
            ask(E104, 4);
        }
    });
    run("13c cond 104: huge volume 1e9", [](const Api &a) {
        menus_ready(a);
        s.volume = 1e9f;
        ask(E104, 4);
    });
    run("13d cond 104: +infinity volume", [](const Api &a) {
        menus_ready(a);
        s.volume = std::numeric_limits<float>::infinity();
        ask(E104, 4);
    });
    run("13e cond 104: volume 107374182.4 (v just past 2^31)", [](const Api &a) {
        menus_ready(a);
        for (float v : {107374176.0f, 107374184.0f, 3.4e38f}) {
            s.volume = v;
            ask(E104, 4);
        }
    });
}

// ---- playlists ------------------------------------------------------------------------------------------
void pick_and_types(const Api &a, const std::vector<uint32_t> &ids, unsigned seed) {
    std::srand(seed);
    for (uint32_t id : ids) {
        put<uint32_t>(NPD + 0x104, id);
        std::string picks;
        for (int i = 0; i < 12; ++i) picks += std::to_string(a.pick()) + " ";
        rec("pick id %08x: %s next rand %d", id, picks.c_str(), std::rand());
        std::vector<int32_t> types = {42, 43};
        const bool ok = a.types(types);
        std::string t;
        for (int32_t v : types) t += std::to_string(v) + " ";
        rec("types id %08x: %d [%s]", id, ok ? 1 : 0, t.c_str());
    }
}

uint32_t first_playlist_id() {
    const uint64_t p = get<uint64_t>(NGM + 0x20a0);
    return p ? *reinterpret_cast<const uint32_t *>(p) : 0;
}
uint32_t playlist_id(int k) {
    const uint64_t p = get<uint64_t>(NGM + 0x20a0 + 8 * k);
    return p ? *reinterpret_cast<const uint32_t *>(p) : 0;
}

// After a build (the image is fresh, so the first call republishes): descriptors, apply, pick.
void post_build_scenarios(const std::string &tag, void (*data)()) {
    run(tag + " 18a count cleared: pointers rewritten, no log", [data](const Api &a) {
        menus_ready(a);
        data();
        ask(E55, 1);
        put<uint32_t>(NGM + 0x21a0, 0);
        put<uint64_t>(NGM + 0x20a0, 0);
        ask(E55, 1);
        put<uint32_t>(NGM + 0x21a0, 2);   // someone else's count: left alone
        ask(E55, 1);
    });
    run(tag + " 19 op 3 descriptors", [data](const Api &a) {
        menus_ready(a);
        data();
        ask(E55, 1);
        const uint32_t n = get<uint32_t>(NGM + 0x21a0);
        for (uint32_t row : {0u, n - 1, n, 0xffffffffu, 5u}) ask(E55, 3, int32_t(row), 0x25);
        ask(E55, 3, 0, 0x24);
        ask(E55, 3, 0, 0x26);
        g_orig_hook = [](uint32_t, uint8_t *q) { put<uint32_t>(q - mem + 8, 0x25); };   // the original sets the type
        ask(E55, 3, 1, 3);
        ask(E55, 2, 1, 3);   // op 2 with type 0x25: nothing
    });
    run(tag + " 20 op 4 apply", [data](const Api &a) {
        menus_ready(a);
        data();
        put<int32_t>(NI + 0x2c4, 1);
        put<int16_t>(NGM + 0x1ca0, -1);
        put<uint32_t>(NPD + 0x104, 0x12345);
        g_orig_hook = [](uint32_t, uint8_t *q) {
            if (get<uint32_t>(q - mem) == 4) put<int32_t>(q - mem + 4, -1);
        };
        ask(E55, 4);   // saved id not ours -> first; apply (current < 0 resets applied)
        g_orig_hook = nullptr;
        rec("npd %08x", get<uint32_t>(NPD + 0x104));
        const uint32_t n = get<uint32_t>(NGM + 0x21a0);
        put<int16_t>(NGM + 0x1ca0, 5);
        ask(E55, 4, 0);   // same id: applied, nothing
        for (uint32_t k = 0; k < n; ++k) {   // every playlist chosen in turn, current game 5
            put<uint32_t>(NPD + 0x104, playlist_id(int(k)));
            ask(E55, 4, int32_t(k));
        }
        // current game already of the chosen type: no SetNetGame
        put<uint32_t>(NPD + 0x104, first_playlist_id());
        for (int16_t cur : {0, 1, 2, 3, 7, 16, 17, 18, 19, 500, 32767, -32768}) {
            put<int16_t>(NGM + 0x1ca0, cur);
            put<uint32_t>(NPD + 0x104, playlist_id(int(cur & 3) % int(n ? n : 1)));
            ask(E55, 4, 0);
            put<uint32_t>(NPD + 0x104, playlist_id((int(cur & 3) + 1) % int(n ? n : 1)));
            ask(E55, 4, 0);
        }
        // client / mode 0 / unknown id
        put<int16_t>(NGM + 0x1ca0, 3);
        s.host = false;
        put<uint32_t>(NPD + 0x104, playlist_id(1));
        ask(E55, 4, 0);
        s.host = true;
        put<int32_t>(NI + 0x2c4, 0);
        ask(E55, 4, 0);
        put<int32_t>(NI + 0x2c4, 1);
        put<uint32_t>(NPD + 0x104, 0xdeadbeef);
        ask(E55, 4, 0);
        // no available game of the type: every game refused
        s.dlc_off.clear();
        for (int i = 0; i < 64; ++i) s.dlc_off.push_back(i);
        put<uint32_t>(NPD + 0x104, playlist_id(int(n) - 1));
        ask(E55, 4, 0);
        ask(E55, 4, 0);
        s.dlc_off.clear();
        // games table gone / records NULL at apply time
        put<uint32_t>(NPD + 0x104, playlist_id(0));
        g_dc.erase(0x3b9a067d);
        ask(E55, 4, 0);
        g_dc[0x3b9a067d] = GAMES;
        put<uint64_t>(GAMES + 8, 0);
        ask(E55, 4, 0);
        put<uint64_t>(GAMES + 8, addr(GAMES_REC));
        put<int32_t>(GAMES, 0);   // count 0: not checked at apply, nothing found
        ask(E55, 4, 0);
        put<int32_t>(GAMES, -3);
        ask(E55, 4, 0);
        data();
        ask(E55, 4, 0);
        // leaving Multiplayer resets applied: same id applied again
        put<int16_t>(NGM + 0x1ca0, -1);
        ask(E55, 1);
        put<int16_t>(NGM + 0x1ca0, 5);
        ask(E55, 4, 0);
        ask(E55, 4, 0);
        // op 4 when the original answers >= 0: no rewrite
        put<uint32_t>(NPD + 0x104, 0x999);
        ask(E55, 4, 3);
        rec("npd %08x", get<uint32_t>(NPD + 0x104));
    });
    run(tag + " 21 pick and types", [data](const Api &a) {
        menus_ready(a);
        data();
        ask(E55, 1);
        std::vector<uint32_t> ids;
        for (int k = 0; k < 32; ++k)
            if (playlist_id(k)) ids.push_back(playlist_id(k));
        ids.push_back(0xdeadbeef);
        ids.push_back(0);
        pick_and_types(a, ids, 12345);
        pick_and_types(a, ids, 1);
    });
}

void build_scenario(const std::string &name, void (*data)(), int calls) {
    run(name, [data, calls](const Api &a) {
        menus_ready(a);
        data();
        for (int i = 0; i < calls; ++i) ask(E55, 1);
        rec_playlists();
    });
}

void playlist_prebuild_scenarios() {
    run("21a pick before build", [](const Api &a) {
        menus_ready(a);
        put<uint32_t>(NPD + 0x104, sid("u2mp/playlist/all-competitive"));
        pick_and_types(a, {sid("u2mp/playlist/all-competitive"), 0}, 99);
    });
    struct V {
        const char *name;
        void (*f)();
    };
    const V v[] = {
        {"games NULL", [] { g_dc.erase(0x3b9a067d); }},
        {"types NULL", [] { g_dc.erase(0xf985c7b6); }},
        {"both NULL", [] { g_dc.clear(); }},
        {"games count 0", [] { put<int32_t>(GAMES, 0); }},
        {"types count 0", [] { put<int32_t>(TYPES, 0); }},
        {"games count negative", [] { put<int32_t>(GAMES, -4); }},
        {"types count negative", [] { put<int32_t>(TYPES, -1); }},
        {"games records NULL", [] { put<uint64_t>(GAMES + 8, 0); }},
        {"types records NULL", [] { put<uint64_t>(TYPES + 8, 0); }},
    };
    for (const V &x : v)
        run(std::string("17 no build: ") + x.name, [x](const Api &a) {
            menus_ready(a);
            full_playlist_data();
            x.f();
            ask(E55, 1);
            ask(E55, 4);
            ask(E55, 3, 0, 0x25);
        });
    run("18b count set by someone else: no build", [](const Api &a) {
        menus_ready(a);
        full_playlist_data();
        put<uint32_t>(NGM + 0x21a0, 3);
        ask(E55, 1);
        ask(E55, 4, -1);
    });
    run("16b fallback with nothing playable: count 0, line repeated", [](const Api &a) {
        menus_ready(a);
        full_playlist_data();
        for (int i = 0; i < 64; ++i) s.dlc_off.push_back(i);
        ask(E55, 1);
        ask(E55, 4, -1);
        ask(E55, 3, 0, 0x25);
        rec_playlists();
    });
    run("16c fallback with only Machinima playable", [](const Api &a) {
        menus_ready(a);
        full_playlist_data();
        for (int i = 0; i < 64; ++i)
            if (i != 14) s.dlc_off.push_back(i);
        ask(E55, 1);
    });
}

// ======================================================================================================
// menuimages scenarios
// ======================================================================================================
void menuimages_install_scenarios() {
    setenv_scale(nullptr);
    run("22a install: default env", [](const Api &a) {
        mi_image();
        mi_install(a);
        rec("jump %s", hex(mem + 0x5c2ec4, 0x98).c_str());
    });
    for (const char *e : {"0", "-2", "abc", "", "0.75", "1", "1.0", "0.5"}) {
        setenv_scale(e);
        run(std::string("22b install: env \"") + e + "\"", [](const Api &a) {
            mi_image();
            mi_install(a);
            float out[2] = {0, 0};
            put<float>(SOUT, 0);
            size_call(0x5bcc1a, reinterpret_cast<float *>(mem + SOUT), mem + NE);
        });
    }
    setenv_scale("0.5");
    run("23a first retarget fails", [](const Api &a) {
        mi_image();
        hx::fail_offsets.push_back(0x5bcc1a);
        mi_install(a);
    });
    run("23b second retarget fails", [](const Api &a) {
        mi_image();
        hx::fail_offsets.push_back(0x5bdb30);
        mi_install(a);
    });
    run("24a converter bytes differ", [](const Api &a) {
        mi_image();
        mem[0x5bc290 + 17] = 1;
        mi_install(a);
    });
    run("24b no stub memory at all", [](const Api &a) {
        mi_image();
        hx::exec_left = 0;
        mi_install(a);
    });
    run("24c stub memory for the converter only", [](const Api &a) {
        mi_image();
        hx::exec_left = 1;
        mi_install(a);
    });
    run("24d converter replace_function fails", [](const Api &a) {
        mi_image();
        hx::fail_offsets.push_back(0x5bc290);
        mi_install(a);
    });
    run("24e lookup bytes differ", [](const Api &a) {
        mi_image();
        mem[0x5558c0 + 15] = 0x21;
        mi_install(a);
    });
    run("24f lookup replace_function fails", [](const Api &a) {
        mi_image();
        hx::fail_offsets.push_back(0x5558c0);
        mi_install(a);
    });
    const std::pair<uint64_t, const char *> checks[] = {
        {0x5bd852, "dispatch first byte"}, {0x5bd852 + 35, "dispatch last byte"}, {0x5c1ab1 + 6, "exit"},
        {0xbdce90, "DrawQuad first"},      {0xbdce90 + 15, "DrawQuad last"}};
    for (const auto &c : checks)
        run(std::string("24g renderer check: ") + c.second, [c](const Api &a) {
            mi_image();
            mem[c.first] ^= 0x01;
            mi_install(a);
        });
    run("24h renderer: image size below the jump table end", [](const Api &a) {
        mi_image();
        hx::set_image_both(mem, 0x5c2ec4 + 0x26 * 4 - 1);
        mi_install(a);
    });
    for (uint32_t t : {0x1eu, 0x22u, 0x23u, 0x24u, 0x25u, 0x27u})
        run("24i jump entry not the exit: type " + std::to_string(t), [t](const Api &a) {
            mi_image();
            put<int32_t>(0x5c2ec4 + 4 * (t - 4), int32_t(0x5c1ab1 - 0x5c2ec4) + 1);
            mi_install(a);
            rec("jump %s", hex(mem + 0x5c2ec4, 0x98).c_str());
        });
    run("24j jump entries of other types are not checked", [](const Api &a) {
        mi_image();
        put<int32_t>(0x5c2ec4 + 4 * (0x26 - 4), 0x1000);
        put<int32_t>(0x5c2ec4 + 4 * (0x21 - 4), 0x1000);
        mi_install(a);
        rec("jump %s", hex(mem + 0x5c2ec4, 0x98).c_str());
    });
}

void lookup_scenarios() {
    run("25 lookup by index, NULL tables, other selectors", [](const Api &a) {
        mi_ready(a);
        for (uint32_t sel : {0u, 1u, 2u, 3u, 0xffffffffu})
            for (int32_t idx : {0, 1, 7, 9, 10, 100, 65535, 2147483647}) look(sel, 0x1234, idx);
        look(0, 0x4000, -1);
        look(3, 0x1000, -1);
        look(0xffffffffu, 0x1000, -1);
        look(1, 0x1000, -2147483647 - 1);
        put<uint64_t>(0x1130008 + 0x68, 0);
        look(0, 0x4000, 0);
        put<uint64_t>(0x1130008 + 0x70, 0);
        look(1, 0x1000, -1);
        look(1, 0x1000, 0);
        put<uint64_t>(0x1130008 + 0x78, 0);
        look(2, 0x3000, -1);
    });
    run("26a lookup: direct ids, PS3 names, dumps", [](const Api &a) {
        mi_ready(a);
        for (uint32_t id : {0x1000u, 0x1009u, 0x3007u}) look(id >= 0x3000 ? 2 : 1, id, -1);
        look(1, kPicDm, -1);
        look(1, kPicObj, -1);
        look(1, kRank, -1);
        look(1, kRank2, -1);
        look(1, kMapLong, -1);
        look(1, 0, -1);   // first nameless entry
        look(2, sid("menu/set/item1"), -1);
        look(2, sid("menu/x/y"), -1);
        look(2, sid("menu/set/item6"), -1);
        look(2, 0, -1);
        look(2, 0x77777777, -1);
        look(1, sid("general/net/playlists:deathmatch"), -1);
        look(1, sid("general/net/playlists/deathmatch,{0123-4567}"), -1);
    });
    run("26b lookup: table pointer / count changes, dump again", [](const Api &a) {
        mi_ready(a);
        look(1, kPicDm, -1);
        put<uint16_t>(NT + 2, 4);   // fewer entries: dump again, maps beyond are gone
        look(1, kRank2, -1);
        look(1, kPicObj, -1);
        put<uint16_t>(NT + 2, 0);
        look(1, kPicObj, -1);
        put<uint16_t>(NT + 2, 10);
        // the same pointer and count with other names: the cache stays
        std::snprintf(reinterpret_cast<char *>(mem + NN + 0x30), 0x90, "%s", "general/net/playlists:renamed");
        look(1, sid("general/net/playlists/renamed"), -1);
        look(1, kPicDm, -1);
        // another table pointer (copy at MT+0x800)
        std::memcpy(mem + MT + 0x800, mem + NT, 16);
        put<uint64_t>(0x1130008 + 0x70, addr(MT + 0x800));
        look(1, sid("general/net/playlists/renamed"), -1);
        // menu table with 3 entries (<= 6 dumped lines)
        put<uint16_t>(MT + 2, 3);
        look(2, sid("menu/set/item2"), -1);
        look(2, sid("menu/set/item6"), -1);
    });
    run("26c lookup: index >= 0 after PS3 index, entries NULL with count 0", [](const Api &a) {
        mi_ready(a);
        put<uint16_t>(MT + 2, 0);
        put<uint64_t>(MT + 8, 0);
        look(2, 0x3000, -1);
        look(2, 0x3000, 0);
    });
    run("27a extra: found in net-u2mp, table logged once per pointer", [](const Api &a) {
        mi_ready(a);
        look(1, 0x2000, -1);
        look(1, kLab, -1);
        look(1, 0x2000, -1);
        look(1, 0x2002, -1);
        look(1, 0x2fff, -1);
        // another package table pointer
        std::memcpy(mem + XT + 0x400, mem + XT, 16);
        put<uint64_t>(PK + 0x940, addr(XT + 0x400));
        look(1, 0x2000, -1);
        put<uint64_t>(PK + 0x940, addr(XT));
        look(1, 0x2000, -1);
        look(2, 0x2000, -1);   // menu: no extra search
    });
    run("27b extra: package NULL, state != 7, table NULL, entries NULL", [](const Api &a) {
        mi_ready(a);
        s.package = 0;
        look(1, 0x2000, -1);
        s.package = PK;
        for (int32_t st : {6, 8, -7, 0}) {
            put<int32_t>(PK + 0x30, st);
            look(1, 0x2000, -1);
        }
        put<int32_t>(PK + 0x30, 7);
        put<uint64_t>(PK + 0x940, 0);
        look(1, 0x2000, -1);
        put<uint64_t>(PK + 0x940, addr(XT));
        put<uint64_t>(XT + 8, 0);
        look(1, 0x2000, -1);
        put<uint64_t>(XT + 8, addr(XE));
        put<uint16_t>(XT + 2, 0);
        look(1, 0x2000, -1);
        put<uint16_t>(XT + 2, 3);
        look(1, kLab, -1);
    });
    run("28 lookup log budget: more than 48 misses", [](const Api &a) {
        mi_ready(a);
        for (uint32_t i = 0; i < 60; ++i) look(i & 1 ? 2 : 1, 0xabc00000 + i, -1);
        look(1, kPicDm, -1);
        look(1, 0x2000, -1);
    });
}

void size_scenarios() {
    run("29 size calls", [](const Api &a) {
        mi_ready(a);
        float *out = reinterpret_cast<float *>(mem + SOUT);
        for (uint64_t site : {0x5bcc1au, 0x5bdb30u}) {
            size_call(site, out, mem + NE);                 // first net entry
            size_call(site, out, mem + NE + 0x28 * 9);      // last
            size_call(site, out, mem + NE + 0x28 * 10);     // one past
            size_call(site, out, mem + NE - 1);
            size_call(site, out, mem + NE + 7);             // inside an entry
            size_call(site, out, mem + XE);                 // net-u2mp
            size_call(site, out, mem + HE);                 // hud
            size_call(site, out, nullptr);
            size_call(site, nullptr, mem + NE);
        }
        g_tex_w = std::numeric_limits<float>::quiet_NaN();
        g_tex_h = -std::numeric_limits<float>::infinity();
        size_call(0x5bcc1a, out, mem + NE);
        g_tex_w = 1e38f;
        g_tex_h = -0.0f;
        size_call(0x5bcc1a, out, mem + NE);
        g_tex_w = 1e-45f;
        g_tex_h = 3.0f;
        size_call(0x5bcc1a, out, mem + NE);
        put<uint64_t>(NT + 8, 0);
        size_call(0x5bcc1a, out, mem + NE);
        put<uint64_t>(NT + 8, addr(NE));
        put<uint64_t>(0x1130008 + 0x70, 0);
        size_call(0x5bcc1a, out, mem + NE);
    });
    setenv_scale("0.75");
    run("29b size calls at scale 0.75", [](const Api &a) {
        mi_ready(a);
        float *out = reinterpret_cast<float *>(mem + SOUT);
        g_tex_w = 1000.0f;
        g_tex_h = 333.0f;
        size_call(0x5bcc1a, out, mem + NE);
    });
    setenv_scale("0.5");
    run("30 converter calls", [](const Api &a) {
        mi_ready(a);
        uint8_t *out = mem + COUT;
        const uint8_t *src = mem + CSRC;
        const float *box = reinterpret_cast<const float *>(mem + CBOX);
        put<float>(CBOX, 320.0f);
        put<float>(CBOX + 4, 160.5f);
        g_conv_mat = NE + 0x28;
        conv_src(3, 0x1001, 1);
        rec("r %llx", (unsigned long long)conv_outside(out, src, box, 1.0f));
        rec("r %llx", (unsigned long long)conv_outside(out, src, box, 1.0f));   // same pair: not again
        rec("r %llx", (unsigned long long)conv_inside(out, src, box, 0.333f));   // caller in the image
        rec("r %llx", (unsigned long long)conv_inside(out, src, box, 0.333f));
        conv_src(3, 0x1002, 1);
        g_conv_mat = XE;
        rec("r %llx", (unsigned long long)conv_inside(out, src, nullptr, -2.0f));   // NULL box, net-u2mp material
        conv_src(3, 0x1003, 1);
        g_conv_mat = 0;
        rec("r %llx", (unsigned long long)conv_inside(out, src, box, std::numeric_limits<float>::quiet_NaN()));
        conv_src(2, 0x1004, 1);
        conv_inside(out, src, box, 1.0f);
        conv_src(3, 0x1004, 2);
        conv_inside(out, src, box, 1.0f);
        conv_src(3, 0x1004, 0);
        conv_inside(out, src, box, 1.0f);
        conv_src(3, 0x1005, 1);
        rec("r %llx", (unsigned long long)conv_inside(nullptr, src, box, 1.0f));
        rec("r %llx", (unsigned long long)conv_inside(out, nullptr, box, 1.0f));
        g_conv_mat = HE;
        g_conv_w = 1e10f;
        g_conv_h = -0.04f;
        conv_inside(out, src, box, 1.0f);
        // the logged set: 96 pairs at most
        g_conv_mat = NE;
        for (uint32_t i = 0; i < 100; ++i) {
            conv_src(3, 0x9000 + i, 1);
            conv_inside(out, src, box, 1.0f);
        }
        conv_src(3, 0x1001, 1);
        conv_outside(out, src, box, 1.0f);
    });
}

void draw_scenarios() {
    run("31 each type through the jump table", [](const Api &a) {
        mi_ready(a);
        draw_tables();
        draw({0x27, 9});
        draw({0x1e, 0x00030000});
        draw({0x22, 0x501});
        draw({0x24, 0x77});
        draw({0x25, 0x78});
        draw({0x23, 0x77});
        draw({0x26, 0x77});   // not redirected: straight to the exit
    });
    run("32a rank badges", [](const Api &a) {
        mi_ready(a);
        draw_tables();
        for (uint32_t v : {0u, 9u, 63u, 64u, 100u, 0xffffffffu, 0x80000000u, 0x7fffffffu, 127u, 128u}) draw({0x27, v});
    });
    run("32b boosters", [](const Api &a) {
        mi_ready(a);
        draw_tables();
        for (uint32_t v : {0x00000000u, 0x00010002u, 0x00030003u, 0x00040000u, 0xffff0005u, 0x00000003u}) draw({0x1e, v});
        g_dc.erase(0x916e03b0);
        draw({0x1e, 0x00030000});
        g_dc[0x916e03b0] = BOOST;
        put<uint64_t>(BOOST + 8, 0);
        draw({0x1e, 0x00030000});
        put<uint64_t>(BOOST + 8, addr(BOOST_REC));
        put<int32_t>(BOOST, -1);
        draw({0x1e, 0x00030000});
        put<int32_t>(BOOST, 4);
        DrawIn d{0x1e, 0x00030000};
        d.y = -37.25f;
        d.h = 333.3f;
        d.wd = 77.7f;
        draw(d);
        d.wd = std::numeric_limits<float>::quiet_NaN();
        draw(d);
    });
    run("32c maps and playlists", [](const Api &a) {
        mi_ready(a);
        draw_tables();
        for (uint32_t v : {0x500u, 0x501u, 0x502u, 0x999u}) draw({0x22, v});
        g_dc.erase(0x0dec97a4);
        draw({0x22, 0x500});
        g_dc[0x0dec97a4] = MAPS;
        put<uint64_t>(MAPS + 8, 0);
        draw({0x22, 0x500});
        put<uint64_t>(MAPS + 8, addr(MAPS_REC));
        put<int32_t>(MAPS, -2);
        draw({0x22, 0x500});
        put<int32_t>(MAPS, 3);
        for (uint32_t t : {0x24u, 0x25u})
            for (uint32_t v : {0x77u, 0x78u, 0x79u, 0u}) draw({t, v});
        for (float h : {30.0f, 50.0f, 50.5f, 80.0f, std::numeric_limits<float>::quiet_NaN(), -10.0f}) {
            DrawIn d{0x25, 0x77};
            d.h = h;
            draw(d);
            d.type = 0x24;
            draw(d);
        }
        put<uint32_t>(NGM + 0x21a0, 0);
        draw({0x24, 0x77});
        put<uint32_t>(NGM + 0x21a0, 0xffffffffu);
        draw({0x24, 0x79});
        put<uint64_t>(NGM + 0x20a0 + 8 * 31, addr(GPL + 0x100));
        put<uint32_t>(GPL + 0x100, 0x79);
        put<uint32_t>(GPL + 0x110, 0x1006);
        draw({0x24, 0x79});
    });
    run("32d empty boxes and odd floats", [](const Api &a) {
        mi_ready(a);
        draw_tables();
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        const float vals[] = {0.0f, -0.0f, -1.0f, nan, inf, -inf, 1e-45f, 3.4e38f};
        for (float v : vals) {
            DrawIn d{0x27, 5};
            d.wd = v;
            draw(d);
            d = DrawIn{0x27, 5};
            d.h = v;
            draw(d);
            d = DrawIn{0x27, 5};
            d.x = v;
            d.y = -v;
            draw(d);
            d = DrawIn{0x27, 5};
            d.alpha = v;
            draw(d);
        }
        DrawIn d{0x27, 5};
        d.ebx = 0xffffffff, d.r14 = 0xffffffff, d.r12 = 0xffffffff, d.r13 = 0xffffffff;
        draw(d);
        d.ebx = 0, d.r14 = 0, d.r12 = 0, d.r13 = 0;
        draw(d);
    });
    run("32e draw log budget", [](const Api &a) {
        mi_ready(a);
        draw_tables();
        for (int i = 0; i < 30; ++i) draw({0x23, uint32_t(i)});
    });
    run("33 menu_draw_quad and menu_net_material", [](const Api &a) {
        mi_ready(a);
        draw_tables();
        rec("net_material(0) %s", w(a.net_material(0)).c_str());
        rec("net_material(1000) %s", w(a.net_material(0x1000)).c_str());
        rec("net_material(dm) %s", w(a.net_material(kPicDm)).c_str());
        rec("net_material(lab) %s", w(a.net_material(kLab)).c_str());
        rec("net_material(miss) %s", w(a.net_material(0x5555)).c_str());
        const float nan = std::numeric_limits<float>::quiet_NaN();
        a.draw_quad(mem + NE, 10, 20, 30, 40, 0.1f, 0.2f, 0.3f, 0.4f, 0.75f, 1.0f);
        a.draw_quad(mem + NE, 10, 20, 30, 40, 0, 0, 1, 1, 0.75f, 0.25f);
        a.draw_quad(nullptr, 10, 20, 30, 40, 0, 0, 1, 1, 1, 1);
        a.draw_quad(mem + NE, 10, 20, 0, 40, 0, 0, 1, 1, 1, 1);
        a.draw_quad(mem + NE, 10, 20, 30, -0.0f, 0, 0, 1, 1, 1, 1);
        a.draw_quad(mem + NE, nan, nan, nan, nan, nan, nan, nan, nan, nan, nan);
        a.draw_quad(mem + XE, -1e30f, 1e30f, 1e38f, 1e38f, -1, 2, 3, 4, -5, 6);
    });
}

void extra_scenario(const char *tag) {
    run(std::string("34 extra_net_package twice ") + tag, [](const Api &a) {
        const char *p1 = a.extra();
        const char *p2 = a.extra();
        rec("extra %s %s", p1 ? p1 : "NULL", p2 ? p2 : "NULL");
    });
    run(std::string("34b net lookup miss with this package state ") + tag, [](const Api &a) {
        mi_ready(a);
        look(1, 0x2000, -1);
        look(1, kLab, -1);
        look(1, 0x2fff, -1);
        rec("net_material(lab) %s", w(a.net_material(kLab)).c_str());
    });
}

int summary(const char *mode) {
    int fail = 0;
    for (auto &r : results) fail += r.second ? 0 : 1;
    std::printf("== mode %s: %zu scenarios, %d failed\n", mode, results.size(), fail);
    std::fflush(stdout);
    return fail;
}

}  // namespace

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *, char *, unsigned long);
#endif

int main(int argc, char **argv) {
    hx::init();
    const std::string mode = argc > 1 ? argv[1] : "main";
    // the extra package's file: a regular file, a directory, a missing file
    const auto tmp = std::filesystem::temp_directory_path() / "u2mp_rewrite_menus";
    std::filesystem::create_directories(tmp);
    g_file = (tmp / "net-u2mp.pak").string();
    std::ofstream(g_file) << "pak";
    g_dir = tmp.string();
    g_missing = (tmp / "missing.pak").string();
    setenv_scale(nullptr);

    if (mode == "main") {
        g_translate = 0;
        extra_scenario("(regular file)");
        menus_install_scenarios();
        menus_query_scenarios();
        playlist_prebuild_scenarios();
        build_scenario("14 all game types playable: 11 PS3 playlists", full_playlist_data, 3);
        post_build_scenarios("14", full_playlist_data);
        menuimages_install_scenarios();
        lookup_scenarios();
        size_scenarios();
        draw_scenarios();
        int fail = summary("main");
        char exe[1024] = {};
        GetModuleFileNameA(nullptr, exe, sizeof exe);
        for (const char *child : {"pl-partial", "pl-fallback", "pl-fallback-cap", "extra-fail", "extra-dir", "extra-missing"}) {
            const std::string cmd = std::string("\"\"") + exe + "\" " + child + "\"";
            const int r = std::system(cmd.c_str());
            std::printf("== child %s exit %d\n", child, r);
            fail += r != 0 ? (r > 0 ? r : 1) : 0;
        }
        std::printf("== total failed scenarios: %d\n", fail);
        return fail ? 1 : 0;
    }
    if (mode == "pl-partial") {
        build_scenario("15 some types playable: skipped definitions, dups, 8 cap", partial_playlist_data, 2);
        post_build_scenarios("15", partial_playlist_data);
    } else if (mode == "pl-fallback") {
        build_scenario("16 fallback: Machinima skipped, ids replaced", fallback_playlist_data, 2);
        post_build_scenarios("16", fallback_playlist_data);
    } else if (mode == "pl-fallback-cap") {
        build_scenario("16d fallback capped at 32", fallback_cap_data, 2);
        post_build_scenarios("16d", fallback_cap_data);
    } else if (mode == "extra-fail") {
        g_translate = 1;
        extra_scenario("(translate fails)");
    } else if (mode == "extra-dir") {
        g_translate = 2;
        extra_scenario("(directory)");
    } else if (mode == "extra-missing") {
        g_translate = 3;
        extra_scenario("(missing file)");
    }
    return summary(mode.c_str());
}
