// third split: the old v5.07 grid reader (vis_cell, vis_find_kill, vis_cell_point) against u3x_vis_* on random grids.
static int o33_ok(const void *p) { uintptr_t v = (uintptr_t)p; return v >= 0x10000u && v < (1ull << 47) && !(v & 1); }
static int o33_cell(const uint8_t *g, const int64_t c[3]) {
    uint32_t t = *(const uint32_t *)g, H = t == 2 ? 0x40u : 0x30u; int64_t idx[3], dim[3];
    for (int i = 0; i < 3; i++) { int64_t mn = *(const int32_t *)(g + 4 + 8 * i), mx = *(const int32_t *)(g + 8 + 8 * i); if (c[i] < mn || c[i] >= mx) return 2; idx[i] = c[i] - mn; dim[i] = mx - mn; }
    uint32_t a0 = g[0x1c], a1 = g[0x1d], a2 = g[0x1e]; if (a0 > 2 || a1 > 2 || a2 > 2) return 2;
    const uint8_t *runs = *(const uint8_t *const *)(g + H); uint32_t nsl = *(const uint32_t *)(g + H + 8);
    if (!o33_ok(runs) || (uint64_t)idx[a2] >= nsl) return 2;
    const uint8_t *sl = *(const uint8_t *const *)(g + H + 0x10 + idx[a2] * 8); if (!o33_ok(sl)) return 2;
    uint32_t nrows = *(const uint32_t *)runs, n = *(const uint16_t *)sl; int64_t r = 0, want = idx[a1], col = idx[a0];
    for (uint32_t j = 0; j < n; j++) { uint32_t e = ((const uint16_t *)(sl + 2))[j]; int64_t rep = e >> 14;
        if (want <= r + rep) { if ((e & 0x3fffu) >= nrows) return 1;
            const uint8_t *row = *(const uint8_t *const *)(runs + 8 + (uint64_t)(e & 0x3fffu) * 8); if (!o33_ok(row)) return 1;
            uint8_t v = row[0]; int64_t pos = 0;
            for (int64_t k = 1; pos < dim[a0] && k <= dim[a0] * 2 + 2; k++) { uint32_t L = row[k]; if (col < pos + L) return v == 0; pos += L; if (L != 255) v = (uint8_t)~v; }
            return 1; }
        r += rep + 1; }
    return 1; }
static int o33_find(const uint8_t *g, int32_t c[3]) {
    uint32_t t = *(const uint32_t *)g, H = t == 2 ? 0x40u : 0x30u; int32_t mn[3], dim[3];
    for (int i = 0; i < 3; i++) { mn[i] = *(const int32_t *)(g + 4 + 8 * i); dim[i] = *(const int32_t *)(g + 8 + 8 * i) - mn[i]; if (dim[i] <= 0) return 0; }
    uint32_t a0 = g[0x1c], a1 = g[0x1d], a2 = g[0x1e]; if (a0 > 2 || a1 > 2 || a2 > 2) return 0;
    const uint8_t *runs = *(const uint8_t *const *)(g + H); uint32_t nsl = *(const uint32_t *)(g + H + 8); if (!o33_ok(runs)) return 0;
    uint32_t nrows = *(const uint32_t *)runs;
    for (uint32_t k = 0; k < nsl && k < 4096; k++) { const uint8_t *sl = *(const uint8_t *const *)(g + H + 0x10 + (uint64_t)k * 8); if (!o33_ok(sl)) continue;
        uint32_t n = *(const uint16_t *)sl; int32_t r = 0;
        for (uint32_t j = 0; j < n; j++) { uint32_t e = ((const uint16_t *)(sl + 2))[j]; int32_t rep = (int32_t)(e >> 14);
            if ((e & 0x3fffu) < nrows) { const uint8_t *row = *(const uint8_t *const *)(runs + 8 + (uint64_t)(e & 0x3fffu) * 8);
                if (o33_ok(row)) { uint8_t v = row[0]; int32_t pos = 0;
                    for (int32_t q = 1; pos < dim[a0] && q <= dim[a0] * 2 + 2; q++) { uint32_t L = row[q];
                        if (v && L) { int32_t idx[3]; idx[a0] = pos; idx[a1] = r + rep / 2; idx[a2] = (int32_t)k; for (int i = 0; i < 3; i++) c[i] = idx[i] + mn[i]; return 1; }
                        pos += (int32_t)L; if (L != 255) v = (uint8_t)~v; } } }
            r += rep + 1; } }
    return 0; }
