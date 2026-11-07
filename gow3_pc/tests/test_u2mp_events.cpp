// u2mp net-event serializer tests (gow3_pc/src/u2mp/events.cpp). Build: ninja -C gow3_pc/out/gpu u2mp-events-test
// For every ported type: fill a fake event buffer, Write it into a BitStream, Read it back into a
// fresh buffer and compare the fields; then cut the stream short and check the Read leaves the
// payload in its factory state without reading past maxBits.
#include "../src/u2mp/events.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

// The pieces of install.cpp events.cpp links against.
namespace u2mp {
Image image;
bool Image::matches(uint64_t, const unsigned char *, size_t) const { return false; }
void log(const char *, ...) {}
}  // namespace u2mp

using namespace u2mp;
using namespace u2mp::events;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

struct alignas(16) Event {
    uint8_t b[0x100];
};
template <typename T> static T get(const Event &e, size_t off) {
    T v;
    std::memcpy(&v, e.b + off, sizeof v);
    return v;
}
template <typename T> static void put(Event &e, size_t off, T v) { std::memcpy(e.b + off, &v, sizeof v); }

static const Port &port(int type) {
    for (size_t i = 0; i < kPortCount; ++i)
        if (kPorts[i].type == type) return kPorts[i];
    std::printf("no port %d\n", type);
    static Port none{};
    return none;
}

static int32_t clock_value = 300 * 100;   // 100 s into the match
static int mode = 12;
static int fake_mode() { return mode; }
alignas(16) static uint8_t fake_process[0x50];
static void *fake_lookup(uint32_t id) { return id == 0x1234 ? fake_process : nullptr; }

// Write `in` with the port, read it back into `out` (header bytes copied). Returns bits used.
static uint32_t round_trip(int type, const Event &in, Event &out) {
    const Port &p = port(type);
    std::vector<uint32_t> words(256, 0xa5a5a5a5u);   // dirty: the writer must not rely on zeros
    BitStream w{words.data(), 0, 256 * 32, 0};
    p.write(const_cast<uint8_t *>(in.b), &w);
    std::memset(out.b, 0, sizeof out.b);
    std::memcpy(out.b, in.b, 0xc);
    BitStream r{words.data(), 0, w.bits, 0};
    p.read(out.b, &r);
    CHECK(r.bits == w.bits);
    // Every shorter stream must fail cleanly: cursor within bounds, payload back to factory state.
    for (uint32_t cut = 0; cut < w.bits; cut += (w.bits > 64 ? 7 : 1)) {
        Event t;
        std::memset(t.b, 0x5a, sizeof t.b);
        BitStream s{words.data(), 0, cut, 0};
        p.read(t.b, &s);
        CHECK(s.bits <= cut);
        bool zero = true;
        for (size_t i = 0xc; i < p.size; ++i) {
            const uint8_t want = (type == 49 && i >= 0x10 && i < 0x14) ? 0xff : 0;
            if (t.b[i] != want) zero = false;
        }
        CHECK(zero);
    }
    return w.bits;
}

static void fill(Event &e, int type) {
    std::memset(e.b, 0, sizeof e.b);
    e.b[8] = uint8_t(type);
}

static bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

