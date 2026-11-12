// v7.124 part C: the old helper code (v7.123) for each rewritten piece, and the old-versus-new checks.
static uint32_t old_retag_records(uint8_t *page) {
    uint32_t c = 0; uint8_t *col = *(uint8_t **)(page + 0x48); if (!col) return 0; uint32_t n = *(uint32_t *)col; uint8_t *items = *(uint8_t **)(col + 8);
    if (!items || n > 64) return 0;
    for (uint32_t j = 0; j < n; j++) { uint8_t *it = items + j * 0x68;
        if (*(uint32_t *)(it + 8) == 0x2027d9a3u && *(uint32_t *)(it + 0x30) == 21 && *(uint32_t *)(it + 0x34) == 0xc7 && *(uint32_t *)(it + 0x40) == 0) {
            *(uint32_t *)(it + 0x34) = 0xcf; *(uint32_t *)(it + 0x40) = 0x4c414ec7u; c++; } }
    return c;
}
static uint32_t old_k04(uint8_t *page) {
    uint32_t c4 = 0; uint8_t *c = *(uint8_t **)(page + 0x48); if (!c) return 0; uint32_t n = *(uint32_t *)c; uint8_t *it = *(uint8_t **)(c + 8); if (!it || n > 32) return 0;
    for (uint32_t j = 0; j < n; j++) { uint8_t *w = it + j * 0x68;
        if (*(uint32_t *)(w + 0x10) == 13u && *(uint32_t *)(w + 0x28) == 0x9b7aa1a9u) *(uint32_t *)(w + 0x10) = 6u;
        if (*(uint32_t *)(w + 8) == 0 && *(uint32_t *)(w + 0x30) == 21u && *(uint32_t *)(w + 0x34) == 0x2au && *(uint32_t *)(w + 0x40) == 0) {
            *(uint32_t *)(w + 0x34) = 0xcfu; *(uint32_t *)(w + 0x40) = 0x4c414e2au; c4++; } }
    return c4;
}
static uint32_t new_k04(uint8_t *page) {   // the C wrapper as it is now: the style loop, then the C++ retag
    uint8_t *c = *(uint8_t **)(page + 0x48); if (!c) return 0; uint32_t n = *(uint32_t *)c; uint8_t *it = *(uint8_t **)(c + 8); if (!it || n > 32) return 0;
    for (uint32_t j = 0; j < n; j++) { uint8_t *w = it + j * 0x68; if (*(uint32_t *)(w + 0x10) == 13u && *(uint32_t *)(w + 0x28) == 0x9b7aa1a9u) *(uint32_t *)(w + 0x10) = 6u; }
    return u3x_retag_widget(page, 0, 0x2a, 0x4c414e2au, 32);
}
static char *o_fmt_int(char *o, char *end, int64_t v) {
    char tmp[24]; int n = 0; uint64_t u = v < 0 ? (uint64_t)(-v) : (uint64_t)v;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 22);
    if (v < 0 && o < end) *o++ = '-';
    while (n && o < end) *o++ = tmp[--n]; return o;
}
static char *o_append(char *o, char *end, const char *t) { while (*t && o < end) *o++ = *t++; return o; }
static void old_time_text(char *buf, size_t size, uint8_t *e, int32_t n, const char *t) {
    int64_t v = (int64_t)*(int32_t *)(e + 0x18) * n + *(int32_t *)(e + 0x10);
    char *o = buf, *end = buf + size - 1; o = o_fmt_int(o, end, v); if (*t) { o = o_append(o, end, " "); o = o_append(o, end, t); } *o = 0;
}
static int old_digit(const char *t) { for (const char *c = t; *c; c++) if (*c >= '0' && *c <= '9') return 1; return 0; }
static void old_back(char *name, int32_t leg) { char *o = o_append(name, name + 39, "ranks/prestige-back-"); o = o_fmt_int(o, name + 39, leg); *o = 0; }
static void old_remove(uint8_t names[][64], uint32_t count, uint32_t idx) {
    for (uint32_t i = idx; i + 1 < count; i++) std::memcpy(names[i], names[i + 1], 64);
    std::memset(names[count - 1], 0, 64);
}
static int old_key(const uint8_t *a, const uint8_t *b) { for (int i = 0; i < 32; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; } return 1; }
static uint32_t old_members(uint8_t *S, uint8_t **out) {
    uint32_t n = 0; if (!*(uint8_t **)(S + 8) || !*(uint64_t *)(S + 0x18)) return 0;
    for (int i = 0; i < 10; i++) { uint8_t *m = S + 0x680 + i * 0x120; if (!m[0x25]) continue;
        if (*(uint32_t *)(S + 0x12e4) == *(uint32_t *)(m + 0x2c) && n) { out[n++] = out[0]; out[0] = m; } else out[n++] = m; }
    return n;
}

