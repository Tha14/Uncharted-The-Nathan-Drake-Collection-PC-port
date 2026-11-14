// u2mp rewrite tests, group "session": session.cpp, old (git) against new, on the scenarios of
// u2mp/docs/spec/session.md 7 and the branches of the old code.
//
//   bash gow3_pc/tests/u2mp_rewrite/run.sh session session.cpp      (flags_session.sh adds the clock and renames)
//
// Every scenario runs in a child process of its own (the module keeps state in statics that no call resets: the
// net clock origin, the once-only GetTeamColor check, the "net package" log memory, the missing level package
// wait), first the old build, then the new one, on the same fake image and the same scripted stand-ins
// (deps_session.cpp: a fake net::Transport that records every send and delivers scripted messages and peer events
// at pump time, gamenet, voice, online, matchmaking ...). The clock (steady_clock, fake_clock.h), gamenet::clock(),
// rand (srand(1)) and the environment are set the same for both. Recorded: log lines, guest calls with their
// arguments, every stand-in call (transport bytes in hex), the results of the public functions; the image is
// compared at the end. Host pointers the module writes into guest memory (menu page handler, state wrappers, abort
// wrapper, roster text pointers) are compared by meaning: the slots are called, the text pointers are read, and the
// values are replaced by a constant before the image comparison.
#include "harness.h"

#define HX_NO_CLOCK_MACRO
#include "fake_clock.h"

#include <cmath>
#include <functional>
#include <map>
#include <process.h>
#include <thread>

#include "deps_session.h"
#include "u2mp_internal.h"

extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *, char *, unsigned long);

namespace hx::ss {
State st;
}
using hx::ss::st;
using hx::ss::Addr;
using hx::ss::Api;

namespace u2mp {
void hx_session_api(hx::ss::Api &a);
}
namespace u2mp_old {
void hx_session_api(hx::ss::Api &a);
}

namespace {

using hx::addr;
using hx::get;
using hx::mem;
using hx::put;
using hx::rec;
using hx::where;
using u2mp::sid;
#define G __attribute__((sysv_abi))

// ---- image layout -----------------------------------------------------------------------------------------
constexpr uint64_t MENU = 0x1136640, NETINFO = 0x104f1c8, NGM = 0x113b760, PARTY = 0x105efb8, MATCH = 0x1065b08,
                   NETSM = 0x14ce97c, NPD = 0x11474f8, PT = 0x1130390, CLOCKPTR = 0x113a040, FRAMECTR = 0x11162c4,
                   NETIMG = 0x1130078, LEVELHEAD = 0x1534140;
constexpr uint64_t EV46 = 0x1600000, EV79 = 0x1600080, EVVT = 0x1600100, WAITOBJ = 0x1600200, SELF = 0x1600300,
                   STATEOBJ = 0x1600400, TASK = 0x1600500, TASKA = 0x1600600, TASKB = 0x1600700, OTHERCLOCK = 0x1600800,
                   NETIMGOBJ = 0x1600900;
constexpr uint64_t GAMES = 0x1601000, GAMESREC = 0x1601100, LEVELS = 0x1603000, LEVELSREC = 0x1603100,
                   RANK = 0x1605000, BUCKETS = 0x1606000, ENDNODE = 0x1606f00, NODES = 0x1607000;
constexpr uint64_t PKG0 = 0x1610000;   // packages: PKG0 + i * 0x1000
constexpr uint64_t PAGE_MP = 0x1620000, PAGE_LEAVE = 0x1620100, PAGE_OTHER = 0x1620200;
constexpr uint64_t Q = 0x1630000, WL0 = 0x1640000, WL1 = 0x1640800, LNODES = 0x1641000, STR = 0x1650000,
                   THUNK = 0x1660000, CODE = 0x1700000;
constexpr uint32_t kBuckets = 7;

uint64_t pkg(int i) { return PKG0 + uint64_t(i) * 0x1000; }

// ---- guest functions -------------------------------------------------------------------------------------
G uint8_t *g_lookup(uint32_t id) {
    rec("guest lookup 0x%08x", id);
    auto it = st.lookup.find(id);
    return it == st.lookup.end() || !it->second ? nullptr : mem + it->second;
}
G void g_pop(void *m) { rec("guest PopPage %s", where(m).c_str()); }
G void g_clear_players(void *n) { rec("guest ClearNetPlayers %s", where(n).c_str()); }
G void g_clear_tracker(void *t) { rec("guest ClearTracker %s", where(t).c_str()); }
G uint8_t *g_create_event(int type) {
    rec("guest CreateEvent %d", type);
    if (st.event_null) return nullptr;
    const uint64_t off = type == 46 ? EV46 : EV79;
    std::memset(mem + off, 0, 0x40);
    put<uint64_t>(off, addr(EVVT));
    mem[off + 0x0a] = st.event_refs;
    return mem + off;
}
G void g_msg_apply(uint8_t *ev, uint8_t *conn) {
    const uint32_t code = get<uint32_t>(ev - mem + 0xc), size = get<uint32_t>(ev - mem + 0x18);
    uint64_t p;
    std::memcpy(&p, ev + 0x10, 8);
    bool zero = true;
    for (int i = 0; i < 0x200; i++)
        if ((i < 0x160 || i >= 0x164) && conn[i]) zero = false;
    uint32_t sender;
    std::memcpy(&sender, conn + 0x160, 4);
    rec("guest MessageApply ev %s code 0x%x size %u payload [%s] payload-in-image %d conn aligned %d zero %d sender %u",
        where(ev).c_str(), code, size, p ? hx::ss::hex(reinterpret_cast<uint8_t *>(p), size).c_str() : "NULL",
        p >= addr(0) && p < addr(hx::kImageSize) ? 1 : 0, (uintptr_t(conn) & 15) == 0 ? 1 : 0, zero ? 1 : 0, sender);
}
G void g_send_all(void *n, uint8_t *ev, uint32_t mask) {
    rec("guest SendEventAll %s ev %s id %u mask 0x%x", where(n).c_str(), where(ev).c_str(), get<uint32_t>(ev - mem + 0xc), mask);
}
G void g_ev_dtor(uint8_t *ev) { rec("guest event dtor %s refs %u", where(ev).c_str(), ev[0x0a]); }
G void g_ev_apply(uint8_t *ev, void *c) { rec("guest event Apply %s conn %s refs %u", where(ev).c_str(), where(c).c_str(), ev[0x0a]); }
G uint8_t *g_request(void *t, const char *name, int z) {
    rec("guest RequestPackage %s [%s] %d", where(t).c_str(), name ? name : "(NULL)", z);
    auto it = st.request.find(name ? name : "");
    return it == st.request.end() ? nullptr : mem + it->second;
}
G bool g_loaded(void *t) {
    rec("guest PackagesLoaded %s", where(t).c_str());
    return st.packages_loaded;
}
G bool g_busy() {
    rec("guest LoaderBusy");
    return st.loader_busy;
}
G int g_present(void *n) {
    rec("guest PresentPlayers %s", where(n).c_str());
    return st.present_players;
}
G void g_setnetgame(void *n, int16_t game, const void *p, int m) {
    rec("guest SetNetGame %s %d %s %d", where(n).c_str(), game, where(p).c_str(), m);
}
G void g_menu_reset(void *m) { rec("guest MenuReset %s", where(m).c_str()); }
G void g_team_colour(uint8_t *out, void *n, int team, int variant) {
    bool zero = true;
    for (int i = 0; i < 0x20; i++)
        if (out[i]) zero = false;
    rec("guest GetTeamColor out aligned %d zero %d %s team %d variant %d", (uintptr_t(out) & 15) == 0 ? 1 : 0,
        zero ? 1 : 0, where(n).c_str(), team, variant);
    std::memcpy(out + 0xc, &st.team_colour, 4);
}
G void g_owner_changed(void *p) { rec("guest OwnerChanged %s", where(p).c_str()); }
G void g_abort_orig(void *self) { rec("guest original abort %s", where(self).c_str()); }
template <int I> G uint64_t g_orig(void *s) {
    rec("guest original update %d state %s", I, where(s).c_str());
    return 0x5000 + I;
}

constexpr uint64_t kSlots[10] = {0x100ac58, 0x100ac18, 0x100abd8, 0x100ab98, 0x100ab58,
                                 0x100ab18, 0x100aa98, 0x100aa58, 0x100aa18, 0x100a9d8};
constexpr uint64_t kOrig[10] = {0x5f2ea0, 0x5f2dc0, 0x5f26d0, 0x5f14d0, 0x5f1390,
                                0x5f1000, 0x5f0ec0, 0x5f0b30, 0x5f0a90, 0x5eff30};
void *const kOrigFn[10] = {(void *)g_orig<0>, (void *)g_orig<1>, (void *)g_orig<2>, (void *)g_orig<3>, (void *)g_orig<4>,
                           (void *)g_orig<5>, (void *)g_orig<6>, (void *)g_orig<7>, (void *)g_orig<8>, (void *)g_orig<9>};
constexpr uint64_t kAbortSlot = 0x103fa70;

// ---- image helpers ----------------------------------------------------------------------------------------
uint32_t fnv(uint32_t id) {
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 4; ++i) h = (h ^ ((id >> (8 * i)) & 0xff)) * 0x1000193u;
    return h;
}
int nodes_used = 0;
// a package the table finds (hash node), with state and flags
void add_pkg(uint32_t id, uint64_t p, int32_t state = 7, uint16_t flags = 0x80) {
    const uint64_t node = NODES + uint64_t(nodes_used++) * 0x20;
    put<uint32_t>(node + 0x10, id);
    put<uint64_t>(node + 0x18, addr(p));
    const uint64_t b = BUCKETS + 8 * (fnv(id) % kBuckets);
    put<uint64_t>(node, get<uint64_t>(b));
    put<uint64_t>(b, addr(node));
    put<int32_t>(p + 0x30, state);
    put<uint16_t>(p + 0x18, flags);
}
uint64_t str_used = 0;
uint64_t str(const char *s) {
    const uint64_t o = STR + str_used;
    std::memcpy(mem + o, s, std::strlen(s) + 1);
    str_used += (std::strlen(s) + 16) & ~15ull;
    return o;
}
void game(int i, uint32_t level, int32_t type, uint32_t second = 0) {
    put<uint32_t>(GAMESREC + 0x90 * i + 4, level);
    put<int32_t>(GAMESREC + 0x90 * i + 0x0c, type);
    put<uint32_t>(GAMESREC + 0x90 * i + 0x78, second);
}
void level(int i, uint32_t id, const char *name) {
    put<uint32_t>(LEVELSREC + 0x38 * i, id);
    put<uint64_t>(LEVELSREC + 0x38 * i + 0x18, name ? addr(str(name)) : 0);
}
void pages(std::initializer_list<uint64_t> ps) {
    int n = 0;
    for (uint64_t p : ps) put<uint64_t>(MENU + 0x9d0 + 8 * n++, p ? addr(p) : 0);
    put<int32_t>(MENU + 0x1010, n - 1);
}
void cur_game(int16_t g) { put<int16_t>(NGM + 0x1ca0, g); }
void netsm(int32_t s) { put<int32_t>(NETSM, s); }
void mode(int32_t m) { put<int32_t>(NETINFO + 0x2c4, m); }
void env(const char *k, const char *v) { _putenv_s(k, v); }

