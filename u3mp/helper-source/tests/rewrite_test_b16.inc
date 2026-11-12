// v7.129 part H: the old helper code (v7.128) for the film list and its dates, and the old-versus-new checks.
static void o_ecopy(uint8_t *d, const uint8_t *s) {
    std::memcpy(d, s, 0x100); std::memcpy(d + 0x100, s + 0x100, 0x100); d[0x200] = s[0x200];
    std::memcpy(d + 0x208, s + 0x208, 8); std::memcpy(d + 0x210, s + 0x210, 4); std::memcpy(d + 0x214, s + 0x214, 4);
    std::memcpy(d + 0x218, s + 0x218, 0x1c78);
}
static void o_film_sort(uint8_t *L, int n) {
    static uint8_t tmpe[0x1e90];
    for (int sw = 1; sw;) { sw = 0; for (int i = 0; i < n - 1; i++) { uint8_t *a = L + (size_t)i * 0x1e90, *b = a + 0x1e90;
        int64_t ta, tb; std::memcpy(&ta, a + 0x208, 8); std::memcpy(&tb, b + 0x208, 8);
        if (ta < tb) { o_ecopy(tmpe, a); o_ecopy(a, b); o_ecopy(b, tmpe); sw = 1; } } }
}
static char *o_ccat(char *o, char *end, const char *t) { while (*t && o < end) *o++ = *t++; *o = 0; return o; }
static char *o_cnum(char *o, char *end, int64_t v, int width) {
    char t[24]; int n = 0; uint64_t u = v < 0 ? (uint64_t)-v : (uint64_t)v; do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 22);
    if (v < 0 && o < end) *o++ = '-'; while (n < width && n < 22) t[n++] = '0'; while (n && o < end) *o++ = t[--n]; *o = 0; return o;
}
static void o_rfc(char *o, const int32_t *lt, const int32_t *g) {   // lt / g: sec, min, hour, mday, mon, year, wday, yday, isdst
    static const char wd[] = "SunMonTueWedThuFriSat", mo[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    *o = 0;
    int64_t dd = (int64_t)(lt[5] - g[5]) * 366 + (lt[7] - g[7]); if (dd > 1) dd = -1; if (dd < -1) dd = 1;
    int64_t off = dd * 1440 + (lt[2] - g[2]) * 60 + (lt[1] - g[1]);
    char *e = o + 63, *p = o; char w[4] = {wd[lt[6] % 7 * 3], wd[lt[6] % 7 * 3 + 1], wd[lt[6] % 7 * 3 + 2], 0}; char m[4] = {mo[lt[4] % 12 * 3], mo[lt[4] % 12 * 3 + 1], mo[lt[4] % 12 * 3 + 2], 0};
    p = o_ccat(p, e, w); p = o_ccat(p, e, ", "); p = o_cnum(p, e, lt[3], 2); p = o_ccat(p, e, " "); p = o_ccat(p, e, m); p = o_ccat(p, e, " ");
    p = o_cnum(p, e, lt[5] + 1900, 4); p = o_ccat(p, e, " "); p = o_cnum(p, e, lt[2], 2); p = o_ccat(p, e, ":"); p = o_cnum(p, e, lt[1], 2);
    p = o_ccat(p, e, ":"); p = o_cnum(p, e, lt[0], 2); p = o_ccat(p, e, off < 0 ? " -" : " +"); if (off < 0) off = -off; p = o_cnum(p, e, off / 60, 2); o_cnum(p, e, off % 60, 2);
}

static void batch16() {
    // film lists: up to 20 entries, many equal times, padding bytes set differently per slot
    static uint8_t l1[20 * 0x1e90], l2[20 * 0x1e90], spare[0x1e90];
    for (int t = 0; t < 300; t++) {
        for (uint32_t k = 0; k < sizeof l1; k += 4) { uint32_t v = rnd() << 8 ^ rnd(); std::memcpy(l1 + k, &v, 4); }
        const int n = (int)(rnd() % 21);
        for (int i = 0; i < 20; i++) { int64_t tm = (int64_t)(rnd() % (t % 3 ? 5 : 1000)) - 2; std::memcpy(l1 + (size_t)i * 0x1e90 + 0x208, &tm, 8); }
        std::memcpy(l2, l1, sizeof l1);
        o_film_sort(l1, n); u3x_sort_films(l2, n, spare);
        CHECK(std::memcmp(l1, l2, sizeof l1) == 0);
    }
    { static uint8_t a[0x1e90], b[0x1e90], c[0x1e90]; for (auto &x : a) x = (uint8_t)rnd(); for (uint32_t k = 0; k < sizeof b; k++) b[k] = c[k] = (uint8_t)rnd();
      o_ecopy(b, a); u3x_film_entry_copy(c, a); CHECK(std::memcmp(b, c, sizeof b) == 0); }
    // dates: random local and UTC times, zones up to +-14 h, year changes
    for (int t = 0; t < 200000; t++) {
        int32_t lt[9], g[9];
        g[0] = (int32_t)(rnd() % 60); g[1] = (int32_t)(rnd() % 60); g[2] = (int32_t)(rnd() % 24); g[3] = 1 + (int32_t)(rnd() % 31); g[4] = (int32_t)(rnd() % 12);
        g[5] = 70 + (int32_t)(rnd() % 100); g[6] = (int32_t)(rnd() % 7); g[7] = (int32_t)(rnd() % 366); g[8] = 0;
        std::memcpy(lt, g, sizeof g);
        lt[2] = (int32_t)(rnd() % 24); lt[1] = (int32_t)(rnd() % 60);
        const int s = (int)(rnd() % 5); if (s == 1) lt[7] = g[7] + 1; if (s == 2) lt[7] = g[7] - 1; if (s == 3) { lt[5] = g[5] + 1; lt[7] = 0; } if (s == 4) { lt[5] = g[5] - 1; lt[7] = 364; }
        char a[80], b[80]; std::memset(a, 0x41, sizeof a); std::memset(b, 0x41, sizeof b);
        o_rfc(a, lt, g); u3x_rfc2822(b, lt, g); CHECK(std::memcmp(a, b, sizeof a) == 0);
    }
}