static void test_simple() {
    Event in, out;
    fill(in, 37);
    put<uint32_t>(in, 0xc, 0xdeadbeef); put<uint32_t>(in, 0x10, 0x11223344); put<uint32_t>(in, 0x14, 0x55667788); put<uint32_t>(in, 0x18, 7);
    CHECK(round_trip(37, in, out) == 128);
    CHECK(std::memcmp(in.b, out.b, 0x20) == 0);

    fill(in, 38); put<uint32_t>(in, 0xc, 0xcafef00d);
    CHECK(round_trip(38, in, out) == 32 && get<uint32_t>(out, 0xc) == 0xcafef00d);

    fill(in, 76); put<uint32_t>(in, 0xc, 99);
    CHECK(round_trip(76, in, out) == 32 && get<uint32_t>(out, 0xc) == 99);

    fill(in, 51); put<uint32_t>(in, 0xc, 1); put<uint32_t>(in, 0x10, 0x87654321);
    CHECK(round_trip(51, in, out) == 64 && std::memcmp(in.b, out.b, 0x18) == 0);

    fill(in, 61); put<uint32_t>(in, 0xc, 3); put<uint32_t>(in, 0x10, 4); put<uint32_t>(in, 0x14, 5);
    CHECK(round_trip(61, in, out) == 96 && std::memcmp(in.b, out.b, 0x18) == 0);

    for (int on = 0; on < 2; ++on) {
        fill(in, 52); put<uint32_t>(in, 0xc, 42); put<float>(in, 0x10, 12.5f); put<uint8_t>(in, 0x14, uint8_t(on));
        put<uint32_t>(in, 0x18, uint32_t(on));
        CHECK(round_trip(52, in, out) == uint32_t(65 + 1 * on));
        CHECK(std::memcmp(in.b, out.b, 0x20) == 0);
    }
    for (int small = 0; small < 2; ++small) {
        fill(in, 53); put<uint8_t>(in, 0xc, uint8_t(small)); put<uint32_t>(in, 0x10, small ? 0xabc : 0x89abcdef);
        put<uint32_t>(in, 0x14, 5); put<uint32_t>(in, 0x18, 6); put<float>(in, 0x1c, 1.5f); put<float>(in, 0x20, -2.0f);
        CHECK(round_trip(53, in, out) == uint32_t(small ? 141 : 161));
        CHECK(std::memcmp(in.b, out.b, 0x24) == 0);
    }
}

static void test_time_fields() {
    Event in, out;
    // NpcDealDamage (Read only is installed; the Write is the PS4 layout of 0x61fb10).
    fill(in, 44); put<uint32_t>(in, 0xc, 0xfff); put<uint32_t>(in, 0x10, 0x80000001); put<int32_t>(in, 0x14, -5);
    put<float>(in, 0x18, 99.5f); put<uint32_t>(in, 0x1c, 63);
    CHECK(round_trip(44, in, out) == 12 + 32 + 32 + 11 + 6);
    CHECK(get<uint32_t>(out, 0xc) == 0xfff && get<uint32_t>(out, 0x10) == 0x80000001 && get<int32_t>(out, 0x14) == -5);
    CHECK(near(get<float>(out, 0x18), 99.5f, 1.0f / 150) && get<uint32_t>(out, 0x1c) == 63);
    // The 11-bit time wraps every 2048/150 s; the reader moves it next to the frame clock.
    fill(in, 67); put<float>(in, 0xc, 0.25f); put<float>(in, 0x10, -1.0f); put<uint32_t>(in, 0x14, 77);
    put<float>(in, 0x18, 95.0f); put<uint32_t>(in, 0x1c, 0x123);
    CHECK(round_trip(67, in, out) == 32 + 32 + 32 + 11 + 12);
    CHECK(get<float>(out, 0xc) == 0.25f && get<float>(out, 0x10) == -1.0f && get<uint32_t>(out, 0x14) == 77);
    CHECK(near(get<float>(out, 0x18), 95.0f, 1.0f / 150) && get<uint32_t>(out, 0x1c) == 0x123);
}

static void put_locator(Event &e, size_t off, float x, float y, float z, const float q[4]) {
    put<float>(e, off, x); put<float>(e, off + 4, y); put<float>(e, off + 8, z); put<float>(e, off + 12, y);
    for (int i = 0; i < 4; ++i) put<float>(e, off + 0x10 + 4 * i, q[i]);
}
static bool locator_close(const Event &a, const Event &b, size_t off) {
    for (int i = 0; i < 4; ++i)
        if (!near(get<float>(a, off + 4 * i), get<float>(b, off + 4 * i), 1.0f / 512)) return false;
    for (int i = 0; i < 4; ++i)
        if (!near(get<float>(a, off + 0x10 + 4 * i), get<float>(b, off + 0x10 + 4 * i), 0.002f)) return false;
    return true;
}