void base_image() {
    hx::clear();
    nodes_used = 0;
    str_used = 0;
    static const unsigned char ret[] = {0xc3, 0x66, 0x66, 0x66, 0x66};
    for (uint64_t o : {0x5eb160ull, 0x659bb0ull, 0x659bc0ull, 0x5eb780ull}) std::memcpy(mem + o, ret, 5);
    static const unsigned char lea[] = {0x48, 0x8d, 0x3d, 0x11, 0x22, 0x33, 0x44};
    std::memcpy(mem + 0x5eb180, lea, sizeof lea);
    for (int i = 0; i < 10; i++) {
        hx::stub(kOrig[i], kOrigFn[i]);
        put<uint64_t>(kSlots[i], addr(kOrig[i]));
    }
    hx::stub(0x5e9f10, (void *)g_abort_orig);
    put<uint64_t>(kAbortSlot, addr(0x5e9f10));
    hx::stub(0xa15e70, (void *)g_lookup);
    hx::stub(0x5cc6f0, (void *)g_pop);
    hx::stub(0x63db90, (void *)g_clear_players);
    hx::stub(0x662a30, (void *)g_clear_tracker);
    hx::stub(0x6180f0, (void *)g_create_event);
    hx::stub(0x62dc50, (void *)g_msg_apply);
    hx::stub(0x644ab0, (void *)g_send_all);
    hx::stub(0x56ad90, (void *)g_request);
    hx::stub(0x56ce50, (void *)g_loaded);
    hx::stub(0xa18010, (void *)g_busy);
    hx::stub(0x63ffb0, (void *)g_present);
    hx::stub(0x63dc30, (void *)g_setnetgame);
    hx::stub(0x5cd1e0, (void *)g_menu_reset);
    hx::stub(0x64aeb0, (void *)g_team_colour);
    hx::stub(0x5e9f30, (void *)g_owner_changed);
    hx::stub_ptr(EVVT + 0x08, CODE + 0x00, (void *)g_ev_dtor);
    hx::stub_ptr(EVVT + 0x20, CODE + 0x20, (void *)g_ev_apply);
    static const unsigned char a1[] = {0x89, 0xcb, 0x41, 0x89, 0xd6, 0x49, 0x89, 0xf5, 0x49, 0x89, 0xfc};
    static const unsigned char a2[] = {0xc7, 0x05, 0xc7, 0x3f, 0xe8, 0x00, 0x4b, 0x2a, 0x02, 0x6a};
    std::memcpy(mem + 0x64aec1, a1, sizeof a1);
    std::memcpy(mem + 0x64af57, a2, sizeof a2);
    // menu: wait text object, page stack with PAGE_MULTIPLAYER, an empty page handler table
    put<uint64_t>(MENU + 0x1828, addr(WAITOBJ));
    put<uint32_t>(PAGE_MP + 0x10, 0x53cbb5f2);
    put<uint32_t>(PAGE_LEAVE + 0x10, 0xb808d03c);
    put<uint32_t>(PAGE_OTHER + 0x10, 0x12345678);
    pages({PAGE_MP});
    // package table: 7 buckets, an end node
    put<uint64_t>(PT + 0x10, addr(BUCKETS));
    put<int32_t>(PT + 0x4c, kBuckets);
    put<uint64_t>(BUCKETS + 8 * kBuckets, addr(ENDNODE));
    put<uint32_t>(ENDNODE + 0x10, 0xffffffff);
    // script tables: rank (5 thresholds), *net-games* (4), *levels* (4)
    put<int32_t>(RANK + 0x50, 5);
    put<uint64_t>(RANK + 0x58, addr(RANK + 0x100));
    const int32_t th[] = {0, 100, 500, 2000, 10000};
    std::memcpy(mem + RANK + 0x100, th, sizeof th);
    put<int32_t>(NPD + 0xd8, 600);   // rank 2
    put<int32_t>(GAMES, 4);
    put<uint64_t>(GAMES + 8, addr(GAMESREC));
    game(0, 0xa000, 1, 0xb000);
    game(1, 0xa001, 2, 0xb001);
    game(2, 0xa002, 1, 0xb002);
    game(3, 0xa003, 4, 0xb003);
    put<int32_t>(LEVELS, 4);
    put<uint64_t>(LEVELS + 8, addr(LEVELSREC));
    level(0, 0xa000, "first-name");
    level(1, 0xa001, "plaza-mp");
    level(2, 0xa000, "ice-cave-multi-pak");
    level(3, 0xa002, nullptr);
    cur_game(-1);
}

void base_state() {
    st = hx::ss::State();
    st.lookup = {{0xc85e199d, RANK}, {0x3b9a067d, GAMES}, {0x0dec97a4, LEVELS}};
    st.request = {{"net", pkg(0)}, {"net-u2mp", pkg(1)}};
}

// ---- calling the module -----------------------------------------------------------------------------------
bool host_ptr(uint64_t v) { return v && !(v >= addr(0) && v < addr(hx::kImageSize)); }

void entry(uint64_t off) {
    rec("== call 0x%llx", (unsigned long long)off);
    reinterpret_cast<void(G *)(void *, void *, void *)>(mem + off)(nullptr, nullptr, nullptr);
}
void enter() { entry(0x5eb160); }
void start_match() { entry(0x5eb780); }
void leave() { entry(0x5eb180); }

uint32_t handler_count() { return get<uint32_t>(MENU + 0x24f0); }
uint64_t our_handler() {
    for (uint32_t i = 0; i < handler_count() && i < 40; i++) {
        const uint64_t v = get<uint64_t>(MENU + 0x24f8 + 0x18 * i + 8);
        if (host_ptr(v)) return v;
    }
    return 0;
}
void page_event(uint32_t e) {
    const uint64_t h = our_handler();
    rec("== page event %u%s", e, h ? "" : " (no handler)");
    if (!h) return;
    uint32_t ev[4] = {e, 0, 0, 0};
    reinterpret_cast<void(G *)(uint32_t *)>(h)(ev);
}
void page_event_null() {
    const uint64_t h = our_handler();
    rec("== page event NULL");
    if (h) reinterpret_cast<void(G *)(uint32_t *)>(h)(nullptr);
}
void advance(int n = 1) {
    for (int i = 0; i < n; i++) {
        st.clock += 1.0 / 60;
        hx::fake_clock_ns += 16666667;
    }
}
void frame(int n = 1) {
    for (int i = 0; i < n; i++) {
        advance();
        page_event(6);
    }
}
void state(int i, uint64_t sp = STATEOBJ) {
    const uint64_t v = get<uint64_t>(kSlots[i]);
    if (!host_ptr(v)) {
        rec("== state %d not wrapped", i);
        return;
    }
    const uint64_t r = reinterpret_cast<uint64_t(G *)(void *)>(v)(mem + sp);
    rec("== state %d -> 0x%llx", i, (unsigned long long)r);
}
void sframe(int i, int n = 1) {   // match frames: the state wrapper drives the network
    for (int k = 0; k < n; k++) {
        advance();
        state(i);
    }
}
void abort_from_guest() {
    const uint64_t v = get<uint64_t>(kAbortSlot);
    rec("== abort from guest (slot %s)", host_ptr(v) ? "wrapped" : "not wrapped");
    unsigned char t[] = {0x48, 0x83, 0xec, 0x08, 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0, 0x48, 0x83, 0xc4, 0x08, 0xc3};
    std::memcpy(t + 6, &v, 8);
    std::memcpy(mem + THUNK, t, sizeof t);
    reinterpret_cast<void(G *)(void *)>(mem + THUNK)(mem + SELF);
    std::memset(mem + THUNK, 0, sizeof t);
}
__attribute__((noinline)) void abort_from_host() {
    const uint64_t v = get<uint64_t>(kAbortSlot);
    rec("== abort from host");
    if (host_ptr(v)) {
        reinterpret_cast<void(G *)(void *)>(v)(mem + SELF);
        // the raw host return address differs per build only by where this function is: it is the same function
    }
}

std::map<uint64_t, int> text_ids;   // roster text buffers by first use (per build)
void query(uint64_t fn, uint32_t op, uint32_t value, uint64_t arg) {
    std::memset(mem + Q, 0xee, 0x50);
    put<uint32_t>(Q, op);
    put<uint32_t>(Q + 4, value);
    put<uint64_t>(Q + 0x48, arg);
    reinterpret_cast<void(G *)(void *)>(mem + fn)(mem + Q);
    const uint64_t p = get<uint64_t>(Q + 0x40);
    std::string t;
    if (p == 0xeeeeeeeeeeeeeeeeull) t = "untouched";
    else if (!p) t = "NULL";
    else {
        if (!text_ids.count(p)) text_ids[p] = int(text_ids.size());
        t = "buffer#" + std::to_string(text_ids[p]) + " [" + reinterpret_cast<const char *>(p) + "]";
    }
    rec("query 0x%llx op %u value %u arg %llu -> [%s] text %s", (unsigned long long)fn, op, value,
        (unsigned long long)arg, hx::ss::hex(mem + Q, 0x40).c_str(), t.c_str());
    put<uint64_t>(Q + 0x40, 0);
}

void inst(const Api &a) {
    int n = 0;
    a.install(n);
    rec("installed %d", n);
}
void dump(const Api &a) {
    std::string rows;
    for (int r = -1; r <= 11; r++) {
        uint32_t id = 0xdead;
        std::string name = "-";
        const bool ok = a.member_at(r, id, name);
        if (ok) rows += " " + std::to_string(r) + ":" + std::to_string(id) + "/" + name;
    }
    std::string ranks;
    for (uint32_t id = 0; id <= 12; id++) {
        std::string n;
        if (a.member_name(id, n)) ranks += " " + std::to_string(id) + "=" + n + "/r" + std::to_string(a.member_rank(id));
    }
    rec("dump: count %d local_id %u is_host %d name [%s] rows%s names%s", a.member_count(), a.local_id(), a.is_host() ? 1 : 0,
        a.local_name().c_str(), rows.c_str(), ranks.c_str());
}

