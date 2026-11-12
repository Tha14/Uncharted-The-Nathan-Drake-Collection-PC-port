// v7.127 part F: the old helper code (v7.126) for the game-socket bits and XP thresholds, and the old-versus-new checks.
typedef struct { uint32_t *w; uint32_t pos, max, x, pad; } OGtBits;
static void o_wb(OGtBits *b, uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) { uint32_t p = b->pos++; if (p >= b->max) continue;
    uint32_t m = 0x80000000u >> (p & 31); if ((v >> i) & 1) b->w[p >> 5] |= m; else b->w[p >> 5] &= ~m; } }
static uint32_t o_rb(OGtBits *b, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) { uint32_t p = b->pos++; uint32_t bit = p < b->max ? (b->w[p >> 5] >> (31 - (p & 31))) & 1 : 0; v = v << 1 | bit; } return v; }
static uint32_t o_words(const OGtBits *b) { uint32_t n = b->pos < b->max ? b->pos : b->max; return ((n + 31) >> 5) * 4; }
static int32_t o_scale(int32_t v, int32_t legacy) { if (legacy == 4) return (int32_t)((float)v * 2.5f); if (legacy == 5) return (int32_t)((float)v * 5.0f); return v; }
static int32_t o_thresh(int32_t *a, int32_t n, int32_t lvl, int32_t legacy) { int32_t i = lvl < 0 ? 0 : lvl; if (i > n - 1) i = n - 1; return o_scale(a[i], legacy); }
static int32_t o_from_xp(int32_t *a, int32_t n, int32_t xp, int32_t legacy) {
    int32_t lvl = n - 1; for (int32_t i = 0; i + 1 < n; i++) { int32_t t0 = o_scale(a[i], legacy), t1 = o_scale(a[i + 1], legacy); if (t0 <= xp && xp < t1) { lvl = i; break; } } return lvl;
}

static void batch14() {
    // bit streams: random write / read sequences on buffers of random size, past the end too
    for (int t = 0; t < 5000; t++) {
        uint32_t w1[20], w2[20]; for (int k = 0; k < 20; k++) w1[k] = w2[k] = rnd() << 8 ^ rnd();
        const uint32_t bytes = rnd() % 80; OGtBits a = {w1, 0, bytes << 3, 0, 0}; GtStream b = {w2, 0, bytes << 3, 0, 0};
        for (int step = 0; step < 30; step++) {
            const int n = 1 + (int)(rnd() % 32); const uint32_t v = rnd() << 8 ^ rnd();
            if (rnd() % 3) { o_wb(&a, v, n); u3x_gt_write(&b, v, n); } else CHECK(o_rb(&a, n) == u3x_gt_read(&b, n));
            if (rnd() % 7 == 0) { const uint32_t p = rnd() % (bytes * 8 + 40); a.pos = p; b.pos = p; }
            CHECK(a.pos == b.pos); CHECK(o_words(&a) == u3x_gt_bytes(&b));
        }
        CHECK(std::memcmp(w1, w2, sizeof w1) == 0);
    }
    // XP: every legacy level, random and large values, random tables
    for (int t = 0; t < 1000000; t++) {
        const int32_t v = t % 3 ? (int32_t)(rnd() % 20000000) : (int32_t)(rnd() << 8 ^ rnd());
        for (int32_t legacy = 0; legacy < 7; legacy++) CHECK(o_scale(v, legacy) == u3x_xp_scale(v, legacy));
    }
    for (int t = 0; t < 3000; t++) {
        int32_t a[90]; const int32_t n = 1 + (int32_t)(rnd() % 90); int32_t acc = 0; for (int32_t k = 0; k < n; k++) { acc += (int32_t)(rnd() % 200000); a[k] = acc; }
        const int32_t legacy = (int32_t)(rnd() % 7);
        for (int32_t lvl = -2; lvl < n + 2; lvl++) CHECK(o_thresh(a, n, lvl, legacy) == u3x_level_threshold(a, n, lvl, legacy));
        for (int k = 0; k < 40; k++) { const int32_t xp = (int32_t)(rnd() % (uint32_t)(acc * 6 + 10)) - 5; CHECK(o_from_xp(a, n, xp, legacy) == u3x_level_for_xp(a, n, xp, legacy)); }
    }
}