static void test_locators() {
    Event in, out;
    const float q[4] = {0.0f, 0.70710678f, 0.0f, 0.70710678f};   // two components absent on the wire
    fill(in, 54); put_locator(in, 0x10, 12.5f, -3.25f, 200.0f, q);
    put<uint32_t>(in, 0x30, 0x1234); put<float>(in, 0x34, 100.0f); put<uint32_t>(in, 0x38, 0x456);
    CHECK(round_trip(54, in, out) == 54 + 4 + 2 * 13 + 32 + 11 + 12);
    CHECK(locator_close(in, out, 0x10));
    CHECK(get<uint32_t>(out, 0x30) == 0x1234 && near(get<float>(out, 0x34), 100.0f, 1.0f / 150) && get<uint32_t>(out, 0x38) == 0x456);

    const float q2[4] = {0.5f, -0.5f, 0.5f, -0.5f};
    fill(in, 55); put_locator(in, 0x10, -100.0f, 0.5f, 1.0f, q); put_locator(in, 0x30, 255.0f, -255.0f, 0.0f, q2);
    put<uint32_t>(in, 0x50, 0xabc); put<uint32_t>(in, 0x54, 0xfedcba98);
    for (size_t off = 0x58; off <= 0x68; off += 4) put<uint32_t>(in, off, uint32_t(off * 3));
    put<float>(in, 0x6c, 3.0f); put<float>(in, 0x70, 4.0f); put<uint32_t>(in, 0x74, 33); put<uint8_t>(in, 0x78, 1); put<uint8_t>(in, 0x79, 0);
    round_trip(55, in, out);
    CHECK(locator_close(in, out, 0x10) && locator_close(in, out, 0x30));
    CHECK(std::memcmp(in.b + 0x50, out.b + 0x50, 0x2a) == 0);

    fill(in, 57); put<float>(in, 0x10, -7.5f); put<float>(in, 0x14, 8.0f); put<float>(in, 0x18, 0.001953125f);
    put<float>(in, 0x1c, 8.0f); put<uint32_t>(in, 0x20, 0x31337);
    CHECK(round_trip(57, in, out) == 54 + 32);
    CHECK(std::memcmp(in.b + 0x10, out.b + 0x10, 0x14) == 0);
}

static void test_territory_own() {
    Event in, out;
    for (int m : {12, 3}) {
        mode = m;
        fill(in, 49); put<uint32_t>(in, 0xc, 0x777); put<int16_t>(in, 0x10, -1); put<int16_t>(in, 0x12, 5);
        put<uint32_t>(in, 0x14, 10);
        for (int i = 0; i < 10; ++i) put<uint32_t>(in, 0x18 + 4 * i, uint32_t(0x100 + i));
        put<uint32_t>(in, 0x40, 0x201); put<uint32_t>(in, 0x44, 0x202); put<float>(in, 0x48, m == 12 ? 0.75f : 0.0f);
        CHECK(round_trip(49, in, out) == uint32_t(32 + 32 + 4 + 120 + 24 + (m == 12 ? 32 : 0)));
        CHECK(std::memcmp(in.b + 0xc, out.b + 0xc, 0x40) == 0);
    }
    mode = 12;
    // A count beyond the 10 slots is rejected (PS3 would write past the array).
    std::vector<uint32_t> words(8, 0);
    BitStream w{words.data(), 0, 256, 0};
    write_bits(w, 1, 32); write_bits(w, 0, 16); write_bits(w, 0, 16); write_bits(w, 11, 4);
    BitStream r{words.data(), 0, 256, 0};
    fill(out, 49);
    port(49).read(out.b, &r);
    CHECK(get<uint32_t>(out, 0xc) == 0 && get<uint32_t>(out, 0x14) == 0 && get<uint32_t>(out, 0x10) == 0xffffffffu);
}

