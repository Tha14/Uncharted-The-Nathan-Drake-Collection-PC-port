// u2mp rewrite tests, group "remaining": remaining.cpp and clantag.cpp, old (git) against new, on the scenarios of
// u2mp/docs/spec/remaining.md 1.6 / 2.6 and the branches of the old code.
//
//   bash gow3_pc/tests/u2mp_rewrite/run.sh remaining remaining.cpp clantag.cpp
//
// flags_remaining.sh renames the old build's C text-input calls; deps_remaining.cpp has the stand-ins (deps_remaining.h
// their scenario state). HX_SHOW=<part of a scenario name> prints the old build's record of matching scenarios.
//
// Every scenario installs the module on a fake image (so the entry points are the stubs replace_function wrote), then
// drives the entry points. Recorded: log lines, replace/retarget calls, guest calls (PopPage, ShowPopup with the
// 19 descriptor words, the kill-info sender with its 7 arguments, IsDead / TakeDamage, the data lookup), the
// stand-ins of deps_remaining.cpp, and return values. Host pointers that differ between builds by nature (the
// functions written into the menu callback table, the query text pointers, the TextDb jump stub's encoding) are
// compared by meaning: callback table function fields become ordinals, query pointers are compared by the text and
// by "same buffer as before", the TextDb hook is executed instead of compared byte for byte.
#include "harness.h"

#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>

#include "deps_remaining.h"
#include "online.h"
#include "texts.h"
#include "u2mp_internal.h"

namespace hx::fk {
State st;
}
using hx::fk::st;

namespace u2mp_old {
void install_remaining(int &installed);
void show_notice(const char *text);
void cinema_pick_friend();
bool edit_clan_tag();
void clan_tag_frame();
extern u2mp::Image image;
}  // namespace u2mp_old
namespace u2mp {
bool edit_clan_tag();
void cinema_pick_friend();
void clan_tag_frame();
}  // namespace u2mp

