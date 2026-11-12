// rewrite_test: the v7.113+ C++ rewrites against the old helper code (copied here as the reference) on the same inputs.
// Build: clang++ -std=c++20 -O1 rewrite_test.cpp -o rewrite_test.exe (includes the helper's u3x_fixes.cpp).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#define U3X_FN [[gnu::noinline]]
#define U3X_RO
#define U3X_BSS [[gnu::used]]
#include "u3x_fixes_for_test.inc"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// ---- old code (v7.112), reduced to the logic under test
static void old_match_defaults(uint8_t *m) {
    *(int32_t *)(m + 0x350) = -1; m[0x354] = 0; *(int32_t *)(m + 0x358) = 0; *(int32_t *)(m + 0x35c) = 0;
    m[0x6a24] = 0; m[0x6a48] = 2;
}
static uint8_t *fake_ag(uintptr_t base, uint32_t row, uint32_t col) {
    return (row * 7 + col) % 3 == 0 ? nullptr : reinterpret_cast<uint8_t *>(base + row * 100 + col + 1);
}
static uint32_t old_anim(uintptr_t base, uint8_t *g) {
    uint8_t **t = (uint8_t **)(g + 0x68); uint32_t row = *(uint32_t *)(g + 0x60), got = 0;
    for (uint32_t i = 0; i < 8; i++) { t[i] = fake_ag(base, row, i); if (t[i]) got |= 1u << i; }
    return got;
}
static int old_oasis(uint32_t id) { return id == 0xdeac0df3u || id == 0x82fb8e9eu || id == 0x421d5e2cu; }
static uint8_t *old_playlist(uint8_t *t, uint32_t id) {
    if (!t || *(int32_t *)t <= 0) return 0; uint8_t *e = *(uint8_t **)(t + 8); if (!e) return 0;
    for (int32_t i = 0; i < *(int32_t *)t; i++) if (*(uint32_t *)(e + (size_t)i * 0x68) == id) return e + (size_t)i * 0x68;
    return e;
}
static std::vector<uint8_t *> applied, after_seen;
static void rec_apply(void *o) { applied.push_back(static_cast<uint8_t *>(o)); }
static void rec_after(uintptr_t, uint8_t *o) { after_seen.push_back(o); }
static void old_skins(uint8_t *l) {
    for (int i = 0; i < 64; i++, l += 0x10) {
        uint8_t **h = *(uint8_t ***)l; uint32_t id = *(uint32_t *)(l + 8); if (!h || !id) continue;
        uint8_t *o = *h; if (!o || *(uint32_t *)(o + 0x7c) != id) continue;
        rec_apply(o); rec_after(0, o);
    }
}

#include "rewrite_test_b2.inc"
#include "rewrite_test_b3.inc"
#include "rewrite_test_b4.inc"
#include "rewrite_test_b5.inc"
#include "rewrite_test_b6.inc"
#include "rewrite_test_b7.inc"
#include "rewrite_test_b8.inc"
#include "rewrite_test_b9.inc"
#include "rewrite_test_b10.inc"
#include "rewrite_test_b11.inc"
#include "rewrite_test_b12.inc"
#include "rewrite_test_b13.inc"
#include "rewrite_test_b14.inc"
#include "rewrite_test_b15.inc"
#include "rewrite_test_b16.inc"
#include "rewrite_test_b17.inc"
#include "rewrite_test_b18.inc"
#include "rewrite_test_b19.inc"
#include "rewrite_test_b20.inc"
#include "rewrite_test_b21.inc"
#include "rewrite_test_b22.inc"
#include "rewrite_test_b23.inc"
#include "rewrite_test_b25.inc"
#include "rewrite_test_b26.inc"
#include "rewrite_test_b27.inc"
#include "rewrite_test_b28.inc"
#include "rewrite_test_b29.inc"
#include "rewrite_test_b30.inc"
#include "rewrite_test_b31.inc"
#include "rewrite_test_b32.inc"