static void batch11() {
    // retags: random columns with the widget ids, styles and sources mixed
    for (int t = 0; t < 2000; t++) {
        std::vector<uint8_t> it(0x68 * 70), it2;
        for (int k = 0; k < 70; k++) { uint8_t *w = it.data() + k * 0x68; for (int b = 0; b < 0x68; b++) w[b] = (uint8_t)rnd();
            uint32_t wid = rnd() % 3 == 0 ? 0x2027d9a3u : rnd() % 2 ? 0 : 5, ty = rnd() % 3 ? 21 : 20, src = rnd() % 3 == 0 ? 0xc7 : rnd() % 2 ? 0x2a : 0x2b, tag = rnd() % 4 ? 0 : 1;
            uint32_t style = rnd() % 2 ? 13 : 12, key = rnd() % 2 ? 0x9b7aa1a9u : 0;
            std::memcpy(w + 8, &wid, 4); std::memcpy(w + 0x30, &ty, 4); std::memcpy(w + 0x34, &src, 4); std::memcpy(w + 0x40, &tag, 4); std::memcpy(w + 0x10, &style, 4); std::memcpy(w + 0x28, &key, 4); }
        it2 = it;
        uint32_t n = t % 7 == 0 ? 33 + rnd() % 40 : rnd() % 66;
        alignas(8) uint8_t c1[16] = {}, c2[16] = {}, p1[0x50] = {}, p2[0x50] = {}; uint8_t *i1 = it.data(), *i2 = it2.data();
        std::memcpy(c1, &n, 4); std::memcpy(c1 + 8, &i1, 8); std::memcpy(c2, &n, 4); std::memcpy(c2 + 8, &i2, 8);
        uint8_t *a = c1, *b = c2; std::memcpy(p1 + 0x48, &a, 8); std::memcpy(p2 + 0x48, &b, 8);
        if (t & 1) { CHECK(old_retag_records(p1) == u3x_retag_widget(p2, 0x2027d9a3u, 0xc7, 0x4c414ec7u, 64)); }
        else { CHECK(old_k04(p1) == new_k04(p2)); }
        CHECK(it == it2);
    }
    // time limit text: random rules, counts, labels (long ones reach the buffer end)
    for (int t = 0; t < 100000; t++) {
        alignas(8) uint8_t e[0x20]; for (auto &x : e) x = (uint8_t)rnd(); int32_t base = (int32_t)(rnd() << 8 ^ rnd()), step = (int32_t)(rnd() % 2000) - 1000;
        if (t % 3 == 0) { base = (int32_t)(rnd() % 100); step = (int32_t)(rnd() % 60); }
        std::memcpy(e + 0x10, &base, 4); std::memcpy(e + 0x18, &step, 4); const int32_t n = (int32_t)(rnd() % 200) - 100;
        char label[64]; const int len = (int)(rnd() % 60); for (int k = 0; k < len; k++) label[k] = (char)('a' + rnd() % 26); label[len] = 0;
        char a[48], b[48]; std::memset(a, 0x33, 48); std::memset(b, 0x33, 48);
        old_time_text(a, 48, e, n, label); u3x_time_limit_text(b, b + 47, e, n, label); CHECK(std::memcmp(a, b, 48) == 0);
        CHECK(old_digit(label) == u3x_has_digit(label));
    }
    for (const char *s : {"", "10", "Minutes", "x9", "abc 0"}) CHECK(old_digit(s) == u3x_has_digit(s));
    // legacy back picture name
    for (int32_t leg : {-2147483647 - 1, -12, -1, 0, 1, 5, 6, 99, 2147483647}) { char a[40], b[40]; std::memset(a, 1, 40); std::memset(b, 1, 40); old_back(a, leg); u3x_legacy_back_name(b, b + 39, leg); CHECK(std::memcmp(a, b, 40) == 0); }
    // remove a profile name
    for (int t = 0; t < 2000; t++) {
        static uint8_t n1[16][64], n2[16][64]; for (auto &r : n1) for (auto &x : r) x = (uint8_t)rnd(); std::memcpy(n2, n1, sizeof n1);
        const uint32_t count = 1 + rnd() % 16, idx = rnd() % count;
        old_remove(n1, count, idx); u3x_remove_name(&n2[0][0], count, idx, 64); CHECK(std::memcmp(n1, n2, sizeof n1) == 0);
    }
    // keys
    for (int t = 0; t < 100000; t++) { uint8_t a[33], b[33]; for (int k = 0; k < 33; k++) { a[k] = (uint8_t)(rnd() % 3); b[k] = rnd() % 4 ? a[k] : (uint8_t)(rnd() % 3); } CHECK(old_key(a, b) == u3x_key_equal(a, b)); }
    // session members with own-first ordering
    for (int t = 0; t < 5000; t++) {
        static uint8_t S[0x1300]; for (auto &x : S) x = 0; uint8_t *be = t % 9 ? S : nullptr; std::memcpy(S + 8, &be, 8);
        uint64_t sid = t % 7 ? 1 + rnd() : 0; std::memcpy(S + 0x18, &sid, 8); uint32_t me = rnd() % 5; std::memcpy(S + 0x12e4, &me, 4);
        for (int i = 0; i < 10; i++) { uint8_t *m = S + 0x680 + i * 0x120; m[0x25] = (uint8_t)(rnd() % 2); uint32_t id = rnd() % 5; std::memcpy(m + 0x2c, &id, 4); }
        uint8_t *a[10] = {}, *b[10] = {}; const uint32_t na = old_members(S, a), nb = u3x_session_members(S, b);
        CHECK(na == nb); CHECK(std::memcmp(a, b, sizeof a) == 0);
    }
}