void in_msg(uint8_t slot, std::vector<uint8_t> d) { st.incoming.push_back({false, slot, false, std::move(d)}); }
void in_sess(uint8_t slot, std::vector<uint8_t> d) {
    d.insert(d.begin(), 1);
    in_msg(slot, std::move(d));
}
void in_peer(uint8_t slot, bool joined) { st.incoming.push_back({true, slot, joined, {}}); }
std::vector<uint8_t> info(const std::string &name, int len = -1, std::vector<uint8_t> tail = {}) {
    std::vector<uint8_t> b{1, uint8_t(len < 0 ? name.size() : len)};
    b.insert(b.end(), name.begin(), name.end());
    b.insert(b.end(), tail.begin(), tail.end());
    return b;
}

// common flows
void host_party(const Api &a, std::initializer_list<int> peers = {}) {
    inst(a);
    enter();
    for (int s : peers) in_peer(uint8_t(s), true);
    frame();
}
void client_party(const Api &a) {   // setup must call client_env()
    inst(a);
    enter();
    in_peer(0, true);
    frame();
}
void client_env() {
    env("UNCHARTED_U2MP_JOIN", "10.0.0.5:27300");
    st.local_slot = 2;
}

// ---- the scenario runner ----------------------------------------------------------------------------------
struct Scenario {
    std::string name;
    std::function<void()> setup;
    std::function<void(const Api &)> body;
};
std::vector<Scenario> all;
void add(std::string name, std::function<void()> setup, std::function<void(const Api &)> body) {
    all.push_back({std::move(name), std::move(setup), std::move(body)});
}

const char *kEnv[] = {"UNCHARTED_U2MP_NAME", "UNCHARTED_U2MP_PORT", "UNCHARTED_U2MP_JOIN", "UNCHARTED_U2MP_PRELOAD",
                      "UNCHARTED_U2MP_HOST_MIGRATION"};

struct Result {
    std::vector<std::string> calls;
    std::vector<uint8_t> img;
};
Result run_side(const Scenario &s, const Api &a, const std::vector<uint8_t> &base) {
    std::printf("-- %s build\n", a.tag);
    std::fflush(stdout);
    hx::restore(base);
    hx::bind_both();
    base_state();
    for (const char *k : kEnv) _putenv_s(k, "");
    nodes_used = 0;
    str_used = 0x800;
    text_ids.clear();
    hx::fake_clock_ns = 1000000000;
    std::srand(1);
    hx::calls.clear();
    hx::recording = true;
    if (s.setup) s.setup();
    s.body(a);
    // host pointers in guest memory, by meaning: the page handler entries, state slots and abort slot
    for (uint32_t i = 0; i < 40; i++) {
        const uint64_t o = MENU + 0x24f8 + 0x18 * i + 8;
        if (host_ptr(get<uint64_t>(o))) put<uint64_t>(o, 0x484f5354484f5354ull);
    }
    for (uint64_t o : kSlots)
        if (host_ptr(get<uint64_t>(o))) put<uint64_t>(o, 0x484f5354484f5354ull);
    if (host_ptr(get<uint64_t>(kAbortSlot))) put<uint64_t>(kAbortSlot, 0x484f5354484f5354ull);
    Result r{hx::calls, hx::snapshot()};
    return r;
}

int run_one(size_t i) {
    const Scenario &s = all[i];
    Api old_api, new_api;
    u2mp_old::hx_session_api(old_api);
    u2mp::hx_session_api(new_api);
    old_api.tag = "old";
    new_api.tag = "new";
    hx::init();
    base_image();
    std::vector<uint8_t> base(mem, mem + hx::kImageSize);
    Result o = run_side(s, old_api, base);
    Result n = run_side(s, new_api, base);
    const bool ok = hx::same(s.name.c_str(), o.calls, n.calls, &o.img, &n.img);
    if (std::getenv("HX_SESSION_TRACE")) {
        for (auto &c : o.calls) std::printf("  old| %s\n", c.c_str());
        for (auto &c : n.calls) std::printf("  new| %s\n", c.c_str());
    }
    std::printf("%s %s (%zu calls)\n", ok ? "PASS" : "FAIL", s.name.c_str(), o.calls.size());
    return ok ? 0 : 1;
}

