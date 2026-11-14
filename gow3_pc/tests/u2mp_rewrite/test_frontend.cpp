// u2mp rewrite tests, group "frontend": frontend.cpp, frontend2.cpp, frontend3.cpp, frontend6.cpp (spec
// u2mp/docs/spec/frontend.md), old (git) against new, on the same fake image and the same scripted session.
//
// Every scenario runs in a child process of its own (the modules keep per-process state that only an install can
// set, and UNCHARTED_U2MP_MM_MIN is read once per process). In the child the old code runs, then the image is
// cleared and the new code runs the same steps; both records (guest calls, session / online calls, log lines,
// return values, state dumps) and the final images must be equal.
//
//   test_frontend.exe                 all scenarios (one child each), summary at the end
//   test_frontend.exe <name> [-v]     one scenario in this process (-v prints both records)
//
// Host pointers compared by meaning: the "Leave Matchmaking" yes callback in the menu callback table (recorded as
// "host", then called to see what it does) and the two retargeted registrations of frontend6 (the near stub's form
// is checked and the callbacks are called through it); their raw values are blanked before the image comparison.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <process.h>
#include <string>
#include <vector>

#include "deps_frontend.h"
#include "harness.h"

extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *, char *, unsigned long);

#define FE_DECL(NS)                                                                \
    namespace NS {                                                                 \
    void show_page(uint32_t page);                                                 \
    void install_frontend(int &installed);                                         \
    void install_frontend2(int &installed);                                        \
    void install_frontend3(int &installed);                                        \
    void install_frontend6(int &installed);                                        \
    namespace matchmaking {                                                        \
    bool installed();                                                              \
    void leader_start();                                                           \
    void on_message(uint8_t from, const uint8_t *data, size_t size);               \
    void on_peer_joined(uint8_t slot);                                             \
    void on_migrated(bool host);                                                   \
    }                                                                              \
    namespace frontend6 {                                                          \
    bool installed();                                                              \
    bool leader_starting(bool matchmaking);                                        \
    void member_starting();                                                        \
    bool popup_up();                                                               \
    void preload_net_game(bool request, bool keep_menu, bool flags_only);          \
    }                                                                              \
    }
FE_DECL(u2mp)
FE_DECL(u2mp_old)

namespace hx::fe {
State s;
}

namespace {

using namespace hx;
using hx::fe::s;

struct Api {
    void (*show_page)(uint32_t);
    void (*install_frontend)(int &);
    void (*install_frontend2)(int &);
    void (*install_frontend3)(int &);
    void (*install_frontend6)(int &);
    bool (*mm_installed)();
    void (*leader_start)();
    void (*on_message)(uint8_t, const uint8_t *, size_t);
    void (*on_peer_joined)(uint8_t);
    void (*on_migrated)(bool);
    bool (*f6_installed)();
    bool (*leader_starting)(bool);
    void (*member_starting)();
    bool (*popup_up)();
    void (*preload)(bool, bool, bool);
};
#define FE_API(NS)                                                                                                   \
    Api {                                                                                                            \
        NS::show_page, NS::install_frontend, NS::install_frontend2, NS::install_frontend3, NS::install_frontend6,   \
            NS::matchmaking::installed, NS::matchmaking::leader_start, NS::matchmaking::on_message,                  \
            NS::matchmaking::on_peer_joined, NS::matchmaking::on_migrated, NS::frontend6::installed,                 \
            NS::frontend6::leader_starting, NS::frontend6::member_starting, NS::frontend6::popup_up,                 \
            NS::frontend6::preload_net_game                                                                          \
    }
const Api kOld = FE_API(u2mp_old);
const Api kNew = FE_API(u2mp);

// ---- the fake game ----------------------------------------------------------------------------------------------
constexpr uint64_t MENU = 0x1136640, NETINFO = 0x104f1c8, NETSM = 0x14ce97c, NGM = 0x113b760, VOTE = 0x113db50,
                   TIP = 0x113ddac, NPD = 0x11474f8;
constexpr uint64_t PAGES = 0x1700000, GAMES = 0x1710000, TIPS = 0x17f0000, EV = 0x17f1000, BOX = 0x17f2000,
                   DESC = 0x17f3000, ARG = 0x17f4000, POPOBJ = 0x17a0000;
constexpr uint32_t PG_MM = 0xe35f0f1a, PG_BOX = 0x719e9fec, PG_POPUP = 0x453470dd;
constexpr uint32_t YES_ID = 0x7d1ecad7;

std::map<uint32_t, uint64_t> dc;   // DC lookup id -> table offset

uint64_t page_obj(uint32_t id) {   // one page object per id (id at +0x10)
    for (int i = 0; i < 256; i++) {
        const uint64_t o = PAGES + 0x100 * uint64_t(i);
        if (get<uint32_t>(o) == 0) {
            put<uint32_t>(o, 1);
            put<uint32_t>(o + 0x10, id);
            return addr(o);
        }
        if (get<uint32_t>(o + 0x10) == id) return addr(o);
    }
    return 0;
}
int32_t top() { return get<int32_t>(MENU + 0x1010); }
void set_top(int32_t t) { put<int32_t>(MENU + 0x1010, t); }
void push_page(uint32_t id) {
    const int32_t t = top() + 1;
    set_top(t);
    put<uint64_t>(MENU + 0x9d0 + 8 * uint64_t(t), page_obj(id));
}
uint32_t top_id() {
    const int32_t t = top();
    if (t < 0 || t > 0x40) return 0;
    const uint64_t p = get<uint64_t>(MENU + 0x9d0 + 8 * uint64_t(t));
    return p ? *reinterpret_cast<uint32_t *>(uintptr_t(p) + 0x10) : 0;
}

U2MP_GUEST void g_show_page(void *menu, uint32_t page, uint32_t flags) {
    rec("ShowPage %s 0x%08x %u", where(menu).c_str(), page, flags);
    push_page(page);
}
U2MP_GUEST void g_pop_page(void *menu) {
    rec("PopPage %s (top 0x%08x)", where(menu).c_str(), top_id());
    const int32_t t = top();
    if (t >= 0 && t <= 0x40) {
        put<uint64_t>(MENU + 0x9d0 + 8 * uint64_t(t), 0);
        set_top(t - 1);
    }
}
U2MP_GUEST void g_set_net_game(void *ngm, int index, const void *p, int n) {
    rec("SetNetGame %s %d %s %d", where(ngm).c_str(), int16_t(index), where(p).c_str(), n);
    put<int16_t>(NGM + 0x1ca0, int16_t(index));
}
U2MP_GUEST void *g_dc_lookup(uint32_t id) {
    rec("DcLookup 0x%08x", id);
    const auto it = dc.find(id);
    return it == dc.end() ? nullptr : reinterpret_cast<void *>(addr(it->second));
}
U2MP_GUEST void g_start_matchmaking(void *a, void *b, void *c) {
    rec("START_MATCHMAKING %s %s %s", where(a).c_str(), where(b).c_str(), where(c).c_str());
    put<int32_t>(NETSM, 19);   // menu reset, NET_SM 19, the matchmaking page
    std::memset(mem + MENU + 0x9d0, 0, 8 * 0x41);
    set_top(-1);
    push_page(PG_MM);
}
U2MP_GUEST void g_publish() { rec("publish player info"); }
U2MP_GUEST void g_set_timer(void *desc, int seconds) { rec("SetPopupTimer %s %d", where(desc).c_str(), seconds); }
U2MP_GUEST void g_preload(void *ngm, int request, int keep, int flags) {
    rec("LevelPreload %s %d %d %d", where(ngm).c_str(), request, keep, flags);
}

void bytes(uint64_t off, std::initializer_list<uint8_t> b) {
    uint64_t i = off;
    for (uint8_t c : b) mem[i++] = c;
}

struct Check {
    uint64_t at;
    std::vector<uint8_t> b;
};
const std::vector<Check> kPopupChecks = {
    {0x5c7981, {0xbe, 0xdd, 0x70, 0x34, 0x45}},
    {0x5cfb54, {0x49, 0x8b, 0x86, 0xb8, 0x09, 0x00, 0x00}},
    {0x5cfb64, {0x81, 0x78, 0x10, 0xdd, 0x70, 0x34, 0x45}},
    {0x5cfb71, {0x49, 0x8b, 0x86, 0x80, 0x18, 0x00, 0x00}},
    {0x5cfb85, {0x3b, 0x48, 0x44}},
    {0x5cfbed, {0x41, 0x8b, 0xb6, 0xd4, 0x18, 0x00, 0x00}},
    {0x5caf6c, {0x41, 0x8b, 0xb5, 0xd8, 0x18, 0x00, 0x00}},
    {0x5a3416, {0x80, 0xbb, 0xf1, 0x00, 0x00, 0x00, 0x00}},
    {0x5c9068, {0x41, 0x8b, 0x85, 0xf0, 0x24, 0x00, 0x00}},
    {0x5c90ed, {0x49, 0x8d, 0x94, 0xc5, 0xf8, 0x24, 0x00, 0x00}},
    {0x5cb8d4, {0x41, 0x8b, 0x85, 0xec, 0x18, 0x00, 0x00}},
    {0x5ae8a0, {0x48, 0x8d, 0x05, 0xd9, 0x78, 0xb6, 0x00, 0x8b, 0x80, 0x04, 0x01, 0x00, 0x00, 0x89, 0x47, 0x40}},
    {0x5eba40, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8d, 0x1d}},
    {0x5ef30f, {0xc7, 0x84, 0xc3, 0xf0, 0x18, 0x00, 0x00, 0xbe, 0x7d, 0x70, 0xe2}},
    {0x5ef413, {0xc7, 0x84, 0xc3, 0xf0, 0x18, 0x00, 0x00, 0x5f, 0xf4, 0x55, 0x40}},
};
const std::vector<Check> kPreloadChecks = {
    {0x641690, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53}},
    {0x6416ba, {0xbf, 0x7d, 0x06, 0x9a, 0x3b}},
    {0x6418bc, {0x45, 0x88, 0xa7, 0x9d, 0x20, 0x00, 0x00}},
};

