// v7.128 part G: the old helper code (v7.127) and the old-versus-new checks.
static int o_scmp(const char *a, const char *b) {
    for (;;) {
        int x = (uint8_t)*a++, y = (uint8_t)*b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}
static void o_sncpy(char *d, const char *s, uint32_t n) { uint32_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; }
static int o_in_title(uint8_t *e, const char *title) {
    int32_t st = *(int32_t *)(e + 0x1b0); if (st == 1) return 1; if (st != 2) return 0; return o_scmp((const char *)e + 0x28, title) == 0;
}
static int o_same(const uint8_t *a, const uint8_t *b) { if (!a[0]) return 0; for (uint32_t i = 0; i < 0x20; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; } return 1; }
static void o_i05_defaults(uint8_t *p, uint8_t *d) {
    *(uint32_t *)(p + 0x248) = 0x56; std::memset(p + 0x24c, 0, 0x2824);
    if (d) *(uint32_t *)(p + 0x74c) = *(uint32_t *)d;
    *(uint32_t *)(p + 0x260) = 0x6afcc877u; *(uint32_t *)(p + 0x264) = 0xbe693215u; *(uint32_t *)(p + 0x26c) = 0x9e96d6d7u;
    *(uint32_t *)(p + 0x320) = 0xe0b92307u; *(uint32_t *)(p + 0x324) = 0x85e71b0eu; *(uint32_t *)(p + 0x32c) = 0x6bbaf02au;
    *(uint32_t *)(p + 0x784) = 0xc56be227u; *(uint32_t *)(p + 0x1870) = 0x3f000000u;
}
static int o_dtype(uint32_t t) { return t == 4 || t == 8 || t == 9 || t == 0xb || t == 0xd || t == 0x10 || t == 0x11; }
static void o_vopen(uint8_t *W) { *(int32_t *)(W + 4) = -1; W[1] = 0; W[0] = 1; for (int k = 0; k < 4; k++) *(int32_t *)(W + 8 + 4 * k) = 0; }
static int o_fake(uint32_t now) { uint32_t r = ((now * 2654435761u) >> 9) % 10u; return r == 0 ? 0 : r < 5 ? 1 : r < 9 ? 2 : 3; }

static void batch15() {
    static const char pool[] = "aAbBzZ[`@{\x7f~";
    for (int t = 0; t < 200000; t++) {
        char a[12], b[12];
        const int la = (int)(rnd() % 11), lb = (int)(rnd() % 11);
        for (int k = 0; k < la; k++) a[k] = pool[rnd() % 12];
        a[la] = 0;
        for (int k = 0; k < lb; k++) b[k] = pool[rnd() % 12];
        b[lb] = 0;
        if (t % 4 == 0) std::memcpy(b, a, sizeof a);
        CHECK(o_scmp(a, b) == u3x_strcasecmp(a, b));
        char d1[16], d2[16];
        std::memset(d1, 9, 16); std::memset(d2, 9, 16);
        const uint32_t n = rnd() % 15;
        o_sncpy(d1, a, n); u3x_strncpy_pad(d2, a, n);
        CHECK(std::memcmp(d1, d2, 16) == 0);
    }
    for (int t = 0; t < 20000; t++) {
        static uint8_t e[0x240];
        for (auto &x : e) x = (uint8_t)rnd();
        int32_t st = (int32_t)(rnd() % 4); std::memcpy(e + 0x1b0, &st, 4);
        const char *title = "Uncharted 3";
        if (t % 2) std::strcpy((char *)e + 0x28, t % 4 == 1 ? title : "UNCHARTED 3");
        else e[0x28 + rnd() % 8] = 0;
        CHECK(o_in_title(e, title) == u3x_friend_in_game(e, title));
        uint8_t x[0x24], y[0x24];
        for (int k = 0; k < 0x24; k++) { x[k] = (uint8_t)(rnd() % 3); y[k] = rnd() % 4 ? x[k] : (uint8_t)(rnd() % 3); }
        CHECK(o_same(x, y) == u3x_same_player(x, y));
    }
    for (int v = 0; v < 2; v++) {
        static uint8_t p1[0x7b00], p2[0x7b00];
        for (uint32_t k = 0; k < sizeof p1; k++) p1[k] = p2[k] = (uint8_t)rnd();
        uint32_t val = 0xabcdef12u; uint8_t *d = v ? (uint8_t *)&val : nullptr;
        o_i05_defaults(p1, d);
        std::memset(p2 + 0x24c, 0, 0x2824);
        uint32_t *dp = (uint32_t *)d;
        u3x_record_defaults(p2 + 0x248, 0x56, d ? (const uint32_t *const *)&dp : nullptr);
        CHECK(std::memcmp(p1, p2, sizeof p1) == 0);
    }
    for (uint32_t t = 0; t < 40; t++) CHECK(o_dtype(t) == u3x_has_difficulty_vote(t));
    { uint8_t a[0x18], b[0x18]; for (int k = 0; k < 0x18; k++) a[k] = b[k] = (uint8_t)rnd(); o_vopen(a); u3x_vote_open(b); CHECK(std::memcmp(a, b, sizeof a) == 0); }
    for (int t = 0; t < 1000000; t++) { const uint32_t now = rnd() << 8 ^ rnd(); CHECK(o_fake(now) == u3x_stand_in_vote(now)); }
}