static uint32_t o33_point(int32_t c, uint32_t sc, uint32_t o) { uint32_t r;
    __asm__("cvtsi2ss %1,%%xmm0\n\tmovd %2,%%xmm1\n\tdivss %%xmm1,%%xmm0\n\tmovd %3,%%xmm1\n\taddss %%xmm1,%%xmm0\n\tmovd %%xmm0,%0" : "=r"(r) : "r"(c), "r"(sc), "r"(o) : "xmm0", "xmm1"); return r; }
static void batch33() {
    static uint8_t g[0x200], runs[8 + 8 * 16], rowbuf[16][40], slbuf[12][2 + 2 * 20];
    for (int t = 0; t < 4000; t++) {
        for (auto &b : g) b = 0;
        const uint32_t type = 1 + rnd() % 2, H = type == 2 ? 0x40u : 0x30u; *(uint32_t *)g = type;
        for (int i = 0; i < 3; i++) { int32_t mn = (int32_t)(rnd() % 10) - 5; *(int32_t *)(g + 4 + 8 * i) = mn; *(int32_t *)(g + 8 + 8 * i) = mn + (int32_t)(rnd() % 9) - (t % 50 == 0); }
        uint32_t perm = rnd() % 6; const uint8_t ax[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
        g[0x1c] = ax[perm][0]; g[0x1d] = ax[perm][1]; g[0x1e] = t % 97 == 0 ? 3 : ax[perm][2];
        const uint32_t nrows = 1 + rnd() % 16; *(uint32_t *)runs = nrows;
        for (uint32_t r = 0; r < 16; r++) { for (int k = 0; k < 40; k++) rowbuf[r][k] = (uint8_t)(rnd() % 4 == 0 ? 255 : rnd() % 5); rowbuf[r][0] = (uint8_t)(rnd() % 2 ? 0 : 0xff);
            *(uint8_t **)(runs + 8 + r * 8) = r % 11 == 10 ? nullptr : rowbuf[r]; }
        *(uint8_t **)(g + H) = runs; const uint32_t nsl = rnd() % 12; *(uint32_t *)(g + H + 8) = nsl;
        for (uint32_t k = 0; k < 12 && H + 0x10 + k * 8 + 8 <= sizeof(g); k++) { const uint16_t n = (uint16_t)(rnd() % 20); *(uint16_t *)slbuf[k] = n;
            for (uint16_t j = 0; j < n; j++) ((uint16_t *)(slbuf[k] + 2))[j] = (uint16_t)(((rnd() % 4) << 14) | (rnd() % 20));
            *(uint8_t **)(g + H + 0x10 + k * 8) = k % 7 == 6 ? nullptr : slbuf[k]; }
        for (int q = 0; q < 60; q++) { int64_t c[3]; for (int i = 0; i < 3; i++) c[i] = (int64_t)(rnd() % 20) - 10; CHECK(o33_cell(g, c) == u3x_vis_cell(g, c)); }
        int32_t a[3] = {7, 7, 7}, b[3] = {7, 7, 7}; CHECK(o33_find(g, a) == u3x_vis_find_kill(g, b)); CHECK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);
        const int32_t cc = (int32_t)rnd(); const uint32_t sc = rnd(), o = rnd(); CHECK(o33_point(cc, sc, o) == u3x_vis_cell_point(cc, sc, o));
    }
}