void back_fn(uint64_t at) {   // lea rdi,[menu]; jmp PopPage
    bytes(at, {0x48, 0x8d, 0x3d});
    put<int32_t>(at + 3, int32_t(int64_t(MENU) - int64_t(at + 7)));
    mem[at + 7] = 0xe9;
    put<int32_t>(at + 8, int32_t(int64_t(0x5cc6f0) - int64_t(at + 12)));
}
void lea_rcx(uint64_t site, uint64_t target) {
    bytes(site, {0x48, 0x8d, 0x0d});
    put<int32_t>(site + 3, int32_t(int64_t(target) - int64_t(site + 7)));
}

// Every byte run the four installs check, intact.
void write_all() {
    bytes(0x609680, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x81, 0xec, 0x58, 0x06, 0x00, 0x00, 0x48, 0x8b, 0x1d});
    bytes(0x609680 + 0x1a,
          {0x8b, 0x47, 0x04, 0x48, 0x8d, 0x04, 0x80, 0x48, 0xc1, 0xe0, 0x04, 0x48, 0x8b, 0x84, 0x05, 0xf8, 0xf9, 0xff, 0xff});
    bytes(0x64e910, {0xc3, 0x66, 0x66, 0x66, 0x66});
    bytes(0x64e8e0, {0x83, 0x7f, 0x04, 0x00, 0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x8d, 0x0d});
    put<int32_t>(0x64e8ed, int32_t(int64_t(VOTE) - int64_t(0x64e8e0 + 17)));
    back_fn(0x5eba80);
    back_fn(0x5eba30);
    put<uint32_t>(0x5eba40 + 0x1b, 0x13);
    put<uint32_t>(0x5eba40 + 0x20, PG_MM);
    for (const Check &c : kPopupChecks) std::memcpy(mem + c.at, c.b.data(), c.b.size());
    for (const Check &c : kPreloadChecks) std::memcpy(mem + c.at, c.b.data(), c.b.size());
    lea_rcx(0x5ef31a, 0x5eb830);
    lea_rcx(0x5ef41e, 0x5eba40);
}

void guest_stubs() {   // after the installs: they overwrite checked bytes
    stub(0x5cb990, (void *)&g_show_page);
    stub(0x5cc6f0, (void *)&g_pop_page);
    stub(0x63dc30, (void *)&g_set_net_game);
    stub(0xa15e70, (void *)&g_dc_lookup);
    stub(0x5eba40, (void *)&g_start_matchmaking);
    stub(0x64e810, (void *)&g_publish);
    stub(0x5ae8a0, (void *)&g_set_timer);
    stub(0x641690, (void *)&g_preload);
}

void install_all(const Api &a) {
    int n = 0;
    a.install_frontend2(n);
    a.install_frontend3(n);
    a.install_frontend6(n);
    rec("installed %d, matchmaking %d, popup %d", n, a.mm_installed(), a.f6_installed());
}

void menu_init() {
    set_top(-1);
    put<uint64_t>(MENU + 0x17d0, addr(BOX));
    put<uint64_t>(MENU + 0x1880, addr(DESC));
    put<int16_t>(NGM + 0x1ca0, -1);
    put<uint32_t>(NPD + 0x104, 0xabc123);
}

// The usual start: everything intact and installed, guest stubs, a menu.
void std_setup(const Api &a, bool host = true, int members = 1) {
    write_all();
    install_all(a);
    guest_stubs();
    menu_init();
    s.host = host;
    s.members = members;
}

void games_table(const std::vector<int32_t> &types) {
    put<int32_t>(GAMES, int32_t(types.size()));
    put<uint64_t>(GAMES + 8, addr(GAMES + 0x100));
    for (size_t i = 0; i < types.size(); i++) put<int32_t>(GAMES + 0x100 + 0x90 * i + 0x0c, types[i]);
    dc[0x3b9a067d] = GAMES;
}
void tips_table(int32_t count) {
    put<int32_t>(TIPS, count);
    put<uint64_t>(TIPS + 8, addr(TIPS + 0x100));
    dc[0xb25ab071] = TIPS;
}

void dump(const char *what = "") {
    rec("state%s: sm %d phase %d deadline %u err %u byme %u top 0x%08x(%d) ngm %d vote %u/%u/%u/%u mine %d games %d,%d "
        "counts %d,%d tip %d",
        what, get<int32_t>(NETSM), get<int32_t>(NETINFO + 0x34), get<uint32_t>(NETINFO + 0x20), get<uint8_t>(NETINFO + 0x1c),
        get<uint8_t>(NETINFO + 0x08), top_id(), top(), get<int16_t>(NGM + 0x1ca0), get<uint8_t>(VOTE), get<uint8_t>(VOTE + 1),
        get<uint8_t>(VOTE + 2), get<uint8_t>(VOTE + 3), get<int32_t>(VOTE + 4), get<int32_t>(VOTE + 8),
        get<int32_t>(VOTE + 12), get<int32_t>(VOTE + 16), get<int32_t>(VOTE + 20), get<int32_t>(TIP));
}

void page_ev(uint32_t type, uint8_t held = 0xee) {
    put<uint32_t>(EV, type);
    put<uint8_t>(EV + 0x10, held);
    reinterpret_cast<void(U2MP_GUEST *)(void *)>(mem + 0x64e910)(reinterpret_cast<void *>(addr(EV)));
    rec("event %u -> held 0x%02x", type, get<uint8_t>(EV + 0x10));
}
void frame(uint32_t t) {
    s.clock = t;
    rec("-- frame %u", t);
    page_ev(6);
    dump();
}
void vote_cb(uint32_t row, bool null_arg = false) {
    put<uint32_t>(ARG + 4, row);
    reinterpret_cast<void(U2MP_GUEST *)(void *)>(mem + 0x64e8e0)(null_arg ? nullptr : reinterpret_cast<void *>(addr(ARG)));
    rec("vote row %u%s", row, null_arg ? " (NULL arg)" : "");
    dump();
}
void msg(const Api &a, uint8_t from, std::vector<uint8_t> d) {
    rec("-- message from %u, %zu bytes, type 0x%02x", from, d.size(), d.empty() ? 0 : d[0]);
    static const uint8_t none[1] = {0xee};
    a.on_message(from, d.empty() ? none : d.data(), d.size());
    dump();
}
void leader_back(uint64_t at) {
    reinterpret_cast<void(U2MP_GUEST *)(void *, void *, void *)>(mem + at)(nullptr, nullptr, nullptr);
    rec("leader back 0x%llx called", (unsigned long long)at);
}
bool call_yes() {   // the "Yes" of "Leave Matchmaking?" as the game calls it: through the callback table
    const uint32_t count = get<uint32_t>(MENU + 0x18ec);
    for (uint32_t i = 0; i < count && i < 128; i++) {
        const uint64_t e = MENU + 0x18f0 + 0x18 * uint64_t(i);
        if (get<uint32_t>(e) == YES_ID && get<uint64_t>(e + 8)) {
            reinterpret_cast<void(U2MP_GUEST *)(void *)>(uintptr_t(get<uint64_t>(e + 8)))(reinterpret_cast<void *>(addr(ARG)));
            rec("yes callback (entry %u) called", i);
            return true;
        }
    }
    rec("no yes callback registered");
    return false;
}
uint64_t reg_target(uint64_t site) { return uint64_t(int64_t(site + 7) + get<int32_t>(site + 3)); }
void call_reg(uint64_t site) {   // the game's registry calling the done callback registered at `site`
    const uint64_t t = reg_target(site);
    if (t < kImageSize) {
        rec("registration 0x%llx still loads img+0x%llx, not called", (unsigned long long)site, (unsigned long long)t);
        return;
    }
    put<uint32_t>(ARG, 0x4055f45f);
    reinterpret_cast<void(U2MP_GUEST *)(void *)>(mem + t)(reinterpret_cast<void *>(addr(ARG)));
    rec("registration 0x%llx callback called", (unsigned long long)site);
}