namespace {

using hx::addr;
using hx::get;
using hx::mem;
using hx::put;
using hx::rec;
using u2mp::sid;

struct Ns {
    const char *tag;
    void (*install)(int &);
    void (*notice)(const char *);
    void (*pick)();
    bool (*edit)();
    void (*frame)();
    u2mp::Image *image;
};
const Ns kOld{"old", u2mp_old::install_remaining, u2mp_old::show_notice, u2mp_old::cinema_pick_friend,
              u2mp_old::edit_clan_tag, u2mp_old::clan_tag_frame, &u2mp_old::image};
const Ns kNew{"new", u2mp::install_remaining, u2mp::show_notice, u2mp::cinema_pick_friend, u2mp::edit_clan_tag,
              u2mp::clan_tag_frame, &u2mp::image};

// ---- image layout ---------------------------------------------------------------------------------------
constexpr uint64_t MENU = 0x1136640;
constexpr uint64_t BOX = 0x1600000;        // message box descriptor
constexpr uint64_t ARG = 0x1600100;        // menu callback argument
constexpr uint64_t Q = 0x1600200;          // cond 48 query object
constexpr uint64_t DST = 0x1600400;        // NetStatGame name field
constexpr uint64_t SRC = 0x1600500;
constexpr uint64_t EV = 0x1600600;         // event payload
constexpr uint64_t TBL = 0x1600700;        // B3 tag table
constexpr uint64_t GAMES = 0x1610000, GAMES_REC = 0x1610100, LEVELS = 0x1611000, LEVELS_REC = 0x1611100;
constexpr uint64_t NAMES = 0x1612000, PKGS = 0x1614000;
constexpr uint64_t VT = 0x1620000, SLOT_A = 0x1630000, PROC_A = 0x1630100, SLOT_N = 0x1630200, NPC = 0x1631000;
constexpr uint64_t BUCKETS = 0x1634000, NODE1 = 0x1634100, NODE2 = 0x1634200, END = 0x1634300, NODE3 = 0x1634400;
constexpr uint64_t GSTUBS = 0x1700000;     // stubs of the test's guest functions
constexpr uint64_t NGM = 0x113b760, MATCH = 0x1065b08, PARTY = 0x105efb8, NETINFO = 0x104f1c8, NET_SM = 0x14ce97c;
constexpr uint64_t CLANTAG = 0x11476a0;

template <size_t N> void bytes(uint64_t off, const unsigned char (&b)[N]) { std::memcpy(mem + off, b, N); }
void call_rel(uint64_t at, uint64_t target) {   // E8 rel32 at `at`
    mem[at] = 0xe8;
    put<int32_t>(at + 1, int32_t(int64_t(target) - int64_t(at + 5)));
}

const uint64_t kRetStubs[20] = {0x61b900, 0x5ec490, 0x5ec460, 0x5ebde0, 0x5ec4a0, 0x5ec4b0, 0x5ec480,
                                0x5ec4c0, 0x5ecb70, 0x5ebdd0, 0x5ebea0, 0x5ec000, 0x5ed8a0, 0x5ed8b0,
                                0x5ed8c0, 0x64e900, 0x6769d0, 0x6769e0, 0x5e4510, 0x5e4520};

// the clean image: every check of install_remaining passes
void base() {
    static const unsigned char level_pkg[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x10, 0x48, 0x89, 0xfb};
    static const unsigned char tracker[] = {0xbf, 0x00, 0x00, 0x00, 0x00, 0x48, 0x0f, 0x45, 0xfe};
    static const unsigned char tag[] = {0x48, 0x8b, 0x02, 0x8b, 0x4a, 0x08, 0x31, 0xd2, 0xeb, 0x0a};
    static const unsigned char len_site[] = {0x31, 0xff};
    static const unsigned char copy_site[] = {0x48, 0x8d, 0x50, 0x01, 0x31, 0xf6, 0x48, 0x89, 0xdf};
    static const unsigned char find[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x89, 0xf3, 0x49, 0x89, 0xfe,
                                         // the rest of the fake TextDb::Find: return id (mov eax,ebx; pop rbx; pop r14; pop rbp; ret)
                                         0x89, 0xd8, 0x5b, 0x41, 0x5e, 0x5d, 0xc3};
    static const unsigned char kill[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38};
    static const unsigned char popup[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xf0};
    static const unsigned char deal[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xec, 0x10, 0x4c, 0x8b, 0x05};
    static const unsigned char invite[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8d, 0x05, 0xcb,
                                           0x30, 0xa7, 0x00, 0x80, 0xb8, 0xc8, 0x00, 0x00, 0x00, 0x00};
    static const unsigned char query[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb, 0x8b, 0x03, 0x83, 0xf8, 0x02};
    static const unsigned char met[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x58, 0x48, 0x8b, 0x1d};
    static const unsigned char ret[] = {0xc3, 0x66, 0x66, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const unsigned char loop_mark[] = {0x41, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};   // mov r8d,1; ret
    static const unsigned char nf_mark[] = {0x41, 0xb8, 0x02, 0x00, 0x00, 0x00, 0xc3};     // mov r8d,2; ret
    bytes(0x644130, level_pkg);
    bytes(0x640598, tracker);
    bytes(0x4bf44c, tag);
    bytes(0x4bf460, loop_mark);
    bytes(0x4bf510, nf_mark);
    bytes(0x66c75d, len_site);
    call_rel(0x66c75f, 0xde6490);
    bytes(0x66c764, copy_site);
    call_rel(0x66c76d, 0xde6410);
    bytes(0xcddc00, find);
    bytes(0x60eb90, kill);
    for (int i = 0; i < 45; i++) put<uint32_t>(0xdef290 + 4 * i, i == 0 ? 0 : i == 2 ? 0xdf95da88u : 0xa0000000u + i * 0x111);
    bytes(0x5cc480, popup);
    bytes(0x61fa70, deal);
    bytes(0x5ebee0, invite);
    bytes(0x659b50, query);
    bytes(0x5ebdf0, met);
    for (uint64_t s : kRetStubs) bytes(s, ret);
    put<uint64_t>(MENU + 0x17d0, addr(BOX));
}

// ---- the test's guest functions ----------------------------------------------------------------------------
std::map<uint32_t, uint64_t> g_tables;   // data lookup id -> image offset of the table (0: NULL)
std::vector<uint8_t> g_dead;             // IsDead answers, front first (0 when empty)

U2MP_GUEST void g_pop_page(void *m) { rec("PopPage %s", hx::where(m).c_str()); }
U2MP_GUEST void g_show_popup(void *m, const uint32_t *d, uint64_t flag) {
    std::string s;
    char b[16];
    for (int i = 0; i < 0x13; i++) {
        std::snprintf(b, sizeof b, "%s%x", i ? " " : "", d[i]);
        s += b;
    }
    rec("ShowPopup %s [%s] %u", hx::where(m).c_str(), s.c_str(), unsigned(flag));
}
U2MP_GUEST void g_kill_info(uint32_t type, void *att, void *npc, int32_t *t, uint64_t a, uint64_t b, uint64_t c) {
    rec("KillInfo type=0x%x att=%s npc=%s time=%s%d a=%llx b=%llx c=%x", type, hx::where(att).c_str(), hx::where(npc).c_str(),
        t ? "" : "NULL:", t ? *t : 0, (unsigned long long)a, (unsigned long long)b, unsigned(c));
}
U2MP_GUEST void *g_lookup(uint32_t id) {
    rec("DataLookup 0x%x", id);
    auto it = g_tables.find(id);
    return it == g_tables.end() || !it->second ? nullptr : mem + it->second;
}
U2MP_GUEST uint8_t g_is_dead(void *npc) {
    uint8_t r = 0;
    if (!g_dead.empty()) {
        r = g_dead.front();
        g_dead.erase(g_dead.begin());
    }
    rec("IsDead %s -> %u", hx::where(npc).c_str(), r);
    return r;
}
U2MP_GUEST void g_take_damage(void *npc, int32_t d) { rec("TakeDamage %s %d", hx::where(npc).c_str(), d); }

void guest_stubs() {
    hx::stub(0x5cc6f0, reinterpret_cast<void *>(&g_pop_page));
    hx::stub(0x5cc480, reinterpret_cast<void *>(&g_show_popup));
    hx::stub(0x60eb90, reinterpret_cast<void *>(&g_kill_info));
    hx::stub(0xa15e70, reinterpret_cast<void *>(&g_lookup));
    hx::stub_ptr(VT + 0x250, GSTUBS, reinterpret_cast<void *>(&g_is_dead));
    hx::stub_ptr(VT + 0x370, GSTUBS + 0x20, reinterpret_cast<void *>(&g_take_damage));
}

// ---- callback table ----------------------------------------------------------------------------------------
// `n` foreign entries (ids 0x10000+i), count n
void fill_cbs(uint32_t n) {
    for (uint32_t i = 0; i < n && i < 200; i++) {
        const uint64_t e = MENU + 0x18f0 + uint64_t(i) * 0x18;
        put<uint32_t>(e, 0x10000 + i);
        put<uint32_t>(e + 4, 0xabcd0000 + i);
        put<uint64_t>(e + 8, 0x5555000000000000ull + i);
        put<uint64_t>(e + 0x10, 0x6666000000000000ull + i);
    }
    put<uint32_t>(MENU + 0x18ec, n);
}
void put_cb(uint32_t i, uint32_t id) { put<uint32_t>(MENU + 0x18f0 + uint64_t(i) * 0x18, id); }

using Cb = void(U2MP_GUEST *)(void *, void *, void *);
void call_registered(uint32_t id) {
    const uint32_t n = get<uint32_t>(MENU + 0x18ec);
    for (uint32_t i = 0; i < n && i < 128; i++) {
        const uint64_t e = MENU + 0x18f0 + uint64_t(i) * 0x18;
        if (get<uint32_t>(e) == id) {
            rec("-> callback 0x%x (entry %u)", id, i);
            reinterpret_cast<Cb>(uintptr_t(get<uint64_t>(e + 8)))(nullptr, nullptr, nullptr);
            return;
        }
    }
    rec("-> callback 0x%x not registered", id);
}

// The image compared: stubs zeroed (harness), callback functions as ordinals of distinct values.
std::vector<uint8_t> masked() {
    std::vector<uint8_t> s = hx::snapshot();
    const uint32_t n = get<uint32_t>(MENU + 0x18ec);
    std::vector<uint64_t> seen;
    for (uint32_t i = 0; i < n && i < 200; i++) {
        const uint64_t o = MENU + 0x18f0 + uint64_t(i) * 0x18 + 8;
        uint64_t v;
        std::memcpy(&v, s.data() + o, 8);
        if (!v) continue;
        size_t k = 0;
        while (k < seen.size() && seen[k] != v) k++;
        if (k == seen.size()) seen.push_back(v);
        const uint64_t ord = 0xf00d000000000000ull + k;
        std::memcpy(s.data() + o, &ord, 8);
    }
    return s;
}

// ---- running a scenario --------------------------------------------------------------------------------------
int g_total = 0, g_pass = 0;
std::vector<std::string> g_failed;

void run(const char *name, const std::function<void()> &setup, const std::function<void(const Ns &)> &act) {
    std::vector<std::string> calls[2];
    std::vector<uint8_t> img[2];
    for (int k = 0; k < 2; k++) {
        const Ns &ns = k ? kNew : kOld;
        hx::clear();
        hx::bind_both();
        st = hx::fk::State{};
        g_tables.clear();
        g_dead.clear();
        base();
        if (setup) setup();
        int installed = 0;
        ns.install(installed);
        rec("installed %d", installed);
        guest_stubs();
        if (act) act(ns);
        calls[k] = hx::calls;
        img[k] = masked();
    }
    g_total++;
    // HX_SHOW=<part of a scenario name>: print the old build's record of the matching scenarios
    if (const char *show = std::getenv("HX_SHOW"); show && std::strstr(name, show)) {
        std::printf("---- %s (old)\n", name);
        for (const auto &c : calls[0]) std::printf("  %s\n", c.c_str());
    }
    if (hx::same(name, calls[0], calls[1], &img[0], &img[1])) {
        g_pass++;
        std::printf("PASS %s\n", name);
    } else {
        g_failed.push_back(name);
    }
}

void cb(uint64_t off, uint32_t ev = 0, uint32_t row = 0, uint32_t n = 0) {
    put<uint32_t>(ARG, ev);
    put<uint32_t>(ARG + 4, row);
    put<uint32_t>(ARG + 8, n);
    reinterpret_cast<Cb>(mem + off)(mem + ARG, mem + ARG + 0x40, mem + ARG + 0x80);
}
void cb_null(uint64_t off) { reinterpret_cast<Cb>(mem + off)(nullptr, nullptr, nullptr); }

std::string hex(const uint8_t *p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; i++) {
        std::snprintf(b, sizeof b, "%02x", p[i]);
        s += b;
    }
    return s;
}

// install details outside the image: the tag search stub, the TextDb trampoline, the retargeted host functions
void dump_install(const Ns &) {
    if (mem[0x4bf44c] == 0xe9) {
        const uint8_t *stub = mem + 0x4bf451 + get<int32_t>(0x4bf44d);
        rec("tag stub %s", hex(stub, 45).c_str());
        rec("tag stub loop=%s notfound=%s", hx::where(reinterpret_cast<void *>(*reinterpret_cast<const uint64_t *>(stub + 23))).c_str(),
            hx::where(reinterpret_cast<void *>(*reinterpret_cast<const uint64_t *>(stub + 37))).c_str());
    }
    if (mem[0xcddc00] == 0xe9) {
        const uint8_t *js = mem + 0xcddc05 + get<int32_t>(0xcddc01);
        const uint8_t *block = js - 0x80;
        rec("TextDb trampoline %s back=%s, tramp handed over: %d", hex(block, 18).c_str(),
            hx::where(reinterpret_cast<void *>(*reinterpret_cast<const uint64_t *>(block + 18))).c_str(),
            st.tramp == block ? 1 : 0);
    }
    rec("retarget hosts: len %d copy %d", hx::retarget_host(0x66c75f) ? 1 : 0, hx::retarget_host(0x66c76d) ? 1 : 0);
}

void inst(const char *name, const std::function<void()> &setup) {
    run((std::string("install: ") + name).c_str(), setup, dump_install);
}

// ---- B1 -------------------------------------------------------------------------------------------------------
struct LevelRec {
    uint32_t id, pkg;
    const char *name;   // NULL: name pointer 0
};
void b1_setup(const std::vector<LevelRec> &levels, int16_t index = 1, int32_t games = 3, uint32_t level = 0x777) {
    g_tables[0x3b9a067d] = GAMES;
    g_tables[0x0dec97a4] = LEVELS;
    put<int32_t>(GAMES, games);
    put<uint64_t>(GAMES + 8, addr(GAMES_REC));
    for (int i = 0; i < 4; i++) put<uint32_t>(GAMES_REC + 0x90 * i + 4, i == 1 ? level : 0x100 + i);
    put<int32_t>(LEVELS, int32_t(levels.size()));
    put<uint64_t>(LEVELS + 8, addr(LEVELS_REC));
    uint64_t name_at = NAMES;
    for (size_t i = 0; i < levels.size(); i++) {
        const uint64_t r = LEVELS_REC + 0x38 * i;
        put<uint32_t>(r, levels[i].id);
        put<uint32_t>(r + 0x20, levels[i].pkg);
        if (levels[i].name) {
            std::strcpy(reinterpret_cast<char *>(mem + name_at), levels[i].name);
            put<uint64_t>(r + 0x18, addr(name_at));
            name_at += 0x40;
        }
    }
    put<int16_t>(NGM + 0x1ca0, index);
}
// a loaded package named `name`, with (table) or without a spawner table
void pkg(int k, const char *name, bool table) {
    const uint64_t p = PKGS + 0x1000 * uint64_t(k);
    st.packages[sid(name)] = p;
    put<uint64_t>(p + 0x9a0, table ? 0x1234567800ull + k : 0);
}
void b1_call() {
    auto f = reinterpret_cast<const uint8_t *(U2MP_GUEST *)(void *)>(mem + 0x644130);
    rec("level_record_package -> %s", hx::where(f(mem + 0x1234)).c_str());
}

// ---- B3 -------------------------------------------------------------------------------------------------------
void b3_run(uint64_t data, int32_t count) {
    put<uint64_t>(TBL, data);
    put<int32_t>(TBL + 8, count);
    put<uint32_t>(TBL + 12, 0xfeedface);
    // caller: rdx = table; preset rax/rcx/r8; call image+0x4bf44c; store rax, rcx, rdx, r8 to [rsi]
    unsigned char *c = hx::exec_alloc(96);
    size_t i = 0;
    auto b = [&](std::initializer_list<int> v) {
        for (int x : v) c[i++] = uint8_t(x);
    };
    auto q = [&](uint64_t v) {
        std::memcpy(c + i, &v, 8);
        i += 8;
    };
    b({0x48, 0x89, 0xfa});   // mov rdx, rdi
    b({0x49, 0x89, 0xf1});   // mov r9, rsi
    b({0x49, 0xb8});
    q(0x1111111111111111ull);   // mov r8, imm
    b({0x48, 0xb9});
    q(0xdeadbeefdeadbeefull);   // mov rcx, imm
    b({0x48, 0xb8});
    q(0x2222222222222222ull);   // mov rax, imm
    b({0x49, 0xbb});
    q(addr(0x4bf44c));          // mov r11, image+0x4bf44c
    b({0x41, 0xff, 0xd3});      // call r11
    b({0x49, 0x89, 0x01});      // mov [r9], rax
    b({0x49, 0x89, 0x49, 0x08});   // mov [r9+8], rcx
    b({0x49, 0x89, 0x51, 0x10});   // mov [r9+16], rdx
    b({0x4d, 0x89, 0x41, 0x18});   // mov [r9+24], r8
    b({0xc3});
    uint64_t out[4] = {};
    reinterpret_cast<void(U2MP_GUEST *)(void *, uint64_t *)>(c)(mem + TBL, out);
    rec("tag search {%llx, %d}: %s rax=%llx rcx=%llx rdx=%s", (unsigned long long)data, count,
        out[3] == 1 ? "loop" : out[3] == 2 ? "not found" : "?", (unsigned long long)out[0], (unsigned long long)out[1],
        out[2] == addr(TBL) ? "table" : std::to_string(out[2]).c_str());
}

// ---- B6 -------------------------------------------------------------------------------------------------------
uint32_t fnv(uint32_t id) {
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 4; i++) h = (h ^ ((id >> (8 * i)) & 0xff)) * 0x01000193u;
    return h;
}
constexpr uint32_t kNpcId = 0x4242;
void b6_setup() {
    for (int i = 0; i < 10; i++) put<uint32_t>(NGM + 0x10 + i * 0x2c0 + 0x120, 100 + i);
    const uint64_t t9 = NGM + 0x10 + 9 * 0x2c0;
    put<uint64_t>(t9 + 0x10, addr(SLOT_A));
    put<uint32_t>(t9 + 0x18, 0x51);
    put<uint64_t>(SLOT_A, addr(PROC_A));
    put<uint32_t>(PROC_A + 0x44, 0x51);
    const uint32_t count = 4;
    put<uint32_t>(NGM + 0x1bdc, count);
    put<uint64_t>(NGM + 0x1ba0, addr(BUCKETS));
    for (uint32_t i = 0; i < count; i++) put<uint64_t>(BUCKETS + 8 * i, addr(END));
    put<uint64_t>(BUCKETS + 8 * count, addr(END));
    put<uint32_t>(END + 0x10, 0xeeee);
    put<uint64_t>(BUCKETS + 8 * (fnv(kNpcId) % count), addr(NODE1));
    put<uint32_t>(NODE1 + 0x10, 0x9999);
    put<uint64_t>(NODE1, addr(NODE2));
    put<uint32_t>(NODE2 + 0x10, kNpcId);
    put<uint64_t>(NODE2 + 0x18, addr(SLOT_N));
    put<uint32_t>(NODE2 + 0x20, 0x77);
    put<uint64_t>(SLOT_N, addr(NPC));
    put<uint64_t>(NPC, addr(VT));
    put<uint32_t>(NPC + 0x44, 0x77);
    mem[NPC + 0x262a] = 0x81;
    put<uint32_t>(EV + 0x0c, 109);
    put<uint32_t>(EV + 0x10, kNpcId);
    put<int32_t>(EV + 0x14, -25);
    put<float>(EV + 0x18, 1.5f);
    put<uint32_t>(EV + 0x1c, 2);
    g_dead = {0, 1};
}
void b6_call() {
    reinterpret_cast<void(U2MP_GUEST *)(void *, void *)>(mem + 0x61fa70)(mem + EV, mem + 0x4321);
    rec("npc flags %02x", mem[NPC + 0x262a]);
}
void b6(const char *name, const std::function<void()> &tweak) {
    run((std::string("B6 ") + name).c_str(),
        [&] {
            b6_setup();
            if (tweak) tweak();
        },
        [](const Ns &) { b6_call(); });
}

