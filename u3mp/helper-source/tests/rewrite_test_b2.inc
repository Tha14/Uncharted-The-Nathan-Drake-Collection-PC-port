// v7.114 batch: the old helper code (v7.113) for each rewritten section, and the old-versus-new checks.
static uint32_t rng_state = 12345;
static uint32_t rnd() { rng_state = rng_state * 1103515245u + 12345u; return rng_state >> 8; }

static void old_record_defaults(uint8_t *r, uint32_t **v) {
    std::memset(r, 0, 0x2828); *(uint32_t *)r = 0x56u; if (v && *v) *(uint32_t *)(r + 0x504) = **v;
    *(uint32_t *)(r + 0x18) = 0x6afcc877u; *(uint32_t *)(r + 0x1c) = 0xbe693215u; *(uint32_t *)(r + 0x24) = 0x9e96d6d7u; *(uint32_t *)(r + 0xd8) = 0xe0b92307u;
    *(uint32_t *)(r + 0xdc) = 0x85e71b0eu; *(uint32_t *)(r + 0xe4) = 0x6bbaf02au; *(uint32_t *)(r + 0x53c) = 0xc56be227u; *(uint32_t *)(r + 0x1628) = 0x3f000000u;
}
static void old_dialog(uint8_t *d, uint32_t body, uint32_t title, uint32_t action, int32_t buttons, uint32_t a6, uint32_t a7) {
    if (!a7) a7 = buttons == 0 ? 0x576ed245u : 0xd75be949u;
    *(uint32_t *)(d + 0x08) = body; *(uint32_t *)(d + 0x14) = title; *(uint32_t *)(d + 0x18) = 0x11d8e78au;
    *(uint32_t *)(d + 0x20) = 0x1519fa3du; *(uint32_t *)(d + 0x28) = action; *(uint32_t *)(d + 0x24) = a7;
    *(int32_t *)(d + 0x1c) = buttons; *(uint32_t *)(d + 0x10) = buttons != 0; *(uint32_t *)(d + 0x2c) = a6;
}
static uint8_t *games_table = nullptr;
static void *games_handle = nullptr;
static void **fake_lookup(uint32_t id) { return id == 0x3b9a067du ? (games_table ? &games_handle : nullptr) : nullptr; }
static uint32_t old_x61_start = 0;
static void old_round_clock(uint8_t *m, uint32_t *clock) {
    uint32_t st = *(uint32_t *)(m + 0x66ec);
    if (st == old_x61_start) return;
    old_x61_start = st;
    void **h = fake_lookup(0x3b9a067du); uint8_t *t = h ? (uint8_t *)*h : 0; int32_t i = *(int16_t *)(m + 0x661c);
    if (!t || i < 0 || i >= *(int32_t *)t) return;
    if (*(int32_t *)(*(uint8_t **)(t + 8) + (size_t)i * 0xd0 + 0xc) != 10) return;
    *clock = st;
}
static int old_marked(uint8_t *spawn) {
    uint8_t *props = *(uint8_t **)(spawn + 0x40); int on = 0;
    if (props) { uint8_t *tab = *(uint8_t **)(props + 0x68);
        if (tab) { uint8_t *e = *(uint8_t **)tab; int32_t lo = 0, hi = (int32_t)*(uint32_t *)(tab + 8) - 1;
            while (e && lo <= hi) { int32_t mid = (lo + hi) / 2; uint32_t sid = *(uint32_t *)(e + (size_t)mid * 16);
                if (sid == 0x22b25554u) { uint8_t *v = *(uint8_t **)(e + (size_t)mid * 16 + 8);
                    if (v && *(uint16_t *)v == 1 && *(uint32_t **)(v + 8)) on = **(uint32_t **)(v + 8) != 0; break; }
                if (sid < 0x22b25554u) lo = mid + 1; else hi = mid - 1; } } }
    return on;
}
static uint32_t old_report_rows(uint8_t *t) {
    if (!t) return 0; uint32_t n = *(uint32_t *)t; uint8_t *e = *(uint8_t **)(t + 8); if (!e || !n) return 0; if (n > 8) n = 8; return n;
}
static const char *old_report_reason(uint8_t *t, uint32_t k) {
    if (!t || k >= *(uint32_t *)t) return 0; uint8_t *e = *(uint8_t **)(t + 8); if (!e) return 0;
    return *(const char **)(e + (size_t)k * 0x10 + 8);
}
static uint32_t old_lz(const uint8_t *s, uint32_t n, uint8_t *d, uint32_t cap) {
    const uint8_t *e = s + n; uint8_t *o = d, *oe = d + cap;
    while (s < e) { uint32_t c = *s++;
        if (c <= 0x1f) { uint32_t l = c + 1; if (o + l > oe || s + l > e) return 0; while (l--) *o++ = *s++; }
        else { uint32_t l = c >> 5; if (l == 7) { if (s >= e) return 0; l = (uint32_t)*s + 7; s++; }
            if (s >= e) return 0; uint32_t b = *s++; l += 2; if (o + l > oe) return 0;
            uint32_t off = ((c & 0x1fu) << 8 | b) + 1; if ((uint32_t)(o - d) < off) return 0;
            const uint8_t *p = o - off; while (l--) *o++ = *p++; } }
    return (uint32_t)(o - d);
}
// a valid stream: literal runs and back references
static std::vector<uint8_t> make_stream(uint32_t pieces) {
    std::vector<uint8_t> s; uint32_t out = 0;
    for (uint32_t p = 0; p < pieces; p++) {
        if (out == 0 || rnd() % 2) { uint32_t run = 1 + rnd() % 32; s.push_back((uint8_t)(run - 1)); for (uint32_t k = 0; k < run; k++) s.push_back((uint8_t)rnd()); out += run; }
        else { uint32_t len = 3 + rnd() % 200, dist = 1 + rnd() % (out < 8192 ? out : 8192);
            uint32_t l = len - 2; uint32_t top = l >= 7 ? 7 : l;
            s.push_back((uint8_t)((top << 5) | ((dist - 1) >> 8))); if (top == 7) s.push_back((uint8_t)(l - 7)); s.push_back((uint8_t)((dist - 1) & 0xff)); out += len; }
    }
    return s;
}