// Host pointers in the image, compared by meaning and blanked.
void normalize() {
    const uint32_t count = get<uint32_t>(MENU + 0x18ec);
    for (uint32_t i = 0; i < count && i < 256; i++) {
        const uint64_t e = MENU + 0x18f0 + 0x18 * uint64_t(i);
        if (get<uint32_t>(e) != YES_ID) continue;
        const uint64_t fn = get<uint64_t>(e + 8);
        const bool in_image = fn >= addr(0) && fn < addr(kImageSize);
        rec("callback entry %u: id 0x%08x fn %s arg 0x%llx", i, YES_ID, fn == 0 ? "NULL" : in_image ? where((void *)fn).c_str() : "host",
            (unsigned long long)get<uint64_t>(e + 0x10));
        if (fn && !in_image) put<uint64_t>(e + 8, 0x484f5354);
    }
    for (uint64_t site : {0x5ef31aull, 0x5ef41eull}) {
        if (!(mem[site] == 0x48 && mem[site + 1] == 0x8d && mem[site + 2] == 0x0d)) continue;
        const uint64_t t = reg_target(site);
        if (t < kImageSize) continue;
        const uint8_t *st = mem + t;
        const bool form = st[0] == 0x48 && st[1] == 0xb8 && st[10] == 0xff && st[11] == 0xe0;
        rec("registration 0x%llx -> near stub, %s", (unsigned long long)site, form ? "mov rax,imm64; jmp rax" : "BAD FORM");
        put<int32_t>(site + 3, 0x5354);
    }
}

// ---- the scenarios ----------------------------------------------------------------------------------------------
struct Scen {
    std::string name;
    std::function<void(const Api &)> body;
    unsigned seed;
};
std::vector<Scen> all;
void S(const std::string &name, std::function<void(const Api &)> body, unsigned seed = 4242) { all.push_back({name, body, seed}); }

void setenv_mm_min(const char *v) { _putenv_s("UNCHARTED_U2MP_MM_MIN", v ? v : ""); }

// host LAN: leader start at t0, first frame (19 -> 16)
void host_lan_start(const Api &a, uint32_t t0) {
    s.clock = t0;
    a.leader_start();
    dump("after leader_start");
    frame(t0);
}
// host LAN with one member: the vote opens at t0 + 900; returns the vote time
uint32_t host_to_vote(const Api &a, uint32_t t0) {
    host_lan_start(a, t0);
    frame(t0 + 900);
    return t0 + 900;
}
// a member with the vote options received
void member_in_vote(const Api &a, int16_t ga, int16_t gb, uint32_t ticks, uint32_t t) {
    s.clock = t;
    msg(a, 0, {0x12, uint8_t(ga), uint8_t(uint16_t(ga) >> 8), uint8_t(gb), uint8_t(uint16_t(gb) >> 8), uint8_t(ticks),
               uint8_t(ticks >> 8), uint8_t(ticks >> 16), uint8_t(ticks >> 24)});
}