// ---- players / friends ------------------------------------------------------------------------------------
void members() {
    st.members = {{0, 11, "Alice"}, {1, 22, "Bob"}, {3, 33, "Carol"}, {9, 0, "Zero"}};
    st.local_id = 11;
}
hx::fk::Friend F(const char *row, const char *name, int kind, bool online, bool joinable) {
    return {row, name, kind, online, joinable, std::string("profile of ") + name};
}
void friends5() {
    st.friends = {F("Ann - online", "ann", 0, true, true), F("Ben - offline", "ben", 0, false, false),
                  F("Cid - request in", "cid", 1, true, false), F("Dee - request out", "dee", 2, true, true),
                  F("Eve - odd", "eve", 7, true, false)};
}

std::string repeat(char c, size_t n) { return std::string(n, c); }

// cond 48: the text pointer written at q+0x40 is a host pointer; compared by its text, and the count buffer by "same
// buffer as the previous count of this build" (one shared buffer)
const void *g_count_buf[2] = {};
void q_call_ns(const Ns &ns, uint32_t op, uint32_t column) {
    const int w = &ns == &kNew ? 1 : 0;
    put<uint32_t>(Q, op);
    put<uint32_t>(Q + 0x54, column);
    put<uint64_t>(Q + 0x40, 0x5e5e5e5e5e5e5e5eull);
    reinterpret_cast<void(U2MP_GUEST *)(void *)>(mem + 0x659b50)(mem + Q);
    const uint64_t v = get<uint64_t>(Q + 0x40);
    if (v == 0x5e5e5e5e5e5e5e5eull) {
        rec("query op=%u col=%u: no write", op, column);
    } else {
        const char *s = reinterpret_cast<const char *>(uintptr_t(v));
        rec("query op=%u col=%u: [%s]", op, column, s);
        if (op == 2) {
            rec("query: count buffer same as before: %d", g_count_buf[w] == nullptr ? -1 : g_count_buf[w] == s ? 1 : 0);
            g_count_buf[w] = s;
        }
    }
    put<uint64_t>(Q + 0x40, 0);
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    hx::init();


    // ======== state as the process starts (must run first) ========
    run("initial: Kick From Party, no member selected", nullptr, [](const Ns &) { cb(0x5ec4a0); });
    run("initial: Promote, no member selected", [] { st.promote_result = true; }, [](const Ns &) { cb(0x5ec4c0); });
    run("initial: Mute, no member selected", members, [](const Ns &) { cb(0x5ec4b0); });
    run("initial: Join Party online, no friend selected", [] { st.active = true; }, [](const Ns &) { cb(0x5ec000); });
    run("initial: Invite To Party online, no friend selected", [] { st.active = true; }, [](const Ns &) { cb_null(0x5ebee0); });
    run("initial: clan_tag_frame not editing (no poll)", [] { st.poll_results = {1}; },
        [](const Ns &ns) {
            ns.frame();
            cb(0x5ed8b0, 6);
        });

    // ======== install ========
    inst("clean image (29)", nullptr);
    inst("0x644130 differs", [] { mem[0x644130 + 13] = 0; });
    inst("0x644130 replace fails", [] { hx::fail_offsets = {0x644130}; });
    inst("0x640598 differs", [] { mem[0x640598 + 8] = 0; });
    inst("0x4bf44c differs", [] { mem[0x4bf44c + 9] = 0; });
    inst("0x66c75d differs", [] { mem[0x66c75d] = 0x90; });
    inst("0x66c764 differs", [] { mem[0x66c764 + 3] = 0x02; });
    inst("0x66c76d E8 missing", [] { mem[0x66c76d] = 0xe9; });
    inst("retarget 0x66c75f fails (wrong E8 target)", [] { hx::fail_offsets = {0x66c75f}; });
    inst("retarget 0x66c76d fails only", [] { hx::fail_offsets = {0x66c76d}; });
    inst("0xcddc00 differs", [] { mem[0xcddc00 + 11] = 0; });
    inst("0xcddc00 not executable", [] { hx::noexec = {{0xcddc00, 1}}; });
    inst("0x60eb90 differs", [] { mem[0x60eb90 + 16] = 0; });
    inst("attack type [0] non-zero", [] { put<uint32_t>(0xdef290, 5); });
    inst("attack type [2] wrong", [] { put<uint32_t>(0xdef298, 0xdf95da89); });
    inst("image size < 0xdef290+180", [] { u2mp::image.size = u2mp_old::image.size = 0xdef290 + 179; });
    inst("image size = 0xdef290+180", [] { u2mp::image.size = u2mp_old::image.size = 0xdef290 + 180; });
    inst("0x61fa70 differs", [] { mem[0x61fa70 + 10] = 0; });
    inst("0x61fa70 replace fails", [] { hx::fail_offsets = {0x61fa70}; });
    inst("0x61fa70 differs and kill-info bad", [] {
        mem[0x61fa70 + 10] = 0;
        mem[0x60eb90] = 0;
    });
    inst("0x5ebee0 differs", [] { mem[0x5ebee0 + 19] = 1; });
    inst("0x5ebee0 replace fails", [] { hx::fail_offsets = {0x5ebee0}; });
    inst("0x659b50 differs", [] { mem[0x659b50 + 13] = 3; });
    inst("0x659b50 replace fails", [] { hx::fail_offsets = {0x659b50}; });
    inst("0x5ebdf0 differs", [] { mem[0x5ebdf0 + 11] = 0; });
    inst("0x5ebdf0 replace fails", [] { hx::fail_offsets = {0x5ebdf0}; });
    inst("0x5cc480 differs (popup)", [] { mem[0x5cc480 + 9] = 0; });
    inst("0x5cc480 differs and 0x5ec490 not a stub", [] {
        mem[0x5cc480 + 9] = 0;
        mem[0x5ec490] = 0x55;
    });
    for (uint64_t s : kRetStubs) {
        char n[96];
        std::snprintf(n, sizeof n, "ret stub 0x%llx differs", (unsigned long long)s);
        inst(n, [s] { mem[s + 4] = 0x90; });
        std::snprintf(n, sizeof n, "ret stub 0x%llx replace fails", (unsigned long long)s);
        inst(n, [s] { hx::fail_offsets = {s}; });
    }
    inst("near_stub exhausted for 0x4bf44c", [] { hx::exec_left = 0; });
    inst("near_stub exhausted for TextDb::Find", [] { hx::exec_left = 1; });
    inst("near_stub exhausted, 0x4bf44c differs (TextDb gets the block)", [] {
        hx::exec_left = 1;
        mem[0x4bf44c] = 0;
    });
    inst("everything differs", [] {
        std::memset(mem + 0x5e0000, 0, 0x100000);
        std::memset(mem + 0x4bf44c, 0, 16);
        std::memset(mem + 0x66c75d, 0, 32);
        std::memset(mem + 0xcddc00, 0, 16);
    });

    // ======== B1 level record package ========
    const std::vector<LevelRec> lv = {{0x700, 0xaaa, "other-pak"}, {0x777, 0xbbb, "yemen-pak"}};
    run("B1 games table NULL", [&] { b1_setup(lv); g_tables[0x3b9a067d] = 0; }, [](const Ns &) { b1_call(); });
    run("B1 levels table NULL", [&] { b1_setup(lv); g_tables[0x0dec97a4] = 0; }, [](const Ns &) { b1_call(); });
    run("B1 games records NULL", [&] { b1_setup(lv); put<uint64_t>(GAMES + 8, 0); }, [](const Ns &) { b1_call(); });
    run("B1 levels records NULL", [&] { b1_setup(lv); put<uint64_t>(LEVELS + 8, 0); }, [](const Ns &) { b1_call(); });
    run("B1 index -1", [&] { b1_setup(lv, -1); }, [](const Ns &) { b1_call(); });
    run("B1 index = count", [&] { b1_setup(lv, 3, 3); }, [](const Ns &) { b1_call(); });
    run("B1 index = count-1 (last)", [&] { b1_setup(lv, 2, 3, 0x777); put<uint32_t>(GAMES_REC + 0x90 * 2 + 4, 0x777); pkg(0, "yemen-ingame10", true); },
        [](const Ns &) { b1_call(); });
    run("B1 no level record", [&] { b1_setup(lv, 1, 3, 0x123); }, [](const Ns &) { b1_call(); });
    run("B1 levels count negative", [&] { b1_setup(lv); put<int32_t>(LEVELS, -2); }, [](const Ns &) { b1_call(); });
    run("B1 record package loaded", [&] { b1_setup(lv); st.packages[0xbbb] = PKGS + 0x8000; }, [](const Ns &) { b1_call(); });
    run("B1 name NULL", [&] { b1_setup({{0x777, 0xbbb, nullptr}}); }, [](const Ns &) { b1_call(); });
    run("B1 name abc", [&] { b1_setup({{0x777, 0xbbb, "abc"}}); }, [](const Ns &) { b1_call(); });
    run("B1 name -pak (4 bytes)", [&] { b1_setup({{0x777, 0xbbb, "-pak"}}); }, [](const Ns &) { b1_call(); });
    run("B1 name without -pak", [&] { b1_setup({{0x777, 0xbbb, "yemen-pac"}}); }, [](const Ns &) { b1_call(); });
    run("B1 x-pak, none loaded", [&] { b1_setup({{0x777, 0xbbb, "x-pak"}}); }, [](const Ns &) { b1_call(); });
    run("B1 map-pak: -ingame10 with table", [&] { b1_setup({{0x777, 0xbbb, "map-pak"}}); pkg(0, "map-ingame10", true); pkg(1, "map-ingame", true); },
        [](const Ns &) { b1_call(); });
    run("B1 map-pak: only -ingame with table", [&] { b1_setup({{0x777, 0xbbb, "map2-pak"}}); pkg(0, "map2-ingame10", false); pkg(1, "map2-ingame", true); pkg(2, "map2", true); },
        [](const Ns &) { b1_call(); });
    run("B1 map-pak: only map loaded without table", [&] { b1_setup({{0x777, 0xbbb, "map3-pak"}}); pkg(2, "map3", false); },
        [](const Ns &) { b1_call(); });
    run("B1 map-pak: ingame and map loaded without table (first fallback)", [&] { b1_setup({{0x777, 0xbbb, "map4-pak"}}); pkg(1, "map4-ingame", false); pkg(2, "map4", false); },
        [](const Ns &) { b1_call(); });
    run("B1 map-pak: none loaded", [&] { b1_setup({{0x777, 0xbbb, "map5-pak"}}); }, [](const Ns &) { b1_call(); });
    run("B1 two records with the level id (first counts)", [&] {
        b1_setup({{0x777, 0xccc, "first"}, {0x777, 0xbbb, "second-pak"}});
        st.packages[0xbbb] = PKGS + 0x8000;
        pkg(0, "second-ingame10", true);
    }, [](const Ns &) { b1_call(); });
    run("B1 same name twice, other, first again", [&] { b1_setup({{0x777, 0xbbb, "rep-pak"}}); pkg(2, "rep", false); pkg(3, "alt-ingame", true); },
        [](const Ns &) {
            b1_call();
            b1_call();
            std::strcpy(reinterpret_cast<char *>(mem + NAMES), "alt-pak");
            b1_call();
            b1_call();
            std::strcpy(reinterpret_cast<char *>(mem + NAMES), "rep-pak");
            b1_call();
        });
    run("B1 name not logged then NULL-returning path keeps stored name", [&] { b1_setup({{0x777, 0xbbb, "rep-pak"}}); },
        [](const Ns &) {
            b1_call();
            put<int16_t>(NGM + 0x1ca0, -1);
            b1_call();
            put<int16_t>(NGM + 0x1ca0, 1);
            b1_call();
        });

    // ======== B2 / B3 / B5 ========
    run("B2 tracker values bytes", nullptr, [](const Ns &) { rec("640598: %s", hex(mem + 0x640598, 9).c_str()); });
    run("B3 tag search {NULL,5} {p,0} {p,-1} {p,3} {p,INT_MIN} {NULL,0}", nullptr, [](const Ns &) {
        b3_run(0, 5);
        b3_run(0x123456789abcull, 0);
        b3_run(0x123456789abcull, -1);
        b3_run(0x123456789abcull, 3);
        b3_run(0x123456789abcull, INT32_MIN);
        b3_run(0, 0);
        b3_run(0x8000000000000000ull, 0x7fffffff);
    });
    run("B5 TextDb::Find hook and trampoline", nullptr, [](const Ns &) {
        using Find = uint64_t(U2MP_GUEST *)(void *, uint32_t);
        const uint64_t r = reinterpret_cast<Find>(mem + 0xcddc00)(mem + 0x2000, 0xc6467f2d);
        rec("hooked Find -> %s", reinterpret_cast<const char *>(uintptr_t(r)));
        const uint64_t o = st.tramp ? reinterpret_cast<Find>(st.tramp)(mem + 0x2000, 0x12345678) : 0;
        rec("trampoline -> 0x%x", unsigned(o));
        rec("0xcddc00: %02x .. %s", mem[0xcddc00], hex(mem + 0xcddc05, 7).c_str());
    });

    // ======== B4 NetStatGame host name ========
    auto b4 = [](const char *name, std::function<void()> setup, std::function<void()> body) {
        run(name, [setup] { members(); if (setup) setup(); std::memset(mem + DST, 0xaa, 0x60); }, [body](const Ns &) { body(); });
    };
    using Len = uint64_t(U2MP_GUEST *)(const char *);
    using Cpy = void *(U2MP_GUEST *)(void *, const void *, uint64_t);
    auto len = [] { return reinterpret_cast<Len>(hx::retarget_host(0x66c75f)); };
    auto cpy = [] { return reinterpret_cast<Cpy>(hx::retarget_host(0x66c76d)); };
    b4("B4 non-NULL pass-through", nullptr, [len, cpy] {
        std::strcpy(reinterpret_cast<char *>(mem + SRC), "hello world");
        rec("len -> %llu", (unsigned long long)len()(reinterpret_cast<char *>(mem + SRC)));
        rec("copy -> %s", hx::where(cpy()(mem + DST, mem + SRC, 5)).c_str());
        rec("dst %s", hex(mem + DST, 0x50).c_str());
        rec("len empty -> %llu", (unsigned long long)len()(reinterpret_cast<char *>(mem + SRC + 0x40)));
        rec("copy n=0 -> %s", hx::where(cpy()(mem + DST + 0x20, mem + SRC, 0)).c_str());
    });
    auto b4null = [len, cpy] {
        rec("len -> %llu", (unsigned long long)len()(nullptr));
        rec("copy -> %s", hx::where(cpy()(mem + DST, nullptr, 3)).c_str());
        rec("dst %s", hex(mem + DST, 0x50).c_str());
    };
    b4("B4 NULL, host found", [] { put<uint32_t>(MATCH + 0xb04, 22); }, b4null);
    b4("B4 NULL, host not found (local name)", [] { put<uint32_t>(MATCH + 0xb04, 99); }, b4null);
    b4("B4 NULL, host name 63 bytes", [] { put<uint32_t>(MATCH + 0xb04, 44); st.members.push_back({20, 44, repeat('a', 63)}); }, b4null);
    b4("B4 NULL, host name 64 bytes", [] { put<uint32_t>(MATCH + 0xb04, 44); st.members.push_back({20, 44, repeat('b', 64)}); }, b4null);
    b4("B4 NULL, host name 100 bytes", [] { put<uint32_t>(MATCH + 0xb04, 44); st.members.push_back({20, 44, repeat('c', 100)}); }, b4null);
    b4("B4 NULL, local name 70 bytes", [] { put<uint32_t>(MATCH + 0xb04, 99); st.local_name = repeat('d', 70); }, b4null);
    b4("B4 NULL, empty host name", [] { put<uint32_t>(MATCH + 0xb04, 44); st.members.push_back({20, 44, ""}); }, b4null);

    // ======== B6 NpcDealDamage ========
    b6("full kill, tracker 9, host via +0x10 == 0", nullptr);
    b6("no player tracker", [] { put<uint32_t>(EV + 0x0c, 55); });
    b6("net id compared in full 32 bits", [] { put<uint32_t>(EV + 0x0c, 109 + 0x10000); });
    b6("bucket count 0", [] { put<uint32_t>(NGM + 0x1bdc, 0); });
    b6("bucket array 0", [] { put<uint64_t>(NGM + 0x1ba0, 0); });
    b6("npc id not in chain (NULL end)", [] { put<uint32_t>(EV + 0x10, kNpcId); put<uint32_t>(NODE2 + 0x10, 0x4343); });
    b6("npc found in first node", [] {
        put<uint64_t>(BUCKETS + 8 * (fnv(kNpcId) % 4), addr(NODE2));
    });
    b6("chain ends at the end node holding the id", [] {
        put<uint64_t>(NODE1, addr(END));
        put<uint32_t>(END + 0x10, kNpcId);
        put<uint64_t>(END + 0x18, addr(SLOT_N));
        put<uint32_t>(END + 0x20, 0x77);
    });
    b6("walk passes through the end node", [] {
        put<uint64_t>(NODE1, addr(END));
        put<uint64_t>(END, addr(NODE2));
    });
    b6("attacker slot 0", [] { put<uint64_t>(NGM + 0x10 + 9 * 0x2c0 + 0x10, 0); });
    b6("attacker id 0", [] { put<uint32_t>(NGM + 0x10 + 9 * 0x2c0 + 0x18, 0); put<uint32_t>(PROC_A + 0x44, 0); });
    b6("attacker slot holds NULL", [] { put<uint64_t>(SLOT_A, 0); });
    b6("attacker unique id mismatch", [] { put<uint32_t>(PROC_A + 0x44, 0x52); });
    b6("npc slot 0", [] { put<uint64_t>(NODE2 + 0x18, 0); });
    b6("npc id 0", [] { put<uint32_t>(NODE2 + 0x20, 0); put<uint32_t>(NPC + 0x44, 0); });
    b6("npc slot holds NULL", [] { put<uint64_t>(SLOT_N, 0); });
    b6("npc unique id mismatch", [] { put<uint32_t>(NPC + 0x44, 0x78); });
    b6("npc already dead", [] { g_dead = {1}; });
    b6("npc already dead (IsDead 0x80)", [] { g_dead = {0x80}; });
    b6("damage not lethal", [] { g_dead = {0, 0}; });
    b6("lethal, not host", [] { put<uint64_t>(MATCH + 0x10, 1); put<uint32_t>(MATCH + 0xb00, 3); put<uint32_t>(MATCH + 0xb04, 4); });
    b6("lethal, host via equal ids", [] { put<uint64_t>(MATCH + 0x10, 1); put<uint32_t>(MATCH + 0xb00, 4); put<uint32_t>(MATCH + 0xb04, 4); });
    b6("attack index 0", [] { put<uint32_t>(EV + 0x1c, 0); });
    b6("attack index 44", [] { put<uint32_t>(EV + 0x1c, 44); });
    b6("attack index 45", [] { put<uint32_t>(EV + 0x1c, 45); });
    b6("attack index 0xffffffff", [] { put<uint32_t>(EV + 0x1c, 0xffffffffu); });
    b6("time -0.0", [] { put<float>(EV + 0x18, -0.0f); });
    b6("time -1.5", [] { put<float>(EV + 0x18, -1.5f); });
    b6("time 0.0049", [] { put<float>(EV + 0x18, 0.0049f); });
    b6("time 1e10", [] { put<float>(EV + 0x18, 1e10f); });
    b6("time -1e10", [] { put<float>(EV + 0x18, -1e10f); });
    b6("time 2^31/300 edge", [] { put<float>(EV + 0x18, 7158278.0f); });
    b6("time NaN", [] { put<float>(EV + 0x18, std::numeric_limits<float>::quiet_NaN()); });
    b6("time +inf", [] { put<float>(EV + 0x18, std::numeric_limits<float>::infinity()); });
    b6("npc flags byte 0x02 already set", [] { mem[NPC + 0x262a] = 0x02; });
    b6("npc flags byte 0xfd", [] { mem[NPC + 0x262a] = 0xfd; });
    b6("damage INT_MIN", [] { put<int32_t>(EV + 0x14, INT32_MIN); });

    // ======== B7 KickGriefer ========
    auto b7 = [](const char *n, std::function<void()> s) {
        run(n, [s] { put<uint32_t>(EV + 0x0c, 0x80000007u); if (s) s(); },
            [](const Ns &) { reinterpret_cast<void(U2MP_GUEST *)(void *, void *)>(mem + 0x61b900)(mem + EV, nullptr); });
    };
    b7("B7 not host", [] { put<uint64_t>(MATCH + 0x10, 1); put<uint32_t>(MATCH + 0xb04, 1); });
    b7("B7 host, kick true", [] { st.kick_result = true; });
    b7("B7 host, kick false", [] { st.kick_result = false; });
    b7("B7 host via equal ids, kick false", [] { put<uint64_t>(MATCH + 0x10, 1); st.kick_result = false; });

    // ======== B8 / B9 / B10 ========
    run("B8 party row 15 -> 5 not found", members, [](const Ns &) { cb(0x5ec490, 0, 0, 15); });
    run("B8 party row 13 -> 3 (other member, client)", members, [](const Ns &) { cb(0x5ec490, 0, 0, 13); });
    run("B8 party row 0xffffffff % 10", members, [](const Ns &) { cb(0x5ec490, 0, 0, 0xffffffffu); });
    run("B8 own row", members, [](const Ns &) { cb(0x5ec490, 0, 0, 0); });
    run("B8 match row other member as host offline", [] { members(); st.is_host = true; }, [](const Ns &) { cb(0x5ec460, 0, 7, 1); });
    run("B8 match row other member as host online", [] { members(); st.is_host = true; st.active = true; }, [](const Ns &) { cb(0x5ec460, 0, 0, 3); });
    run("B8 match row client online", [] { members(); st.active = true; }, [](const Ns &) { cb(0x5ec460, 0, 0, 1); });
    run("B8 match row -1 (signed)", members, [](const Ns &) { cb(0x5ec460, 0, 0, 0xffffffffu); });
    run("B8 match row 13 (no % 10)", members, [](const Ns &) { cb(0x5ec460, 0, 0, 13); });
    run("B8 arg NULL (party, match)", members, [](const Ns &) { cb_null(0x5ec490); cb_null(0x5ec460); });
    run("B8 member with id 0 selected, then kick", [] { members(); st.local_id = 5; st.is_host = true; },
        [](const Ns &) { cb(0x5ec490, 0, 0, 9); cb(0x5ec4a0); });
    run("B9 LIST_FRIENDS offline: row from +4", members, [](const Ns &) { cb(0x5ebde0, 0, 1, 3); });
    run("B9 LIST_FRIENDS offline: arg NULL", members, [](const Ns &) { cb_null(0x5ebde0); });
    run("B9 LIST_FRIENDS online row 0 (join list)", [] { st.active = true; friends5(); }, [](const Ns &) { cb(0x5ebde0, 0, 0, 0); });
    run("B9 LIST_FRIENDS online arg NULL", [] { st.active = true; friends5(); }, [](const Ns &) { cb_null(0x5ebde0); });
    for (int r = 1; r <= 7; r++) {
        char n[64];
        std::snprintf(n, sizeof n, "B9 LIST_FRIENDS online row %d", r);
        run(n, [] { st.active = true; friends5(); }, [r](const Ns &) {
            cb(0x5ebde0, 0, uint32_t(r), 0);
            rec("cb count %u", get<uint32_t>(MENU + 0x18ec));
        });
    }
    run("B9 LIST_FRIENDS online row -1 (0xffffffff)", [] { st.active = true; friends5(); }, [](const Ns &) { cb(0x5ebde0, 0, 0xffffffffu, 0); });
    for (int c = 0; c < 4; c++) {
        char n[64];
        std::snprintf(n, sizeof n, "B9 kind 0 joinable=%d online=%d", c & 1, c >> 1);
        run(n, [c] { st.active = true; st.friends = {F("X - x", "x", 0, (c >> 1) != 0, (c & 1) != 0)}; },
            [](const Ns &) { cb(0x5ebde0, 0, 1, 0); });
    }
    for (int r = 1; r <= 5; r++) {
        char n[64];
        std::snprintf(n, sizeof n, "B9 table full (128 foreign), row %d", r);
        run(n, [] { st.active = true; friends5(); fill_cbs(128); }, [r](const Ns &) { cb(0x5ebde0, 0, uint32_t(r), 0); });
    }
    run("B9 table 127 entries (only ACCEPT fits), kind 1 and 0", [] { st.active = true; friends5(); fill_cbs(127); },
        [](const Ns &) {
            cb(0x5ebde0, 0, 3, 0);
            cb(0x5ebde0, 0, 1, 0);
            rec("cb count %u", get<uint32_t>(MENU + 0x18ec));
        });
    run("B9 table 126 entries (ACCEPT, REMOVE fit)", [] { st.active = true; friends5(); fill_cbs(126); },
        [](const Ns &) { cb(0x5ebde0, 0, 3, 0); cb(0x5ebde0, 0, 4, 0); });
    run("B9 our ids already present (overwritten in place)", [] {
        st.active = true;
        friends5();
        fill_cbs(10);
        put_cb(2, sid("U2MP_PC_CB_FRIEND_REMOVE"));
        put_cb(5, 0x3fe6f70e);
        put_cb(7, sid("U2MP_PC_CB_FRIEND_ACCEPT"));
    }, [](const Ns &) { cb(0x5ebde0, 0, 3, 0); rec("cb count %u", get<uint32_t>(MENU + 0x18ec)); });
    run("B9 our id at entry 5 with count 200 (found, no append)", [] {
        st.active = true; friends5(); fill_cbs(200);
        put_cb(5, sid("U2MP_PC_CB_FRIEND_ACCEPT"));
        put_cb(6, sid("U2MP_PC_CB_FRIEND_REMOVE"));
        put_cb(130, 0x3fe6f70e);
    }, [](const Ns &) { cb(0x5ebde0, 0, 1, 0); cb(0x5ebde0, 0, 3, 0); });
    run("B9 duplicate id in table (first one gets it)", [] {
        st.active = true; friends5(); fill_cbs(6);
        put_cb(1, 0x3fe6f70e);
        put_cb(4, 0x3fe6f70e);
    }, [](const Ns &) { cb(0x5ebde0, 0, 1, 0); });
    run("B10 friend callbacks: accept, remove, profile", [] { st.active = true; friends5(); },
        [](const Ns &) {
            cb(0x5ebde0, 0, 3, 0);
            call_registered(sid("U2MP_PC_CB_FRIEND_ACCEPT"));
            call_registered(sid("U2MP_PC_CB_FRIEND_REMOVE"));
            call_registered(0x3fe6f70e);
            cb(0x5ebde0, 0, 1, 0);
            call_registered(0x3fe6f70e);
            call_registered(sid("U2MP_PC_CB_FRIEND_REMOVE"));
        });
    run("B10 out-of-range row keeps the selection", [] { st.active = true; friends5(); },
        [](const Ns &) {
            cb(0x5ebde0, 0, 2, 0);
            cb(0x5ebde0, 0, 50, 0);
            call_registered(0x3fe6f70e);
            cb(0x5ec000);
            cb(0x5ebee0);
        });

    // ======== B11 / B12 ========
    auto recent50 = [] {
        st.recent.clear();
        for (int i = 0; i < 50; i++) st.recent.push_back("met" + std::to_string(i));
    };
    run("B11 empty list", nullptr, [](const Ns &) { cb(0x5ebdf0, 0, 0, 0); });
    run("B11 row 49 of 50 online", [recent50] { recent50(); st.active = true; }, [](const Ns &) { cb(0x5ebdf0, 0, 49, 0); });
    run("B11 row 50 of 50", [recent50] { recent50(); }, [](const Ns &) { cb(0x5ebdf0, 0, 50, 0); });
    run("B11 row -1", [recent50] { recent50(); }, [](const Ns &) { cb(0x5ebdf0, 0, 0xffffffffu, 0); });
    run("B11 row 0 offline, arg NULL", [recent50] { recent50(); }, [](const Ns &) { cb_null(0x5ebdf0); });
    run("B11 more than 50 recent (limit asked)", [] { for (int i = 0; i < 60; i++) st.recent.push_back("p" + std::to_string(i)); }, [](const Ns &) { cb(0x5ebdf0, 0, 55, 0); });
    run("B11 registration failure (table full)", [recent50] { recent50(); st.active = true; fill_cbs(128); }, [](const Ns &) { cb(0x5ebdf0, 0, 2, 0); });
    run("B11 registration: only MET_ADD fits", [recent50] { recent50(); st.active = true; fill_cbs(127); },
        [](const Ns &) { cb(0x5ebdf0, 0, 2, 0); rec("cb count %u", get<uint32_t>(MENU + 0x18ec)); });
    run("B12 add true / false, profile non-empty / empty", [recent50] { recent50(); st.active = true; st.summary = "summary text"; },
        [](const Ns &) {
            cb(0x5ebdf0, 0, 7, 0);
            call_registered(sid("U2MP_PC_CB_MET_ADD"));
            st.friend_add_result = false;
            call_registered(sid("U2MP_PC_CB_MET_ADD"));
            call_registered(sid("U2MP_PC_CB_MET_PROFILE"));
            st.summary.clear();
            call_registered(sid("U2MP_PC_CB_MET_PROFILE"));
        });

    // ======== B13 .. B17 ========
    auto sel = [](uint32_t row) { cb(0x5ec460, 0, 0, row); };   // selects members[row]
    run("B13 kick true", [] { members(); st.is_host = true; }, [sel](const Ns &) { sel(1); cb(0x5ec4a0); });
    run("B13 kick false", [] { members(); st.is_host = true; st.kick_result = false; }, [sel](const Ns &) { sel(3); cb(0x5ec4a0); });
    run("B14 voice off", members, [sel](const Ns &) { sel(1); cb(0x5ec4b0); cb(0x5ec480); });
    run("B14 member gone", members, [sel](const Ns &) { sel(1); st.members.clear(); cb(0x5ec4b0); });
    run("B14 voice off overrides member gone", [] { members(); st.voice_enabled = false; }, [sel](const Ns &) { sel(1); st.members.clear(); cb(0x5ec480); });
    run("B14 toggle to muted / unmuted", members, [sel](const Ns &) {
        sel(3);
        cb(0x5ec4b0);
        st.toggle_result = false;
        cb(0x5ec480);
    });
    run("B15 promote true", [] { members(); st.promote_result = true; }, [sel](const Ns &) { sel(1); cb(0x5ec4c0); });
    run("B15 promote false", members, [sel](const Ns &) { sel(1); cb(0x5ec4c0); });
    run("B15 promote, id 0 selected", [] { members(); st.promote_result = true; }, [sel](const Ns &) { sel(9); cb(0x5ec4c0); cb(0x5ec4b0); });
    auto ishost = [](const Ns &) {
        rec("party IsHost -> %u", unsigned(reinterpret_cast<uint8_t(U2MP_GUEST *)(void *, void *, void *)>(mem + 0x5ecb70)(nullptr, nullptr, nullptr)));
    };
    run("B16 party IsHost: no room", nullptr, ishost);
    run("B16 party IsHost: room, equal ids", [] { put<uint64_t>(PARTY + 0x10, 7); put<uint32_t>(PARTY + 0xb00, 3); put<uint32_t>(PARTY + 0xb04, 3); }, ishost);
    run("B16 party IsHost: room, ids differ", [] { put<uint64_t>(PARTY + 0x10, 7); put<uint32_t>(PARTY + 0xb00, 3); put<uint32_t>(PARTY + 0xb04, 4); }, ishost);
    run("B16 party IsHost: match session not used", [] { put<uint64_t>(MATCH + 0x10, 7); put<uint32_t>(MATCH + 0xb04, 4); }, ishost);
    run("B17 Save", nullptr, [](const Ns &) { cb(0x5ebdd0); });

    // ======== B18 .. B21 ========
    run("B18 join list true", [] { st.open_join_result = true; put<int32_t>(NETINFO + 0x2c4, 1); }, [](const Ns &) { cb(0x5ebea0); });
    run("B18 join list false, mode 0", nullptr, [](const Ns &) { cb(0x5ebea0); });
    run("B18 join list false, mode 3", [] { put<int32_t>(NETINFO + 0x2c4, 3); }, [](const Ns &) { cb(0x5ebea0); });
    run("B18 join list false, mode -1", [] { put<int32_t>(NETINFO + 0x2c4, -1); }, [](const Ns &) { cb(0x5ebea0); });
    run("B19/B20 online off, friend selected", [] { st.active = true; friends5(); },
        [](const Ns &) { cb(0x5ebde0, 0, 2, 0); st.active = false; cb(0x5ec000); cb(0x5ebee0); });
    run("B19/B20 valid row", [] { st.active = true; friends5(); },
        [](const Ns &) { cb(0x5ebde0, 0, 5, 0); cb(0x5ec000); cb(0x5ebee0); });
    run("B21 cond 48 queries", nullptr, [](const Ns &ns) {
        q_call_ns(ns, 1, 0);
        st.invite_count = 3;
        q_call_ns(ns, 1, 0);
        st.invite_count = 0;
        q_call_ns(ns, 2, 1);
        st.invite_count = 12;
        q_call_ns(ns, 2, 1);
        st.invite_count = 7;
        q_call_ns(ns, 2, 1);
        q_call_ns(ns, 2, 0);
        q_call_ns(ns, 3, 1);
        q_call_ns(ns, 0, 1);
        st.invite_count = -2;
        q_call_ns(ns, 2, 1);
        st.invite_count = 2147483647;
        q_call_ns(ns, 2, 1);
        st.invite_count = 0;
        q_call_ns(ns, 1, 1);
    });

    // ======== B22 .. B27 and clantag.cpp ========
    run("B22 clan tag begin fails", [] { st.begin_result = 0; }, [](const Ns &ns) {
        cb(0x5ed8a0);
        rec("edit -> %d", ns.edit() ? 1 : 0);
    });
    run("B22/2.6 begin ok with tag AB, second edit while editing", [] { std::memcpy(mem + CLANTAG, "AB\0\0\0\0\0\0", 8); },
        [](const Ns &ns) {
            cb(0x5ed8a0);
            rec("edit -> %d", ns.edit() ? 1 : 0);
            cb(0x5ed8a0);
        });
    run("2.6 poll 0 (still editing), event 5 does not poll", [] { st.poll_results = {0, 0}; put<int32_t>(NET_SM, 2); }, [](const Ns &ns) {
        cb(0x5ed8b0, 6);
        cb(0x5ed8b0, 5);
        ns.frame();
        rec("edit -> %d", ns.edit() ? 1 : 0);
    });
    run("2.6 poll 2: unchanged, editing ends", [] { st.poll_results = {2}; put<int32_t>(NET_SM, 2); }, [](const Ns &ns) {
        cb(0x5ed8b0, 6);
        cb(0x5ed8b0, 6);
    });
    auto accept = [](const char *name, std::string text, const char *tag) {
        run(name, [text, tag] {
            st.poll_results = {1};
            st.poll_text = text;
            put<int32_t>(NET_SM, 1);
            std::memcpy(mem + CLANTAG, tag, 8);
        }, [](const Ns &ns) {
            rec("edit -> %d", ns.edit() ? 1 : 0);
            cb(0x5ed8b0, 6);
            rec("tag %s", hex(mem + CLANTAG, 8).c_str());
            ns.frame();
        });
    };
    accept("2.6 full 8-byte tag, accept ab-1c9z", "ab-1c9z", "ABCDEFGH");
    accept("2.6 accept a", "a", "XY\0\0\0\0\0\0");
    accept("2.6 accept !! (empty tag)", "!!", "XY\0\0\0\0\0\0");
    accept("2.6 accept UTF-8 e-acute 1", "\xc3\xa9" "1", "\0\0\0\0\0\0\0\0");
    accept("2.6 accept 63-byte text", repeat('!', 59) + "q9Zx", "Q\0\0\0\0\0\0\0");
    accept("2.6 accept bytes >= 0x80 and edges", "\x80\xff@[`{/:Zz09", "Q\0\0\0\0\0\0\0");
    accept("2.6 accept empty", "", "QQ\0\0\0\0\0\0");
    run("2.6 poll 3 and poll -1: unchanged", [] { st.poll_results = {3}; put<int32_t>(NET_SM, 2); }, [](const Ns &ns) {
        rec("edit -> %d", ns.edit() ? 1 : 0);
        ns.frame();
        st.poll_results = {-1};
        rec("edit -> %d", ns.edit() ? 1 : 0);
        ns.frame();
        ns.frame();
    });
    run("2.6 after accept a new edit opens again", [] { st.poll_results = {1}; st.poll_text = "zz"; }, [](const Ns &ns) {
        rec("edit -> %d", ns.edit() ? 1 : 0);
        ns.frame();
        rec("edit -> %d", ns.edit() ? 1 : 0);
        st.poll_results = {2};
        ns.frame();
    });
    run("B23 event 6 with NET_SM 1 (pop) / 2 / -5, event 5, NULL arg", nullptr, [](const Ns &) {
        put<int32_t>(NET_SM, 1);
        cb(0x5ed8b0, 6);
        put<int32_t>(NET_SM, 2);
        cb(0x5ed8b0, 6);
        put<int32_t>(NET_SM, -5);
        cb(0x5ed8b0, 6);
        cb(0x5ed8b0, 5);
        put<int32_t>(NET_SM, 0);
        cb_null(0x5ed8b0);
    });
    run("B24 store", nullptr, [](const Ns &) { cb(0x5ed8c0); });
    run("B25 matchmaking answers 0, 7, NULL, 0xffffffff", nullptr, [](const Ns &) {
        cb(0x64e900, 0, 0, 0);
        cb(0x64e900, 0, 0, 7);
        cb_null(0x64e900);
        cb(0x64e900, 0, 0, 0xffffffffu);
    });
    run("B26 twitter page event 6, 2, NULL", nullptr, [](const Ns &) {
        cb(0x6769d0, 6);
        cb(0x6769d0, 2);
        cb_null(0x6769d0);
    });
    run("B27 twitter button", nullptr, [](const Ns &) { cb(0x6769e0); });
    run("message box with NULL descriptor", [] { put<uint64_t>(MENU + 0x17d0, 0); }, [](const Ns &ns) {
        cb(0x5ed8c0);
        cb(0x6769e0);
        ns.notice("text");
    });
    run("message box words overwrite old content", [] { std::memset(mem + BOX, 0x77, 0x80); }, [](const Ns &) { cb(0x5ed8c0); });

    // ======== B28 / B29, show_notice ========
    run("B28 not linked", nullptr, [](const Ns &ns) { cb(0x5e4510); ns.pick(); });
    run("B28 no friend online", [] {
        st.linked = true;
        st.friends = {F("a", "a", 0, false, false), F("b", "b", 1, true, false), F("c", "c", 2, true, false)};
    }, [](const Ns &) { cb(0x5e4510); });
    run("B28 3 of 5 online, pick callbacks", [] {
        st.linked = true;
        st.friends = {F("a", "a", 0, true, false), F("b", "b", 0, false, false), F("c", "c", 0, true, false),
                      F("d", "d", 0, false, true), F("e", "e", 0, true, false)};
    }, [](const Ns &) {
        cb(0x5e4510);
        for (int i = 0; i < 8; i++) call_registered(u2mp::sid(("U2MP_PC_CB_PICK" + std::to_string(i)).c_str()));
    });
    run("B28 10 online friends (only 8), request row skipped", [] {
        st.linked = true;
        st.friends.push_back(F("r", "req", 1, true, false));
        for (int i = 0; i < 10; i++) st.friends.push_back(F("f", ("friend" + std::to_string(i)).c_str(), 0, true, false));
    }, [](const Ns &ns) {
        ns.pick();
        for (int i = 0; i < 8; i++) call_registered(u2mp::sid(("U2MP_PC_CB_PICK" + std::to_string(i)).c_str()));
    });
    run("B28 registration failing at slot 5", [] {
        st.linked = true;
        for (int i = 0; i < 9; i++) st.friends.push_back(F("f", ("g" + std::to_string(i)).c_str(), 0, true, false));
        fill_cbs(123);
    }, [](const Ns &) {
        cb(0x5e4510);
        rec("cb count %u", get<uint32_t>(MENU + 0x18ec));
        call_registered(u2mp::sid("U2MP_PC_CB_PICK4"));
    });
    run("B28 all registrations fail (empty popup shown)", [] {
        st.linked = true;
        st.friends = {F("a", "a", 0, true, false)};
        fill_cbs(128);
    }, [](const Ns &) { cb(0x5e4510); });
    run("B28 pick beyond the list, list kept when not linked", [] {
        st.linked = true;
        for (int i = 0; i < 8; i++) st.friends.push_back(F("f", ("h" + std::to_string(i)).c_str(), 0, true, false));
    }, [](const Ns &ns) {
        ns.pick();
        st.friends.resize(2);
        ns.pick();   // list now 2, PICK5 still registered
        call_registered(u2mp::sid("U2MP_PC_CB_PICK5"));
        call_registered(u2mp::sid("U2MP_PC_CB_PICK1"));
        st.linked = false;
        cb(0x5e4510);
        call_registered(u2mp::sid("U2MP_PC_CB_PICK0"));
        st.linked = true;
        st.friends.clear();
        cb(0x5e4510);   // none online: list cleared
        call_registered(u2mp::sid("U2MP_PC_CB_PICK0"));
    });
    run("B28 friend_count shrinking during the walk", [] {
        st.linked = true;
        st.friends = {F("a", "a", 0, true, false), F("b", "b", 0, true, false)};
    }, [](const Ns &) { cb(0x5e4510); });
    run("B29 not linked", [] { put<int32_t>(NETINFO + 0x18, 1); }, [](const Ns &) { cb(0x5e4520); });
    run("B29 0 files", [] { st.linked = true; put<int32_t>(NETINFO + 0x18, 1); }, [](const Ns &) { cb(0x5e4520); rec("flag %d", get<int32_t>(NETINFO + 0x18)); });
    run("B29 2 files", [] { st.linked = true; st.cinema_received = 2; put<int32_t>(NETINFO + 0x18, 5); }, [](const Ns &) { cb(0x5e4520); });
    run("B29 -3 files", [] { st.linked = true; st.cinema_received = -3; put<int32_t>(NETINFO + 0x18, 5); }, [](const Ns &) { cb(0x5e4520); });
    run("show_notice NULL, empty, text", nullptr, [](const Ns &ns) {
        ns.notice(nullptr);
        ns.notice("");
        ns.notice("Hello there");
    });

    std::printf("\n%d scenarios, %d passed, %d failed\n", g_total, g_pass, g_total - g_pass);
    for (const auto &f : g_failed) std::printf("  failed: %s\n", f.c_str());
    return g_total == g_pass ? 0 : 1;
}
