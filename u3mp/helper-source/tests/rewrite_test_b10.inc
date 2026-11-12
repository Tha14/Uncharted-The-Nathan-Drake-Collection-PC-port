// v7.123 part B: the old helper code (v7.122) for each rewritten piece, and the old-versus-new checks.
static void old_icon46(int32_t *event, uint32_t id, int32_t idx) { event[2] = 3; event[4] = 0; event[5] = 0; event[6] = 0; event[7] = (int32_t)id; event[8] = 1; event[9] = idx; event[10] = 0; event[11] = -1; event[12] = 0x3f800000; }
static void old_newmark(int32_t *event) { event[2] = 0x2f; event[4] = 0; event[5] = 0; event[6] = 0; event[7] = 0; event[8] = 0; event[9] = -1; event[10] = 0; event[11] = -1; event[12] = 0x3f800000; }
static void old_rank(int32_t *event, uint32_t rk) { event[2] = 0x29; *(uint64_t *)(event + 4) = 0; event[6] = (int32_t)rk; event[7] = 0; event[8] = 0; event[9] = -1; event[10] = 0; event[11] = -1; event[12] = 0x3f800000; }
static void old_spin(int32_t *event) { event[2] = 16; *(uint64_t *)(event + 4) = 0; event[6] = 0; event[7] = 0; event[8] = 0; event[9] = -1; event[10] = 0; event[11] = -1; event[12] = 0x3f800000; }
static uint32_t old_unlock(int st) { return st == 1 ? 0xc2bb8920u : st == 2 ? 0x83817313u : st == 3 ? 0x7bebc95fu : 0; }
static const char *old_other(uint8_t *ms, uint32_t r) {
    const char *nm = 0; uint32_t k = 0, me = *(uint32_t *)(ms + 0x12e4);
    for (int i = 0; i < 10 && !nm; i++) { uint8_t *m = ms + 0x680 + i * 0x120; if (!m[0x25] || *(uint32_t *)(m + 0x2c) == me) continue; if (++k == r) nm = (const char *)m; }
    return nm;
}
static uint32_t old_fnv(uint32_t sid) { uint32_t h = 0x811c9dc5u; for (int k = 0; k < 4; k++) h = (h ^ ((sid >> (8 * k)) & 0xffu)) * 0x1000193u; return h; }
static int old_loaded(uint8_t *p, uint32_t sid) { uint32_t c = *(uint32_t *)(p + 0x8c8); if (c > 64) return 0; for (uint32_t i = 0; i < c; i++) if (*(uint32_t *)(p + 0x298 + (size_t)i * 0x30) == sid) return 1; return 0; }
static int old_joiner(uint8_t *np) { return np[0xe1] && !np[0x5e4] && np[0x5f9] && !np[0x5fa]; }

static void batch10() {
    auto same = [](void (*fill_old)(int32_t *), void (*fill_new)(int32_t *)) { int32_t a[16], b[16]; for (int i = 0; i < 16; i++) a[i] = b[i] = 0x55550000 + i; fill_old(a); fill_new(b); return std::memcmp(a, b, sizeof a) == 0; };
    for (int32_t idx : {-1, 0, 7}) for (uint32_t id : {0u, 0xd8b04236u}) { int32_t a[16], b[16]; for (int i = 0; i < 16; i++) a[i] = b[i] = 0x66660000 + i; old_icon46(a, id, idx); u3x_icon_answer(b, 3, 0, (int32_t)id, 1, idx); CHECK(std::memcmp(a, b, sizeof a) == 0); }
    CHECK(same(old_newmark, [](int32_t *e) { u3x_icon_answer(e, 0x2f, 0, 0, 0, -1); }));
    CHECK(same(old_spin, [](int32_t *e) { u3x_icon_answer(e, 16, 0, 0, 0, -1); }));
    for (uint32_t rk : {0u, 1u, 77u, 0x40000005u}) { int32_t a[16], b[16]; for (int i = 0; i < 16; i++) a[i] = b[i] = 0x77770000 + i; old_rank(a, rk); u3x_icon_answer(b, 0x29, (int32_t)rk, 0, 0, -1); CHECK(std::memcmp(a, b, sizeof a) == 0); }
    for (int st = -2; st < 8; st++) CHECK(old_unlock(st) == u3x_unlock_icon(st));
    for (int t = 0; t < 5000; t++) {
        static uint8_t ms[0x1300]; for (auto &x : ms) x = 0; uint32_t me = rnd() % 6; std::memcpy(ms + 0x12e4, &me, 4);
        for (int i = 0; i < 10; i++) { uint8_t *m = ms + 0x680 + i * 0x120; m[0x25] = (uint8_t)(rnd() % 2); uint32_t id = rnd() % 6; std::memcpy(m + 0x2c, &id, 4); }
        for (uint32_t r = 1; r < 12; r++) CHECK(old_other(ms, r) == u3x_other_member(ms, r));
    }
    for (int t = 0; t < 200000; t++) { const uint32_t id = rnd() << 8 ^ rnd(); CHECK(old_fnv(id) == u3x_fnv1a_id(id)); }
    for (int t = 0; t < 2000; t++) {
        static uint8_t p[0x8d0]; for (auto &x : p) x = (uint8_t)rnd(); uint32_t c = t % 9 == 0 ? 65 + rnd() % 3 : rnd() % 30; std::memcpy(p + 0x8c8, &c, 4);
        for (uint32_t i = 0; i < 30; i++) { uint32_t id = rnd() % 40; std::memcpy(p + 0x298 + i * 0x30, &id, 4); }
        for (uint32_t id = 0; id < 41; id++) CHECK(old_loaded(p, id) == u3x_module_loaded(p, id));
    }
    for (int m = 0; m < 16; m++) { static uint8_t np[0x600]; np[0xe1] = m & 1; np[0x5e4] = (m >> 1) & 1; np[0x5f9] = (m >> 2) & 1; np[0x5fa] = (m >> 3) & 1; CHECK(old_joiner(np) == u3x_waiting_joiner(np)); }
}