static void test_event() {
    Event in, out;
    fill(in, 47);
    put<uint32_t>(in, 0xfc, 0xa0a0a0a0);   // target
    put<uint32_t>(in, 0xf8, 0x1234);       // sender (a known process)
    put<uint32_t>(in, 0x18, 0x5eed1d);     // message id
    put<int8_t>(in, 0x30, 4);
    const uint32_t types[4] = {1, 2, 3, 6};
    const uint64_t values[4] = {0xff01, 0xffffffff, 0x9abc, 0x3f800000};
    for (int i = 0; i < 4; ++i) {
        put<uint64_t>(in, 0x38 + 0x18 * i, values[i]);
        put<uint32_t>(in, 0x38 + 0x18 * i + 0x10, types[i]);
    }
    const uint64_t proc_handle = 0x1122334455667788ull;
    const uint32_t proc_id = 0x99;
    std::memcpy(fake_process + 0x38, &proc_handle, 8);
    std::memcpy(fake_process + 0x44, &proc_id, 4);
    CHECK(round_trip(47, in, out) == 32 * 3 + 4 + 4 * 36);
    CHECK(get<uint32_t>(out, 0xfc) == 0xa0a0a0a0 && get<uint32_t>(out, 0xf8) == 0x1234 && get<uint32_t>(out, 0x18) == 0x5eed1d);
    CHECK(get<uint64_t>(out, 0x10) == 0 && get<int8_t>(out, 0x30) == 4);
    uint64_t handle;
    std::memcpy(&handle, fake_process + 0x38, 8);
    CHECK(get<uint64_t>(out, 0x20) == handle && get<uint32_t>(out, 0x28) == 0x99);
    const uint64_t want[4] = {1, 0xffffffff, 0x9abc, 0x3f800000};
    for (int i = 0; i < 4; ++i) {
        CHECK(get<uint64_t>(out, 0x38 + 0x18 * i) == want[i]);
        CHECK(get<uint32_t>(out, 0x38 + 0x18 * i + 0x10) == types[i]);
    }
    // Unknown sender: no process handle. More than 7 arguments on the wire: 7 kept, all consumed.
    std::vector<uint32_t> words(32, 0);
    BitStream w{words.data(), 0, 32 * 32, 0};
    write_bits(w, 1, 32); write_bits(w, 2, 32); write_bits(w, 3, 32); write_bits(w, 9, 4);
    for (int i = 0; i < 9; ++i) { write_bits(w, 2, 4); write_bits(w, uint32_t(i), 32); }
    BitStream r{words.data(), 0, w.bits, 0};
    fill(out, 47);
    port(47).read(out.b, &r);
    CHECK(r.bits == w.bits && get<int8_t>(out, 0x30) == 7 && get<uint64_t>(out, 0x20) == 0);
    CHECK(get<uint64_t>(out, 0x38 + 0x18 * 6) == 6 && get<uint32_t>(out, 0x38 + 0x18 * 7 + 0x10) == 0);
}

static void test_writer_bounds() {
    // A Write that does not fit never touches memory past maxBits.
    Event in;
    fill(in, 61); put<uint32_t>(in, 0xc, 0xffffffff); put<uint32_t>(in, 0x10, 0xffffffff); put<uint32_t>(in, 0x14, 0xffffffff);
    uint32_t words[4] = {0, 0, 0, 0x12345678};
    BitStream w{words, 0, 40, 0};
    port(61).write(in.b, &w);
    CHECK(words[0] == 0xffffffff && words[3] == 0x12345678 && (words[1] & 0x00ffffff) == 0);
}

int main() {
    game.frame_clock = &clock_value;
    game.game_mode = fake_mode;
    game.lookup_process = fake_lookup;
    test_simple();
    test_time_fields();
    test_locators();
    test_territory_own();
    test_event();
    test_writer_bounds();
    std::printf("%s: %zu ported types\n", failures ? "FAILED" : "ok", kPortCount);
    return failures ? 1 : 0;
}
