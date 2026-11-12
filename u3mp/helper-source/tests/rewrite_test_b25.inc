// v7.138 part P: the old helper code (v7.137) for the ring colour, the page stack and the match start states.
static uint32_t t25_i2f(int32_t v) { float f = (float)v; uint32_t u; std::memcpy(&u, &f, 4); return u; }
static int32_t t25_trunc(uint32_t a) { float f; std::memcpy(&f, &a, 4); return (int32_t)f; }
static int t25_lt(uint32_t a, uint32_t b) { float x, y; std::memcpy(&x, &a, 4); std::memcpy(&y, &b, 4); return x < y; }
static void o_ring(uint32_t *col, uint32_t mode, uint32_t alpha, uint32_t p, int32_t ticks) {
    if (mode == 1) { col[0] = 0x3f70f0f2u; col[1] = 0x3dc0c0c2u; col[2] = 0x3d808081u; }
    else if (mode == 2) { col[0] = 0x3e008081u; col[1] = 0x3f008081u; col[2] = 0x3f70f0f2u; }
    else { col[0] = 0x3e008081u; col[1] = 0x3e008081u; col[2] = 0x3f70f0f2u; }
    col[3] = alpha;
    if (t25_lt(p, 0x3e800000u)) {
        uint32_t t = f_mul(f_mul(t25_i2f(ticks), 0x3b5a740eu), 0x40400000u);
        uint32_t fr = f_sub(t, t25_i2f(t25_trunc(t)));
        if (t25_lt(fr, 0x3f000000u)) { col[0] = col[1] = col[2] = F_ONE; } }
}
static int o_in_stack(uint8_t *mm, uint32_t page) { int32_t dp = *(int32_t *)(mm + 0xdb0); for (int q = 0; q <= dp && q < 160; q++) { uint8_t *p = *(uint8_t **)(mm + 0x8b0 + (int64_t)q * 8); if (p && *(uint32_t *)(p + 0x10) == page) return 1; } return 0; }

static void batch25() {
    for (int t = 0; t < 300000; t++) {
        uint32_t a[4], b[4]; const uint32_t mode = rnd() % 7, alpha = rnd();
        float pf = (float)(rnd() % 1000) / 1000.0f; uint32_t p; std::memcpy(&p, &pf, 4); if (t % 50 == 0) p = 0x7fc00000u;
        const int32_t ticks = (int32_t)(rnd() << 8 ^ rnd());
        o_ring(a, mode, alpha, p, ticks); u3x_ring_colour(b, mode, alpha, p, ticks); CHECK(std::memcmp(a, b, sizeof a) == 0);
    }
    static uint8_t pages[6][0x20]; for (int k = 0; k < 6; k++) { uint32_t id = 0x900 + (uint32_t)k; std::memcpy(pages[k] + 0x10, &id, 4); }
    for (int t = 0; t < 5000; t++) {
        static uint8_t mm[0xdc0]; for (int q = 0; q < 160; q++) { uint8_t *p = rnd() % 4 ? pages[rnd() % 6] : nullptr; std::memcpy(mm + 0x8b0 + q * 8, &p, 8); }
        int32_t dp = (int32_t)(rnd() % 170) - 3; std::memcpy(mm + 0xdb0, &dp, 4);
        for (uint32_t id = 0x8ff; id < 0x907; id++) CHECK(o_in_stack(mm, id) == u3x_page_in_stack(mm, id));
    }
    for (uint32_t ns = 0; ns < 0x40; ns++) for (int bg = 0; bg < 2; bg++) {
        CHECK((ns >= 0x2c && ns <= 0x30) == (u3x_mm_starting(ns) != 0));
        CHECK((bg && (ns == 0xb || ns == 0xf || ns == 0x11 || ns == 0x28)) == (u3x_mm_joining(ns, bg) != 0));
    }
}
