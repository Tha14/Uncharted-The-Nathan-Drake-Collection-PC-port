// v7.122 part A: the old helper code (v7.121) for each rewritten piece, and the old-versus-new checks.
static void old_pager(int32_t *event, int32_t *eu_cur, int32_t pages) {
    switch (event[0]) {
    case 5: event[1] = *eu_cur; break;
    case 6: event[1] = pages - 1; break;
    case 7: event[1] = 0; break;
    case 10: { int32_t v = event[1]; *eu_cur = v < 0 ? 0 : v >= pages ? pages - 1 : v; break; }
    case 23: if (event[14]) *eu_cur = 0; break;
    }
}
static void old_step(int32_t *eu_cur, int32_t pages, int r) { if (r == 3 && *eu_cur > 0) (*eu_cur)--; else if (r == 4 && *eu_cur < pages - 1) (*eu_cur)++; }
static int32_t old_xp(int32_t t0, int32_t t1, int32_t xp) { return (t0 <= xp) ? ((t1 != t0) ? ((xp - t0) * 100) / (t1 - t0) : 0) : 0; }
static uint32_t old_lvicon(uint32_t lv) { return lv == 1 ? 0x5936816cu : lv == 2 ? 0x512da466u : lv == 0 ? 0x5ec0626au : 0; }
static uint32_t old_lvmark(int lv) { return 0x2e4c0000u | ((uint32_t)lv & 0xffu); }
static int old_stats(uint32_t md, uint8_t priv) { return md != 0 && md != 8 && md != 3 && !priv; }
static int32_t old_winner(uint8_t *V) { return *(int32_t *)(V + 0x14) > *(int32_t *)(V + 0x10) ? *(int32_t *)(V + 0xc) : *(int32_t *)(V + 8); }
static uint32_t old_pool(uint8_t *pl) { uint32_t pool = 0; if (pl) { uint8_t *l = *(uint8_t **)(pl + 0x40); uint32_t *a = l ? *(uint32_t **)(l + 8) : 0; if (a && *(uint32_t *)l) pool = a[0]; } return pool; }
static uint8_t old_status(int custom, uint8_t wait, uint8_t playing) { return (uint8_t)((custom) | (wait ? 2 : 0) | (playing ? 4 : 0)); }
static uint8_t old_start(uint8_t at, uint64_t followed, uint64_t room) { return at ? (uint8_t)((uint8_t)(followed ? followed : room) | 1) : 0; }
static uint8_t *old_challenge(uint8_t *t, int32_t want) {
    uint8_t *e = *(uint8_t **)(t + 8), *hit = 0; if (!e) return 0;
    for (int32_t i = 0; i < *(int32_t *)t; i++) if (*(int32_t *)(e + (size_t)i * 0x28 + 8) == want) { hit = e + (size_t)i * 0x28; break; }
    return hit;
}

static void batch9() {
    // pager: every op with values around the ends, the step both ways
    for (int32_t op = 0; op < 25; op++) for (int32_t v = -3; v < 11; v++) for (int32_t cur = 0; cur < 7; cur++) for (int32_t flag : {0, 1}) {
        int32_t a[16] = {}, b[16] = {}; a[0] = b[0] = op; a[1] = b[1] = v; a[14] = b[14] = flag; int32_t c1 = cur, c2 = cur;
        old_pager(a, &c1, 7); const int handled = u3x_pager_answer(b, &c2, 7);
        if (op == 5 || op == 6 || op == 7 || op == 10 || op == 23) CHECK(handled == 1); else CHECK(handled == 0);
        CHECK(std::memcmp(a, b, sizeof a) == 0); CHECK(c1 == c2);
    }
    for (int r = 0; r < 6; r++) for (int32_t cur = 0; cur < 7; cur++) { int32_t c1 = cur, c2 = cur; old_step(&c1, 7, r); u3x_pager_step(&c2, 7, r); CHECK(c1 == c2); }
    // experience percent: random thresholds and xp, equal thresholds, xp below
    for (int t = 0; t < 500000; t++) {
        const int32_t t0 = (int32_t)(rnd() % 2000000), t1 = t % 7 == 0 ? t0 : t0 + 1 + (int32_t)(rnd() % 1000000), xp = (int32_t)(rnd() % 3000000);
        CHECK(old_xp(t0, t1, xp) == u3x_xp_percent(t0, t1, xp));
    }
    for (uint32_t lv = 0; lv < 300; lv++) CHECK(old_lvicon(lv) == u3x_booster_level_icon(lv));
    for (int lv = -5; lv < 300; lv++) CHECK(old_lvmark(lv < 0 ? 0 : lv) == u3x_booster_level_mark(lv));   // the caller clamps below 0
    for (uint32_t md = 0; md < 25; md++) for (uint8_t p : {0, 1, 7}) CHECK(old_stats(md, p) == u3x_shows_match_stats(md, p));
    for (int t = 0; t < 20000; t++) { alignas(8) uint8_t V[0x18]; for (auto &x : V) x = (uint8_t)rnd(); int32_t c0 = (int32_t)(rnd() % 5) - 1, c1 = (int32_t)(rnd() % 5) - 1; std::memcpy(V + 0x10, &c0, 4); std::memcpy(V + 0x14, &c1, 4); CHECK(old_winner(V) == u3x_vote_winner(V)); }
    { alignas(8) uint8_t pl[0x48] = {}, list[16] = {}; uint32_t ids[2] = {0x77, 0x88};
      for (int v = 0; v < 4; v++) { uint32_t n = v == 3 ? 0 : 2; uint32_t *ip = v == 2 ? nullptr : ids; uint8_t *lp = v == 1 ? nullptr : list;
          std::memcpy(list, &n, 4); std::memcpy(list + 8, &ip, 8); std::memcpy(pl + 0x40, &lp, 8); CHECK(old_pool(pl) == u3x_playlist_pool(pl)); }
      CHECK(old_pool(nullptr) == u3x_playlist_pool(nullptr)); }
    for (int c = 0; c < 2; c++) for (uint8_t w : {0, 1, 5}) for (uint8_t p : {0, 1, 9}) CHECK(old_status(c, w, p) == u3x_member_status(c, w, p));
    for (int t = 0; t < 20000; t++) { const uint8_t at = (uint8_t)(rnd() % 3); const uint64_t f = (t % 3) ? ((uint64_t)rnd() << 32 | rnd()) : 0, r = (uint64_t)rnd() << 32 | rnd(); CHECK(old_start(at, f, r) == u3x_member_start(at, f, r)); }
    // challenge table
    for (int t = 0; t < 2000; t++) {
        alignas(8) uint8_t ent[0x28 * 12]; for (auto &x : ent) x = (uint8_t)rnd();
        for (int k = 0; k < 12; k++) { int32_t num = (int32_t)(rnd() % 20); std::memcpy(ent + k * 0x28 + 8, &num, 4); }
        alignas(8) uint8_t tab[16] = {}; int32_t n = (int32_t)(rnd() % 14) - 2; uint8_t *ep = t % 11 == 0 ? nullptr : ent; std::memcpy(tab, &n, 4); std::memcpy(tab + 8, &ep, 8);
        for (int32_t want = -1; want < 21; want++) CHECK(old_challenge(tab, want) == u3x_challenge_entry(tab, want));
    }
}
