// v7.120 batch: the old helper code (v7.119) for each rewritten section, and the old-versus-new checks.
static int o_strcasecmp(const char *a, const char *b) {
    for (;;) { int x = (uint8_t)*a++, y = (uint8_t)*b++; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return x < y ? -1 : 1; if (!x) return 0; }
}
static int o_rank(int32_t st) { return st == 1 ? 2 : st == 2 ? 1 : 0; }
static int o_cmp(uint8_t *a, uint8_t *b) {
    int x = o_rank(*(int32_t *)(a + 0x1b0)), y = o_rank(*(int32_t *)(b + 0x1b0));
    if (x < y) return 1; if (x > y) return -1; return o_strcasecmp((const char *)a + 1, (const char *)b + 1);
}
static void o_swap(uint8_t *a, uint8_t *b) { for (uint32_t k = 0; k < 0x240; k++) { uint8_t t = a[k]; a[k] = b[k]; b[k] = t; } }
static void old_fr_sort(uint8_t *v, int64_t n) {
    static uint8_t piv[0x240];
#define OFRV(i) (v + (size_t)(i) * 0x240)
    int64_t st[132]; uint32_t sp = 0; int64_t lo = 0, hi = n - 1;
    for (;;) {
        while (hi - lo > 6) {
            o_swap(OFRV((lo + hi) >> 1), OFRV(lo + 1));
            if (o_cmp(OFRV(lo), OFRV(hi)) > 0) o_swap(OFRV(lo), OFRV(hi));
            if (o_cmp(OFRV(lo + 1), OFRV(hi)) > 0) o_swap(OFRV(lo + 1), OFRV(hi));
            if (o_cmp(OFRV(lo), OFRV(lo + 1)) > 0) o_swap(OFRV(lo), OFRV(lo + 1));
            std::memcpy(piv, OFRV(lo + 1), 0x240);
            int64_t i = lo + 1, j = hi, r = hi - lo;
            for (;;) { i++; r--;
                if (j < i || o_cmp(OFRV(i), piv) >= 0) {
                    for (;;) { j--; if (j < i) goto cross; if (o_cmp(OFRV(j), piv) <= 0) break; }
                    o_swap(OFRV(i), OFRV(j)); } }
        cross:
            std::memcpy(OFRV(lo + 1), OFRV(j), 0x240); std::memcpy(OFRV(j), piv, 0x240);
            sp += 2; if (sp >= 130) return;
            if (r < j - lo) { st[sp] = j - 1; st[sp - 1] = lo; lo = i; } else { st[sp] = hi; st[sp - 1] = i; hi = j - 1; } }
        for (int64_t k = lo + 1; k <= hi; k++) {
            std::memcpy(piv, OFRV(k), 0x240); int64_t m = k - 1;
            while (m >= 0 && o_cmp(OFRV(m), piv) > 0) { std::memcpy(OFRV(m + 1), OFRV(m), 0x240); m--; }
            std::memcpy(OFRV(m + 1), piv, 0x240); }
        if (!sp) return;
        lo = st[sp - 1]; hi = st[sp]; sp -= 2; }
#undef OFRV
}
static int old_death_find(uint8_t *e, uint32_t n, uint32_t sid) {
    if (!e) return -1;
    uint32_t lo = 0, hi = n - 1;
    while (n && lo <= hi && hi < n) { uint32_t m = (lo + hi) >> 1, v = *(uint32_t *)(e + (uint64_t)m * 16);
        if (v > sid) { if (!m) break; hi = m - 1; } else if (v < sid) lo = m + 1; else return (int)m; }
    return -1;
}
static uint32_t old_bs_read(uint32_t *w, uint32_t pos, uint32_t n) {
    uint32_t sh = pos & 31; uint64_t v = (uint64_t)(uint32_t)(w[pos >> 5] << sh);
    uint32_t r = (uint32_t)(v >> (32 - n)); if (32 - sh < n) r |= w[(pos >> 5) + 1] >> (32 - (n - (32 - sh)));
    return r;
}
static void old_bs_write(uint32_t *w, uint32_t pos, uint32_t val, uint32_t n) {
    uint32_t v = (uint32_t)((uint64_t)val << (32 - n)); uint32_t sh = pos & 31, room = 32 - sh;
    uint32_t *p = w + (pos >> 5); *p = (uint32_t)(*p & (uint32_t)((uint64_t)0xffffffffu << room)) | (uint32_t)(v >> sh);
    if (room < n) p[1] = (uint32_t)((uint64_t)v << room);
}
static uint8_t *old_find_member(uint8_t *ss, uint32_t id) {
    for (int i = 0; i < 10; i++) { uint8_t *m = ss + 0x680 + i * 0x120; if (m[0x25] && *(uint32_t *)(m + 0x2c) == id) return m; } return 0;
}
static int old_party_count(uint8_t *ss) {
    if (!*(uint64_t *)(ss + 0x18)) return 0;
    int n = 0; for (int i = 0; i < 10; i++) { uint8_t *m = ss + 0x680 + i * 0x120; if (m[0x25]) n += 1 + ((int8_t)m[0x40 + 0x19] != 0); } return n;
}