// ===========================================================================================================
void scenarios() {
    // ---------------- install ----------------
    add("01 install: all bytes as expected", nullptr, [](const Api &a) {
        inst(a);
        for (int i = 0; i < 10; i++) state(i);
        abort_from_guest();
    });
    add("02 install: 0x659bc0 not a ret stub", [] { mem[0x659bc0 + 2] = 0x90; }, [](const Api &a) { inst(a); });
    add("02b install: LEAVE_MULTIPLAYER bytes differ", [] { mem[0x5eb182] = 0x05; }, [](const Api &a) { inst(a); });
    add("02c install: replace_function fails for Start Match", [] { hx::fail_offsets = {0x5eb780}; },
        [](const Api &a) { inst(a); });
    add("03 install: gamenet::install false", [] { st.gamenet_install = false; }, [](const Api &a) {
        inst(a);
        state(0);
    });
    add("04 install: one state slot differs", [] { put<uint64_t>(kSlots[6], addr(0x123456)); }, [](const Api &a) {
        inst(a);
        state(0);
        abort_from_guest();
    });
    add("05 install: abort slot holds something else", [] { put<uint64_t>(kAbortSlot, addr(0x5e9f11)); },
        [](const Api &a) {
            inst(a);
            state(3);
        });

    // ---------------- Multiplayer entry ----------------
    add("06 entry: LAN host", nullptr, [](const Api &a) {
        inst(a);
        enter();
        dump(a);
        frame();
    });
    add("06b entry: LAN host, net-u2mp extra, net package missing, preload partly present/requested", [] {
        st.extra_net = true;
        st.request = {{"net-u2mp", pkg(1)}, {"rpg", pkg(5)}, {"shotgun", pkg(6)}};
        add_pkg(sid("beretta-pistol"), pkg(7));
        add_pkg(sid("m32-grenade-launcher"), pkg(8));
        add_pkg(sid("machine-gun"), pkg(9));
    }, [](const Api &a) {
        inst(a);
        enter();
    });
    for (const char *p : {"0", "-5", "70000", "abc", "27300"})
        add(std::string("07 entry: UNCHARTED_U2MP_PORT=") + p, [p] { env("UNCHARTED_U2MP_PORT", p); },
            [](const Api &a) {
                inst(a);
                enter();
            });
    add("08 entry: host cannot listen", [] { st.host_ok = false; }, [](const Api &a) {
        inst(a);
        netsm(5);
        enter();
        dump(a);
        frame();
        enter();
    });
    add("09 entry: LAN join valid", [] { client_env(); }, [](const Api &a) {
        inst(a);
        enter();
        dump(a);
        frame(2);
        dump(a);
    });
    add("09b entry: LAN join unparsable", [] {
        env("UNCHARTED_U2MP_JOIN", "x");
        st.parse_ok = false;
    }, [](const Api &a) {
        inst(a);
        enter();
        dump(a);
    });
    add("09c entry: LAN join fails", [] {
        env("UNCHARTED_U2MP_JOIN", "10.1.2.3");
        env("UNCHARTED_U2MP_PORT", "4000");
        st.join_ok = false;
    }, [](const Api &a) {
        inst(a);
        enter();
    });
    add("10 entry: online link", [] {
        st.link = true;
        env("UNCHARTED_U2MP_JOIN", "10.0.0.5");
    }, [](const Api &a) {
        inst(a);
        enter();
        dump(a);
    });
    add("10b entry: online link, host_link false", [] {
        st.link = true;
        st.host_link_ok = false;
    }, [](const Api &a) {
        inst(a);
        enter();
        dump(a);
    });
    add("11 entry: second entry while active", nullptr, [](const Api &a) {
        inst(a);
        enter();
        netsm(16);
        mode(2);
        frame();
        enter();
        frame();
        dump(a);
    });
    add("12 entry: UNCHARTED_U2MP_PRELOAD=0", [] { env("UNCHARTED_U2MP_PRELOAD", "0"); }, [](const Api &a) {
        inst(a);
        enter();
    });
    add("12b entry: UNCHARTED_U2MP_PRELOAD=00 (not exactly 0)", [] { env("UNCHARTED_U2MP_PRELOAD", "00"); },
        [](const Api &a) {
            inst(a);
            enter();
        });
    add("13 entry: handler table full", [] {
        put<uint32_t>(MENU + 0x24f0, 32);
        for (int i = 0; i < 32; i++) put<uint64_t>(MENU + 0x24f8 + 0x18 * i + 8, addr(0x1000 + i));
    }, [](const Api &a) {
        inst(a);
        enter();
        frame();
    });
    add("13b entry: handler table with other entries", [] {
        put<uint32_t>(MENU + 0x24f0, 2);
        for (int i = 0; i < 2; i++) {
            put<uint32_t>(MENU + 0x24f8 + 0x18 * i, 0x777 + i);
            put<uint32_t>(MENU + 0x24f8 + 0x18 * i + 4, 0xabcdef01);
            put<uint64_t>(MENU + 0x24f8 + 0x18 * i + 8, addr(0x1000 + i));
            put<uint64_t>(MENU + 0x24f8 + 0x18 * i + 0x10, 0x55);
        }
        // stale bytes where ours goes (+4..+7 must stay)
        put<uint32_t>(MENU + 0x24f8 + 0x18 * 2, 0x999);
        put<uint32_t>(MENU + 0x24f8 + 0x18 * 2 + 4, 0x13572468);
        put<uint64_t>(MENU + 0x24f8 + 0x18 * 2 + 0x10, 0x66);
    }, [](const Api &a) {
        inst(a);
        enter();
        frame();
    });
    add("14 entry: UNCHARTED_U2MP_NAME longer than 31 bytes",
        [] { env("UNCHARTED_U2MP_NAME", "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789xyz"); }, [](const Api &a) {
            inst(a);
            dump(a);
            enter();
            dump(a);
            in_peer(1, true);
            frame();
            start_match();
        });

    // ---------------- peers and messages (host) ----------------
    add("15 host: peer joins slot 3", nullptr, [](const Api &a) {
        host_party(a, {3});
        dump(a);
        frame(2);
    });
    add("15b host: ten peers (11 members, 10 records / trackers)", nullptr, [](const Api &a) {
        host_party(a, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
        dump(a);
        start_match();
        query(0x659bb0, 7, 0, 9);
        query(0x659bb0, 1, 0, 19);
        a.send_message(-1, {0x11, 1}, 4);
        a.send_message(-1, {0x11, 2}, -1);
        a.send_message(5, {0x11, 3}, -1);
    });
    for (int sm : {16, 17})
        for (uint64_t room : {0ull, 0x1122334455667788ull})
            add("16 host online: peer joins, match+0x99=1, NET_SM " + std::to_string(sm) + ", room " + std::to_string(room != 0),
                [room] {
                    st.link = true;
                    st.online_active = true;
                    st.match_room = room;
                    st.my_npid = "HostNp";
                },
                [sm](const Api &a) {
                    host_party(a);
                    mem[MATCH + 0x99] = 1;
                    netsm(sm);
                    in_peer(4, true);
                    frame();
                });
    add("16b host online: peer joins, match+0x99=0, NET_SM 16", [] {
        st.link = true;
        st.online_active = true;
    }, [](const Api &a) {
        host_party(a);
        netsm(16);
        in_peer(4, true);
        frame();
    });
    add("17 host: peer leaves", nullptr, [](const Api &a) {
        host_party(a, {2, 3});
        in_sess(3, info("Carol", -1, {4, 1}));
        frame();
        in_peer(3, false);
        frame();
        dump(a);
    });
    add("17b host: peer leaves, event not created", [] { st.event_null = true; }, [](const Api &a) {
        host_party(a, {3});
        in_peer(3, false);
        frame();
    });
    add("17c host: peer leaves, event keeps a reference", [] { st.event_refs = 2; }, [](const Api &a) {
        host_party(a, {3});
        in_peer(3, false);
        frame();
    });
    add("17d host: peer leaves an empty slot, match session up", nullptr, [](const Api &a) {
        host_party(a);
        put<uint64_t>(MATCH + 0x10, 7);
        in_peer(5, false);
        frame();
    });
    for (uint32_t fp : {0u, 0xabcdu, 0x1234u})
        add("18 host: Info full, fingerprint " + std::to_string(fp), [] { st.fingerprint = 0xabcd; },
            [fp](const Api &a) {
                host_party(a, {3});
                in_sess(3, info("Alice", -1, {7, 2, uint8_t(fp), uint8_t(fp >> 8), 0, 0}));
                frame();
                dump(a);
            });
    add("18b host: Info differing fingerprint, ours 0", nullptr, [](const Api &a) {
        host_party(a, {3});
        in_sess(3, info("Alice", -1, {7, 2, 9, 9, 9, 9, 5}));
        frame();
    });
    add("19 host: Info short variants", nullptr, [](const Api &a) {
        host_party(a, {2, 3, 4});
        in_sess(2, info("OnlyName"));
        in_sess(3, info("NameRank", -1, {9}));
        in_sess(4, info("Cut", 40));
        frame();
        dump(a);
        in_sess(2, {1});
        in_sess(0, info("FromHost", -1, {1, 1}));
        in_msg(11, {1, 1, 4, 'E', 'l', 'e', 'v', 3, 1});
        in_sess(5, info("UnusedSlot", -1, {6, 2}));
        frame();
        dump(a);
        in_sess(2, info("", 0, {3, 7}));
        in_sess(3, info("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-long-name", -1, {2, 1}));
        frame();
        dump(a);
    });
    add("19b client: Info arrives on a client (ignored)", [] { client_env(); }, [](const Api &a) {
        client_party(a);
        in_sess(0, info("Host", -1, {1, 1}));
        frame();
        dump(a);
    });
    add("20 host online: roster with Ids", [] {
        st.link = true;
        st.online_active = true;
        st.my_npid = "HostNpIdLongerThan16Chars";
        st.peer_addr = {{2, {0x0a000002, 5000}}, {3, {0x0a000003, 5001}}};
        st.npid_by_ip = {{0x0a000002, "Second"}, {0x0a000003, ""}};
    }, [](const Api &a) {
        host_party(a, {2, 3, 4});
        for (const char *n : {"HostNpIdLongerThan16Chars", "HostNpIdLongerTh", "Second", "", "x"})
            rec("net_id_of [%s] = %u", n, a.net_id_of(n));
    });

    // ---------------- messages (client) ----------------
    add("21 client: Roster, Teams, Roster again, bad Rosters", [] { client_env(); }, [](const Api &a) {
        client_party(a);
        put<uint64_t>(MATCH + 0x10, 9);
        in_sess(0, {2, 0, 4, 3, 'H', 'o', 's', 't', 2, 3, 1, 'M', 'e', 'X', 5, 2, 0, 'Q', 'Q'});
        frame();
        dump(a);
        in_sess(0, {6, 0, 1, 2, 2, 5, 1});
        frame();
        in_sess(0, {2, 0, 4, 3, 'H', 'o', 's', 't', 2, 3, 1, 'M', 'e', 'X'});
        frame();
        dump(a);
        query(0x659bb0, 17, 0, 10);
        in_sess(0, {2, 0, 1, 1, 'H', 11, 1, 1, 'Z', 2, 1, 1, 'Y'});
        frame();
        dump(a);
        in_sess(0, {2, 0, 1, 1, 'H', 2, 9, 1, 'Y'});
        frame();
        dump(a);
        in_sess(0, {2, 0, 1, 1, 'H', 3, 1});
        frame();
        dump(a);
        in_sess(0, {2});
        frame();
        dump(a);
    });
    add("22 client: Teams with slot 12 in the middle, odd length", [] { client_env(); }, [](const Api &a) {
        client_party(a);
        in_sess(0, {2, 0, 1, 1, 'H', 2, 1, 1, 'M', 5, 1, 1, 'F'});
        in_sess(0, {6, 0, 2, 12, 1, 5, 1, 2});
        frame();
        in_sess(0, {2, 0, 1, 1, 'H', 2, 1, 1, 'M', 5, 1, 1, 'F'});
        frame();
        mode(2);
        cur_game(0);
        for (uint64_t arg = 10; arg < 13; arg++) query(0x659bb0, 17, 0, arg);
    });
    add("23 client: Mode", [] { client_env(); }, [](const Api &a) {
        client_party(a);
        in_sess(0, {3, 2, 0, 0, 0});
        in_sess(0, {3, 1, 0, 0});
        in_sess(0, {3, 0xff, 0xff, 0xff, 0x7f, 9});
        frame();
    });
    add("23b client: Mode, event not created", [] {
        client_env();
        st.event_null = true;
    }, [](const Api &a) {
        client_party(a);
        in_sess(0, {3, 2, 0, 0, 0});
        frame();
    });
    add("23c host: Mode from a client ignored", nullptr, [](const Api &a) {
        host_party(a, {2});
        in_sess(2, {3, 2, 0, 0, 0});
        frame();
    });
    add("24 client: Kick from slot 0", [] { client_env(); }, [](const Api &a) {
        client_party(a);
        in_sess(0, {5});
        frame();
        dump(a);
        frame();
        pages({PAGE_MP, PAGE_LEAVE});
        page_event(6);
        dump(a);
        enter();
        dump(a);
    });
    add("24b client: Kick from another slot, host gets Kick", [] { client_env(); }, [](const Api &a) {
        client_party(a);
        in_sess(3, {5});
        frame(2);
        dump(a);
    });
    add("24c host: Kick from a client ignored", nullptr, [](const Api &a) {
        host_party(a, {2});
        in_sess(2, {5});
        frame(2);
        dump(a);
    });
    for (bool online : {false, true})
        add(std::string("25 client: Follow ") + (online ? "online (9 bytes)" : "LAN (1 byte)"), [] {
            client_env();
            put<uint32_t>(NGM + 0x10 + 0x20, 0x41414141);   // a tracker in use with another id
            put<uint32_t>(NGM + 0x10 + 0x120, 7);
        }, [online](const Api &a) {
            client_party(a);
            in_sess(0, {2, 0, 4, 1, 'H', 'o', 's', 't', 2, 2, 3, 'M', 'e'});
            if (online) in_sess(0, {4, 1, 2, 3, 4, 5, 6, 7, 8});
            else in_sess(0, {4});
            frame();
            in_sess(0, {4, 1, 2, 3, 4, 5, 6, 7});
            frame();
        });
    add("25b host: Follow from a client ignored", nullptr, [](const Api &a) {
        host_party(a, {2});
        in_sess(2, {4});
        frame();
    });
    add("26 client: Ids parse, truncated last entry", [] { client_env(); }, [](const Api &a) {
        client_party(a);
        in_sess(0, {8, 1, 3, 'a', 'b', 'c', 2, 0, 3, 4, 'x', 'y', 'z', 'w', 7, 9, 'q'});
        frame();
        for (const char *n : {"abc", "", "xyzw", "q", "x"}) rec("net_id_of [%s] = %u", n, a.net_id_of(n));
        in_sess(0, {8, 5, 3, 'x', 'y', 'z', 6, 3, 'x', 'y', 'z'});
        frame();
        for (const char *n : {"abc", "xyz"}) rec("net_id_of [%s] = %u", n, a.net_id_of(n));
        in_sess(0, {8, 4});
        frame();
        rec("net_id_of [xyz] = %u", a.net_id_of("xyz"));
    });
    add("26b host: Ids from a client ignored", [] {
        st.online_active = true;
        st.link = true;
        st.my_npid = "HostNp";
    }, [](const Api &a) {
        host_party(a, {2});
        in_sess(2, {8, 1, 3, 'a', 'b', 'c'});
        frame();
        rec("net_id_of [abc] = %u, [HostNp] = %u", a.net_id_of("abc"), a.net_id_of("HostNp"));
    });
    for (bool hostside : {false, true})
        add(std::string("27 dispatch: tags and types on ") + (hostside ? "host" : "client"), [hostside] {
            if (!hostside) client_env();
        }, [hostside](const Api &a) {
            if (hostside) host_party(a, {2});
            else client_party(a);
            const uint8_t f = hostside ? 2 : 0;
            in_sess(f, {7, 1, 2, 3});
            in_sess(f, {7});
            in_sess(f, {0x10, 4, 5});
            in_sess(f, {0x17});
            in_sess(f, {0xff, 1});
            in_sess(f, {9, 1});
            in_sess(f, {0x0f, 1});
            in_sess(f, {0, 1});
            in_sess(f, {});
            in_msg(f, {0, 0xaa, 0xbb});
            in_msg(f, {0});
            in_msg(f, {2, 5, 6});
            in_msg(f, {3, 1});
            in_msg(f, {});
            frame();
        });
    add("28 client: joined at local slot 0 (host id)", [] {
        env("UNCHARTED_U2MP_JOIN", "10.0.0.5");
        st.local_slot = 0;
    }, [](const Api &a) {
        client_party(a);
        dump(a);
    });
    add("28b client: peer events for other slots ignored", [] { client_env(); }, [](const Api &a) {
        inst(a);
        enter();
        in_peer(3, true);
        in_peer(11, true);
        in_peer(3, false);
        frame();
        dump(a);
    });
    add("28c client: rehost pending, joined finishes it", [] {
        client_env();
        st.link = false;
        st.online_active = true;
    }, [](const Api &a) {
        client_party(a);
        in_sess(0, {2, 0, 1, 1, 'H', 2, 1, 1, 'M', 4, 1, 1, 'O'});
        frame();
        Addr owner{0x0a000009, 9000};
        st.local_slot = 2;
        a.online_rehost(&owner, 5);
        dump(a);
        in_peer(0, true);
        frame();
        dump(a);
        in_peer(0, true);
        frame();
    });
    add("29 client: host lost, host change expected", [] {
        client_env();
        st.host_change = true;
    }, [](const Api &a) {
        client_party(a);
        in_peer(0, false);
        frame(2);
        dump(a);
    });
    add("29b client: host lost, online takes it (calls back online_reopen)", [] {
        client_env();
        st.host_lost_taken = true;
    }, [](const Api &a) {
        st.on_host_lost_cb = [&a] {
            Addr owner{0x0a000004, 27210};
            a.online_reopen(&owner);
        };
        client_party(a);
        in_peer(0, false);
        frame(2);
        dump(a);
    });
    add("29c client: host lost, plain", [] { client_env(); }, [](const Api &a) {
        client_party(a);
        in_peer(0, false);
        frame();
        dump(a);
        frame();
        dump(a);
        enter();
        dump(a);
    });

    // ---------------- frames ----------------
    add("30 frames: event 5, NULL, LEAVING_GAME on top", nullptr, [](const Api &a) {
        host_party(a);
        page_event(5);
        page_event_null();
        pages({PAGE_MP, PAGE_LEAVE});
        put<int32_t>(NETINFO + 0x34, 5);
        page_event(5);
        frame();
        put<int32_t>(NETINFO + 0x34, 5);
        frame();
        mem[MATCH + 0xc8] = 0;
        sframe(3);
        pages({PAGE_LEAVE, PAGE_MP});
        mem[MATCH + 0xc8] = 0;
        frame();
        put<int32_t>(MENU + 0x1010, 0x41);
        put<uint64_t>(MENU + 0x9d0 + 8 * 0x41, addr(PAGE_LEAVE));
        frame();
        put<int32_t>(MENU + 0x1010, 0x40);
        put<uint64_t>(MENU + 0x9d0 + 8 * 0x40, addr(PAGE_LEAVE));
        frame();
        put<uint64_t>(MENU + 0x9d0 + 8 * 0x40, 0);
        frame();
    });
    add("31 frames: Multiplayer page closed for 29 / 30 events", nullptr, [](const Api &a) {
        host_party(a);
        pages({PAGE_OTHER});
        frame(29);
        pages({PAGE_OTHER, PAGE_MP});
        frame();
        pages({0, PAGE_OTHER});
        frame(29);
        netsm(17);
        frame();
        netsm(18);
        frame(29);
        put<int32_t>(MENU + 0x1010, 250);
        frame(3);
        put<int32_t>(MENU + 0x1010, -1);
        frame(3);
        put<int32_t>(MENU + 0x1010, 199);
        for (int i = 0; i < 200; i++) put<uint64_t>(MENU + 0x9d0 + 8 * i, i == 150 ? addr(PAGE_MP) : 0);
        frame(31);
        put<uint64_t>(MENU + 0x9d0 + 8 * 150, 0);
        frame(30);
        dump(a);
        frame();
    });
    add("32 frames: same clock twice", nullptr, [](const Api &a) {
        host_party(a);
        page_event(6);
        hx::fake_clock_ns += 5000000;
        page_event(6);
        state(3);
        advance();
        state(3);
        page_event(6);
    });
    add("33 frames: net package log, frame-rate log windows", [] {
        add_pkg(sid("net"), pkg(0), 5, 0x41);
        put<uint64_t>(NETIMG, addr(NETIMGOBJ));
        put<uint16_t>(NETIMGOBJ + 2, 12);
    }, [](const Api &a) {
        hx::recording = false;
        host_party(a);
        hx::recording = true;
        frame(2);
        hx::recording = false;
        frame(57);
        hx::recording = true;
        frame(2);   // frame 61: same values, no log
        put<int32_t>(pkg(0) + 0x30, 7);
        hx::recording = false;
        frame(58);
        hx::recording = true;
        frame(2);   // 121: state changed
        put<uint64_t>(NETIMG, 0);
        hx::recording = false;
        frame(58);
        hx::recording = true;
        frame(2);
        netsm(26);   // not the menus: no request
        put<uint64_t>(NETIMG, addr(NETIMGOBJ));
        put<uint16_t>(NETIMGOBJ + 2, 13);
        for (int k = 0; k < 20; k++) {
            hx::recording = false;
            frame(58);
            hx::recording = true;
            frame(2);
        }
    });
    add("33b frames: net package missing, then not created", [] { st.request.clear(); }, [](const Api &a) {
        host_party(a);
        netsm(3);
        hx::recording = false;
        frame(59);
        hx::recording = true;
        frame(2);
        netsm(12);
        hx::recording = false;
        frame(58);
        hx::recording = true;
        frame(2);
    });
    add("34 frames: host rank / team changes every 120 frames", nullptr, [](const Api &a) {
        host_party(a, {2});
        hx::recording = false;
        frame(5);
        hx::recording = true;
        frame(2);   // frame 7
        put<int32_t>(NPD + 0xd8, 2500);
        put<int32_t>(NPD + 0x114, 2);
        hx::recording = false;
        frame(118);
        hx::recording = true;
        frame(2);
        put<int32_t>(NPD + 0x114, 3);
        hx::recording = false;
        frame(118);
        hx::recording = true;
        frame(2);
        put<int32_t>(NPD + 0xd8, -5);
        put<int32_t>(NPD + 0x114, 1);
        hx::recording = false;
        frame(118);
        hx::recording = true;
        frame(2);
        put<int32_t>(NPD + 0xd8, 999999);
        hx::recording = false;
        frame(118);
        hx::recording = true;
        frame(2);
        dump(a);
    });
    add("34b frames: client rank / team changes every 120 frames", [] { client_env(); }, [](const Api &a) {
        client_party(a);
        hx::recording = false;
        frame(4);
        hx::recording = true;
        frame(2);
        put<int32_t>(NPD + 0x114, 1);
        hx::recording = false;
        frame(118);
        hx::recording = true;
        frame(2);
        put<int32_t>(NPD + 0xd8, 150);
        hx::recording = false;
        frame(118);
        hx::recording = true;
        frame(2);
        hx::recording = false;
        frame(118);
        hx::recording = true;
        frame(2);
    });
    add("34c frames: client before join (my id 0), rank table edge cases", [] {
        client_env();
        put<int32_t>(RANK + 0x50, 1);
    }, [](const Api &a) {
        inst(a);
        enter();
        hx::recording = false;
        frame(6);
        hx::recording = true;
        frame(2);
        rec("rank %d", a.rank());
        put<int32_t>(RANK + 0x50, 2);
        put<uint64_t>(RANK + 0x58, 0);
        rec("rank %d", a.rank());
        put<uint64_t>(RANK + 0x58, addr(RANK + 0x100));
        put<int32_t>(RANK + 0x50, 300);
        for (int i = 0; i < 300; i++) put<int32_t>(RANK + 0x100 + 4 * i, i * 10);
        for (int m : {-1, 0, 5, 2555, 2560, 2989, 2990, 5000}) {
            put<int32_t>(NPD + 0xd8, m);
            rec("rank(cash %d) %d", m, a.rank());
        }
        st.lookup.erase(0xc85e199d);
        rec("rank %d", a.rank());
    });
    add("35 frames: host mode changes", nullptr, [](const Api &a) {
        host_party(a);
        frame();
        mode(2);
        frame(2);
        mode(-7);
        frame();
        a.online_reopen(nullptr);
        frame();
    });
    add("35b frames: client never sends Mode", [] { client_env(); }, [](const Api &a) {
        client_party(a);
        mode(2);
        frame(2);
    });
    add("36 frames: NET_SM changes (host)", nullptr, [](const Api &a) {
        host_party(a);
        for (int s : {16, 35, 3, 3, 12, 35, 4, 3, 0, 18}) {
            mem[MATCH + 0x99] = 1;
            netsm(s);
            frame();
        }
    });
    add("36b frames: NET_SM 35 -> 3 on a client", [] { client_env(); }, [](const Api &a) {
        client_party(a);
        mem[MATCH + 0x99] = 1;
        netsm(35);
        frame();
        netsm(3);
        frame();
    });
    add("37 frames: net image fade", [] {
        add_pkg(sid("net"), pkg(0), 7);
        add_pkg(sid("net-u2mp"), pkg(1), 6);
        put<int32_t>(FRAMECTR, 1000);
        put<int32_t>(pkg(0) + 0x924, 900);
        put<int32_t>(pkg(1) + 0x924, 900);
    }, [](const Api &a) {
        host_party(a);
        put<int32_t>(pkg(0) + 0x924, 701);
        frame();
        put<int32_t>(pkg(0) + 0x924, 700);
        put<int32_t>(pkg(1) + 0x30, 7);
        put<int32_t>(FRAMECTR, -2147483000);
        put<int32_t>(pkg(1) + 0x924, 2147483000);
        frame();
    });
    add("37b frames: package table edge cases (end node, no buckets)", [] {
        put<uint32_t>(ENDNODE + 0x10, sid("net"));
        put<uint64_t>(BUCKETS + 8 * (fnv(sid("net")) % kBuckets), addr(ENDNODE));
        add_pkg(sid("net-u2mp"), pkg(1), 7);
        put<int32_t>(pkg(1) + 0x924, 5);
    }, [](const Api &a) {
        host_party(a);
        rec("package_by_id net %s", where(a.package_by_id(sid("net"))).c_str());
        rec("package_by_id net-u2mp %s", where(a.package_by_id(sid("net-u2mp"))).c_str());
        rec("package_by_id other %s", where(a.package_by_id(0x1234)).c_str());
        put<int32_t>(PT + 0x4c, 0);
        rec("package_by_id net-u2mp (count 0) %s", where(a.package_by_id(sid("net-u2mp"))).c_str());
        put<int32_t>(PT + 0x4c, -3);
        rec("package_by_id net-u2mp (count -3) %s", where(a.package_by_id(sid("net-u2mp"))).c_str());
        put<int32_t>(PT + 0x4c, kBuckets);
        put<uint64_t>(PT + 0x10, 0);
        rec("package_by_id net-u2mp (no buckets) %s", where(a.package_by_id(sid("net-u2mp"))).c_str());
        frame();
    });

    // ---------------- net clock ----------------
    add("38 net clock: created while active, ticks each frame", nullptr, [](const Api &a) {
        rec("ticks outside MP %u", a.net_ticks());
        hx::fake_clock_ns += 1000000000;
        rec("ticks outside MP %u", a.net_ticks());
        host_party(a);
        rec("ticks %u", a.net_ticks());
        for (int i = 0; i < 5; i++) {
            hx::fake_clock_ns += 123456789;
            frame();
            rec("ticks %u", a.net_ticks());
        }
        leave();
        hx::fake_clock_ns += 1000000000;
        page_event(6);
        rec("ticks after leave %u", a.net_ticks());
        sframe(4);
        rec("ticks after state %u", a.net_ticks());
        put<uint64_t>(CLOCKPTR, addr(OTHERCLOCK));
        put<uint32_t>(OTHERCLOCK + 0xc, 4321);
        sframe(4);
        rec("ticks other %u", a.net_ticks());
        put<uint64_t>(CLOCKPTR, 0);
        rec("ticks own %u", a.net_ticks());
        sframe(4);
        rec("ticks own %u", a.net_ticks());
    });
    add("38b net clock: global set before", [] {
        put<uint64_t>(CLOCKPTR, addr(OTHERCLOCK));
        put<uint32_t>(OTHERCLOCK + 0xc, 77);
    }, [](const Api &a) {
        host_party(a);
        frame(3);
        rec("ticks %u", a.net_ticks());
        put<uint64_t>(CLOCKPTR, 0);
        rec("ticks own %u", a.net_ticks());
        frame();
        rec("ticks %u", a.net_ticks());
    });
    add("38c net clock: netheap_alloc NULL", [] { st.netheap_null = true; }, [](const Api &a) {
        hx::fake_clock_ns += 3000000000;
        host_party(a);
        frame(2);
        rec("ticks %u", a.net_ticks());
    });

    // ---------------- match states ----------------
    add("39 states: EstablishConnection with match-over bytes, trackers cleared", nullptr, [](const Api &a) {
        host_party(a, {2});
        mem[0x104f1c0] = 1;
        mem[0x14ce978] = 3;
        sframe(0);
        sframe(0);
        mem[0x14ce978] = 1;
        put<uint64_t>(NGM + 8, 0);
        sframe(0);
        put<uint64_t>(NGM + 8, 0);
        sframe(1);
        put<uint64_t>(NGM + 8, 0);
        sframe(2);
        put<uint64_t>(NGM + 8, 0);
        sframe(3);
    });
    add("39b states: trackers cleared while not active", nullptr, [](const Api &a) {
        inst(a);
        mem[0x104f1c0] = 2;
        sframe(0);
        sframe(2);
        for (int i = 3; i < 10; i++) sframe(i);
    });
    add("39c states: trackers full, names cut", [] {
        for (int i = 0; i < 10; i++) {
            put<uint8_t>(NGM + 0x10 + 0x2c0 * i + 0x20, 'x');
            put<uint32_t>(NGM + 0x10 + 0x2c0 * i + 0x120, 50 + i);
        }
        put<uint32_t>(NGM + 0x10 + 0x2c0 * 4 + 0x120, 3);
        env("UNCHARTED_U2MP_NAME", "");
    }, [](const Api &a) {
        host_party(a, {2, 3});
        in_sess(2, info("a-very-long-player-name-that-goes-over-sixty-four-bytes-of-tracker-name-field", -1, {1}));
        in_sess(3, info("", 0, {1}));
        frame();
        sframe(1);
        start_match();
    });
    add("40 states: LoadLevels diagnostics (entry, 10 s, packages not loaded)", [] {
        st.packages_loaded = false;
        st.loader_busy = true;
        put<uint64_t>(STATEOBJ + 8, addr(TASK));
        put<uint64_t>(TASK + 0x80, addr(TASKA));
        put<uint64_t>(TASKA + 0x88, addr(TASKB));
        put<uint32_t>(TASKB + 0x28, 0xfeedf00d);
        cur_game(1);
        // level list: 30 nodes (at most 24 shown, 496 characters)
        put<uint64_t>(LEVELHEAD, addr(LNODES));
        for (int i = 0; i < 30; i++) {
            const uint64_t n = LNODES + 0x100 * i;
            put<uint64_t>(n, i == 29 ? addr(LEVELHEAD) : addr(n + 0x100));
            put<int32_t>(n + 0x10, i % 2 ? -2147483647 - 1 : i);
            put<uint32_t>(n + 0x98, 0xc0de0000 + i);
        }
        // wait lists
        add_pkg(0x111, pkg(2), 7, 0x80);
        add_pkg(0x222, pkg(3), 6, 0x80);
        add_pkg(0x333, pkg(4), 7, 0x40);
        put<uint32_t>(PT + 0x5a98, 5);
        put<uint64_t>(PT + 0x5aa0, addr(WL0));
        const uint32_t ids0[] = {0x111, 0x222, 0x333, 0x444, 0x555};
        const uint32_t fl0[] = {0, 0, 2, 0, 1};
        for (int i = 0; i < 5; i++) {
            put<uint32_t>(WL0 + 0x10 * i + 8, ids0[i]);
            put<uint32_t>(WL0 + 0x10 * i + 0xc, fl0[i]);
        }
        put<uint32_t>(PT + 0x5aa8, 3);
        put<uint64_t>(PT + 0x5ab0, addr(WL1));
        const uint32_t ids1[] = {0x555, 0x222, 0x111};
        for (int i = 0; i < 3; i++) {
            put<uint32_t>(WL1 + 0x10 * i + 8, ids1[i]);
            put<uint32_t>(WL1 + 0x10 * i + 0xc, 1);
        }
        put<float>(0x14cec0c, 12.34f);
        put<int32_t>(NPD + 0x44, 3);
        put<int32_t>(NPD + 0x6c0, -1);
        mem[0x113a270] = 1;
        put<uint64_t>(NGM + 8, addr(NGM + 0x10));
        mem[NGM + 0x10 + 0x26c] = 9;
    }, [](const Api &a) {
        host_party(a);
        sframe(1);
        sframe(1);
        hx::fake_clock_ns += 9990000000;
        sframe(1);
        hx::fake_clock_ns += 10000000;
        sframe(1);
        st.packages_loaded = true;
        sframe(2);
        sframe(1);
        put<uint64_t>(STATEOBJ + 8, 0);
        put<uint64_t>(LEVELHEAD, 0);
        cur_game(4);
        sframe(2);
        sframe(1);
        put<uint64_t>(TASKA + 0x88, 0);
        put<uint64_t>(STATEOBJ + 8, addr(TASK));
        put<uint64_t>(LEVELHEAD, addr(LEVELHEAD));
        st.packages_loaded = false;
        put<uint32_t>(PT + 0x5a98, 300);
        put<uint64_t>(PT + 0x5ab0, 0);
        sframe(2);
        sframe(1);
    });
    add("40b states: LoadLevels second level with invalid tables", [] {
        cur_game(0);
        st.lookup.erase(0x3b9a067d);
    }, [](const Api &a) {
        inst(a);
        state(1);
        st.lookup[0x3b9a067d] = GAMES;
        put<uint64_t>(GAMES + 8, 0);
        state(2);
        state(1);
        put<uint64_t>(GAMES + 8, addr(GAMESREC));
        cur_game(4);
        state(2);
        state(1);
        cur_game(3);
        state(2);
        state(1);
    });
    add("41 states: missing <map>-pak, no file", [] {
        cur_game(0);
        st.translate_result = -1;
        put<uint32_t>(PT + 0x5a98, 4);
        put<uint64_t>(PT + 0x5aa0, addr(WL0));
        for (int i = 0; i < 4; i++) {
            put<uint32_t>(WL0 + 0x10 * i + 8, i == 2 ? 0x999 : sid("ice-cave-multi-pak"));
            put<uint32_t>(WL0 + 0x10 * i + 0xc, i == 1 ? 3 : 4);
        }
    }, [](const Api &a) {
        inst(a);
        state(1);
        state(1);
        put<uint32_t>(WL0 + 0xc, 0);
        state(1);
    });
    add("41b states: missing <map>-pak, file present: 20 s, then once", [] {
        cur_game(0);
        char path[1024];
        GetModuleFileNameA(nullptr, path, sizeof path);
        st.file_path = path;
        put<uint32_t>(PT + 0x5a98, 2);
        put<uint64_t>(PT + 0x5aa0, addr(WL0));
        for (int i = 0; i < 2; i++) put<uint32_t>(WL0 + 0x10 * i + 8, sid("ice-cave-multi-pak"));
    }, [](const Api &a) {
        inst(a);
        state(1);
        hx::fake_clock_ns += 19999999999;
        state(1);
        hx::fake_clock_ns += 1;
        state(1);
        state(1);
        put<uint32_t>(WL0 + 0xc, 0);
        hx::fake_clock_ns += 60000000000;
        state(1);
    });
    add("41c states: missing pak, file present, game A -> B -> A (log once per id?)", [] {
        cur_game(0);
        char path[1024];
        GetModuleFileNameA(nullptr, path, sizeof path);
        st.file_path = path;
        level(1, 0xa001, "other-map-pak");
    }, [](const Api &a) {
        inst(a);
        for (int g : {0, 1, 0}) {
            cur_game(int16_t(g));
            state(1);
            hx::fake_clock_ns += 21000000000;
            state(1);
            state(1);
        }
    });
    add("41d states: missing pak checks: not -pak, short, found, NULL name, invalid tables", [] {
        st.translate_result = -1;
        level(1, 0xa001, "pak");
        add_pkg(sid("other-pak"), pkg(3));
        put<uint32_t>(LEVELSREC + 0x38 * 3, 0xa005);
        put<uint64_t>(LEVELSREC + 0x38 * 3 + 0x18, addr(str("other-pak")));
        game(2, 0xa005, 1);
        game(3, 0xa002, 1);
        put<uint32_t>(PT + 0x5a98, 1);
        put<uint64_t>(PT + 0x5aa0, addr(WL0));
        put<uint32_t>(WL0 + 8, sid("pak"));
    }, [](const Api &a) {
        inst(a);
        for (int16_t g : {-1, 0, 1, 2, 3, 4}) {
            cur_game(g);
            state(1);
        }
        level(3, 0xa002, "ab");
        cur_game(3);
        state(1);
        level(3, 0xa002, "x-pak");
        put<uint64_t>(PT + 0x5aa0, 0);
        state(1);
        put<uint64_t>(PT + 0x5aa0, addr(WL0));
        level(0, 0xa009, "z-pak");
        level(1, 0xa009, "y-pak");
        level(2, 0xa009, "w-pak");
        game(0, 0xa009, 1);
        put<uint64_t>(LEVELSREC + 0x38 * 2 + 0x18, 0);   // last match has no name
        cur_game(0);
        state(1);
        st.lookup.erase(0x0dec97a4);
        state(1);
        st.lookup[0x0dec97a4] = LEVELS;
        put<uint64_t>(LEVELS + 8, 0);
        state(1);
    });
    add("42 states: Leave diagnostics", [] {
        put<float>(0x14cec0c, -0.05f);
        mem[0x104f1c0] = 1;
    }, [](const Api &a) {
        host_party(a);
        sframe(9);
        sframe(9);
        sframe(8);
        sframe(9);
        put<uint64_t>(NGM + 8, addr(NGM + 0x10 + 0x2c0));
        mem[NGM + 0x10 + 0x2c0 + 0x26c] = 4;
        hx::fake_clock_ns += 10000000000;
        sframe(9);
    });
    add("43 states: abort wrapper from guest and host code", nullptr, [](const Api &a) {
        inst(a);
        abort_from_guest();
        abort_from_host();
    });

    // ---------------- Start Match ----------------
    add("44 start: not hosting", [] { client_env(); }, [](const Api &a) {
        client_party(a);
        mode(2);
        start_match();
        a.create_match();
        mode(1);
        start_match();
    });
    add("44b start: hosting but not active (after kick-less teardown)", nullptr, [](const Api &a) {
        host_party(a);
        leave();
        mode(2);
        start_match();
        a.create_match();
    });
    add("44c start: mode 2, popup takes it", [] { st.leader_starting = true; }, [](const Api &a) {
        host_party(a, {2});
        mode(2);
        start_match();
    });
    add("44d start: mode 2, popup not installed: full create, LAN follow", [] {
        cur_game(1);
        put<int32_t>(0x113ddb0, 6);
    }, [](const Api &a) {
        host_party(a, {2, 5});
        mode(2);
        frame();
        start_match();
        dump(a);
        frame();
        a.create_match();
    });
    add("44e start: mode 0 / 3", nullptr, [](const Api &a) {
        host_party(a);
        mode(3);
        start_match();
    });
    add("45 start: online create_match_room true, then match ready", [] {
        st.link = true;
        st.online_active = true;
        st.create_room = true;
        st.match_room = 0xabcdef0123;
        cur_game(0);
    }, [](const Api &a) {
        host_party(a, {3});
        mode(2);
        start_match();
        rec("match+0x99 %u", mem[MATCH + 0x99]);
        a.online_match_ready(0xabcdef0123);
        rec("match+0x99 %u", mem[MATCH + 0x99]);
    });
    add("45b online_match_ready: not hosting / not active", [] { client_env(); }, [](const Api &a) {
        a.online_match_ready(0x55);
        client_party(a);
        a.online_match_ready(0x66);
    });
    add("46 start: mode 1 with matchmaking", [] { st.mm_installed = true; }, [](const Api &a) {
        host_party(a);
        mode(1);
        start_match();
        st.leader_starting = true;
        start_match();
    });
    add("47 start: mode 1, playlist type, DLC filter, rand pick", [] {
        st.playlist_type = 1;
        st.dlc_off = {2};
        put<int32_t>(GAMES, 4);
        game(3, 0xa003, 1);
    }, [](const Api &a) {
        host_party(a);
        mode(1);
        start_match();
        start_match();
        start_match();
    });
    add("47b start: mode 1, current game's type", [] { cur_game(1); }, [](const Api &a) {
        host_party(a);
        cur_game(1);
        mode(1);
        start_match();
        cur_game(-3);
        start_match();
        cur_game(3);
        start_match();
    });
    add("47c start: mode 1, no candidates", [] { st.playlist_type = 77; }, [](const Api &a) {
        host_party(a);
        mode(1);
        start_match();
    });
    add("47d start: mode 1, invalid tables", nullptr, [](const Api &a) {
        host_party(a);
        mode(1);
        st.lookup.erase(0x3b9a067d);
        start_match();
        st.lookup[0x3b9a067d] = GAMES;
        put<int32_t>(GAMES, 0);
        start_match();
        put<int32_t>(GAMES, -2);
        start_match();
        put<int32_t>(GAMES, 4);
        put<uint64_t>(GAMES + 8, 0);
        start_match();
    });
    add("47e start: mode 1, current game past the table end (no bound check)", nullptr, [](const Api &a) {
        host_party(a);
        mode(1);
        cur_game(5);
        game(5, 0xa005, 2);   // record 5 exists in memory beyond count 4
        start_match();
    });
    add("48 start: host create, existing 0x471007a1 entry reused", [] {
        put<uint32_t>(MENU + 0x24f0, 3);
        for (int i = 0; i < 3; i++) {
            put<uint32_t>(MENU + 0x24f8 + 0x18 * i, i == 1 ? 0x471007a1 : 0x100 + i);
            put<uint32_t>(MENU + 0x24f8 + 0x18 * i + 4, 0xcafe0000 + i);
            put<uint64_t>(MENU + 0x24f8 + 0x18 * i + 8, addr(0x2000 + i));
            put<uint64_t>(MENU + 0x24f8 + 0x18 * i + 0x10, 0x77);
        }
    }, [](const Api &a) {
        host_party(a);
        mode(2);
        start_match();
        start_match();
    });
    add("48b start: host create, table full", [] {
        put<uint32_t>(MENU + 0x24f0, 32);
        for (int i = 0; i < 32; i++) put<uint64_t>(MENU + 0x24f8 + 0x18 * i + 8, addr(0x3000 + i));
    }, [](const Api &a) {
        host_party(a);
        mode(2);
        start_match();
    });
    add("48c start: host create, table with 31 entries (ours not yet in)", [] {
        put<uint32_t>(MENU + 0x24f0, 31);
        for (int i = 0; i < 31; i++) put<uint64_t>(MENU + 0x24f8 + 0x18 * i + 8, addr(0x3000 + i));
    }, [](const Api &a) {
        host_party(a);
        mode(2);
        start_match();
        leave();
    });
    add("48d create_match public, NET_SM 35 -> 3 after a match", nullptr, [](const Api &a) {
        host_party(a, {2});
        a.create_match();
        frame();
        netsm(35);
        frame();
        a.create_match();
        frame();
    });
    add("49 match choice: invalid games", nullptr, [](const Api &a) {
        host_party(a);
        mode(2);
        cur_game(-1);
        start_match();
        cur_game(4);
        start_match();
        st.lookup.erase(0x3b9a067d);
        cur_game(0);
        start_match();
        st.lookup[0x3b9a067d] = GAMES;
        put<uint64_t>(GAMES + 8, 0);
        start_match();
    });
    add("49b match choice: unknown level, first match name, NULL names, no *levels*", nullptr, [](const Api &a) {
        host_party(a);
        mode(2);
        cur_game(0);
        start_match();   // first-name (first match)
        cur_game(2);
        start_match();   // level record with NULL name -> ?
        game(2, 0xa0ff, 1);
        start_match();   // unknown -> ?
        put<uint64_t>(LEVELSREC + 0x18, 0);
        cur_game(0);
        start_match();   // first match NULL name: stops, ?
        st.lookup.erase(0x0dec97a4);
        start_match();
    });
    add("49c match choice: *levels* with records NULL and count > 0", nullptr, [](const Api &a) {
        host_party(a);
        mode(2);
        cur_game(0);
        put<uint64_t>(LEVELS + 8, 0);
        start_match();
    });

    // ---------------- roster queries ----------------
    for (uint64_t fn : {0x659bb0ull, 0x659bc0ull})
        add("50 roster query " + std::to_string(fn) + ": op 7, 3, 1, unknown", [] {
            st.talking = {3};
            st.muted = {3, 4};
        }, [fn](const Api &a) {
            query(fn, 7, 0, 0);
            query(fn, 1, 0, 0);
            host_party(a, {2, 3, 5});
            in_sess(2, info("Two", -1, {4}));
            in_sess(3, info("ThreeHasAVeryLongNameThatIsMoreThanSixtyThreeCharactersLongReallyLong", 70, {9}));
            frame();
            for (uint64_t arg : {0ull, 1ull, 3ull, 4ull, 9ull, 10ull, 13ull, 14ull, 25ull, 0xffffffffffffffffull}) {
                query(fn, 7, 5, arg);
                query(fn, 3, 0, arg);
                query(fn, 3, 1, arg);
                query(fn, 3, 2, arg);
                query(fn, 1, 0, arg);
                query(fn, 99, 7, arg);
                query(fn, 0, 7, arg);
            }
            in_peer(2, false);
            frame();
            query(fn, 1, 0, 1);
            query(fn, 1, 0, 3);
        });
    add("50b roster query op 17: stop conditions and colour", nullptr, [](const Api &a) {
        host_party(a, {2, 3});
        in_sess(2, info("Red", -1, {1, 1}));
        in_sess(3, info("Blue", -1, {1, 2}));
        frame();
        put<int32_t>(NPD + 0x114, 2);
        frame(7);   // own team 2 at frame 7
        cur_game(0);
        query(0x659bb0, 17, 0, 1);    // arg <= 9
        mode(1);
        query(0x659bb0, 17, 0, 11);   // mode != 2
        mode(2);
        query(0x659bb0, 17, 0, 15);   // no member
        cur_game(-1);
        query(0x659bb0, 17, 0, 11);
        cur_game(4);
        query(0x659bb0, 17, 0, 11);
        cur_game(3);
        query(0x659bb0, 17, 0, 11);   // co-op type 4
        cur_game(0);
        for (uint64_t arg : {10ull, 11ull, 12ull}) query(0x659bb0, 17, 0, arg);
        st.team_colour = 0x00abcdef;
        query(0x659bc0, 17, 0, 12);
        for (int t : {8, 9, 12, 13}) {
            game(1, 0xa001, t);
            cur_game(1);
            query(0x659bb0, 17, 0, 11);
        }
        st.lookup.erase(0x3b9a067d);
        query(0x659bb0, 17, 0, 11);
        st.lookup[0x3b9a067d] = GAMES;
        put<uint64_t>(GAMES + 8, 0);
        query(0x659bb0, 17, 0, 11);
    });
    add("50c roster query op 17: member without team", nullptr, [](const Api &a) {
        host_party(a, {2});
        in_sess(2, info("NoTeam", -1, {1, 3}));
        frame();
        mode(2);
        cur_game(0);
        query(0x659bb0, 17, 0, 10);
        query(0x659bb0, 17, 0, 11);
    });
    add("50d roster query op 17: GetTeamColor bytes differ (logged once)", [] { mem[0x64af57 + 9] = 0x6b; },
        [](const Api &a) {
            host_party(a, {2});
            in_sess(2, info("Red", -1, {1, 1}));
            frame();
            mode(2);
            cur_game(0);
            query(0x659bb0, 17, 0, 11);
            query(0x659bb0, 17, 0, 11);
            mem[0x64af57 + 9] = 0x6a;
            query(0x659bb0, 17, 0, 11);
        });
    add("50e roster query op 17: first check passes, later bytes differ (still colours)", nullptr, [](const Api &a) {
        host_party(a, {2});
        in_sess(2, info("Red", -1, {1, 1}));
        frame();
        mode(2);
        cur_game(0);
        query(0x659bb0, 17, 0, 11);
        mem[0x64aec1] = 0;
        query(0x659bb0, 17, 0, 11);
    });

    // ---------------- teardown and public calls ----------------
    add("51 LEAVE_MULTIPLAYER: full sequence, entries after ours shift down", [] {
        put<uint32_t>(MENU + 0x24f0, 1);
        put<uint32_t>(MENU + 0x24f8, 0x55);
        put<uint64_t>(MENU + 0x24f8 + 8, addr(0x4000));
    }, [](const Api &a) {
        host_party(a, {2});
        put<uint64_t>(MATCH + 0x10, 5);
        for (int i = 0; i < 10; i++) put<uint32_t>(MATCH + 0x228 + 0xd0 * i + 0x18, 0x99 + i);
        mode(2);
        cur_game(2);
        start_match();   // adds 0x471007a1 after ours
        put<uint32_t>(MENU + 0x24f8 + 0x18 * 3, 0x66);
        put<uint64_t>(MENU + 0x24f8 + 0x18 * 3 + 8, addr(0x4003));
        put<uint32_t>(MENU + 0x24f0, 4);
        leave();
        dump(a);
        rec("handlers %u", handler_count());
        frame();
        leave();
        rec("handlers %u", handler_count());
        enter();
        dump(a);
    });
    add("51b LEAVE_MULTIPLAYER without entering", nullptr, [](const Api &a) {
        inst(a);
        leave();
        dump(a);
    });
    add("52 kick", nullptr, [](const Api &a) {
        rec("kick inactive %d", a.kick(3));
        host_party(a, {2, 10});
        for (uint32_t id : {0u, 1u, 2u, 3u, 11u, 12u, 0xffffffffu}) rec("kick %u -> %d", id, a.kick(id) ? 1 : 0);
        frame();
    });
    add("52b kick as client", [] { client_env(); }, [](const Api &a) {
        client_party(a);
        rec("kick 3 -> %d", a.kick(3) ? 1 : 0);
        rec("kick 1 -> %d", a.kick(1) ? 1 : 0);
    });
    add("53 set_local_name: inactive, empty, same, new, long", nullptr, [](const Api &a) {
        a.set_local_name("");
        a.set_local_name("Drake");
        a.set_local_name("Elena");
        a.set_local_name("Elena");
        dump(a);
        env("UNCHARTED_U2MP_NAME", "EnvName");
        a.set_local_name("Elena");
        dump(a);
        a.set_local_name("A-name-that-is-longer-than-thirty-one-bytes");
        a.set_local_name("A-name-that-is-longer-than-thirty-one-bytes-2");
        dump(a);
    });
    add("53b set_local_name while hosting", nullptr, [](const Api &a) {
        host_party(a, {2});
        a.set_local_name("Sully");
        dump(a);
        a.set_local_name("Sully");
    });
    add("53c set_local_name while client (joined / not yet)", [] { client_env(); }, [](const Api &a) {
        inst(a);
        enter();
        a.set_local_name("Chloe");
        in_peer(0, true);
        frame();
        a.set_local_name("Flynn");
    });
    add("54 online_reopen without owner", [] { st.link = true; }, [](const Api &a) {
        host_party(a, {2, 3});
        a.online_reopen(nullptr);
        dump(a);
        frame();
    });
    add("54b online_reopen without owner, host_link false", [] { st.host_link_ok = false; }, [](const Api &a) {
        a.online_reopen(nullptr);
        dump(a);
        frame();
        start_match();
    });
    add("54c online_reopen with owner", [] { st.local_slot = 4; }, [](const Api &a) {
        host_party(a, {2});
        Addr o{0x0a0a0a0a, 1234};
        a.online_reopen(&o);
        dump(a);
        in_peer(0, true);
        frame();
        dump(a);
    });
    add("54d online_migrated / online_host_gone", [] { client_env(); }, [](const Api &a) {
        a.online_host_gone();
        mem[MATCH + 0x99] = 1;
        put<uint64_t>(MATCH + 0x10, 3);
        a.online_migrated(true, true);
        a.online_migrated(false, true);
        a.online_migrated(true, false);
        mem[MATCH + 0x99] = 1;
        a.online_migrated(false, false);
        client_party(a);
        a.online_host_gone();
        frame();
        dump(a);
    });
    add("54e online_host_gone while hosting", nullptr, [](const Api &a) {
        host_party(a);
        a.online_host_gone();
        frame();
        dump(a);
    });
    add("55 match_migration_ok branches", [] { client_env(); }, [](const Api &a) {
        rec("inactive %d", a.match_migration_ok());
        client_party(a);
        rec("online off %d", a.match_migration_ok());
        st.online_active = true;
        rec("NET_SM 18 %d", a.match_migration_ok());
        netsm(26);
        cur_game(0);
        rec("ok %d", a.match_migration_ok());
        env("UNCHARTED_U2MP_HOST_MIGRATION", "0");
        rec("env 0 %d", a.match_migration_ok());
        env("UNCHARTED_U2MP_HOST_MIGRATION", "01");
        rec("env 01 %d", a.match_migration_ok());
        env("UNCHARTED_U2MP_HOST_MIGRATION", "1");
        rec("env 1 %d", a.match_migration_ok());
        cur_game(3);
        rec("co-op %d", a.match_migration_ok());
        for (int t : {8, 9, 12, 5}) {
            game(3, 0xa003, t);
            rec("type %d: %d", t, a.match_migration_ok());
        }
        cur_game(9);
        rec("game out of range %d", a.match_migration_ok());
        cur_game(-1);
        rec("game -1 %d", a.match_migration_ok());
        st.lookup.erase(0x3b9a067d);
        cur_game(3);
        rec("no table %d", a.match_migration_ok());
    });
    add("55b match_migration_ok while hosting", [] { st.online_active = true; }, [](const Api &a) {
        host_party(a);
        netsm(26);
        rec("hosting %d", a.match_migration_ok());
    });
    add("56 online_rehost as the new host", [] {
        client_env();
        st.local_slot = 3;
        put<int32_t>(NPD + 0x114, 2);
    }, [](const Api &a) {
        client_party(a);
        in_sess(0, {2, 0, 1, 1, 'H', 3, 1, 1, 'M'});
        frame();
        a.online_rehost(nullptr, 4);
        dump(a);
        in_peer(5, true);
        frame();
        dump(a);
        a.online_rehost(nullptr, 4);
    });
    add("56b online_rehost as member, joined finishes it", [] {
        client_env();
        st.local_slot = 4;
    }, [](const Api &a) {
        client_party(a);
        Addr o{0x0b0b0b0b, 4321};
        a.online_rehost(&o, 3);
        dump(a);
        frame();
        in_peer(0, true);
        frame();
        dump(a);
        a.online_rehost(&o, 3);
    });
    add("56c online_rehost while inactive with my id 0", nullptr, [](const Api &a) {
        a.online_rehost(nullptr, 2);
        dump(a);
        frame();
    });
    add("57 send_message", [] { client_env(); }, [](const Api &a) {
        a.send_message(0, {0x10, 1}, -1);
        client_party(a);
        a.send_message(0, {}, -1);
        a.send_message(0, {0x10, 1}, -1);
        a.send_message(-1, {0x10, 2}, -1);
        a.send_message(-1, {0x10, 3}, 0);
        in_sess(0, {2, 0, 1, 1, 'H', 2, 1, 1, 'M', 6, 1, 1, 'S'});
        frame();
        a.send_message(-1, {0x10, 4}, 6);
        a.send_message(-1, {0x10, 5}, 2);
    });
    add("57b send_message on host", nullptr, [](const Api &a) {
        host_party(a, {1, 4, 9});
        a.send_message(-1, {0x12}, 4);
        a.send_message(-1, {0x12, 1}, 0);
        a.send_message(-1, {0x12, 2}, 20);
        a.send_message(3, {0x12, 3}, 1);
    });
    add("58 member_address", [] { st.peer_addr = {{2, {0x0c0c0c0c, 777}}}; }, [](const Api &a) {
        Addr o;
        rec("inactive %d", a.member_address(3, o));
        host_party(a, {2});
        for (uint32_t id : {1u, 2u, 3u, 11u, 12u}) {
            o = Addr{0xeeee, 1};
            const bool r = a.member_address(id, o);
            rec("member_address %u -> %d %08x:%u", id, r, o.ip, o.port);
        }
    });
    add("59 public calls before anything", nullptr, [](const Api &a) {
        dump(a);
        rec("rank %d", a.rank());
        a.create_match();
        rec("package_by_id %s", where(a.package_by_id(sid("net"))).c_str());
        rec("net_id_of %u", a.net_id_of(""));
        frame();
    });
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    scenarios();
    if (argc == 3 && std::strcmp(argv[1], "--one") == 0) {
        // watchdog: a scenario that runs away (a loop in one build) fails with the last calls it made
        std::thread([] {
            for (int i = 0; i < 300; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const size_t n = hx::calls.size();
            std::printf("FAIL: scenario still running after 30 s; last calls:\n");
            for (size_t k = n > 12 ? n - 12 : 0; k < n; k++) std::printf("  %s\n", hx::calls[k].c_str());
            std::fflush(stdout);
            std::_Exit(3);
        }).detach();
        return run_one(size_t(std::atoi(argv[2])));
    }
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (size_t i = 0; i < all.size(); i++) std::printf("%zu %s\n", i, all[i].name.c_str());
        return 0;
    }
    char self[1024];
    GetModuleFileNameA(nullptr, self, sizeof self);
    const char *only = std::getenv("HX_SESSION_ONLY");
    int failed = 0, run = 0;
    std::vector<std::string> bad;
    for (size_t i = 0; i < all.size(); i++) {
        if (only && all[i].name.find(only) == std::string::npos) continue;
        ++run;
        char idx[16];
        std::snprintf(idx, sizeof idx, "%zu", i);
        const intptr_t rc = _spawnl(_P_WAIT, self, self, "--one", idx, nullptr);
        if (rc != 0) {
            ++failed;
            if (rc != 1) std::printf("FAIL %s: child exited with 0x%llx (crash)\n", all[i].name.c_str(), (unsigned long long)rc);
            bad.push_back(all[i].name);
        }
    }
    std::printf("\nsession: %d scenarios, %d passed, %d failed\n", run, run - failed, failed);
    for (auto &b : bad) std::printf("  failed: %s\n", b.c_str());
    return failed ? 1 : 0;
}