static void batch2() {
    // record defaults: with and without the current value
    for (int variant = 0; variant < 3; variant++) {
        std::vector<uint8_t> a(0x2828, 0xcd), b(0x2828, 0xcd);
        uint32_t cur = 0x12345678u; uint32_t *cp = &cur; uint32_t **v = variant == 0 ? &cp : nullptr; uint32_t *np = nullptr;
        if (variant == 2) v = &np;
        old_record_defaults(a.data(), v);
        std::memset(b.data(), 0, b.size()); u3x_record_defaults(b.data(), 0x56u, (const uint32_t *const *)v);
        CHECK(a == b);
    }
    // dialog: buttons 0/1/2, close given or not
    for (int32_t buttons : {0, 1, 2, -1}) for (uint32_t close : {0u, 0xabcdu}) {
        uint8_t a[0x40], b[0x40]; std::memset(a, 0x77, sizeof a); std::memset(b, 0x77, sizeof b);
        old_dialog(a, 1, 2, 3, buttons, 4, close); u3x_fill_dialog(b, 1, 2, 3, buttons, 4, close);
        CHECK(std::memcmp(a, b, sizeof a) == 0);
    }
    // round clock: a sequence of frames with changing stamps, picks and game types
    alignas(8) uint8_t games[0xd0 * 4] = {}; alignas(8) uint8_t table[16] = {}; int32_t cnt = 4; uint8_t *gp = games;
    std::memcpy(table, &cnt, 4); std::memcpy(table + 8, &gp, 8);
    for (int i = 0; i < 4; i++) { int32_t type = i == 2 ? 10 : i; std::memcpy(games + i * 0xd0 + 0xc, &type, 4); }
    std::vector<uint8_t> mgr(0x7000, 0);
    uint32_t clock_old = 0, clock_new = 0;
    for (int frame = 0; frame < 400; frame++) {
        uint32_t st = rnd() % 4 ? *(uint32_t *)(mgr.data() + 0x66ec) : rnd() % 50;
        int16_t pick = (int16_t)((int)(rnd() % 7) - 1);
        std::memcpy(mgr.data() + 0x66ec, &st, 4); std::memcpy(mgr.data() + 0x661c, &pick, 2);
        games_table = (frame % 37 == 5) ? nullptr : table; games_handle = games_table;
        old_round_clock(mgr.data(), &clock_old); u3x_round_clock(mgr.data(), fake_lookup, &clock_new);
        CHECK(clock_old == clock_new);
    }
    // marked spawners: sorted tables of several sizes, with and without the property, several value kinds
    for (int t = 0; t < 300; t++) {
        uint32_t n = rnd() % 12; std::vector<uint32_t> ids;
        for (uint32_t k = 0; k < n; k++) ids.push_back(0x22b25554u + (rnd() % 5) * 0x1000u - 0x2000u + k);
        if (t % 3 == 0 && n) ids[rnd() % n] = 0x22b25554u;
        std::sort(ids.begin(), ids.end()); ids.erase(std::unique(ids.begin(), ids.end()), ids.end()); n = (uint32_t)ids.size();
        alignas(16) uint8_t ent[16 * 16] = {}; alignas(8) uint8_t val[16] = {}; uint32_t data = rnd() % 3;
        uint16_t type = (uint16_t)(rnd() % 3 ? 1 : 5); std::memcpy(val, &type, 2);
        uint32_t *dp = (rnd() % 5) ? &data : nullptr; std::memcpy(val + 8, &dp, 8);
        for (uint32_t k = 0; k < n; k++) { std::memcpy(ent + k * 16, &ids[k], 4); uint8_t *vp = (rnd() % 6) ? val : nullptr; std::memcpy(ent + k * 16 + 8, &vp, 8); }
        alignas(8) uint8_t tab[16] = {}; uint8_t *ep = (t % 23 == 0) ? nullptr : ent; std::memcpy(tab, &ep, 8); std::memcpy(tab + 8, &n, 4);
        alignas(8) uint8_t props[0x70] = {}; uint8_t *tp = (t % 29 == 0) ? nullptr : tab; std::memcpy(props + 0x68, &tp, 8);
        alignas(8) uint8_t spawn[0x48] = {}; uint8_t *pp = (t % 31 == 0) ? nullptr : props; std::memcpy(spawn + 0x40, &pp, 8);
        CHECK(old_marked(spawn) == u3x_spawn_marked(spawn));
    }
    // report reasons: counts 0..12, rows inside and outside, missing entries
    for (uint32_t n = 0; n < 13; n++) for (int miss = 0; miss < 2; miss++) {
        alignas(16) uint8_t ent[16 * 12] = {}; static const char txt[] = "reason";
        for (uint32_t k = 0; k < 12; k++) { const char *p = (k % 4 == 3) ? nullptr : txt + (k % 6); std::memcpy(ent + k * 16 + 8, &p, 8); }
        alignas(8) uint8_t tab[16] = {}; uint8_t *ep = miss ? nullptr : ent; std::memcpy(tab, &n, 4); std::memcpy(tab + 8, &ep, 8);
        CHECK(old_report_rows(tab) == u3x_report_rows(tab));
        for (uint32_t k = 0; k < 14; k++) CHECK(old_report_reason(tab, k) == u3x_report_reason(tab, k));
    }
    CHECK(u3x_report_rows(nullptr) == 0 && u3x_report_reason(nullptr, 0) == nullptr);
    // LZ: valid streams (whole and cut), small caps, random garbage
    for (int t = 0; t < 3000; t++) {
        std::vector<uint8_t> s;
        if (t % 3 == 0) { s.resize(rnd() % 64); for (auto &c : s) c = (uint8_t)rnd(); }
        else { s = make_stream(1 + rnd() % 40); if (t % 3 == 2 && !s.empty()) s.resize(rnd() % s.size()); }
        uint32_t cap = (t % 5 == 0) ? rnd() % 300 : 0x2828;
        std::vector<uint8_t> a(cap + 1, 0x5a), b(cap + 1, 0x5a);
        uint32_t ra = old_lz(s.data(), (uint32_t)s.size(), a.data(), cap), rb = u3x_lz_decode(s.data(), (uint32_t)s.size(), b.data(), cap);
        CHECK(ra == rb); if (ra) CHECK(std::memcmp(a.data(), b.data(), ra) == 0);
    }
}
