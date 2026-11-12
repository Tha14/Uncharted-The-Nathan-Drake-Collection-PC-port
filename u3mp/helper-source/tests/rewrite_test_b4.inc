// v7.116 batch: the old helper code (v7.115) for each rewritten section, and the old-versus-new checks.
static void old_image_answer(int32_t *ev, void *t) {
    for (int k = 3; k <= 12; k++) ev[k] = 0; ev[9] = -1; ev[11] = -1;
    if (t) { ev[2] = 2; *(void **)(ev + 4) = t; ev[12] = 0x3f800000; } else { ev[2] = 0x10; ev[12] = 0x3f800000; }
}
static void old_rank_name(char *name, uint32_t v, uint32_t limit) {
    uint32_t rank = (v & 0xffff) + 1, pres = (v >> 16) & 0x3fff; char *o = name, *end = name + 39;
    const char *pre = (pres && pres < limit) ? "ranks/prestige-" : "ranks/";
    while (*pre && o < end) *o++ = *pre++;
    char num[12]; int n = 0; uint32_t r = rank; do { num[n++] = (char)('0' + r % 10); r /= 10; } while (r && n < 11);
    while (n && o < end) *o++ = num[--n]; *o = 0;
}
static int old_vote_query(uint32_t q, uint8_t closed, int32_t mine, int32_t selected, int32_t winner, int32_t *out) {
    if (q == 0x7754e7afu) { *out = closed ? (selected != winner) : -1; return 1; }
    if (q == 0x17809498u) { *out = closed ? 0 : ((uint32_t)mine > 1u); return 1; }
    return 0;
}
static uint32_t old_wstat(uint32_t i, const uint32_t *wtab, uint8_t *l, int *miss) {
    uint32_t id = 0; *miss = 0;
    if (i < 20) id = wtab[i];
    else { if (l && i - 20 < *(uint32_t *)l && *(uint32_t **)(l + 8)) id = (*(uint32_t **)(l + 8))[i - 20]; else *miss = 1; }
    return id;
}
static float f_bits(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
static int old_round_over(int32_t left, int32_t el) {   // the clock getter returned the same value for a, b and c
    const float k = f_bits(0x3b5a740eu);
    if (0.0f < (float)left * k) {
        if (((float)(int32_t)((uint32_t)left - (uint32_t)el) * k) / ((float)left * k) < f_bits(0x3e800000u)) return 1; }
    return 0;
}
static int old_decided(int16_t wins, int32_t rounds, int32_t round) { return wins > 1 || rounds - 1 <= round; }

static void batch4() {
    // image answer: texture and spinner
    for (int t = 0; t < 2; t++) {
        int32_t a[16], b[16]; for (int i = 0; i < 16; i++) a[i] = b[i] = 0x22220000 + i;
        static int tex; void *tp = t ? &tex : nullptr;
        old_image_answer(a, tp); u3x_image_answer(b, tp); CHECK(std::memcmp(a, b, sizeof a) == 0);
    }
    // rank names: every rank value, prestige levels around both limits
    for (uint32_t rank = 0; rank < 0x10000; rank += (rank < 1200 ? 1 : 97)) for (uint32_t pres : {0u, 1u, 3u, 4u, 5u, 6u, 0x3fffu}) for (uint32_t limit : {4u, 6u}) {
        char a[40], b[40]; std::memset(a, 0x7e, sizeof a); std::memset(b, 0x7e, sizeof b);
        const uint32_t v = rank | pres << 16 | (rank & 1 ? 0x40000000u : 0);
        old_rank_name(a, v, limit); u3x_rank_icon_name(b, v, limit); CHECK(std::strcmp(a, b) == 0);
    }
    { char a[40], b[40]; old_rank_name(a, 0xffffu, 6); u3x_rank_icon_name(b, 0xffffu, 6); CHECK(std::strcmp(a, b) == 0 && std::strcmp(b, "ranks/65536") == 0); }
    // vote queries
    for (uint32_t q : {0x7754e7afu, 0x17809498u, 0u, 0x7754e7aeu}) for (int closed : {0, 1, 7}) for (int32_t mine : {-1, 0, 1, 2, 9}) for (int32_t sel : {-1, 0, 3}) for (int32_t win : {-1, 0, 3}) {
        int32_t a = 0x1234, b = 0x1234;
        CHECK(old_vote_query(q, (uint8_t)closed, mine, sel, win, &a) == u3x_vote_query(q, closed, mine, sel, win, &b)); CHECK(a == b);
    }
    // weapon stat ids
    uint32_t wtab[20]; for (uint32_t i = 0; i < 20; i++) wtab[i] = 0xa000 + i;
    uint32_t ids[6]; for (uint32_t i = 0; i < 6; i++) ids[i] = 0xb000 + i;
    for (int variant = 0; variant < 4; variant++) {
        alignas(8) uint8_t list[16] = {}; uint32_t n = variant == 3 ? 0 : 6; uint32_t *ip = variant == 2 ? nullptr : ids;
        std::memcpy(list, &n, 4); std::memcpy(list + 8, &ip, 8); uint8_t *lp = variant == 1 ? nullptr : list;
        for (uint32_t i = 0; i < 32; i++) { int m1, m2; CHECK(old_wstat(i, wtab, lp, &m1) == u3x_weapon_stat_id(i, wtab, lp, &m2)); CHECK(m1 == m2); }
        int m1, m2; CHECK(old_wstat(0xffffffffu, wtab, lp, &m1) == u3x_weapon_stat_id(0xffffffffu, wtab, lp, &m2)); CHECK(m1 == m2);
    }
    // late join: every clock up to 3 minutes against a sweep of elapsed times, then random values up to 2^21 ticks (116 minutes)
    for (int32_t left = -3; left < 54000; left += (left < 3000 ? 1 : 37)) for (int32_t el = -50; el < left + 50; el += (left < 400 ? 1 : 1 + left / 211))
        CHECK(old_round_over(left, el) == u3x_round_mostly_over(left, el));
    for (int t = 0; t < 2000000; t++) {
        int32_t left = (int32_t)(rnd() % (1u << 21)) + 1, el = (t & 1) ? left - (left + (int32_t)(rnd() % 7) - 3) / 4 : (int32_t)(rnd() % (1u << 21));
        CHECK(old_round_over(left, el) == u3x_round_mostly_over(left, el));
    }
    for (int32_t wins : {-1, 0, 1, 2, 5}) for (int32_t rounds : {0, 1, 3, 5}) for (int32_t round : {-1, 0, 1, 2, 3, 4, 5})
        CHECK(old_decided((int16_t)wins, rounds, round) == u3x_match_nearly_decided(wins, rounds, round));
}
