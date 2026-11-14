// u2mp rewrite test "events": the old (git) and the new events.cpp + gameplay2.cpp on the same inputs.
//   bash gow3_pc/tests/u2mp_rewrite/run.sh events events.cpp gameplay2.cpp
//
// Serializers run on host buffers (event bytes, BitStream words, cursor, logs, fake game calls are compared
// byte for byte); the Applies and the installs run on the fake image (guest calls, logs, image compared).
// Host pointers (stack buffers of the NpcSpawn Apply, installed host functions) are compared by content,
// alignment and identity between calls, never by address.
#include <cfloat>
#include <cinttypes>
#include <cmath>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "events.h"
#include "harness.h"

// The old module (compiled with -Du2mp=u2mp_old; same headers, so the same layouts).
namespace u2mp_old {
namespace events {
extern const u2mp::events::Port kPorts[];
extern const size_t kPortCount;
extern u2mp::events::Game game;
void install_events(int &installed);
}  // namespace events
void install_gameplay2(int &installed);
}  // namespace u2mp_old

using u2mp::BitStream;
using u2mp::events::ApplyFn;
using u2mp::events::Game;
using u2mp::events::Port;

namespace {

// ---- the two sides ----------------------------------------------------------------------------------
struct Side {
    const char *tag;
    const Port *ports;
    const size_t *count;
    Game *game;
    void (*install_events)(int &);
    void (*install_gameplay2)(int &);
    // captured from an install on a good image
    ApplyFn start_apply = nullptr, done_apply = nullptr;
    int (*game_mode)() = nullptr;
    void *(*lookup_process)(uint32_t) = nullptr;
};
Side OLD{"old", u2mp_old::events::kPorts, &u2mp_old::events::kPortCount, &u2mp_old::events::game,
         u2mp_old::events::install_events, u2mp_old::install_gameplay2};
Side NEW{"new", u2mp::events::kPorts, &u2mp::events::kPortCount, &u2mp::events::game, u2mp::events::install_events,
         u2mp::install_gameplay2};
Side *const SIDES[2] = {&OLD, &NEW};

const Port &port(const Side &s, int type) {
    for (size_t i = 0; i < *s.count; ++i)
        if (s.ports[i].type == type) return s.ports[i];
    std::printf("no port %d in %s\n", type, s.tag);
    std::exit(2);
}

const int kTypes[] = {37, 38, 44, 47, 49, 51, 52, 53, 54, 55, 57, 61, 67, 76};

// ---- result bookkeeping ------------------------------------------------------------------------------
struct Group {
    std::string name;
    long cases = 0, fails = 0;
};
std::vector<Group> groups;
Group *G = nullptr;
long total_cases = 0;

void begin(const std::string &name) {
    groups.push_back({name});
    G = &groups.back();
}
void check(const std::string &what, const std::vector<std::string> &a, const std::vector<std::string> &b,
           const std::vector<uint8_t> *ia = nullptr, const std::vector<uint8_t> *ib = nullptr) {
    ++G->cases;
    ++total_cases;
    const bool eq = a == b && (!ia || !ib || *ia == *ib);
    if (eq) return;
    if (++G->fails <= 3) {
        hx::same((G->name + " [" + what + "]").c_str(), a, b, ia, ib);
    }
}

std::string hex(const void *p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    s.reserve(2 * n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = static_cast<const uint8_t *>(p)[i];
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}
uint32_t fbits(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}
float bitsf(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// ---- fake game for the serializers (host functions in Game) ---------------------------------------------
int32_t clockv = 300 * 100;
int modev = 12;
uint32_t known_sender = 0x1234;
alignas(16) uint8_t fake_proc[0x50];
int fake_mode() {
    hx::rec("game_mode -> %d", modev);
    return modev;
}
void *fake_lookup(uint32_t id) {
    void *r = id == known_sender ? fake_proc : nullptr;
    hx::rec("lookup_process(%u) -> %s", id, r ? "proc" : "null");
    return r;
}
struct GameCfg {
    bool clock = true, mode = true, lookup = true;
};
void set_games(const GameCfg &c) {
    for (Side *s : SIDES) {
        s->game->frame_clock = c.clock ? &clockv : nullptr;
        s->game->game_mode = c.mode ? fake_mode : nullptr;
        s->game->lookup_process = c.lookup ? fake_lookup : nullptr;
    }
}

// ---- events and streams ------------------------------------------------------------------------------
constexpr size_t kEv = 0x120;   // 0x100 payload max + guard bytes
struct alignas(16) Ev {
    uint8_t b[kEv];
};
template <typename T> void put(Ev &e, size_t off, T v) { std::memcpy(e.b + off, &v, sizeof v); }
void fill(Ev &e, uint8_t v) { std::memset(e.b, v, kEv); }

constexpr size_t kWords = 40;   // 1280 bits; every max_bits used stays below this
using Words = std::vector<uint32_t>;

// my own MSB-first bit writer for building read streams
struct BW {
    Words w = Words(kWords, 0);
    uint32_t n = 0;
    BW &put(uint32_t v, unsigned bits) {
        for (unsigned i = 0; i < bits; ++i, ++n)
            if ((v >> (bits - 1 - i)) & 1) w[n >> 5] |= 0x80000000u >> (n & 31);
        return *this;
    }
};

// One Write: returns the observable record.
std::vector<std::string> do_write(const Side &s, int type, const Ev &ev, const Words &words0, uint32_t start,
                                  uint32_t max) {
    hx::calls.clear();
    Ev e = ev;
    Words w = words0;
    BitStream bs{w.data(), start, max, 0x77};
    port(s, type).write(e.b, &bs);
    hx::rec("bs bits=%u max=%u pad=%u data=%s", bs.bits, bs.max_bits, bs.pad, bs.data == w.data() ? "same" : "moved");
    hx::rec("words %s", hex(w.data(), w.size() * 4).c_str());
    hx::rec("event %s", std::memcmp(e.b, ev.b, kEv) ? hex(e.b, kEv).c_str() : "unchanged");
    return hx::calls;
}
std::vector<std::string> do_read(const Side &s, int type, const Ev &pre, const Words &words0, uint32_t start,
                                 uint32_t max) {
    hx::calls.clear();
    Ev e = pre;
    Words w = words0;
    BitStream bs{w.data(), start, max, 0x55};
    port(s, type).read(e.b, &bs);
    hx::rec("bs bits=%u max=%u pad=%u", bs.bits, bs.max_bits, bs.pad);
    hx::rec("words %s", w == words0 ? "unchanged" : "CHANGED");
    hx::rec("event %s", hex(e.b, kEv).c_str());
    return hx::calls;
}
void cmp_write(const std::string &what, int type, const Ev &ev, const Words &w, uint32_t start, uint32_t max) {
    auto a = do_write(OLD, type, ev, w, start, max);
    auto b = do_write(NEW, type, ev, w, start, max);
    check(what, a, b);
}
void cmp_read(const std::string &what, int type, const Ev &pre, const Words &w, uint32_t start, uint32_t max) {
    auto a = do_read(OLD, type, pre, w, start, max);
    auto b = do_read(NEW, type, pre, w, start, max);
    check(what, a, b);
}
// Write with the new code into a clean stream (bits used, words).
uint32_t bits_of(int type, const Ev &ev, Words &out) {
    out.assign(kWords, 0);
    BitStream bs{out.data(), 0, uint32_t(kWords * 32), 0};
    const bool rec = hx::recording;
    hx::recording = false;
    port(NEW, type).write(const_cast<uint8_t *>(ev.b), &bs);
    hx::recording = rec;
    return bs.bits;
}
// Write then read back (old/new each its own pair), with every cut.
void cmp_round_trip(const std::string &what, int type, const Ev &ev, bool cuts) {
    Words w;
    const uint32_t L = bits_of(type, ev, w);
    Words dirty(kWords, 0xa5a5a5a5u);
    cmp_write(what + " write", type, ev, dirty, 0, uint32_t(kWords * 32));
    cmp_write(what + " write clean", type, ev, Words(kWords, 0), 0, uint32_t(kWords * 32));
    Ev pre;
    fill(pre, 0);
    std::memcpy(pre.b, ev.b, 0xc);
    cmp_read(what + " read", type, pre, w, 0, L);
    fill(pre, 0x5a);
    cmp_read(what + " read 5a", type, pre, w, 0, uint32_t(kWords * 32));
    if (cuts)
        for (uint32_t cut = 0; cut < L; ++cut) cmp_read(what + " cut " + std::to_string(cut), type, pre, w, 0, cut);
}

// ---- interesting values -------------------------------------------------------------------------------
const uint32_t kSpecialFloats[] = {
    0x7fc00000u, 0xffc00000u, 0x7f800001u, 0x7fbfffffu, 0x7f800000u, 0xff800000u, 0x00000000u, 0x80000000u,
    fbits(1e30f), fbits(-1e30f), fbits(FLT_MAX), fbits(-FLT_MAX), 0x00000001u, 0x80000001u, fbits(255.998f),
    fbits(256.0f), fbits(-256.0f), fbits(256.002f), fbits(-256.002f), fbits(2048.0f / 150.0f), fbits(13.6534f),
    fbits(-0.0066f), fbits(0.0066f), fbits(9.999999747378752e-05f), fbits(-9.999999747378752e-05f),
    fbits(std::nextafter(9.999999747378752e-05f, 1.0f)), fbits(std::nextafter(-9.999999747378752e-05f, -1.0f)),
    fbits(1.0f), fbits(-1.0f), fbits(1.0001f), fbits(-1.0001f), fbits(2147483648.0f), fbits(-2147483648.0f),
    fbits(2147483520.0f), fbits(9223372036854775808.0f), fbits(-9223372036854775808.0f), fbits(1.5f), fbits(4.2e9f),
    fbits(-0.5f), fbits(0.5f), fbits(0.70710678f), fbits(-0.70710678f), fbits(4194304.0f), fbits(-4194304.0f),
    fbits(13.65333366394043f), fbits(6.826666831970215f), fbits(-0.001953125f), fbits(0.0009765625f),
    fbits(-1e-30f), fbits(3.0f), fbits(1.0f / 3.0f), fbits(-13.6533f), fbits(1e10f), fbits(-1e10f)};
template <size_t N> uint32_t pick(std::mt19937 &r, const uint32_t (&a)[N]) { return a[r() % N]; }

uint32_t rand_word(std::mt19937 &r) {
    std::uniform_real_distribution<float> big(-300.0f, 300.0f), unit(-1.5f, 1.5f), tiny(-1e-3f, 1e-3f), tm(-50.0f, 50.0f);
    switch (r() % 9) {
    case 0: return r();
    case 1: return r() % 21;
    case 2: return pick(r, kSpecialFloats);
    case 3: return fbits(big(r));
    case 4: return fbits(unit(r));
    case 5: return 0;
    case 6: return fbits(tiny(r));
    case 7: return fbits(tm(r));
    default: return r() & 0x3ffff;
    }
}

void rand_event(std::mt19937 &r, int type, Ev &e) {
    for (size_t o = 0; o < kEv; o += 4) put<uint32_t>(e, o, rand_word(r));
    const uint8_t flags[] = {0, 1, 2, 3, 0xff, 0x80, 0xfe};
    switch (type) {
    case 47:
        if (r() % 3) {
            const int8_t counts[] = {-128, -1, 0, 1, 3, 4, 7, 8, 9, 15, 0x7f, 2, 5, 6};
            put<int8_t>(e, 0x30, counts[r() % sizeof counts]);
        }
        for (int i = 0; i < 8; ++i)
            if (r() % 3) {
                const uint32_t t[] = {0, 1, 2, 3, 4, 6, 15, 16, 0x11, 0xffffffffu, 1, 2, 3, 6};
                put<uint32_t>(e, 0x38 + 0x18 * i + 0x10, pick(r, t));
            }
        break;
    case 49:
        if (r() % 2) {
            const uint32_t c[] = {0, 1, 5, 9, 10, 11, 15, 16, 0xffffffffu, 0x80000000u, 10};
            put<uint32_t>(e, 0x14, pick(r, c));
        }
        break;
    case 52: e.b[0x14] = r() % 2 ? flags[r() % sizeof flags] : uint8_t(r()); break;
    case 53: e.b[0x0c] = r() % 2 ? flags[r() % sizeof flags] : uint8_t(r()); break;
    case 55:
        e.b[0x78] = flags[r() % sizeof flags];
        e.b[0x79] = flags[r() % sizeof flags];
        break;
    default: break;
    }
}

void rand_game(std::mt19937 &r) {
    GameCfg c;
    c.clock = r() % 6 != 0;
    c.mode = r() % 6 != 0;
    c.lookup = r() % 6 != 0;
    set_games(c);
    const int32_t clocks[] = {0, 1, -1, 30000, 2048, 2049, INT32_MAX, INT32_MIN, 123456, -300000, 4096, 6144};
    clockv = r() % 3 ? clocks[r() % (sizeof clocks / 4)] : int32_t(r() % 2000000);
    if (clockv == INT32_MAX && r() % 4) clockv = 30000;   // keep the slow (many wrap steps) case rarer
    const int modes[] = {12, 0, 3, 7, 12, -12, 11, 13};
    modev = modes[r() % 8];
    known_sender = r() % 2 ? 0x1234 : uint32_t(r());
    for (uint8_t &b : fake_proc) b = uint8_t(r());
}

// ============================================================================================================
// Serializer scenarios
// ============================================================================================================

Ev typical(int type, int variant = 0) {
    Ev e;
    fill(e, 0);
    e.b[8] = uint8_t(type);
    switch (type) {
    case 37: put<uint32_t>(e, 0xc, 0xdeadbeef); put<uint32_t>(e, 0x10, 0x11223344); put<uint32_t>(e, 0x14, 0x55667788); put<uint32_t>(e, 0x18, 7); break;
    case 38: case 76: put<uint32_t>(e, 0xc, 0xcafef00d); break;
    case 44: put<uint32_t>(e, 0xc, 0xfff); put<uint32_t>(e, 0x10, 0x80000001); put<int32_t>(e, 0x14, -5); put<float>(e, 0x18, 99.5f); put<uint32_t>(e, 0x1c, 63); break;
    case 47:
        put<uint32_t>(e, 0xfc, 0xa0a0a0a0); put<uint32_t>(e, 0xf8, 0x1234); put<uint32_t>(e, 0x18, 0x5eed1d);
        put<int8_t>(e, 0x30, int8_t(variant ? variant : 4));
        for (int i = 0; i < 8; ++i) {
            const uint32_t t[] = {1, 2, 3, 6, 1, 0, 4, 15};
            put<uint64_t>(e, 0x38 + 0x18 * i, 0xff01ull * (i + 1) + (uint64_t(i) << 40));
            put<uint32_t>(e, 0x38 + 0x18 * i + 0x10, t[i]);
        }
        break;
    case 49:
        put<uint32_t>(e, 0xc, 0x777); put<int16_t>(e, 0x10, -1); put<int16_t>(e, 0x12, 5); put<uint32_t>(e, 0x14, uint32_t(variant));
        for (int i = 0; i < 10; ++i) put<uint32_t>(e, 0x18 + 4 * i, uint32_t(0x100 + i));
        put<uint32_t>(e, 0x40, 0x201); put<uint32_t>(e, 0x44, 0x202); put<float>(e, 0x48, 0.75f);
        break;
    case 51: put<uint32_t>(e, 0xc, 1); put<uint32_t>(e, 0x10, 0x87654321); break;
    case 52: put<uint32_t>(e, 0xc, 42); put<float>(e, 0x10, 12.5f); put<uint8_t>(e, 0x14, uint8_t(variant)); put<uint32_t>(e, 0x18, 3); break;
    case 53:
        put<uint8_t>(e, 0xc, uint8_t(variant)); put<uint32_t>(e, 0x10, 0x89abcdef); put<uint32_t>(e, 0x14, 5);
        put<uint32_t>(e, 0x18, 6); put<float>(e, 0x1c, 1.5f); put<float>(e, 0x20, -2.0f);
        break;
    case 54: {
        const float q[4] = {0.0f, 0.70710678f, 0.0f, 0.70710678f};
        put<float>(e, 0x10, 12.5f); put<float>(e, 0x14, -3.25f); put<float>(e, 0x18, 200.0f); put<float>(e, 0x1c, -3.25f);
        for (int i = 0; i < 4; ++i) put<float>(e, 0x20 + 4 * i, q[i]);
        put<uint32_t>(e, 0x30, 0x1234); put<float>(e, 0x34, 100.0f); put<uint32_t>(e, 0x38, 0x456);
        break;
    }
    case 55: {
        const float q[4] = {0.5f, -0.5f, 0.5f, -0.5f};
        for (int l = 0; l < 2; ++l) {
            const size_t o = 0x10 + 0x20 * l;
            put<float>(e, o, -100.0f + l); put<float>(e, o + 4, 0.5f); put<float>(e, o + 8, 1.0f);
            for (int i = 0; i < 4; ++i) put<float>(e, o + 0x10 + 4 * i, l ? q[i] : (i == 3 ? 1.0f : 0.0f));
        }
        put<uint32_t>(e, 0x50, 0xabc); put<uint32_t>(e, 0x54, 0xfedcba98);
        for (size_t off = 0x58; off <= 0x68; off += 4) put<uint32_t>(e, off, uint32_t(off * 3));
        put<float>(e, 0x6c, 3.0f); put<float>(e, 0x70, 4.0f); put<uint32_t>(e, 0x74, 33); put<uint8_t>(e, 0x78, 1); put<uint8_t>(e, 0x79, 0);
        break;
    }
    case 57: put<float>(e, 0x10, -7.5f); put<float>(e, 0x14, 8.0f); put<float>(e, 0x18, 0.001953125f); put<float>(e, 0x1c, 8.0f); put<uint32_t>(e, 0x20, 0x31337); break;
    case 61: put<uint32_t>(e, 0xc, 3); put<uint32_t>(e, 0x10, 4); put<uint32_t>(e, 0x14, 5); break;
    case 67: put<float>(e, 0xc, 0.25f); put<float>(e, 0x10, -1.0f); put<uint32_t>(e, 0x14, 77); put<float>(e, 0x18, 95.0f); put<uint32_t>(e, 0x1c, 0x123); break;
    }
    return e;
}

std::vector<int> variants(int type) {
    switch (type) {
    case 47: return {4, 0, 1, 7, 8, 9, -1};
    case 49: return {0, 10, 3};
    case 52: return {0, 1};
    case 53: return {0, 1};
    default: return {0};
    }
}

void scen_round_trip_and_cuts() {   // spec 6.1, 6.2, 6.3, 6.5
    for (int mode : {12, 3}) {
        set_games({});
        modev = mode;
        clockv = 300 * 100;
        for (int t : kTypes)
            for (int v : variants(t)) {
                begin("S1/S2/S3/S5 round trip + every cut: type " + std::to_string(t) + " variant " + std::to_string(v) +
                      " mode " + std::to_string(mode));
                cmp_round_trip("rt", t, typical(t, v), true);
            }
    }
}

void scen_write_overflow() {   // spec 6.4 (runs before any other overflow of the process)
    set_games({});
    modev = 12;
    begin("S4 write past maxBits: first overflow logs once, later ones silent");
    for (int t : kTypes) {
        Words w;
        const Ev e = typical(t, t == 49 ? 10 : t == 47 ? 4 : 1);
        const uint32_t L = bits_of(t, e, w);
        // inside the first field, a middle field, the last field; also maxBits 0 and start past maxBits
        const uint32_t cuts[] = {1, L / 2, L - 1, 0};
        for (uint32_t c : cuts) {
            cmp_write("type " + std::to_string(t) + " max " + std::to_string(c), t, e, Words(kWords, 0xa5a5a5a5u), 0, c);
            cmp_write("type " + std::to_string(t) + " max " + std::to_string(c) + " start 3", t, e, Words(kWords, 0x3c3c3c3cu), 3,
                      c + 3);
        }
        cmp_write("type " + std::to_string(t) + " start past max", t, e, Words(kWords, 0xa5a5a5a5u), 40, 20);
        cmp_write("type " + std::to_string(t) + " exact fit", t, e, Words(kWords, 0xa5a5a5a5u), 5, L + 5);
    }
}

// 6.6 Time read
void scen_time_read() {
    begin("S6 time read (clock NULL / behind / exactly 1024/150 s / far ahead / negative)");
    const int32_t clocks[] = {0, 2048, 2049, 4095, 4096, 6143, 6144, 30000, INT32_MAX, -1, -300000, INT32_MIN, 1, 600};
    const uint32_t vs[] = {0, 1, 2, 1023, 1024, 1025, 2046, 2047, 150, 300};
    for (int withclock = 0; withclock < 2; ++withclock)
        for (int32_t c : clocks) {
            if (!withclock && c != 0) continue;
            set_games({withclock != 0, true, true});
            clockv = c;
            for (uint32_t v : vs)
                for (int t : {44, 54, 67}) {
                    BW bw;
                    if (t == 44) bw.put(1, 12).put(2, 32).put(3, 32).put(v, 11).put(5, 6);
                    if (t == 67) bw.put(fbits(1.0f), 32).put(fbits(2.0f), 32).put(3, 32).put(v, 11).put(9, 12);
                    if (t == 54) {
                        bw.put(1, 18).put(2, 18).put(3, 18);
                        for (int i = 0; i < 4; ++i) bw.put(0, 1);
                        bw.put(7, 32).put(v, 11).put(9, 12);
                    }
                    Ev pre;
                    fill(pre, 0x5a);
                    cmp_read("type " + std::to_string(t) + " clock " + std::to_string(c) + " v " + std::to_string(v), t, pre, bw.w, 0, bw.n);
                }
        }
}

// 6.7 Time write, 6.8 Position, 6.9 Locator
void scen_encodings() {
    set_games({});
    begin("S7 time write (negative, NaN, inf, 1e30, truncation, 2048/150 and above)");
    for (uint32_t f : kSpecialFloats)
        for (int t : {44, 54, 67}) {
            Ev e = typical(t);
            put<uint32_t>(e, t == 54 ? 0x34 : 0x18, f);
            cmp_write("type " + std::to_string(t) + " t bits " + std::to_string(f), t, e, Words(kWords, 0xa5a5a5a5u), 0, 1024);
        }
    for (float t : {-1.0f / 150, -0.0066f, 0.0066f, 0.0067f, 2048.0f / 150, 2049.0f / 150, 4096.0f / 150, -13.65f, 1e19f, -1e19f}) {
        Ev e = typical(44);
        put<float>(e, 0x18, t);
        cmp_write("type 44 t " + std::to_string(t), 44, e, Words(kWords, 0), 0, 1024);
    }

    begin("S8 position write (NaN, inf, beyond +-256, negative fractions) and read of every sign boundary");
    for (uint32_t f : kSpecialFloats)
        for (int comp = 0; comp < 4; ++comp) {
            Ev e = typical(57);
            put<uint32_t>(e, 0x10 + 4 * comp, f);
            cmp_write("57 comp " + std::to_string(comp) + " bits " + std::to_string(f), 57, e, Words(kWords, 0xa5a5a5a5u), 0, 1024);
        }
    for (float x : {-0.001f, -0.5f, -0.0019f, -255.999f, 255.999f, 256.0f, -256.0f, 300.0f, -300.0f, 4194304.0f, -4194304.0f, 4194305.0f}) {
        Ev e = typical(57);
        put<float>(e, 0x10, x);
        put<float>(e, 0x14, -x);
        put<float>(e, 0x18, x / 3);
        cmp_write("57 x " + std::to_string(x), 57, e, Words(kWords, 0), 0, 1024);
    }
    const uint32_t raws[] = {0, 1, 0x1ffff, 0x20000, 0x3ffff, 0x20001, 0x1fffe, 0x10000, 0x3fffe};
    for (uint32_t x : raws)
        for (uint32_t y : raws) {
            BW bw;
            bw.put(x, 18).put(y, 18).put(x ^ 0x3ffff, 18).put(0x31337, 32);
            Ev pre;
            fill(pre, 0x5a);
            cmp_read("57 read " + std::to_string(x) + "," + std::to_string(y), 57, pre, bw.w, 0, bw.n);
        }

    begin("S9 locator write (+-1e-4 exact and above, NaN, >1, -1) and read (absent, zero, mixed)");
    for (uint32_t f : kSpecialFloats)
        for (int comp = 0; comp < 4; ++comp) {
            Ev e = typical(54);
            put<uint32_t>(e, 0x20 + 4 * comp, f);
            cmp_write("54 q" + std::to_string(comp) + " bits " + std::to_string(f), 54, e, Words(kWords, 0xa5a5a5a5u), 0, 1024);
        }
    {
        Ev e = typical(54);
        for (int i = 0; i < 4; ++i) put<float>(e, 0x20 + 4 * i, 0.0f);
        cmp_write("54 all absent", 54, e, Words(kWords, 0), 0, 1024);
        for (int i = 0; i < 4; ++i) put<float>(e, 0x20 + 4 * i, 2.5f);
        cmp_write("54 all >1", 54, e, Words(kWords, 0), 0, 1024);
        for (int i = 0; i < 4; ++i) put<float>(e, 0x20 + 4 * i, -1.0f);
        cmp_write("54 all -1", 54, e, Words(kWords, 0), 0, 1024);
    }
    // reads: presence bit + 13-bit values
    const uint32_t qv[] = {0, 1, 4094, 4095, 4096, 8190, 8191, 2047, 6000};
    std::mt19937 r(66);
    for (int c = 0; c < 4000; ++c) {
        BW bw;
        bw.put(r() & 0x3ffff, 18).put(r() & 0x3ffff, 18).put(r() & 0x3ffff, 18);
        const int pattern = c % 4;   // 0 all absent, 1 all present, 2 mixed, 3 random
        for (int i = 0; i < 4; ++i) {
            const bool present = pattern == 0 ? false : pattern == 1 ? true : pattern == 2 ? (i & 1) : (r() & 1);
            bw.put(present, 1);
            if (present) bw.put(c < 400 ? qv[(c + i) % 9] : (r() % 3 ? 4095 + int(r() % 9) - 4 : r() & 0x1fff), 13);
        }
        bw.put(r(), 32).put(r() & 0x7ff, 11).put(r() & 0xfff, 12);
        Ev pre;
        fill(pre, 0x5a);
        cmp_read("54 read " + std::to_string(c), 54, pre, bw.w, 0, bw.n);
    }
    // all present with value 4095 (component 0): s == 0 path; tiny s
    {
        BW bw;
        bw.put(0, 54);
        for (int i = 0; i < 4; ++i) bw.put(1, 1).put(4095, 13);
        bw.put(0, 32 + 11 + 12);
        Ev pre;
        fill(pre, 0x5a);
        cmp_read("54 all present 4095", 54, pre, bw.w, 0, bw.n);
        BW b2;
        b2.put(0, 54);
        for (int i = 0; i < 4; ++i) b2.put(1, 1).put(i == 2 ? 4096 : 4095, 13);
        b2.put(0, 55);
        cmp_read("54 tiny s", 54, pre, b2.w, 0, b2.n);
    }
}

// 6.10 TerritoryOwn
void scen_territory_own() {
    begin("S10 type 49: counts 0/10/11/15, mode 12 / not 12 / game_mode NULL, short streams, mode lookup calls");
    for (int hasmode = 0; hasmode < 2; ++hasmode)
        for (int mode : {12, 0, 3}) {
            set_games({true, hasmode != 0, true});
            modev = mode;
            for (uint32_t count : {0u, 1u, 10u, 11u, 15u}) {
                BW bw;
                bw.put(0x777, 32).put(0xffff, 16).put(5, 16).put(count, 4);
                for (uint32_t i = 0; i < count; ++i) bw.put(0x100 + i, 12);
                bw.put(0x201, 12).put(0x202, 12).put(fbits(0.75f), 32);
                for (uint8_t pre_b : {uint8_t(0x5a), uint8_t(0)}) {
                    Ev pre;
                    fill(pre, pre_b);
                    const std::string w = "mode " + std::to_string(hasmode ? mode : -999) + " count " + std::to_string(count);
                    cmp_read(w + " full", 49, pre, bw.w, 0, bw.n);
                    for (uint32_t cut = 0; cut < bw.n; ++cut) cmp_read(w + " cut " + std::to_string(cut), 49, pre, bw.w, 0, cut);
                    cmp_read(w + " start 7", 49, pre, bw.w, 7, bw.n + 7);
                }
                for (uint32_t ec : {0u, 3u, 10u, 11u, 15u, 0xffffffffu}) {
                    Ev e = typical(49, int(ec));
                    cmp_write(std::to_string(ec) + " write count", 49, e, Words(kWords, 0xa5a5a5a5u), 0, 1024);
                    Words dummy;
                    const uint32_t L = bits_of(49, e, dummy);
                    cmp_write(std::to_string(ec) + " write short", 49, e, Words(kWords, 0xa5a5a5a5u), 0, L - 1);
                    cmp_write(std::to_string(ec) + " write max 60", 49, e, Words(kWords, 0xa5a5a5a5u), 0, 60);
                }
            }
        }
}

// 6.12 NetEventEvent
void scen_event() {
    begin("S12 type 47: counts, unknown arg types, bool raws, sender known/unknown, lookup NULL, write counts");
    for (int haslookup = 0; haslookup < 2; ++haslookup)
        for (uint32_t sender : {0x1234u, 0x999u}) {
            set_games({true, true, haslookup != 0});
            known_sender = 0x1234;
            for (int b = 0; b < 0x50; ++b) fake_proc[b] = uint8_t(b * 7 + 1);
            for (uint32_t count : {0u, 1u, 4u, 7u, 8u, 9u, 15u}) {
                for (int typeset = 0; typeset < 3; ++typeset) {
                    BW bw;
                    bw.put(0xa0a0a0a0, 32).put(0x5eed1d, 32).put(sender, 32).put(count, 4);
                    for (uint32_t i = 0; i < count; ++i) {
                        const uint32_t ts[3][4] = {{1, 2, 3, 6}, {0, 4, 15, 1}, {1, 1, 5, 7}};
                        const uint32_t raws[] = {0, 0x100, 1, 0xffffffff, 0x80000000, 2};
                        bw.put(ts[typeset][i % 4], 4).put(raws[(i + typeset) % 6], 32);
                    }
                    Ev pre;
                    fill(pre, 0x5a);
                    const std::string w = "read lookup " + std::to_string(haslookup) + " sender " + std::to_string(sender) +
                                          " count " + std::to_string(count) + " typeset " + std::to_string(typeset);
                    cmp_read(w, 47, pre, bw.w, 0, bw.n);
                    cmp_read(w + " start 9", 47, pre, bw.w, 9, bw.n + 9);
                    for (uint32_t cut = 0; cut < bw.n; cut += (cut < 120 ? 1 : 5)) cmp_read(w + " cut " + std::to_string(cut), 47, pre, bw.w, 0, cut);
                }
            }
        }
    set_games({});
    const int8_t counts[] = {-128, -1, 0, 1, 4, 7, 8, 9, 0x7f};
    for (int8_t c : counts)
        for (int slotkind = 0; slotkind < 4; ++slotkind) {
            Ev e = typical(47);
            put<int8_t>(e, 0x30, c);
            for (int i = 0; i < 8; ++i) {
                const size_t s = 0x38 + 0x18 * i;
                switch (slotkind) {
                case 0: put<uint64_t>(e, s, 0xffffff00ull); put<uint32_t>(e, s + 0x10, 1); break;   // bool with first byte 0
                case 1: put<uint64_t>(e, s, 0xfedcba9876543210ull); put<uint32_t>(e, s + 0x10, 2 + (i & 1)); break;   // high bits
                case 2: put<uint64_t>(e, s, 7); put<uint32_t>(e, s + 0x10, 16 + i * 0x101); break;   // type > 15
                default: put<uint64_t>(e, s, 0x100 + i); put<uint32_t>(e, s + 0x10, uint32_t(i == 3 ? 1 : 6)); break;
                }
            }
            cmp_write("write count " + std::to_string(c) + " slotkind " + std::to_string(slotkind), 47, e,
                      Words(kWords, 0xa5a5a5a5u), 0, 1280);
        }
}

// 6.13 types 52 / 53 flag bytes
void scen_flags() {
    set_games({});
    begin("S13 types 52 and 53: flag byte 0/1/2/3 (and 0x80, 0xff) on write; flag bits on read");
    for (int t : {52, 53})
        for (int f : {0, 1, 2, 3, 0x80, 0xff, 0xfe}) {
            Ev e = typical(t, f);
            cmp_write("type " + std::to_string(t) + " flag " + std::to_string(f), t, e, Words(kWords, 0xa5a5a5a5u), 0, 1024);
            Words w;
            const uint32_t L = bits_of(t, e, w);
            Ev pre;
            fill(pre, 0x5a);
            cmp_read("type " + std::to_string(t) + " flag " + std::to_string(f) + " read back", t, pre, w, 0, L);
            for (uint32_t cut = 0; cut < L; ++cut) cmp_read("cut " + std::to_string(cut), t, pre, w, 0, cut);
        }
    // read with the flag bit 0 / 1 and +0x18 pre-filled
    for (int f = 0; f < 2; ++f) {
        BW b52;
        b52.put(42, 32).put(fbits(1.5f), 32).put(f, 1).put(1, 1);
        Ev pre;
        fill(pre, 0x5a);
        cmp_read("52 flag " + std::to_string(f), 52, pre, b52.w, 0, b52.n);
        cmp_read("52 flag " + std::to_string(f) + " exactly 65", 52, pre, b52.w, 0, 65);
        BW b53;
        b53.put(f, 1).put(0xabcdef12u >> (f ? 20 : 0), f ? 12 : 32).put(1, 32).put(2, 32).put(3, 32).put(4, 32);
        cmp_read("53 flag " + std::to_string(f), 53, pre, b53.w, 0, b53.n);
    }
}

// Randomized: random events written with random cursor / maxBits / buffer, random streams read.
void scen_random() {
    std::mt19937 r(20261114);
    for (int t : kTypes) {
        begin("R random writes type " + std::to_string(t));
        for (int c = 0; c < 4000; ++c) {
            rand_game(r);
            Ev e;
            rand_event(r, t, e);
            Words w(kWords);
            for (uint32_t &x : w) x = c % 3 == 0 ? 0xa5a5a5a5u : c % 3 == 1 ? 0 : uint32_t(r());
            const uint32_t start = r() % 4 ? r() % 64 : 0;
            uint32_t max;
            switch (r() % 5) {
            case 0: max = 1024; break;
            case 1: max = start + r() % 200; break;
            case 2: max = r() % 64; break;   // often below start
            case 3: max = start + r() % 800; break;
            default: max = r() % 1100; break;
            }
            cmp_write("case " + std::to_string(c), t, e, w, start, max);
        }
        begin("R random reads type " + std::to_string(t));
        for (int c = 0; c < 4000; ++c) {
            rand_game(r);
            Words w(kWords);
            const int kind = r() % 6;
            for (uint32_t &x : w) x = kind == 0 ? 0 : kind == 1 ? 0xffffffffu : uint32_t(r());
            if (kind == 2) {   // a written random event, so the fields are plausible
                Ev src;
                rand_event(r, t, src);
                bits_of(t, src, w);
            }
            Ev pre;
            if (r() % 2) rand_event(r, t, pre);
            else fill(pre, uint8_t(r()));
            const uint32_t start = r() % 3 ? 0 : r() % 64;
            uint32_t max;
            switch (r() % 4) {
            case 0: max = 1280; break;
            case 1: max = start + r() % 250; break;
            case 2: max = r() % 64; break;
            default: max = r() % 1281; break;
            }
            cmp_read("case " + std::to_string(c), t, pre, w, start, max);
        }
        begin("R random round trips with random cuts type " + std::to_string(t));
        for (int c = 0; c < 2000; ++c) {
            rand_game(r);
            Ev src;
            rand_event(r, t, src);
            Words w;
            const uint32_t L = bits_of(t, src, w);
            Ev pre;
            fill(pre, r() % 2 ? 0x5a : 0);
            const uint32_t max = r() % 3 ? L : r() % (L + 1);
            cmp_read("case " + std::to_string(c), t, pre, w, 0, max);
        }
    }
}

// ============================================================================================================
// Image scenarios: Applies and installs
// ============================================================================================================

// guest functions the code calls
constexpr uint64_t gLookupPointer = 0xa15e70, gLookupProcess = 0x9bbe40, gLookupSpawner = 0x56c7b0,
                   gParamsInit = 0x233600, gParamsSpawner = 0x235360, gSpawnerLocator = 0x4bdfe0, gSpawnNpc = 0x4be330,
                   gAddTracker = 0x644fa0, gSendDone = 0x60c0f0;
// fake objects in the image
constexpr uint64_t oEv = 0x1700000, oSpawner = 0x1710000, oSecond = 0x1711000, oOther = 0x1712000, oNpc = 0x1713000,
                   oProc = 0x1714000, oTable = 0x1720000, oEntries = 0x1721000;
constexpr uint64_t kRecords = 0x1152a18, kNgm = 0x113b760, kMatch = 0x1065b08, kNetState = 0x14ce97c;

std::string W(const void *p) { return hx::where(p); }
std::string hexs(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

// scenario knobs for the fakes
struct Fake {
    uint64_t proc = 0;                     // what LookupProcessByUniqueId returns (image offset, 0 = NULL)
    std::vector<std::pair<uint32_t, uint64_t>> spawners;   // id -> image offset
    uint64_t npc = oNpc;                   // what SpawnNpc returns (0 = NULL)
    uint64_t add_ret = 1;                  // AddNetNpcTracker raw return
    uint64_t table = oTable;               // LookupPointer return (0 = NULL)
    const void *last_params = nullptr, *last_info = nullptr, *last_loc = nullptr;
} fake;

std::string buf(const void *p, size_t n) {
    char a[64];
    std::snprintf(a, sizeof a, "%s a16=%d ", W(p).c_str(), int(uintptr_t(p) & 15) == 0);
    return a + hex(p, n);
}
std::string ident(const void *p, const void *last, const char *name) {
    if (!p) return "null";
    if (p == last) return std::string("same-") + name;
    return W(p);
}

U2MP_GUEST void *f_lookup_pointer(uint32_t id) {
    hx::rec("LookupPointer(%08x) -> %s", id, fake.table ? "table" : "null");
    return fake.table ? hx::mem + fake.table : nullptr;
}
U2MP_GUEST void *f_lookup_process(void *unused, uint32_t id) {
    hx::rec("LookupProcessByUniqueId(%s, %u) -> %s", W(unused).c_str(), id, fake.proc ? "proc" : "null");
    return fake.proc ? hx::mem + fake.proc : nullptr;
}
U2MP_GUEST void *f_lookup_spawner(void *manager, uint32_t id) {
    uint64_t r = 0;
    for (auto &[i, o] : fake.spawners)
        if (i == id) r = o;
    hx::rec("LookupSpawner(%s, %08x) -> %s", W(manager).c_str(), id, r ? W(hx::mem + r).c_str() : "null");
    return r ? hx::mem + r : nullptr;
}
U2MP_GUEST void f_params_init(void *params, void *info) {
    hx::rec("SpawnParamsInit(params %s, info %s)", buf(params, 0xb0).c_str(), buf(info, 0x60).c_str());
    fake.last_params = params;
    fake.last_info = info;
    for (int i = 0; i < 0xb0; ++i) static_cast<uint8_t *>(params)[i] = uint8_t(0xc0 + i);   // the game fills it
}
U2MP_GUEST void f_params_spawner(void *params, void *spawner) {
    hx::rec("SpawnParamsSetSpawner(%s %s, %s)", ident(params, fake.last_params, "params").c_str(), hex(params, 0xb0).c_str(),
            W(spawner).c_str());
    static_cast<uint8_t *>(params)[0x80] ^= 0x5a;
}
U2MP_GUEST void f_spawner_locator(void *loc, void *spawner) {
    hx::rec("SpawnerLocator(%s, %s) info-alive=%s", buf(loc, 0x40).c_str(), W(spawner).c_str(),
            fake.last_info ? hex(fake.last_info, 0x60).c_str() : "-");
    fake.last_loc = loc;
    for (int i = 0; i < 0x40; ++i) static_cast<uint8_t *>(loc)[i] = uint8_t(0x30 + i);
}
U2MP_GUEST void *f_spawn_npc(void *spawner, void *params, void *loc, uint64_t zero) {
    hx::rec("SpawnNpc(%s, %s %s, %s %s, %llu) -> %s", W(spawner).c_str(), ident(params, fake.last_params, "params").c_str(),
            hex(params, 0xb0).c_str(), ident(loc, fake.last_loc, "loc").c_str(), loc ? hex(loc, 0x40).c_str() : "",
            (unsigned long long)zero, fake.npc ? "npc" : "null");
    return fake.npc ? hx::mem + fake.npc : nullptr;
}
U2MP_GUEST uint64_t f_add_tracker(void *ngm, void *npc, uint32_t v) {
    hx::rec("AddNetNpcTracker(%s, %s, %08x) -> %llx", W(ngm).c_str(), W(npc).c_str(), v, (unsigned long long)fake.add_ret);
    return fake.add_ret;
}
U2MP_GUEST void f_send_done(uint32_t target, uint8_t can, int32_t a, int32_t b) {
    hx::rec("SendConnectionDone(%u, %u, %d, %d)", target, unsigned(can), a, b);
}
U2MP_GUEST void f_unexpected(void *a, void *b) { hx::rec("UNEXPECTED guest call(%s, %s)", W(a).c_str(), W(b).c_str()); }

const uint8_t kRet[5] = {0xc3, 0x66, 0x66, 0x66, 0x66};

struct AnchorBytes {
    uint64_t at;
    std::vector<uint8_t> b;
};
const AnchorBytes kAnchors[] = {
    {0x63bfe9, {0x41, 0x0F, 0x95, 0x46, 0x0C}},
    {0x63c059, {0x41, 0x89, 0x5E, 0x10}},
    {0x63c0c9, {0x41, 0x89, 0x5E, 0x14}},
    {0x60c0f0, {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56}},
    {0x60c174, {0x45, 0x88, 0x67, 0x0C, 0x41, 0x89, 0x5F, 0x10}},
    {0x60c17f, {0x41, 0x89, 0x47, 0x14}},
    {0x5f2f13, {0x41, 0x80, 0xBC, 0x15, 0x85, 0x02, 0x00, 0x00, 0x00}},
    {0x5f2ef8, {0x41, 0x80, 0xBC, 0x15, 0x7C, 0x02, 0x00, 0x00, 0x00}},
};
struct Rip {
    uint64_t at, len, disp, target;
};
const Rip kRips[] = {{0x60c165, 7, 3, 0x10414d0}, {0x60bf8c, 7, 3, 0x1041520}, {0x5f2ec2, 7, 3, 0x113b760}};
constexpr uint64_t kStartVt = 0x1041520, kStartStub = 0x63c0e0, kDoneVt = 0x10414d0, kDoneStub = 0x63bda0;
constexpr uint64_t kDamageWriteSentinel = 0x61fdf0;

// A good PS4 1.02 image (for what these modules check).
void good_image() {
    hx::clear();
    hx::bind_both();
    for (size_t i = 0; i < *NEW.count; ++i) {
        const Port &p = NEW.ports[i];
        const uint64_t stubs[3] = {p.read_stub, p.write_stub, p.apply_stub};
        for (int s = 0; s < 3; ++s)
            if (stubs[s]) {
                std::memcpy(hx::mem + stubs[s], kRet, 5);
                hx::put<uint64_t>(p.vtable + 8 * (2 + s), hx::addr(stubs[s]));
            }
        hx::put<uint64_t>(p.vtable, 0x1111000 + i);   // other slots: recognisable
    }
    // type 44's Write slot (never installed): a stub that would be accepted if it were
    std::memcpy(hx::mem + kDamageWriteSentinel, kRet, 5);
    hx::put<uint64_t>(0x10405d0 + 0x18, hx::addr(kDamageWriteSentinel));
    for (const auto &a : kAnchors) std::memcpy(hx::mem + a.at, a.b.data(), a.b.size());
    for (const auto &r : kRips) hx::put<int32_t>(r.at + r.disp, int32_t(int64_t(r.target) - int64_t(r.at + r.len)));
    for (auto [vt, st] : {std::pair{kStartVt, kStartStub}, std::pair{kDoneVt, kDoneStub}}) {
        std::memcpy(hx::mem + st, kRet, 5);
        hx::put<uint64_t>(vt + 0x20, hx::addr(st));
    }
}

// Install both modules on a good image once: capture the Applies and the game functions of each side.
void capture_installed() {
    for (Side *s : SIDES) {
        good_image();
        *s->game = Game{};
        int n = 0;
        s->install_events(n);
        s->install_gameplay2(n);
        s->start_apply = reinterpret_cast<ApplyFn>(hx::get<uint64_t>(kStartVt + 0x20));
        s->done_apply = reinterpret_cast<ApplyFn>(hx::get<uint64_t>(kDoneVt + 0x20));
        s->game_mode = s->game->game_mode;
        s->lookup_process = s->game->lookup_process;
        if (!s->start_apply || !s->done_apply || !s->game_mode || !s->lookup_process || n != 30)
            std::printf("WARN %s: install on the good image gave %d slots\n", s->tag, n);
    }
}

// Replaces host function addresses in the vtable slots by tokens (they differ legitimately) and records them.
void mask_slots(const Side &s) {
    auto one = [&](uint64_t slot_off, const char *what, const void *own = nullptr) {
        const uint64_t v = hx::get<uint64_t>(slot_off);
        const uint64_t lo = hx::addr(0), hi = hx::addr(hx::kImageSize);
        if (v >= lo && v < hi) return;
        std::string name = "host?";
        if (own && v == uint64_t(uintptr_t(own))) name = "its own port function";   // ports may share one function
        else
            for (size_t i = 0; i < *s.count; ++i) {
                const Port &p = s.ports[i];
                if (v == uint64_t(uintptr_t(p.read))) name = "read" + std::to_string(p.type);
                if (v == uint64_t(uintptr_t(p.write))) name = "write" + std::to_string(p.type);
                if (p.apply && v == uint64_t(uintptr_t(p.apply))) name = "apply" + std::to_string(p.type);
            }
        if (s.start_apply && v == uint64_t(uintptr_t(s.start_apply))) name = "StartConnection::Apply";
        if (s.done_apply && v == uint64_t(uintptr_t(s.done_apply))) name = "ConnectionDone::Apply";
        hx::rec("slot img+0x%llx (%s) = %s", (unsigned long long)slot_off, what, name.c_str());
        hx::put<uint64_t>(slot_off, 0xF00D0000ull + slot_off);
    };
    for (size_t i = 0; i < *s.count; ++i)
        for (int sl = 2; sl <= 4; ++sl) {
            const Port &p = s.ports[i];
            one(p.vtable + 8 * sl, p.name, sl == 2 ? (const void *)p.read : sl == 3 ? (const void *)p.write : (const void *)p.apply);
        }
    one(kStartVt + 0x20, "StartConnection");
    one(kDoneVt + 0x20, "ConnectionDone");
}

void record_game(const Side &s) {
    const Game &g = *s.game;
    hx::rec("game.frame_clock=%s game_mode=%s lookup_process=%s", W(g.frame_clock).c_str(),
            !g.game_mode ? "null" : g.game_mode == s.game_mode ? "installed" : "other",
            !g.lookup_process ? "null" : g.lookup_process == s.lookup_process ? "installed" : "other");
}

// Runs `body` for old and new on the same image (built by `setup`) and compares calls and image.
void img_case(const std::string &what, const std::function<void()> &setup, const std::function<void(Side &)> &body) {
    setup();
    const std::vector<uint8_t> raw(hx::mem, hx::mem + hx::kImageSize);
    const Fake f0 = fake;
    std::vector<std::string> rec[2];
    std::vector<uint8_t> img[2];
    for (int i = 0; i < 2; ++i) {
        std::memcpy(hx::mem, raw.data(), hx::kImageSize);
        fake = f0;
        hx::calls.clear();
        hx::recording = true;
        body(*SIDES[i]);
        rec[i] = hx::calls;
        img[i] = hx::snapshot();
    }
    check(what, rec[0], rec[1], &img[0], &img[1]);
}

// ---- NpcSpawn Apply (6.15 - 6.21) ----------------------------------------------------------------------
void spawn_setup() {
    good_image();
    hx::stub(gLookupProcess, (void *)f_lookup_process);
    hx::stub(gLookupSpawner, (void *)f_lookup_spawner);
    hx::stub(gParamsInit, (void *)f_params_init);
    hx::stub(gParamsSpawner, (void *)f_params_spawner);
    hx::stub(gSpawnerLocator, (void *)f_spawner_locator);
    hx::stub(gSpawnNpc, (void *)f_spawn_npc);
    hx::stub(gAddTracker, (void *)f_add_tracker);
    hx::stub(gLookupPointer, (void *)f_unexpected);
    hx::put<uint32_t>(oEv + 0x0c, 4242);
    hx::put<uint32_t>(oEv + 0x10, 0xaaaa0001);
    hx::put<uint32_t>(oEv + 0x14, 0xbbbb0002);
    hx::put<uint32_t>(oEv + 0x18, 0x2be2f3bd);
    hx::put<uint32_t>(oEv + 0x00, 0x99999999);
    hx::put<uint32_t>(oSpawner + 0x40, 0x5157a7e5);
    hx::put<uint32_t>(oSecond + 0x40, 0x5ec0dd00);
    hx::put<uint32_t>(oNpc + 0x68, 0xffffffff);
    hx::put<uint32_t>(oNpc + 0x3708, 0x37083708);
    hx::put<uint64_t>(oProc + 0x38, 0x1122334455667788ull);
}

void scen_npc_spawn() {
    begin("S15-S21 NpcSpawn Apply");
    enum Lk { LkInstalled, LkNull };
    struct C {
        const char *name;
        Lk lk;
        uint64_t proc;
        uint32_t second;
        bool spawner, second_found;
        uint64_t npc, add;
    };
    const C cs[] = {
        {"15 process exists", LkInstalled, oProc, 0xbbbb0002, true, true, oNpc, 1},
        {"16 lookup_process NULL", LkNull, oProc, 0xbbbb0002, true, true, oNpc, 1},
        {"17 spawner missing, second 0", LkInstalled, 0, 0, false, false, oNpc, 1},
        {"17 spawner missing, second set", LkInstalled, 0, 0xbbbb0002, false, true, oNpc, 1},
        {"17 spawner missing, second set not found", LkInstalled, 0, 0xbbbb0002, false, false, oNpc, 1},
        {"18 second id 0", LkInstalled, 0, 0, true, false, oNpc, 1},
        {"18 second id 0, a spawner with id 0 exists", LkInstalled, 0, 0, true, true, oNpc, 1},
        {"19 second found", LkInstalled, 0, 0xbbbb0002, true, true, oNpc, 1},
        {"19 second not found", LkInstalled, 0, 0xbbbb0002, true, false, oNpc, 1},
        {"20 SpawnNpc NULL", LkInstalled, 0, 0xbbbb0002, true, true, 0, 1},
        {"20 add 0", LkInstalled, 0, 0xbbbb0002, true, true, oNpc, 0},
        {"20 add 1", LkInstalled, 0, 0, true, false, oNpc, 1},
        {"20 add 0x100", LkInstalled, 0, 0xbbbb0002, true, true, oNpc, 0x100},
        {"20 add 0xffffffffffffff01", LkInstalled, 0, 0xbbbb0002, true, true, oNpc, 0xffffffffffffff01ull},
        {"21 lookup NULL + second 0 + add 0", LkNull, 0, 0, true, false, oNpc, 0},
    };
    for (const C &c : cs)
        img_case(c.name,
                 [&] {
                     spawn_setup();
                     hx::put<uint32_t>(oEv + 0x14, c.second);
                     fake = Fake{};
                     fake.proc = c.proc;
                     if (c.spawner) fake.spawners.push_back({0xaaaa0001, oSpawner});
                     if (c.second_found) fake.spawners.push_back({c.second, c.second ? oSecond : oOther});
                     fake.npc = c.npc;
                     fake.add_ret = c.add;
                 },
                 [&](Side &s) {
                     s.game->lookup_process = c.lk == LkInstalled ? s.lookup_process : nullptr;
                     port(s, 37).apply(hx::mem + oEv, hx::mem + 0x1234);
                 });
    // the Apply through the vtable slot the install wrote
    img_case("Apply via the installed slot 4", [&] {
        spawn_setup();
        fake = Fake{};
        fake.spawners = {{0xaaaa0001, oSpawner}, {0xbbbb0002, oSecond}};
    },
             [&](Side &s) {
                 *s.game = Game{};
                 int n = 0;
                 s.install_events(n);
                 const auto fn = reinterpret_cast<ApplyFn>(hx::get<uint64_t>(0x1040f30 + 0x20));
                 fn(hx::mem + oEv, nullptr);
                 mask_slots(s);
             });
}

// ---- game mode function (6.11), process lookup (4.7), installed game used by Reads ---------------------
void scen_game_functions() {
    begin("S11 game-mode function (LookupPointer NULL, index -1, index = count, entries NULL, valid) + process lookup");
    struct C {
        const char *name;
        bool table;
        int16_t index;
        int32_t count;
        bool entries;
    };
    const C cs[] = {{"LookupPointer NULL", false, 0, 5, true}, {"index -1", true, -1, 5, true},
                    {"index -32768", true, -32768, 5, true},  {"index = count", true, 5, 5, true},
                    {"index > count", true, 7, 5, true},      {"count negative", true, 0, -1, true},
                    {"entries NULL", true, 2, 5, false},      {"valid 0", true, 0, 5, true},
                    {"valid 4", true, 4, 5, true},            {"valid big index", true, 300, 301, true}};
    for (const C &c : cs)
        img_case(c.name,
                 [&] {
                     good_image();
                     hx::stub(gLookupPointer, (void *)f_lookup_pointer);
                     fake = Fake{};
                     fake.table = c.table ? oTable : 0;
                     hx::put<int16_t>(kNgm + 0x1ca0, c.index);
                     hx::put<int32_t>(oTable, c.count);
                     hx::put<uint64_t>(oTable + 8, c.entries ? hx::addr(oEntries) : 0);
                     for (int i = 0; i < 301; ++i) hx::put<int32_t>(oEntries + i * 0x90 + 0xc, 100 + i);
                     hx::put<int32_t>(oEntries + 4 * 0x90 + 0xc, 12);
                 },
                 [&](Side &s) { hx::rec("mode %d", s.game_mode()); });
    for (uint64_t proc : {uint64_t(0), oProc})
        img_case(proc ? "process lookup found" : "process lookup NULL",
                 [&] {
                     good_image();
                     hx::stub(gLookupProcess, (void *)f_lookup_process);
                     fake = Fake{};
                     fake.proc = proc;
                 },
                 [&](Side &s) { hx::rec("-> %s", W(s.lookup_process(0xabcdef)).c_str()); });

    begin("S10/S12 Reads and Writes with the installed game functions (guest calls only where due)");
    for (int mode : {12, 4})
        for (uint32_t count : {3u, 11u})
            for (int cut : {0, 1}) {
                img_case("49 mode " + std::to_string(mode) + " count " + std::to_string(count) + (cut ? " short" : ""),
                         [&] {
                             good_image();
                             hx::stub(gLookupPointer, (void *)f_lookup_pointer);
                             fake = Fake{};
                             hx::put<int16_t>(kNgm + 0x1ca0, 4);
                             hx::put<int32_t>(oTable, 5);
                             hx::put<uint64_t>(oTable + 8, hx::addr(oEntries));
                             hx::put<int32_t>(oEntries + 4 * 0x90 + 0xc, mode);
                             std::memset(hx::mem + oEv, 0x5a, 0x60);
                         },
                         [&](Side &s) {
                             s.game->game_mode = s.game_mode;
                             BW bw;
                             bw.put(0x777, 32).put(0xffff, 16).put(5, 16).put(count, 4);
                             for (uint32_t i = 0; i < count; ++i) bw.put(0x100 + i, 12);
                             bw.put(0x201, 12).put(0x202, 12).put(fbits(0.75f), 32);
                             BitStream bs{bw.w.data(), 0, cut ? bw.n - 40 : bw.n, 0};
                             port(s, 49).read(hx::mem + oEv, &bs);
                             hx::rec("read bits %u", bs.bits);
                             Words out(kWords, 0xa5a5a5a5u);
                             BitStream ws{out.data(), 0, 1024, 0};
                             port(s, 49).write(hx::mem + oEv, &ws);
                             hx::rec("write bits %u %s", ws.bits, hex(out.data(), 64).c_str());
                         });
            }
    for (uint64_t proc : {uint64_t(0), oProc})
        img_case(std::string("47 read with installed lookup, ") + (proc ? "found" : "not found"),
                 [&] {
                     good_image();
                     hx::stub(gLookupProcess, (void *)f_lookup_process);
                     fake = Fake{};
                     fake.proc = proc;
                     hx::put<uint32_t>(oProc + 0x44, 0x4444);
                     hx::put<uint64_t>(oProc + 0x38, 0x3838383838383838ull);
                     std::memset(hx::mem + oEv, 0x5a, 0x110);
                 },
                 [&](Side &s) {
                     s.game->lookup_process = s.lookup_process;
                     BW bw;
                     bw.put(1, 32).put(2, 32).put(3, 32).put(2, 4).put(1, 4).put(0x100, 32).put(6, 4).put(fbits(1.5f), 32);
                     BitStream bs{bw.w.data(), 0, bw.n, 0};
                     port(s, 47).read(hx::mem + oEv, &bs);
                     hx::rec("read bits %u", bs.bits);
                 });
}

// ---- StartConnection / ConnectionDone Applies (6.22, 6.23) -----------------------------------------------
void conn_setup() {
    good_image();
    hx::stub(gSendDone, (void *)f_send_done);
    for (uint64_t g : {gLookupPointer, gLookupProcess, gLookupSpawner}) hx::stub(g, (void *)f_unexpected);
    hx::put<uint8_t>(oEv + 0x0c, 0xab);
    hx::put<int32_t>(oEv + 0x10, -7);
    hx::put<int32_t>(oEv + 0x14, 123456);
}
void scen_connections() {
    begin("S22 StartConnection Apply");
    struct C {
        const char *name;
        int32_t state;
        uint64_t ctx;
        uint32_t me, host;
        bool conn;
    };
    const C cs[] = {{"net state 0", 0, 1, 3, 7, true},        {"net state 1", 1, 1, 3, 7, true},
                    {"net state 2 client", 2, 1, 3, 7, true}, {"net state -5", -5, 1, 3, 7, true},
                    {"host by context 0", 2, 0, 3, 7, true},  {"host by equal ids", 2, 1, 7, 7, true},
                    {"client, connection NULL", 2, 1, 3, 7, false}, {"client, host id 0xffffffff", 9, 0x100000000ull, 3, 0xffffffffu, true},
                    {"ctx high bits only", 2, 0xffffffff00000000ull, 3, 4, true}};
    for (const C &c : cs)
        for (int twice = 0; twice < 2; ++twice)
            img_case(std::string(c.name) + (twice ? " (twice)" : ""),
                     [&] {
                         conn_setup();
                         hx::put<int32_t>(kNetState, c.state);
                         hx::put<uint64_t>(kMatch + 0x10, c.ctx);
                         hx::put<uint32_t>(kMatch + 0xb00, c.me);
                         hx::put<uint32_t>(kMatch + 0xb04, c.host);
                     },
                     [&](Side &s) {
                         for (int k = 0; k <= twice; ++k) s.start_apply(k ? nullptr : hx::mem + oEv, c.conn ? hx::mem + 0x1152a58 : nullptr);
                     });

    begin("S23 ConnectionDone Apply");
    const uint64_t first = kRecords + 0x40;
    auto tracker = [](int i) { return kNgm + 0x10 + uint64_t(i) * 0x2c0; };
    struct D {
        std::string name;
        int64_t conn;   // image offset of the connection argument; -1 = NULL, -2 = a host pointer
        std::function<void()> extra;
    };
    auto set_record = [&](int r, uint32_t key, uint8_t used) {
        hx::put<uint32_t>(kRecords + r * 0x220, key);
        hx::put<uint8_t>(kRecords + r * 0x220 + 4, used);
    };
    auto set_tracker = [&](int i, const char *name, int32_t key, uint32_t netid) {
        std::strcpy(reinterpret_cast<char *>(hx::mem + tracker(i) + 0x20), name);
        hx::put<int32_t>(tracker(i) + 0x23c, key);
        hx::put<uint32_t>(tracker(i) + 0x120, netid);
        hx::put<uint8_t>(tracker(i) + 0x275, 0x77);
        hx::put<uint8_t>(tracker(i) + 0x27c, 0x66);
    };
    std::vector<D> ds = {
        {"connection NULL", -1, [] {}},
        {"host pointer", -2, [] {}},
        {"below the records", int64_t(first - 0x220), [&] { set_record(0, 5, 1); set_tracker(0, "a", 5, 1); }},
        {"one byte below", int64_t(first - 1), [&] { set_record(0, 5, 1); set_tracker(0, "a", 5, 1); }},
        {"misaligned", int64_t(first + 8), [&] { set_record(0, 5, 1); set_tracker(0, "a", 5, 1); }},
        {"misaligned +0x110", int64_t(first + 0x110), [&] { set_record(0, 5, 1); set_tracker(0, "a", 5, 1); }},
        {"record 0", int64_t(first), [&] { set_record(0, 5, 1); set_tracker(3, "alpha", 5, 11); }},
        {"record 31", int64_t(first + 31 * 0x220), [&] { set_record(31, 9, 1); set_tracker(9, "omega", 9, 99); }},
        {"record 32 (out of range)", int64_t(first + 32 * 0x220), [&] { hx::put<uint32_t>(kRecords + 32 * 0x220, 9); hx::put<uint8_t>(kRecords + 32 * 0x220 + 4, 1); set_tracker(0, "x", 9, 1); }},
        {"record not in use", int64_t(first + 2 * 0x220), [&] { set_record(2, 5, 0); set_tracker(0, "a", 5, 1); }},
        {"record in use, key 0", int64_t(first + 2 * 0x220), [&] { set_record(2, 0, 1); set_tracker(0, "a", 0, 1); }},
        {"record in use byte 0x80", int64_t(first + 4 * 0x220), [&] { set_record(4, 6, 0x80); set_tracker(1, "b", 6, 2); }},
        {"key with no tracker", int64_t(first + 2 * 0x220), [&] { set_record(2, 5, 1); set_tracker(0, "a", 6, 1); }},
        {"empty-name tracker with the key, then a named one", int64_t(first + 2 * 0x220),
         [&] { set_record(2, 5, 1); set_tracker(0, "", 5, 1); set_tracker(4, "named", 5, 44); }},
        {"only an empty-name tracker", int64_t(first + 2 * 0x220), [&] { set_record(2, 5, 1); set_tracker(0, "", 5, 1); }},
        {"two trackers with the key", int64_t(first + 2 * 0x220),
         [&] { set_record(2, 5, 1); set_tracker(2, "first", 5, 21); set_tracker(7, "second", 5, 71); }},
        {"negative key", int64_t(first + 5 * 0x220), [&] { set_record(5, 0xfffffff0u, 1); set_tracker(6, "neg", -16, 66); }},
        {"key 0x80000000", int64_t(first + 5 * 0x220), [&] { set_record(5, 0x80000000u, 1); set_tracker(9, "min", INT32_MIN, 0xffffffffu); }},
        {"tracker 9 only, long name", int64_t(first + 1 * 0x220),
         [&] { set_record(1, 77, 1); set_tracker(9, "a-rather-long-player-name-with-%d-in-it", 77, 1234567); }},
    };
    for (const D &d : ds)
        for (uint8_t can : {uint8_t(0xab), uint8_t(0), uint8_t(1)})
            img_case(d.name + " can " + std::to_string(can),
                     [&] {
                         conn_setup();
                         hx::put<uint8_t>(oEv + 0x0c, can);
                         d.extra();
                     },
                     [&](Side &s) {
                         static uint8_t host_buf[0x400];
                         void *conn = d.conn == -1 ? nullptr : d.conn == -2 ? host_buf + 0x40 : hx::mem + d.conn;
                         s.done_apply(hx::mem + oEv, conn);
                     });
}

// ---- installs (6.14, 6.24 - 6.27) ---------------------------------------------------------------------
void scen_install() {
    begin("S24 good image: both installs");
    img_case("good image", [] { good_image(); },
             [](Side &s) {
                 *s.game = Game{};
                 int n = 5;
                 s.install_events(n);
                 hx::rec("installed after events %d", n);
                 record_game(s);
                 s.install_gameplay2(n);
                 hx::rec("installed after gameplay2 %d", n);
                 mask_slots(s);   // also shows 14: type 44 slot 3 left alone (image compare)
             });
    img_case("good image, installed twice", [] { good_image(); },
             [](Side &s) {
                 *s.game = Game{};
                 int n = 0;
                 s.install_events(n);
                 s.install_gameplay2(n);
                 s.install_events(n);
                 s.install_gameplay2(n);
                 hx::rec("installed %d", n);
                 mask_slots(s);
             });

    begin("S25 slots already replaced / stub bytes differ");
    struct E {
        const char *name;
        std::function<void()> f;
    };
    const E es[] = {
        {"type 38 slot 3 value differs", [] { hx::put<uint64_t>(0x1040760 + 0x18, hx::addr(0x622071)); }},
        {"type 37 slot 4 is a host pointer", [] { hx::put<uint64_t>(0x1040f30 + 0x20, 0x12345678ull); }},
        {"type 54 read stub bytes differ", [] { hx::put<uint8_t>(0x61e2a0 + 4, 0x67); }},
        {"type 76 write stub first byte differs", [] { hx::put<uint8_t>(0x61afc0, 0xc2); }},
        {"type 44 read slot 0", [] { hx::put<uint64_t>(0x10405d0 + 0x10, 0); }},
        {"type 47 both slots", [] { hx::put<uint64_t>(0x1040580 + 0x10, 1); hx::put<uint64_t>(0x1040580 + 0x18, 2); }},
        {"type 37 slots 2,3,4 all differ",
         [] { for (int s = 2; s <= 4; ++s) hx::put<uint64_t>(0x1040f30 + 8 * s, hx::addr(0x62f220 + 0x10 * (4 - s) + 1)); }},
        {"type 49 slot 2 points at the write stub", [] { hx::put<uint64_t>(0x1040440 + 0x10, hx::addr(0x61f020)); }},
    };
    for (const E &e : es)
        img_case(e.name,
                 [&] {
                     good_image();
                     e.f();
                 },
                 [](Side &s) {
                     *s.game = Game{};
                     int n = 0;
                     s.install_events(n);
                     hx::rec("installed %d", n);
                     mask_slots(s);
                 });

    begin("S26 image too small / no image");
    for (uint64_t size : {uint64_t(0), uint64_t(0x1116387), uint64_t(0x1116388), uint64_t(0x1040f57), uint64_t(0x1040f58)})
        img_case("events size " + hexs(size), [] { good_image(); },
                 [&](Side &s) {
                     *s.game = Game{};
                     hx::set_image_both(hx::mem, size);
                     int n = 3;
                     s.install_events(n);
                     hx::rec("installed %d", n);
                     record_game(s);
                     hx::bind_both();
                     mask_slots(s);
                 });
    img_case("events, image base NULL", [] { good_image(); },
             [](Side &s) {
                 *s.game = Game{};
                 hx::set_image_both(nullptr, hx::kImageSize);
                 int n = 3;
                 s.install_events(n);
                 hx::rec("installed %d", n);
                 record_game(s);
                 hx::bind_both();
             });

    begin("S27 gameplay2: anchors, RIP references, image size, slot not the stub");
    auto gp = [](Side &s) {
        int n = 4;
        s.install_gameplay2(n);
        hx::rec("installed %d", n);
        mask_slots(s);
    };
    for (size_t i = 0; i < 8; ++i)
        for (int which : {0, -1}) {
            img_case("anchor " + std::to_string(i) + (which ? " last byte" : " first byte"),
                     [&] {
                         good_image();
                         const auto &a = kAnchors[i];
                         hx::mem[a.at + (which ? a.b.size() - 1 : 0)] ^= 0x01;
                         if (i + 1 < 8) hx::mem[kAnchors[i + 1].at] ^= 0x10;   // a later mismatch too: only the first is logged
                     },
                     gp);
        }
    for (size_t i = 0; i < 3; ++i)
        for (int32_t delta : {1, -1, 0x10000000}) {
            img_case("rip " + std::to_string(i) + " delta " + std::to_string(delta),
                     [&] {
                         good_image();
                         const auto &r = kRips[i];
                         hx::put<int32_t>(r.at + r.disp, hx::get<int32_t>(r.at + r.disp) + delta);
                         if (i + 1 < 3) hx::put<int32_t>(kRips[i + 1].at + kRips[i + 1].disp, 0);
                     },
                     gp);
        }
    for (uint64_t size : {uint64_t(0x63c0cc), uint64_t(0x63c0cd), uint64_t(0x113d45f), uint64_t(0x113d460), uint64_t(0x1156e17),
                          uint64_t(0x1156e18)})
        img_case("gameplay2 size " + hexs(size), [] { good_image(); },
                 [&](Side &s) {
                     hx::set_image_both(hx::mem, size);
                     int n = 0;
                     s.install_gameplay2(n);
                     hx::rec("installed %d", n);
                     hx::bind_both();
                     mask_slots(s);
                 });
    img_case("gameplay2, image base NULL", [] { good_image(); },
             [](Side &s) {
                 hx::set_image_both(nullptr, hx::kImageSize);
                 int n = 0;
                 s.install_gameplay2(n);
                 hx::rec("installed %d", n);
                 hx::bind_both();
             });
    const E gs[] = {
        {"StartConnection slot not the stub", [] { hx::put<uint64_t>(kStartVt + 0x20, hx::addr(kStartStub + 1)); }},
        {"ConnectionDone stub bytes differ", [] { hx::put<uint8_t>(kDoneStub + 2, 0x90); }},
        {"both slots wrong", [] { hx::put<uint64_t>(kStartVt + 0x20, 0); hx::put<uint64_t>(kDoneVt + 0x20, hx::addr(kStartStub)); }},
    };
    for (const E &e : gs)
        img_case(e.name,
                 [&] {
                     good_image();
                     e.f();
                 },
                 gp);
}

void scen_table() {
    begin("kPorts table (types, names, vtables, sizes, stubs, which functions exist)");
    std::vector<std::string> rec[2];
    for (int i = 0; i < 2; ++i) {
        hx::calls.clear();
        const Side &s = *SIDES[i];
        hx::rec("count %zu", *s.count);
        for (size_t k = 0; k < *s.count; ++k) {
            const Port &p = s.ports[k];
            hx::rec("%d %s vt %llx size %x stubs %llx %llx %llx fns %d%d%d", p.type, p.name, (unsigned long long)p.vtable, p.size,
                    (unsigned long long)p.read_stub, (unsigned long long)p.write_stub, (unsigned long long)p.apply_stub, !!p.read,
                    !!p.write, !!p.apply);
        }
        rec[i] = hx::calls;
    }
    check("table", rec[0], rec[1]);
}

}  // namespace

int main() {
    hx::init();
    hx::clear();
    hx::bind_both();
    scen_table();
    scen_write_overflow();   // first: owns the once-per-process overflow log
    scen_round_trip_and_cuts();
    scen_time_read();
    scen_encodings();
    scen_territory_own();
    scen_event();
    scen_flags();
    scen_random();
    capture_installed();
    scen_npc_spawn();
    scen_game_functions();
    scen_connections();
    scen_install();
    long fails = 0;
    for (const Group &g : groups) {
        std::printf("%-4s %-100s %6ld cases%s\n", g.fails ? "FAIL" : "PASS", g.name.c_str(), g.cases,
                    g.fails ? (" (" + std::to_string(g.fails) + " differ)").c_str() : "");
        fails += g.fails;
    }
    std::printf("%s: %zu groups, %ld cases, %ld differ\n", fails ? "FAILED" : "ok", groups.size(), total_cases, fails);
    return fails ? 1 : 0;
}
