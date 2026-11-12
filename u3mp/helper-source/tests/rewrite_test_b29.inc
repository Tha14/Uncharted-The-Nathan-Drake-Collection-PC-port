// third split: the old number formatter (fmt_int) and mod text ids (mod_name / mod_desc) against the C++ ones.
static char *o29_fmt_int(char *o, char *end, int64_t v, int commas) {
    char tmp[24]; int n = 0; uint64_t u = v < 0 ? (uint64_t)(-v) : (uint64_t)v;
    do { if (commas && n && (n % 4) == 3) tmp[n++] = ','; tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 22);
    if (v < 0 && o < end) *o++ = '-';
    while (n && o < end) *o++ = tmp[--n];
    return o; }
static uint32_t o29_mod_name(uint32_t bit) {
    static const uint32_t n[13] = {0x9fe22c98, 0xeebca10f, 0x317982c3, 0x868a039f, 0xcf42ac41, 0x75ac8784, 0x2cb4e03f, 0xca8044df, 0x90a93da0, 0x2be72ef2, 0xc2fb6973, 0xb6ab5db4, 0x815211c8};
    for (int i = 0; i < 13; i++) if (bit == (1u << i)) return n[i];
    return 0x58840ecd; }
static uint32_t o29_mod_desc(uint32_t bit) {
    static const uint32_t d[13] = {0x00ac24a8, 0xaa3e1660, 0xe2fa14a6, 0x2d14e5b5, 0x6ee89d56, 0xd099ee2f, 0xb3fae80f, 0x611ea2f5, 0x3b37db8a, 0x8079c8d8, 0x67ce00d8, 0xe5f1eb97, 0xefad05da};
    for (int i = 0; i < 13; i++) if (bit == (1u << i)) return d[i];
    return 0xd90c7fa3; }
static void batch29() {
    for (int t = 0; t < 400000; t++) {
        int64_t v;
        switch (t & 3) {
        case 0: v = (int64_t)(rnd() % 100000); break;
        case 1: v = (int32_t)rnd(); break;
        case 2: v = (int64_t)(((uint64_t)rnd() << 32) | rnd()); break;
        default: v = t < 8 ? (t == 3 ? INT64_MIN : 0) : (int64_t)rnd(); break;
        }
        const int commas = (t >> 2) & 1, room = (int)(rnd() % 30);
        char a[40], b[40];
        for (int k = 0; k < 40; k++) a[k] = b[k] = 'x';
        char *ea = o29_fmt_int(a, a + room, v, commas), *eb = append_number(b, b + room, v, commas != 0);
        CHECK(ea - a == eb - b);
        for (int k = 0; k < 40; k++) CHECK(a[k] == b[k]);
    }
    for (uint32_t i = 0; i < 40; i++) {
        const uint32_t bits[3] = {i < 32 ? 1u << i : 0u, rnd(), i};
        for (uint32_t bit : bits) { CHECK(o29_mod_name(bit) == u3x_mod_name(bit)); CHECK(o29_mod_desc(bit) == u3x_mod_desc(bit)); }
    }
}