static void batch8() {
    // friend sort: random lists of up to 100 with unique names (mixed case), then names equal ignoring case
    static uint8_t a[100 * 0x240], b[100 * 0x240], spare[0x240];
    int ties_differ = 0;
    for (int t = 0; t < 3000; t++) {
        const uint32_t n = rnd() % 101; const bool unique = t % 5 != 4;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t *e = a + i * 0x240; for (int k = 0; k < 0x240; k++) e[k] = (uint8_t)rnd();
            int32_t st = (int32_t)(rnd() % 4); std::memcpy(e + 0x1b0, &st, 4);
            const int len = 1 + (int)(rnd() % 12);
            for (int k = 0; k < len; k++) { const char c = (char)('a' + rnd() % (unique ? 26 : 2)); e[1 + k] = (uint8_t)(rnd() % 2 ? c - 32 : c); }
            if (unique) { e[1 + len] = (uint8_t)('0' + i / 10); e[2 + len] = (uint8_t)('0' + i % 10); e[3 + len] = 0; } else e[1 + len] = 0;
        }
        std::memcpy(b, a, sizeof a);
        old_fr_sort(a, n); u3x_friend_sort(b, n, spare);
        CHECK(std::memcmp(a, b, (size_t)n * 0x240) == 0);   // v7.121: equal names too, byte for byte
        if (!unique && std::memcmp(a, b, (size_t)n * 0x240)) ties_differ++;
        for (uint32_t i = 1; i < n; i++) CHECK(o_cmp(b + (i - 1) * 0x240, b + i * 0x240) <= 0);
    }
    std::printf("friend sort: %d lists with equal names came out in another order\n", ties_differ);
    // sorted search: random sorted unique tables, every id around them, empty and missing tables
    for (int t = 0; t < 3000; t++) {
        alignas(8) uint8_t tab[16 * 40]; const uint32_t n = rnd() % 41; uint32_t id = rnd() % 5;
        for (uint32_t i = 0; i < n; i++) { id += 1 + rnd() % 4; std::memcpy(tab + i * 16, &id, 4); }
        for (uint32_t q = 0; q < id + 4; q++) CHECK(old_death_find(tab, n, q) == u3x_sorted_index(tab, n, q));
        CHECK(old_death_find(tab, n, 0xffffffffu) == u3x_sorted_index(tab, n, 0xffffffffu));
    }
    CHECK(u3x_sorted_index(nullptr, 3, 1) == -1);
    // bit streams: random reads at every position and width, then random write sequences
    for (int t = 0; t < 300000; t++) {
        uint32_t w[4]; for (auto &x : w) x = rnd() << 8 ^ rnd();
        const uint32_t n = 1 + rnd() % 32, pos = rnd() % 64;
        CHECK(old_bs_read(w, pos, n) == u3x_bits_get(w, pos, n));
        uint32_t w1[4], w2[4]; std::memcpy(w1, w, sizeof w); std::memcpy(w2, w, sizeof w); const uint32_t val = rnd() << 8 ^ rnd();
        old_bs_write(w1, pos, val, n); u3x_bits_put(w2, pos, val, n); CHECK(std::memcmp(w1, w2, sizeof w1) == 0);
    }
    for (int t = 0; t < 2000; t++) {   // a whole stream written front to back, then read back
        uint32_t w1[40] = {}, w2[40] = {}; uint32_t pos = 0, widths[60], vals[60]; int k = 0;
        while (k < 60 && pos < 32 * 38) { widths[k] = 1 + rnd() % 32; vals[k] = rnd() << 8 ^ rnd(); old_bs_write(w1, pos, vals[k], widths[k]); u3x_bits_put(w2, pos, vals[k], widths[k]); pos += widths[k]; k++; }
        CHECK(std::memcmp(w1, w2, sizeof w1) == 0);
        pos = 0; for (int i = 0; i < k; i++) { const uint32_t want = widths[i] == 32 ? vals[i] : vals[i] & ((1u << widths[i]) - 1); CHECK(u3x_bits_get(w2, pos, widths[i]) == want); pos += widths[i]; }
    }
    // members and party size
    for (int t = 0; t < 5000; t++) {
        static uint8_t ss[0x1300]; for (auto &x : ss) x = 0;
        uint64_t party = rnd() % 4 ? rnd() + 1 : 0; std::memcpy(ss + 0x18, &party, 8);
        for (int i = 0; i < 10; i++) { uint8_t *m = ss + 0x680 + i * 0x120; m[0x25] = (uint8_t)(rnd() % 2); uint32_t id = rnd() % 12; std::memcpy(m + 0x2c, &id, 4); m[0x59] = (uint8_t)(rnd() % 3 == 0 ? 0x80 + rnd() % 2 : 0); }
        for (uint32_t id = 0; id < 13; id++) CHECK(old_find_member(ss, id) == u3x_find_member(ss, id));
        CHECK(old_party_count(ss) == u3x_party_players(ss));
    }
}