void register_all() {
    // ===== frontend.cpp =====
    S("fe_show_page", [](const Api &a) {
        write_all();
        guest_stubs();
        a.show_page(0x12345678);
        a.show_page(0);
        a.show_page(0xffffffff);
    });
    S("fe_install_frontend", [](const Api &a) {
        int n = 3;
        s.install_ref = &n;
        a.install_frontend(n);
        rec("counter %d", n);
    });

    // ===== frontend2.cpp =====
    auto f2_call = [](uint32_t row, bool null_arg) {
        put<uint32_t>(ARG + 4, row);
        reinterpret_cast<void(U2MP_GUEST *)(void *, void *, void *)>(mem + 0x609680)(
            null_arg ? nullptr : reinterpret_cast<void *>(addr(ARG)), (void *)addr(ARG + 0x100), (void *)addr(ARG + 0x200));
        rec("select join called (row %u, null %d)", row, null_arg);
    };
    S("fe2_install_and_calls", [f2_call](const Api &a) {
        write_all();
        int n = 7;
        a.install_frontend2(n);
        rec("counter %d", n);
        s.select_join = false;
        f2_call(9, true);
        f2_call(3, false);
        f2_call(0, false);
        f2_call(0xffffffffu, false);
        f2_call(0x80000000u, false);
        s.select_join = true;
        f2_call(5, false);
        f2_call(0, true);
    });
    for (int off : {0, 7, 14}) {
        S("fe2_prologue_byte_" + std::to_string(off), [off](const Api &a) {
            write_all();
            mem[0x609680 + off] ^= 0x01;
            int n = 7;
            a.install_frontend2(n);
            rec("counter %d", n);
        });
    }
    for (int off : {0, 5, 18}) {
        S("fe2_array_read_byte_" + std::to_string(off), [off](const Api &a) {
            write_all();
            mem[0x609680 + 0x1a + off] ^= 0x01;
            int n = 7;
            a.install_frontend2(n);
            rec("counter %d", n);
        });
    }
    S("fe2_replace_fails", [](const Api &a) {
        write_all();
        fail_offsets.push_back(0x609680);
        int n = 7;
        a.install_frontend2(n);
        rec("counter %d", n);
    });

    // ===== frontend3.cpp install =====
    auto install3 = [](const Api &a) {
        rec("before: matchmaking %d", a.mm_installed());
        int n = 10;
        a.install_frontend3(n);
        rec("counter %d, matchmaking %d", n, a.mm_installed());
    };
    S("fe3_install_intact", [install3](const Api &a) {
        write_all();
        install3(a);
    });
    S("fe3_install_page_not_ret", [install3](const Api &a) {
        write_all();
        mem[0x64e910 + 4] = 0x90;
        install3(a);
    });
    S("fe3_install_page_ret_only", [install3](const Api &a) {
        write_all();
        mem[0x64e910 + 1] = 0xcc;
        install3(a);
    });
    S("fe3_install_page_replace_fails", [install3](const Api &a) {
        write_all();
        fail_offsets.push_back(0x64e910);
        install3(a);
    });
    S("fe3_install_vote_lea_elsewhere", [install3](const Api &a) {
        write_all();
        put<int32_t>(0x64e8ed, get<int32_t>(0x64e8ed) + 4);
        install3(a);
    });
    for (int off : {0, 3, 12}) {
        S("fe3_install_vote_byte_" + std::to_string(off), [install3, off](const Api &a) {
            write_all();
            mem[0x64e8e0 + off] ^= 0x01;
            install3(a);
        });
    }
    S("fe3_install_vote_replace_fails", [install3](const Api &a) {
        write_all();
        fail_offsets.push_back(0x64e8e0);
        install3(a);
    });
    for (uint64_t at : {0x5eba80ull, 0x5eba30ull}) {
        const std::string tag = at == 0x5eba80 ? "back1" : "back2";
        S("fe3_install_" + tag + "_lea_target", [install3, at](const Api &a) {
            write_all();
            put<int32_t>(at + 3, get<int32_t>(at + 3) + 8);
            install3(a);
        });
        S("fe3_install_" + tag + "_jmp_target", [install3, at](const Api &a) {
            write_all();
            put<int32_t>(at + 8, get<int32_t>(at + 8) - 16);
            install3(a);
        });
        S("fe3_install_" + tag + "_no_e9", [install3, at](const Api &a) {
            write_all();
            mem[at + 7] = 0xe8;
            install3(a);
        });
        S("fe3_install_" + tag + "_lea_reg", [install3, at](const Api &a) {
            write_all();
            mem[at + 2] = 0x35;
            install3(a);
        });
        S("fe3_install_" + tag + "_replace_fails", [install3, at](const Api &a) {
            write_all();
            fail_offsets.push_back(at);
            install3(a);
        });
    }
    S("fe3_install_start_prologue", [install3](const Api &a) {
        write_all();
        mem[0x5eba40 + 8] ^= 0x01;
        install3(a);
    });
    S("fe3_install_start_prologue_first", [install3](const Api &a) {
        write_all();
        mem[0x5eba40] = 0xc3;
        install3(a);
    });
    S("fe3_install_start_sm", [install3](const Api &a) {
        write_all();
        put<uint32_t>(0x5eba40 + 0x1b, 0x14);
        install3(a);
    });
    S("fe3_install_start_page", [install3](const Api &a) {
        write_all();
        put<uint32_t>(0x5eba40 + 0x20, PG_MM ^ 0x100);
        install3(a);
    });
    S("fe3_install_start_bad_and_page_bad", [install3](const Api &a) {
        write_all();
        mem[0x64e910] = 0x55;
        put<uint32_t>(0x5eba40 + 0x20, 0);
        install3(a);
    });
    S("fe3_install_twice", [install3](const Api &a) {
        write_all();
        install3(a);
        install3(a);   // the page handler is no ret stub any more
    });
    S("fe3_install_nothing", [install3](const Api &a) { install3(a); });

    // ===== frontend6.cpp install =====
    auto install6 = [](const Api &a) {
        rec("before: popup %d", a.f6_installed());
        int n = 10;
        a.install_frontend6(n);
        rec("counter %d, popup %d", n, a.f6_installed());
    };
    S("fe6_install_intact", [install6](const Api &a) {
        write_all();
        install6(a);
        guest_stubs();
        menu_init();
        a.preload(true, false, true);
    });
    for (size_t i = 0; i < kPopupChecks.size(); i++) {
        for (int last : {0, 1}) {
            S("fe6_install_check" + std::to_string(i + 1) + (last ? "_last" : "_first"), [install6, i, last](const Api &a) {
                write_all();
                const Check &c = kPopupChecks[i];
                mem[c.at + (last ? c.b.size() - 1 : 0)] ^= 0x01;
                install6(a);
                guest_stubs();
                menu_init();
                s.host = true;
                rec("leader_starting -> %d", a.leader_starting(true));
                a.member_starting();
                a.preload(true, true, true);
            });
        }
    }
    S("fe6_install_lea1_target", [install6](const Api &a) {
        write_all();
        lea_rcx(0x5ef31a, 0x5eb834);
        install6(a);
    });
    S("fe6_install_lea1_opcode", [install6](const Api &a) {
        write_all();
        mem[0x5ef31a + 2] = 0x15;
        install6(a);
    });
    S("fe6_install_lea2_target", [install6](const Api &a) {
        write_all();
        lea_rcx(0x5ef41e, 0x5eb830);
        install6(a);
        guest_stubs();
        menu_init();
        rec("leader_starting -> %d", a.leader_starting(false));
        call_reg(0x5ef31a);
    });
    S("fe6_install_lea2_opcode", [install6](const Api &a) {
        write_all();
        mem[0x5ef41e] = 0x4c;
        install6(a);
    });
    S("fe6_install_no_stub_memory", [install6](const Api &a) {
        write_all();
        exec_left = 0;
        install6(a);
    });
    S("fe6_install_stub_memory_for_one", [install6](const Api &a) {
        write_all();
        exec_left = 1;
        install6(a);
        guest_stubs();
        menu_init();
        call_reg(0x5ef31a);
    });
    for (size_t i = 0; i < kPreloadChecks.size(); i++) {
        for (int last : {0, 1}) {
            S("fe6_install_preload" + std::to_string(i + 1) + (last ? "_last" : "_first"), [install6, i, last](const Api &a) {
                write_all();
                const Check &c = kPreloadChecks[i];
                mem[c.at + (last ? c.b.size() - 1 : 0)] ^= 0x01;
                install6(a);
                guest_stubs();
                a.preload(true, true, true);
            });
        }
    }
    S("fe6_install_popup_and_preload_broken", [install6](const Api &a) {
        write_all();
        mem[0x5c7981] = 0;
        mem[0x6416ba] = 0;
        install6(a);
    });
    S("fe6_install_nothing", [install6](const Api &a) { install6(a); });

    // ===== frontend6 callbacks through the retargeted registrations =====
    S("fe6_cb_create_game", [](const Api &a) {
        std_setup(a, true, 3);
        call_reg(0x5ef31a);
        s.host = false;
        call_reg(0x5ef31a);
    });
    S("fe6_cb_start_mm_host_installed", [](const Api &a) {
        std_setup(a, true, 3);
        call_reg(0x5ef41e);
        frame(100);
    });
    S("fe6_cb_start_mm_host_not_installed", [](const Api &a) {
        write_all();
        put<uint32_t>(0x5eba40 + 0x1b, 0x12);   // frontend3's START check only
        install_all(a);
        guest_stubs();
        menu_init();
        s.host = true;
        call_reg(0x5ef41e);
    });
    S("fe6_cb_start_mm_member", [](const Api &a) {
        std_setup(a, false, 3);
        call_reg(0x5ef41e);
    });

    // ===== leader_starting / member_starting / popup page =====
    S("fe6_leader_not_installed", [](const Api &a) {
        write_all();
        mem[0x5cfb85] = 0;
        install_all(a);
        guest_stubs();
        menu_init();
        rec("leader_starting -> %d", a.leader_starting(true));
        rec("leader_starting -> %d", a.leader_starting(false));
    });
    S("fe6_leader_member", [](const Api &a) {
        std_setup(a, false);
        rec("leader_starting -> %d", a.leader_starting(true));
    });
    S("fe6_leader_desc_null", [](const Api &a) {
        std_setup(a);
        put<uint64_t>(MENU + 0x1880, 0);
        put<uint32_t>(MENU + 0x18d4, 0x11111111);
        rec("leader_starting -> %d", a.leader_starting(true));
    });
    auto popup_objects = [](uint32_t count, const std::vector<uint32_t> &ids) {   // id 0: a NULL slot
        put<uint32_t>(MENU + 0x9b0, count);
        for (size_t i = 0; i < ids.size(); i++) {
            const uint64_t o = POPOBJ + 0x100 * i;
            put<uint32_t>(o + 0x10, ids[i]);
            put<uint8_t>(o + 0xf1, 0x77);
            put<uint64_t>(MENU + 0x370 + 8 * i, ids[i] ? addr(o) : 0);
        }
    };
    for (int mm : {1, 0}) {
        S(std::string("fe6_leader_") + (mm ? "matchmaking" : "custom"), [mm, popup_objects](const Api &a) {
            std_setup(a, true, 2);
            popup_objects(3, {0x1234, PG_POPUP, 0x5678});
            put<uint32_t>(MENU + 0x24f0, 2);
            put<uint32_t>(MENU + 0x24f8, 0x1000);
            put<uint32_t>(MENU + 0x24f8 + 0x18, 0x2000);
            rec("leader_starting -> %d", a.leader_starting(mm));
            rec("popup_up -> %d", a.popup_up());
            rec("leader_starting -> %d", a.leader_starting(!mm));
        });
    }
    S("fe6_pages_count0", [popup_objects](const Api &a) {
        std_setup(a);
        popup_objects(0, {PG_POPUP, PG_POPUP});
        rec("leader_starting -> %d", a.leader_starting(true));
    });
    S("fe6_pages_null_early", [popup_objects](const Api &a) {
        std_setup(a);
        popup_objects(5, {0x1, PG_POPUP, 0, PG_POPUP, PG_POPUP});
        rec("leader_starting -> %d", a.leader_starting(true));
    });
    S("fe6_pages_two_popups_member", [popup_objects](const Api &a) {
        std_setup(a, false);
        popup_objects(4, {PG_POPUP, 0x2, PG_POPUP, 0x3});
        a.member_starting();
    });
    S("fe6_pages_count300", [popup_objects](const Api &a) {
        std_setup(a, false);
        std::vector<uint32_t> ids(200, PG_POPUP);
        popup_objects(300, ids);
        put<uint32_t>(MENU + 0x9b0, 300);
        a.member_starting();
    });
    S("fe6_pages_count201_popup_at_200", [popup_objects](const Api &a) {
        std_setup(a, false);
        std::vector<uint32_t> ids(200, 0x9);
        ids[199] = PG_POPUP;
        popup_objects(201, ids);
        a.member_starting();
    });
    auto list_entry = [](uint32_t i, uint32_t page) {
        const uint64_t e = MENU + 0x24f8 + 0x18 * uint64_t(i);
        put<uint32_t>(e, page);
        put<uint32_t>(e + 4, 0xabababab);
        put<uint64_t>(e + 8, 0x1111111111111111ull * (i + 1));
        put<uint64_t>(e + 0x10, 0x2222222222222222ull);
    };
    S("fe6_list_existing_twice", [list_entry](const Api &a) {
        std_setup(a);
        for (uint32_t i = 0; i < 5; i++) list_entry(i, i == 1 || i == 3 ? PG_POPUP : 0x100 + i);
        put<uint32_t>(MENU + 0x24f0, 5);
        rec("leader_starting -> %d", a.leader_starting(true));
    });
    S("fe6_list_new", [list_entry](const Api &a) {
        std_setup(a);
        for (uint32_t i = 0; i < 3; i++) list_entry(i, 0x100 + i);
        list_entry(3, PG_POPUP);   // beyond the count: not looked at
        put<uint32_t>(MENU + 0x24f0, 2);
        rec("leader_starting -> %d", a.leader_starting(true));
        rec("leader_starting -> %d", a.leader_starting(true));
    });
    S("fe6_list_full32", [list_entry](const Api &a) {
        std_setup(a);
        for (uint32_t i = 0; i < 33; i++) list_entry(i, 0x100 + i);
        put<uint32_t>(MENU + 0x24f0, 32);
        rec("leader_starting -> %d", a.leader_starting(true));
    });
    S("fe6_list_full32_popup_at_31", [list_entry](const Api &a) {
        std_setup(a);
        for (uint32_t i = 0; i < 33; i++) list_entry(i, i == 31 ? PG_POPUP : 0x100 + i);
        put<uint32_t>(MENU + 0x24f0, 32);
        rec("leader_starting -> %d", a.leader_starting(true));
    });
    S("fe6_list_count40", [list_entry](const Api &a) {
        std_setup(a);
        for (uint32_t i = 0; i < 40; i++) list_entry(i, i == 35 ? PG_POPUP : 0x100 + i);
        put<uint32_t>(MENU + 0x24f0, 40);
        rec("leader_starting -> %d", a.leader_starting(true));
    });
    S("fe6_member_not_installed", [](const Api &a) {
        write_all();
        mem[0x5ef413] = 0;
        install_all(a);
        guest_stubs();
        menu_init();
        s.host = false;
        a.member_starting();
    });
    S("fe6_member_host", [](const Api &a) {
        std_setup(a, true);
        a.member_starting();
    });
    S("fe6_member", [popup_objects](const Api &a) {
        std_setup(a, false);
        popup_objects(1, {PG_POPUP});
        put<uint32_t>(MENU + 0x18d8, 0x5555);
        a.member_starting();
    });
    S("fe6_member_desc_null", [](const Api &a) {
        std_setup(a, false);
        put<uint64_t>(MENU + 0x1880, 0);
        a.member_starting();
    });
    S("fe6_popup_up", [](const Api &a) {
        std_setup(a);
        rec("popup_up (NULL overlay) -> %d", a.popup_up());
        put<uint64_t>(MENU + 0x9b8, page_obj(PG_BOX));
        rec("popup_up (other page) -> %d", a.popup_up());
        put<uint64_t>(MENU + 0x9b8, page_obj(PG_POPUP));
        rec("popup_up (popup) -> %d", a.popup_up());
    });
    S("fe6_preload_on", [](const Api &a) {
        std_setup(a);
        for (int m = 0; m < 8; m++) a.preload(m & 1, m & 2, m & 4);
        put<int16_t>(NGM + 0x1ca0, 7);
        a.preload(true, true, false);
        put<int16_t>(NGM + 0x1ca0, -32768);
        a.preload(false, false, false);
    });
    S("fe6_preload_off", [](const Api &a) {
        write_all();
        mem[0x6418bc + 3] = 0;
        install_all(a);
        guest_stubs();
        menu_init();
        a.preload(true, true, true);
        a.preload(false, false, false);
    });

    // ===== frontend3: leader start =====
    S("mm_leader_start_member", [](const Api &a) {
        std_setup(a, false, 3);
        a.leader_start();
        dump();
    });
    S("mm_leader_start_host", [](const Api &a) {
        std_setup(a, true, 4);
        host_lan_start(a, 1000);
        frame(1001);
    });
    S("mm_leader_start_not_installed", [](const Api &a) {
        write_all();
        put<uint32_t>(0x5eba40 + 0x20, 0);
        install_all(a);
        guest_stubs();
        menu_init();
        s.host = true;
        a.leader_start();
        dump();
    });

    // ===== frontend3: host LAN gather and vote =====
    S("mm_lan_gather_ready", [](const Api &a) {
        std_setup(a, true, 3);
        games_table({1, 2, 1, 3});
        host_lan_start(a, 1000);
        frame(1899);
        frame(1900);
        msg(a, 1, {0x11});
        msg(a, 32, {0x11});
        msg(a, 255, {0x11});
        msg(a, 0, {0x11});
        frame(1901);
        msg(a, 31, {0x11});
        frame(1902);
        frame(1903);
    });
    S("mm_lan_gather_ready_extra_bytes", [](const Api &a) {
        std_setup(a, true, 2);
        host_lan_start(a, 1000);
        msg(a, 1, {0x11, 0x99, 0x98});
        frame(1900);
    });
    S("mm_lan_gather_max", [](const Api &a) {
        std_setup(a, true, 3);
        games_table({1, 2});
        host_lan_start(a, 1000);
        frame(1900);
        frame(3999);
        frame(4000);
    });
    S("mm_lan_gather_one_member", [](const Api &a) {
        std_setup(a, true, 1);
        host_lan_start(a, 1000);
        frame(1899);
        frame(1900);
    });
    S("mm_lan_gather_clock_wrap", [](const Api &a) {
        std_setup(a, true, 2);
        games_table({1, 2, 3});
        host_lan_start(a, 0xfffffe00u);
        frame(0xffffffffu);
        frame(0x183);
        frame(0x184);
        frame(0x185 + 0xbb8);
    });
    S("mm_lan_member_ready_ignored_by_member", [](const Api &a) {
        std_setup(a, false, 3);
        msg(a, 0, {0x10});
        frame(10);
        msg(a, 0, {0x11});
        msg(a, 2, {0x11});
        frame(11);
    });

    // full host round: vote, votes, end, countdown, create
    struct Votes {
        const char *name;
        int own;                                 // row of the host's own vote, -1 none
        std::vector<std::pair<int, int>> msgs;   // (slot, choice)
    };
    for (const Votes &v : std::vector<Votes>{{"own1_m0_m0", 7, {{1, 0}, {2, 0}}},
                                             {"own0_m1_m1", 0, {{1, 1}, {2, 1}}},
                                             {"tie", 0, {{1, 1}}},
                                             {"none", -1, {}},
                                             {"members_only", -1, {{1, 0}, {2, 1}, {3, 1}}}}) {
        for (unsigned seed : {1u, 2u, 7u}) {
            S(std::string("mm_lan_round_") + v.name + "_seed" + std::to_string(seed), [v](const Api &a) {
                std_setup(a, true, 3);
                s.create_clears_phase = true;
                games_table({1, 2, 1, 2, 3});
                host_lan_start(a, 5000);
                msg(a, 1, {0x11});
                msg(a, 2, {0x11});
                frame(5900);   // vote
                const uint32_t tv = 5900;
                if (v.own >= 0) vote_cb(uint32_t(v.own));
                for (auto [slot, c] : v.msgs) msg(a, uint8_t(slot), {0x13, uint8_t(c)});
                frame(tv + 4499);
                frame(tv + 4500);   // end
                vote_cb(1);         // closed: nothing
                msg(a, 1, {0x13, 0});   // closed: not counted, relayed
                frame(tv + 4500 + 2399);
                frame(tv + 4500 + 2400);   // create
                frame(tv + 4500 + 2401);
                put<int32_t>(NETSM, 3);
                frame(tv + 9000);
                page_ev(0);   // closed while created
                dump();
                push_page(PG_MM);
                a.on_peer_joined(4);
            }, seed);
        }
    }

    // vote pick
    struct Pick {
        const char *name;
        std::function<void()> prep;
    };
    const std::vector<Pick> picks = {
        {"table_null_game_none", [] {}},
        {"table_null_game_5", [] { put<int16_t>(NGM + 0x1ca0, 5); }},
        {"records_null", [] {
             games_table({1, 2, 3});
             put<uint64_t>(GAMES + 8, 0);
             put<int16_t>(NGM + 0x1ca0, 2);
         }},
        {"count0", [] {
             games_table({});
             put<int16_t>(NGM + 0x1ca0, 4);
         }},
        {"count_negative", [] {
             games_table({1});
             put<int32_t>(GAMES, -3);
         }},
        {"all_types_many", [] { games_table({1, 2, 3, 4, 5, 6}); }},
        {"one_type", [] {
             s.playlist = true;
             s.types = {2};
             games_table({1, 2, 3, 2, 2, 5});
         }},
        {"two_types", [] {
             s.playlist = true;
             s.types = {1, 3};
             games_table({1, 2, 3, 2, 2, 5, 3});
         }},
        {"pool_one", [] {
             s.playlist = true;
             s.types = {3};
             games_table({1, 2, 3, 2});
         }},
        {"dlc_some_off", [] {
             s.dlc_off = {0, 2, 4};
             games_table({1, 1, 1, 1, 1, 1});
         }},
        {"dlc_all_off", [] {
             s.dlc_off = {0, 1};
             games_table({1, 1});
             put<int16_t>(NGM + 0x1ca0, 9);
         }},
        {"dlc_leaves_one", [] {
             s.dlc_off = {0, 2};
             games_table({1, 1, 1});
         }},
        {"playlist_empty_types", [] {
             s.playlist = true;
             s.types = {};
             games_table({1, 2});
             put<int16_t>(NGM + 0x1ca0, 7);
         }},
        {"playlist_false_with_types", [] {
             s.playlist = false;
             s.types = {9};
             games_table({1, 2, 3});
         }},
        {"negative_types", [] {
             s.playlist = true;
             s.types = {-1};
             games_table({-1, 0, -1, -2});
         }},
        {"two_same_game_records", [] {
             games_table({4, 4});
         }},
    };
    for (const Pick &p : picks) {
        for (unsigned seed : {1u, 99u, 12345u}) {
            S(std::string("mm_pick_") + p.name + "_seed" + std::to_string(seed), [p](const Api &a) {
                std_setup(a, true, 1);
                p.prep();
                host_to_vote(a, 300);
                frame(1201);
            }, seed);
        }
    }

    // vote row callback
    S("mm_vote_cb_cases_host", [](const Api &a) {
        std_setup(a, true, 2);
        games_table({1, 2, 3});
        vote_cb(0);   // no vote yet: inactive
        host_to_vote(a, 100);
        vote_cb(0, true);
        put<uint8_t>(VOTE, 0);
        vote_cb(0);
        put<uint8_t>(VOTE, 2);
        put<uint8_t>(VOTE + 1, 1);
        vote_cb(0);
        put<uint8_t>(VOTE + 1, 0);
        put<int32_t>(VOTE + 4, 0);
        vote_cb(1);
        put<int32_t>(VOTE + 4, 1);
        vote_cb(0);
        put<int32_t>(VOTE + 4, 2);
        vote_cb(0);   // mine 2: allowed, choice 0
        put<int32_t>(VOTE + 4, -1);
        vote_cb(7);   // choice 1
        vote_cb(0);   // already voted
        put<int32_t>(VOTE + 4, -5);
        vote_cb(0x80000000u);
    });
    S("mm_vote_cb_member", [](const Api &a) {
        std_setup(a, false, 3);
        member_in_vote(a, 4, -3, 4500, 50);
        vote_cb(0);
        vote_cb(1);
    });
    S("mm_vote_cb_member_row1", [](const Api &a) {
        std_setup(a, false, 3);
        member_in_vote(a, 300, 301, 100, 50);
        vote_cb(2);
    });

    // ===== frontend3: members =====
    for (int on_top : {0, 1}) {
        S(std::string("mm_member_start_") + (on_top ? "page_on_top" : "page_not_on_top"), [on_top](const Api &a) {
            std_setup(a, false, 3);
            tips_table(5);
            put<int32_t>(TIP, 3);
            if (on_top) {
                push_page(0x1);
                push_page(PG_MM);
                put<int32_t>(NETSM, 17);
            }
            s.clock = 5000;
            msg(a, 0, {0x10});
            msg(a, 3, {0x10});
            frame(5000);
            frame(5001);
            frame(7399);
            frame(7400);
            frame(7401);
            frame(9800);
            frame(9801);
            put<int32_t>(NETINFO + 0x34, 2);
            frame(20000);
            put<int32_t>(NETINFO + 0x34, 1);
            frame(20001);
            msg(a, 0, {0x10});   // reset: ready again, NET_SM line again
            frame(20002);
        });
    }
    S("mm_member_sm18_then_19", [](const Api &a) {
        std_setup(a, false, 3);
        put<int32_t>(NETSM, 18);
        frame(10);
        frame(11);
        put<int32_t>(NETSM, 21);
        frame(12);
        put<int32_t>(NETSM, 19);
        frame(13);
        frame(14);
        put<int32_t>(NETSM, 17);
        frame(15);
    });
    for (int which : {0, 1, 2, 3, 4}) {
        static const char *names[] = {"null", "count0", "count_neg", "count5_index4", "neg_index"};
        S(std::string("mm_member_tips_") + names[which], [which](const Api &a) {
            std_setup(a, false, 3);
            put<int32_t>(NETSM, 17);
            if (which == 1) tips_table(0);
            if (which == 2) tips_table(-2);
            if (which >= 3) tips_table(5);
            put<int32_t>(TIP, which == 4 ? -7 : 4);
            frame(100);
            frame(2499);
            frame(2500);
            frame(2501);
            frame(4900);
            frame(7300);
        });
    }
    S("mm_member_tips_clock_wrap", [](const Api &a) {
        std_setup(a, false, 3);
        put<int32_t>(NETSM, 17);
        tips_table(3);
        frame(0xfffff000u);
        frame(0xfffff000u + 2399);
        frame(0xfffff000u + 2400);
    });
    S("mm_member_messages", [](const Api &a) {
        std_setup(a, false, 3);
        s.clock = 1000;
        msg(a, 0, {0x12, 1, 0, 2, 0, 0x94, 0x11, 0});   // size 8: ignored
        member_in_vote(a, 1, -2, 4500, 1000);
        member_in_vote(a, 5, 6, 3000, 1100);   // again while running: no begin
        frame(1101);
        msg(a, 0, {0x13});
        msg(a, 0, {0x13, 2});
        msg(a, 0, {0x13, 1});
        msg(a, 0, {0x13, 0, 7});
        msg(a, 0, {0x14, 3, 0, 0xb8, 0x0b, 0});   // size 6: ignored
        s.clock = 1200;
        msg(a, 0, {0x14, 3, 0, 0xb8, 0x0b, 0, 0});
        msg(a, 0, {0x13, 0});   // closed: not counted
        frame(1201);
        msg(a, 1, {0x16});
        msg(a, 0, {0x16});   // neither popup nor box
        put<uint64_t>(MENU + 0x9b8, page_obj(PG_POPUP));
        msg(a, 0, {0x16});   // popup up: pop
        put<uint64_t>(MENU + 0x9b8, page_obj(0x42));
        push_page(PG_BOX);
        msg(a, 0, {0x16});   // box on top: pop
        msg(a, 1, {0x17});
        msg(a, 0, {0x17});   // the popup (member_starting)
        msg(a, 0, {0x18});
        msg(a, 0, {0xff, 1, 2});
        msg(a, 0, {});
        msg(a, 0, {0x14, 0xff, 0xff, 0, 0, 0, 0x80});   // game -1, ticks 0x80000000
        frame(1300);
    });
    S("mm_member_vote_options_page_on_top", [](const Api &a) {
        std_setup(a, false, 2);
        push_page(PG_MM);
        put<int32_t>(NETSM, 17);
        member_in_vote(a, -32768, 32767, 0xffffffffu, 77);
        frame(78);
    });
    S("mm_host_ignores_member_messages", [](const Api &a) {
        std_setup(a, true, 3);
        host_lan_start(a, 100);
        msg(a, 1, {0x10});
        msg(a, 1, {0x12, 1, 0, 2, 0, 0x94, 0x11, 0, 0});
        msg(a, 1, {0x14, 3, 0, 0xb8, 0x0b, 0, 0});
        msg(a, 1, {0x16});
        msg(a, 0, {0x16});
        msg(a, 0, {0x17});
        msg(a, 1, {0x18});
        msg(a, 1, {});
        frame(101);
    });

    // ===== frontend3: host messages =====
    S("mm_host_vote_relay_inactive", [](const Api &a) {
        std_setup(a, true, 3);
        msg(a, 2, {0x13, 1});
        msg(a, 3, {0x13, 0});
        msg(a, 1, {0x13});
        msg(a, 1, {0x13, 2});
        msg(a, 1, {0x13, 0xff});
    });
    S("mm_host_vote_relay_active", [](const Api &a) {
        std_setup(a, true, 3);
        games_table({1, 2, 3});
        host_to_vote(a, 10);
        msg(a, 2, {0x13, 1});
        msg(a, 7, {0x13, 0});
        msg(a, 1, {0x13, 1, 9, 9});
        put<uint8_t>(VOTE + 1, 1);
        msg(a, 2, {0x13, 1});
    });
    S("mm_host_leave_lan", [](const Api &a) {
        std_setup(a, true, 3);
        push_page(0x1);
        host_lan_start(a, 10);
        push_page(PG_BOX);
        msg(a, 2, {0x15});
        frame(11);
    });
    S("mm_host_leave_ignored", [](const Api &a) {
        std_setup(a, true, 3);
        push_page(0x1);
        msg(a, 2, {0x15});
    });
    S("mm_host_leave_not_running_page_top", [](const Api &a) {
        std_setup(a, true, 3);
        push_page(PG_MM);
        msg(a, 2, {0x15});
    });
    S("mm_member_leave_messages", [](const Api &a) {
        std_setup(a, false, 3);
        msg(a, 0, {0x10});
        frame(10);
        msg(a, 1, {0x15});
        msg(a, 0, {0x15});
        frame(11);
    });
    S("mm_member_leave_ignored", [](const Api &a) {
        std_setup(a, false, 3);
        msg(a, 0, {0x15});
        push_page(PG_MM);
        msg(a, 0, {0x15});
    });

    // ===== frontend3: leave =====
    S("mm_leave_box_yes", [](const Api &a) {
        std_setup(a, true, 2);
        put<uint32_t>(MENU + 0x18ec, 3);
        for (uint32_t i = 0; i < 3; i++) put<uint32_t>(MENU + 0x18f0 + 0x18 * i, 0x100 + i);
        put<uint32_t>(MENU + 0x18f0 + 0x18 * 3 + 4, 0xcdcdcdcd);   // +4..+7 not written
        host_lan_start(a, 100);
        page_ev(1);
        page_ev(2);
        dump();
        page_ev(3, 0);
        dump();
        call_yes();
        put<int32_t>(NETSM, 20);
        page_ev(2);
        put<int32_t>(NETSM, 19);
        page_ev(3);
        put<int32_t>(NETSM, 18);
        page_ev(2);
        frame(101);
        frame(102);
        frame(103);
    });
    S("mm_leave_box_yes_one_ask", [](const Api &a) {
        std_setup(a, true, 2);
        games_table({1, 2});
        host_to_vote(a, 100);
        page_ev(2);
        call_yes();
        frame(1001);
        frame(1002);
    });
    S("mm_leave_member_yes", [](const Api &a) {
        std_setup(a, false, 2);
        msg(a, 0, {0x10});
        frame(10);
        page_ev(3);
        call_yes();
        frame(11);
    });
    S("mm_leave_box_null", [](const Api &a) {
        std_setup(a, true, 2);
        host_lan_start(a, 100);
        put<uint64_t>(MENU + 0x17d0, 0);
        page_ev(2);
        put<int32_t>(NETSM, 16);
        page_ev(2);
        put<int32_t>(NETSM, 19);
        page_ev(3);
        frame(101);
    });
    S("mm_leave_table_full", [](const Api &a) {
        std_setup(a, true, 2);
        put<uint32_t>(MENU + 0x18ec, 128);
        page_ev(1);
        page_ev(2);
        call_yes();
    });
    S("mm_leave_table_200_id_beyond_128", [](const Api &a) {
        std_setup(a, true, 2);
        put<uint32_t>(MENU + 0x18ec, 200);
        put<uint32_t>(MENU + 0x18f0 + 0x18 * 130, YES_ID);
        page_ev(1);
    });
    S("mm_leave_table_127", [](const Api &a) {
        std_setup(a, true, 2);
        put<uint32_t>(MENU + 0x18ec, 127);
        page_ev(1);
        page_ev(1);
        put<uint32_t>(MENU + 0x18ec, 128);
        page_ev(1);
    });
    S("mm_leave_already_registered", [](const Api &a) {
        std_setup(a, true, 2);
        put<uint32_t>(MENU + 0x18ec, 10);
        put<uint32_t>(MENU + 0x18f0 + 0x18 * 5, YES_ID);
        put<uint64_t>(MENU + 0x18f0 + 0x18 * 5 + 8, 0);   // a NULL fn: still counts as registered
        page_ev(1);
        page_ev(2);
    });
    S("mm_leave_error_flag", [](const Api &a) {
        std_setup(a, true, 2);
        push_page(0x1);
        host_lan_start(a, 100);
        put<uint8_t>(NETINFO + 0x1c, 1);
        frame(101);
        frame(102);
        put<uint8_t>(NETINFO + 0x1c, 0);
        frame(103);
    });
    S("mm_leave_error_flag_member", [](const Api &a) {
        std_setup(a, false, 2);
        put<uint8_t>(NETINFO + 0x1c, 3);
        frame(1);
    });

    // ===== frontend3: page events =====
    S("mm_page_closed_phase3", [](const Api &a) {
        std_setup(a, true, 2);
        host_lan_start(a, 10);
        put<int32_t>(NETINFO + 0x34, 3);
        page_ev(0);
        frame(11);
    });
    S("mm_page_closed_box_on_top", [](const Api &a) {
        std_setup(a, true, 2);
        host_lan_start(a, 10);
        push_page(PG_BOX);
        page_ev(0);
        frame(11);
    });
    S("mm_page_closed_plain", [](const Api &a) {
        std_setup(a, true, 2);
        games_table({1, 2, 3});
        put<int32_t>(VOTE + 0x14, 99);
        host_lan_start(a, 10);
        frame(3010);   // vote
        page_ev(0);
        dump();
        frame(3011);
    });
    S("mm_page_events_misc", [](const Api &a) {
        std_setup(a, true, 2);
        reinterpret_cast<void(U2MP_GUEST *)(void *)>(mem + 0x64e910)(nullptr);
        rec("NULL event record");
        for (uint32_t e : {4u, 5u, 7u, 0xffffffffu}) page_ev(e);
        dump();
    });

    // next round after a match
    S("mm_next_round", [](const Api &a) {
        std_setup(a, true, 1);
        games_table({1, 2, 3});
        const uint32_t tv = host_to_vote(a, 100);
        frame(tv + 4500);
        frame(tv + 4500 + 2400);   // created
        put<int32_t>(NETINFO + 0x34, 1);   // Leave::Enter: phase 1, NET_SM 16
        put<int32_t>(NETSM, 16);
        frame(20000);
        frame(20001);
        frame(20900);
    });
    S("mm_next_round_not_running", [](const Api &a) {
        std_setup(a, true, 1);
        put<int32_t>(NETINFO + 0x34, 3);
        put<int32_t>(NETSM, 18);
        frame(5);
        put<int32_t>(NETINFO + 0x34, 1);
        frame(6);
    });

    // ===== frontend3: online host =====
    S("mm_online_steps", [](const Api &a) {
        std_setup(a, true, 1);
        s.mm_online = true;
        s.mm_step = 0;
        host_lan_start(a, 100);
        frame(101);
        s.mm_step = 1;
        frame(102);
        s.mm_step = 3;
        frame(103);
        s.mm_step = 4;
        frame(104);
        s.mm_step = 0;
        frame(500);
        frame(1003);
        frame(1004);
        s.mm_step = 4;
        frame(1005);
        s.mm_step = 7;
        frame(1006);
        s.mm_step = -1;
        frame(1007);
        s.mm_step = 0;
        frame(1905);
        s.mm_step = 2;
        frame(1906);
        frame(1907);
        s.mm_online = false;
        put<int32_t>(NETSM, 19);
        frame(1908);
    });
    S("mm_online_idle_retry_wrap", [](const Api &a) {
        std_setup(a, true, 1);
        s.mm_online = true;
        s.mm_step = 4;
        host_lan_start(a, 0x7fffff00u);
        s.mm_step = 0;
        frame(0x80000000u);
        frame(0x80000300u);
    });
    struct Online {
        const char *env;
        int members;
        bool ready;
    };
    for (const Online &o : std::vector<Online>{{nullptr, 1, false}, {nullptr, 2, false}, {nullptr, 2, true},
                                               {nullptr, 3, true}, {"3", 2, false}, {"3", 3, false}, {"3", 3, true},
                                               {"0", 1, false}, {"0", 2, false}, {"11", 1, false}, {"11", 2, true},
                                               {"abc", 1, false}, {"abc", 2, false}, {"1", 1, false}, {"1", 1, true},
                                               {"10", 9, true}, {"10", 10, false}, {nullptr, 10, false}, {nullptr, 12, false},
                                               {"-4", 1, false}, {" 5", 4, true}, {"7x", 6, false}}) {
        std::string env = o.env ? o.env : "unset";
        for (char &c : env)
            if (c == ' ') c = '_';
        S("mm_online_gather_env_" + env + "_m" + std::to_string(o.members) + (o.ready ? "_ready" : ""), [o](const Api &a) {
            setenv_mm_min(o.env);
            std_setup(a, true, o.members);
            games_table({1, 2, 3, 4});
            s.mm_online = true;
            s.mm_step = 2;
            host_lan_start(a, 2000);   // Hosting: NET_SM 16, research time
            if (o.ready)
                for (int slot = 1; slot < o.members && slot < 32; slot++) msg(a, uint8_t(slot), {0x11});
            std::vector<uint32_t> offs = {1, 5999, 6000, 6001};
            for (uint32_t x = 6300; x <= 19500; x += 300) {
                if (x == 12000) {
                    offs.push_back(11999);
                    offs.push_back(12000);
                    offs.push_back(12001);
                } else {
                    offs.push_back(x);
                }
            }
            for (uint32_t x : offs) frame(2000 + x);
        });
    }
    S("mm_online_leave_message_room", [](const Api &a) {
        std_setup(a, true, 3);
        s.mm_online = true;
        s.mm_step = 2;
        host_lan_start(a, 100);
        msg(a, 2, {0x15});
        frame(101);
    });

    // ===== on_peer_joined =====
    S("mm_peer_not_running", [](const Api &a) {
        std_setup(a, true, 2);
        push_page(PG_MM);
        a.on_peer_joined(3);
        dump();
    });
    S("mm_peer_member", [](const Api &a) {
        std_setup(a, false, 2);
        msg(a, 0, {0x10});
        frame(1);
        a.on_peer_joined(3);
    });
    S("mm_peer_page_not_on_top", [](const Api &a) {
        std_setup(a, true, 2);
        host_lan_start(a, 1);
        push_page(PG_BOX);
        a.on_peer_joined(3);
    });
    S("mm_peer_phase1", [](const Api &a) {
        std_setup(a, true, 2);
        host_lan_start(a, 1);
        a.on_peer_joined(3);
        a.on_peer_joined(255);
    });
    S("mm_peer_phase2_active", [](const Api &a) {
        std_setup(a, true, 1);
        games_table({1, 2, 3});
        host_to_vote(a, 1);
        s.clock = 1000;
        a.on_peer_joined(5);
        s.clock = 6000;   // deadline passed: ticks left 0
        a.on_peer_joined(6);
    });
    S("mm_peer_phase2_inactive", [](const Api &a) {
        std_setup(a, true, 1);
        games_table({1, 2, 3});
        host_to_vote(a, 1);
        put<uint8_t>(VOTE, 0);
        a.on_peer_joined(5);
    });
    S("mm_peer_phase3", [](const Api &a) {
        std_setup(a, true, 1);
        games_table({1, 2, 3});
        const uint32_t tv = host_to_vote(a, 1);
        frame(tv + 4500);
        s.clock = tv + 4600;
        a.on_peer_joined(2);
        put<uint32_t>(NETINFO + 0x20, s.clock - 5);
        a.on_peer_joined(2);
    });
    S("mm_peer_phase_other", [](const Api &a) {
        std_setup(a, true, 1);
        host_lan_start(a, 1);
        put<int32_t>(NETINFO + 0x34, 7);
        a.on_peer_joined(1);
    });

    // ===== on_migrated =====
    for (int host : {1, 0}) {
        S(std::string("mm_migrated_") + (host ? "host" : "member"), [host](const Api &a) {
            std_setup(a, host, 2);
            games_table({1, 2, 3});
            put<int32_t>(VOTE + 8, 77);
            put<int32_t>(NETSM, 21);
            put<int32_t>(NETINFO + 0x20, 1234);
            frame(5);
            a.on_migrated(host);
            dump();
            frame(6);
            frame(7);
        });
    }

    // ===== leader back-out =====
    for (int host : {1, 0}) {
        S(std::string("mm_leader_back_") + (host ? "host" : "member"), [host](const Api &a) {
            std_setup(a, host, 2);
            push_page(0x1);
            push_page(PG_POPUP);
            push_page(PG_POPUP);
            leader_back(0x5eba80);
            dump();
            leader_back(0x5eba30);
            dump();
        });
    }

    // ===== frontend3 with the preload off =====
    S("mm_round_without_preload", [](const Api &a) {
        write_all();
        mem[0x641690] = 0;
        install_all(a);
        guest_stubs();
        menu_init();
        s.host = true;
        games_table({1, 2});
        const uint32_t tv = host_to_vote(a, 10);
        frame(tv + 4500);
        frame(tv + 4500 + 2400);
    });
}

