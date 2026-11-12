// v7.136 part O: the old helper code (v7.135) for the hat modifiers, 1.19 weapon numbers and mode, and the icon quad.
static int o_hat(uint8_t *np, uint8_t *rows, int32_t n, uint32_t item, int sl, uint32_t *c47) {
    for (int r = 0; r < n; r++) { uint8_t *row = rows + (size_t)r * 16; if (*(uint32_t *)row != item) continue;
        int32_t c = *(int32_t *)(row + 4); uint8_t *pp = *(uint8_t **)(row + 8);
        for (int k = 0; k < c && pp; k++) { uint32_t cnt = *(uint32_t *)(np + 0x84c); if (cnt >= 16) return -1;
            uint8_t *e = np + 0x850 + (size_t)cnt * 16; *(uint32_t *)(np + 0x84c) = cnt + 1;
            *(uint32_t *)e = *(uint32_t *)(pp + k * 8); *(uint32_t *)(e + 4) = *(uint32_t *)(pp + k * 8 + 4); *(uint32_t *)(e + 8) = (uint32_t)sl;
            e[0xc] = 4; e[0xd] = 0; (*c47)++; } }
    return 0;
}
static uint32_t o_witem(const uint32_t *w, uint8_t *l, uint32_t id) {
    for (int i = 0; i < 20; i++) if (w[i] == id) return (uint32_t)i; if (!l) return 0;
    int32_t n = *(int32_t *)l; uint32_t *e = *(uint32_t **)(l + 8); for (int32_t i = 0; i < n && e; i++) if (e[i] == id) return (uint32_t)i + 0x14; return 0;
}
static void o_quad(uint32_t *v, uint32_t x, uint32_t y, uint32_t h, uint32_t a, uint32_t depth) {
    uint32_t x0 = f_sub(x, h), x1 = f_add(x, h), y0 = f_sub(y, h), y1 = f_add(y, h);
    for (int k = 0; k < 0x44; k++) v[k] = 0;
    const uint32_t px[4] = {x0, x1, x1, x0}, py[4] = {y0, y0, y1, y1}, pu[4] = {0, F_ONE, F_ONE, 0}, pv[4] = {0, 0, F_ONE, F_ONE};
    for (int k = 0; k < 4; k++) { uint32_t *w = v + k * 16; w[0] = px[k]; w[1] = py[k]; w[3] = pu[k]; w[4] = pv[k]; w[8] = w[9] = w[10] = F_ONE; w[11] = a; w[12] = w[13] = w[14] = w[15] = F_ONE; }
    v[0x40] = depth;
}

static void batch23() {
    for (int t = 0; t < 5000; t++) {
        static uint8_t np1[0x960], np2[0x960]; for (uint32_t k = 0; k < sizeof np1; k++) np1[k] = np2[k] = (uint8_t)rnd();
        uint32_t cnt = rnd() % 18; std::memcpy(np1 + 0x84c, &cnt, 4); std::memcpy(np2 + 0x84c, &cnt, 4);
        static uint8_t pairs[8][8 * 6]; for (auto &p : pairs) for (auto &x : p) x = (uint8_t)rnd();
        alignas(8) static uint8_t rows[8 * 16]; const int32_t n = (int32_t)(rnd() % 9);
        for (int r = 0; r < 8; r++) { uint32_t item = rnd() % 4; int32_t c = (int32_t)(rnd() % 7) - 1; uint8_t *pp = rnd() % 6 ? pairs[r] : nullptr;
            std::memcpy(rows + r * 16, &item, 4); std::memcpy(rows + r * 16 + 4, &c, 4); std::memcpy(rows + r * 16 + 8, &pp, 8); }
        const uint32_t item = rnd() % 4; const int sl = 2 + (int)(rnd() % 5); uint32_t a47 = 5, b47 = 5;
        CHECK(o_hat(np1, rows, n, item, sl, &a47) == u3x_add_hat_mods(np2, rows, n, item, (uint32_t)sl, &b47)); CHECK(a47 == b47); CHECK(std::memcmp(np1, np2, sizeof np1) == 0);
    }
    uint32_t w20[20]; for (uint32_t k = 0; k < 20; k++) w20[k] = 0x500 + k;
    uint32_t ids[30]; for (uint32_t k = 0; k < 30; k++) ids[k] = 0x600 + k;
    for (int v = 0; v < 4; v++) { alignas(8) uint8_t l[16] = {}; int32_t n = v == 3 ? -1 : 30; uint32_t *ip = v == 2 ? nullptr : ids; std::memcpy(l, &n, 4); std::memcpy(l + 8, &ip, 8);
        uint8_t *lp = v == 1 ? nullptr : l; for (uint32_t id = 0x4f0; id < 0x640; id++) CHECK(o_witem(w20, lp, id) == u3x_weapon_number_119(w20, lp, id)); }
    for (uint32_t m = 0; m < 10; m++) CHECK((m == 1 ? 1u : m == 5 ? 3u : m == 0 ? 0u : 4u) == u3x_weapon_mode_119(m));
    for (int t = 0; t < 20000; t++) {
        uint32_t a[0x44], b[0x44]; const float fx = (float)(rnd() % 4000) - 1000.0f, fy = (float)(rnd() % 3000) - 500.0f, fh = (float)(rnd() % 100) * 0.37f;
        uint32_t x, y, h; std::memcpy(&x, &fx, 4); std::memcpy(&y, &fy, 4); std::memcpy(&h, &fh, 4); const uint32_t al = rnd(), dp = rnd();
        for (int k = 0; k < 0x44; k++) a[k] = b[k] = rnd();
        o_quad(a, x, y, h, al, dp); u3x_icon_quad(b, x, y, h, al, dp); CHECK(std::memcmp(a, b, sizeof a) == 0);
    }
}