int main() {
    // match defaults: same bytes from a filled buffer
    std::vector<uint8_t> a(0x7000, 0xab), b(0x7000, 0xab);
    old_match_defaults(a.data()); u3x_match_defaults(b.data());
    CHECK(a == b);
    // anim tables: same pointers and mask for several rows
    for (uint32_t row = 0; row < 20; row++) {
        alignas(8) uint8_t g1[0xb0] = {}, g2[0xb0] = {};
        std::memcpy(g1 + 0x60, &row, 4); std::memcpy(g2 + 0x60, &row, 4);
        const uint32_t m1 = old_anim(0x100000, g1), m2 = u3x_anim_tables(0x100000, g2, fake_ag);
        CHECK(m1 == m2); CHECK(std::memcmp(g1, g2, sizeof g1) == 0);
    }
    // oasis: the three ids and their neighbours
    for (uint32_t id : {0xdeac0df3u, 0x82fb8e9eu, 0x421d5e2cu, 0u, 1u, 0xdeac0df2u, 0x421d5e2du, 0xffffffffu})
        CHECK(old_oasis(id) == u3x_is_oasis(id));
    // playlist: found, not found (first), empty, missing entries, null table
    alignas(8) uint8_t entries[0x68 * 4] = {};
    for (uint32_t i = 0; i < 4; i++) { const uint32_t id = 0x1000 + i; std::memcpy(entries + i * 0x68, &id, 4); }
    alignas(8) uint8_t table[16] = {};
    int32_t n = 4; uint8_t *ep = entries; std::memcpy(table, &n, 4); std::memcpy(table + 8, &ep, 8);
    for (uint32_t id : {0x1000u, 0x1002u, 0x1003u, 0x9999u, 0u}) CHECK(old_playlist(table, id) == u3x_pick_playlist(table, id));
    n = 0; std::memcpy(table, &n, 4); CHECK(old_playlist(table, 0x1000) == u3x_pick_playlist(table, 0x1000));
    n = 2; std::memcpy(table, &n, 4); ep = nullptr; std::memcpy(table + 8, &ep, 8);
    CHECK(old_playlist(table, 0x1000) == u3x_pick_playlist(table, 0x1000));
    CHECK(u3x_pick_playlist(nullptr, 1) == nullptr);
    // pending skins: live, stale (id changed), empty handle, empty id, null object
    alignas(16) uint8_t objs[5][0x80] = {};
    uint8_t *handles[5];
    alignas(16) uint8_t list[64 * 16] = {};
    for (int i = 0; i < 5; i++) {
        handles[i] = objs[i]; const uint32_t id = 0x500 + i; std::memcpy(objs[i] + 0x7c, &id, 4);
        uint8_t **h = &handles[i]; uint32_t want = (i == 1) ? 0x777 : (i == 3 ? 0 : id);
        std::memcpy(list + i * 16, &h, 8); std::memcpy(list + i * 16 + 8, &want, 4);
    }
    handles[4] = nullptr;
    old_skins(list); auto a1 = applied, f1 = after_seen; applied.clear(); after_seen.clear();
    u3x_apply_pending_skins(0, list, rec_apply, rec_after);
    CHECK(a1 == applied); CHECK(f1 == after_seen); CHECK(applied.size() == 2);
    batch2();
    batch3();
    batch4();
    batch5();
    batch6();
    batch7();
    batch8();
    batch9();
    batch10();
    batch11();
    batch12();
    batch13();
    batch14();
    batch15();
    batch16();
    batch17();
    batch18();
    batch19();
    batch20();
    batch21();
    batch22();
    batch23();
    batch25();
    batch26();
    batch27();
    batch28();
    batch29();
    batch30();
    batch31();
    batch32();
    batch28b();
    std::printf("%s (%d failures)\n", fails ? "FAILED" : "all equal", fails);
    return fails ? 1 : 0;
}