bool run(const Scen &sc, bool verbose) {
    std::vector<std::string> r[2];
    std::vector<uint8_t> im[2];
    const Api *apis[2] = {&kOld, &kNew};
    for (int k = 0; k < 2; k++) {
        clear();
        bind_both();
        s = hx::fe::State{};
        dc.clear();
        std::srand(sc.seed);
        sc.body(*apis[k]);
        normalize();
        rec("rand next %d", std::rand());
        r[k] = calls;
        im[k] = snapshot();
    }
    if (verbose)
        for (int k = 0; k < 2; k++) {
            std::printf("==== %s ====\n", k ? "new" : "old");
            for (const auto &l : r[k]) std::printf("  %s\n", l.c_str());
        }
    const bool ok = same(sc.name.c_str(), r[0], r[1], &im[0], &im[1]);
    if (ok) std::printf("PASS %s (%zu records)\n", sc.name.c_str(), r[0].size());
    return ok;
}

}  // namespace

int main(int argc, char **argv) {
    hx::init();
    register_all();
    if (argc >= 2) {
        const bool verbose = argc >= 3 && std::strcmp(argv[2], "-v") == 0;
        for (const Scen &sc : all)
            if (sc.name == argv[1]) return run(sc, verbose) ? 0 : 1;
        std::printf("no scenario %s\n", argv[1]);
        return 3;
    }
    char self[1024];
    GetModuleFileNameA(nullptr, self, sizeof self);
    _putenv_s("UNCHARTED_U2MP_MM_MIN", "");
    int pass = 0, fail = 0;
    std::vector<std::string> failed;
    for (const Scen &sc : all) {
        std::fflush(stdout);
        const intptr_t rc = _spawnl(_P_WAIT, self, self, sc.name.c_str(), nullptr);
        if (rc == 0) {
            pass++;
        } else {
            fail++;
            failed.push_back(sc.name);
            if (rc != 1) std::printf("FAIL %s: the scenario process ended with %d\n", sc.name.c_str(), int(rc));
        }
    }
    std::printf("frontend: %zu scenarios, %d pass, %d fail\n", all.size(), pass, fail);
    for (const auto &n : failed) std::printf("  failed: %s\n", n.c_str());
    return fail ? 1 : 0;
}
