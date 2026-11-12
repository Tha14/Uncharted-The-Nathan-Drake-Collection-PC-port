// v7.131 part J: the old helper code (v7.130) for the kill-reward helpers, and the old-versus-new checks.
static uint32_t o_pid(uint8_t *np) { uint32_t lo = *(uint32_t *)(np + 0x1e4) & 0xfff; if (np[0x1ec] && lo) lo += 0x800; return lo + *(uint32_t *)(np + 0x1e8) * 0x1000u; }
static int o_active(uint8_t *np) { return np[0xe1] && !np[0x5e4]; }
static int o_tag(uint8_t *end, uint8_t *tags, uint32_t sid) {
    uint8_t *n = *(uint8_t **)end;
    for (int g = 0; n && n != end && g < 4096; g++, n = *(uint8_t **)n) if (*(uint32_t *)(n + 0x10) == sid) { uint32_t b = *(uint32_t *)(n + 0x14); return (int)((*(uint64_t *)(tags + 0x20 + (size_t)(b >> 6) * 8) >> (b & 63)) & 1); }
    return 0;
}
static int o_member(uint8_t *S, uint32_t id) { uint32_t n = *(uint32_t *)S; for (uint32_t i = 0; i < n && i < 128; i++) if (*(uint32_t *)(S + 0x50e4 + (uint64_t)i * 0xa4) == id) return (int)i; return -1; }
static uint32_t o_statidx(const uint32_t *t, uint32_t *l, uint32_t sid) {
    for (uint32_t i = 0; i < 0x13; i++) if (t[i] == sid) return i;
    if (!l) return 0; uint32_t n = l[0]; const uint32_t *a = *(const uint32_t **)(l + 2);
    for (uint32_t j = 0; j < n && j < 0x40; j++) if (a[j] == sid) return j + 0x13;
    return 0;
}
static uint32_t o_statw(const uint32_t *t, uint32_t *l, uint32_t idx) { if (idx < 0x13) return t[idx]; if (!l || idx >= l[0] + 0x13) return 0; return (*(const uint32_t **)(l + 2))[idx - 0x13]; }
static int o_g6(uint32_t w) { return w == 0xb934b2b2u || w == 0xc58ae515u || w == 0x472bca9au || w == 0x16980e31u || w == 0x22bf34f2u || w == 0xb17d9450u; }
static int o_g4(uint32_t w) { return w == 0x0b8bd6d6u || w == 0x3d2f108bu || w == 0x7fe77949u || w == 0x9c766ee3u; }

static void batch18() {
    for (int t = 0; t < 100000; t++) {
        uint8_t np[0x600]; for (auto &x : np) x = (uint8_t)rnd(); if (t % 3 == 0) { np[0x1e4] = 0; np[0x1e5] &= 0xf0; }
        CHECK(o_pid(np) == u3x_net_player_id(np)); CHECK(o_active(np) == u3x_net_player_in_game(np));
    }
    // tag lists: circular lists with random nodes, missing ids, long chains
    for (int t = 0; t < 2000; t++) {
        static uint8_t nodes[40][0x18]; alignas(8) static uint8_t head[8]; alignas(8) static uint8_t tags[0x40];
        for (auto &x : tags) x = (uint8_t)rnd();
        const int len = (int)(rnd() % 40);
        uint8_t *prev = head;
        for (int k = 0; k < len; k++) { uint32_t id = rnd() % 30, bit = rnd() % 256; std::memcpy(nodes[k] + 0x10, &id, 4); std::memcpy(nodes[k] + 0x14, &bit, 4); uint8_t *p = nodes[k]; std::memcpy(prev, &p, 8); prev = nodes[k]; }
        uint8_t *close = t % 7 ? head : nullptr; std::memcpy(prev, &close, 8);
        for (uint32_t id = 0; id < 32; id++) CHECK(o_tag(head, tags, id) == u3x_has_tag(head, tags, id));
    }
    for (int t = 0; t < 2000; t++) {
        static uint8_t S[0x50e4 + 130 * 0xa4]; uint32_t n = rnd() % 140; std::memcpy(S, &n, 4);
        for (int k = 0; k < 130; k++) { uint32_t id = rnd() % 200; std::memcpy(S + 0x50e4 + k * 0xa4, &id, 4); }
        for (uint32_t id = 0; id < 202; id += 3) CHECK(o_member(S, id) == u3x_score_member(S, id));
    }
    uint32_t fixed[0x13]; for (uint32_t k = 0; k < 0x13; k++) fixed[k] = 0x100 + k * 3;
    uint32_t ids[70]; for (uint32_t k = 0; k < 70; k++) ids[k] = 0x200 + k;
    for (int v = 0; v < 4; v++) {
        alignas(8) uint32_t l[4] = {}; l[0] = v == 3 ? 70 : 20 + v; uint32_t *ip = ids; std::memcpy(l + 2, &ip, 8); uint32_t *lp = v == 2 ? nullptr : l;
        for (uint32_t w = 0xf0; w < 0x260; w++) CHECK(o_statidx(fixed, lp, w) == u3x_weapon_stat_number(fixed, lp, w));
        for (uint32_t i = 0; i < 100; i++) CHECK(o_statw(fixed, lp, i) == u3x_weapon_for_stat(fixed, i < 0x13 ? nullptr : lp, i));
    }
    for (uint32_t w : {0xb934b2b2u, 0xc58ae515u, 0x472bca9au, 0x16980e31u, 0x22bf34f2u, 0xb17d9450u, 0x0b8bd6d6u, 0x3d2f108bu, 0x7fe77949u, 0x9c766ee3u, 0u, 1u})
        { CHECK(o_g6(w) == (u3x_weapon_group(w) == 6)); CHECK(o_g4(w) == (u3x_weapon_group(w) == 4)); }
    for (int t = 0; t < 100000; t++) { const uint32_t w = rnd() << 8 ^ rnd(); CHECK(o_g6(w) == (u3x_weapon_group(w) == 6)); CHECK(o_g4(w) == (u3x_weapon_group(w) == 4)); }
}
