// v7.126 part E: the old helper code (v7.125) for the invite frames and base64, and the old-versus-new checks.
static uint8_t o_key[16];
static uint32_t o_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t o_rol(uint32_t x, int n) { return x << n | x >> (32 - n); }
static void o_run(uint32_t *s, uint8_t *p, uint32_t n, int mode) {
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
    for (uint32_t i = 0; i + 4 <= n; i += 4) {
        d ^= 0x5b3aa654u; c += a; c += d; c = o_rol(c, 7); b += d; b += c; c ^= 0x75970a4du; b = o_rol(b, 11); a += c; a += b; a = o_rol(a, 17);
        uint32_t t = a & b; b = ~b; d += t | (c & b); a = ~a;
        uint32_t w = o_le32(p + i);
        if (mode == 2) { uint32_t o = d ^ w; d = w; w = o; } else { d ^= w; w = d; }
        if (mode) { p[i] = (uint8_t)w; p[i + 1] = (uint8_t)(w >> 8); p[i + 2] = (uint8_t)(w >> 16); p[i + 3] = (uint8_t)(w >> 24); } }
    s[0] = a; s[1] = b; s[2] = c; s[3] = d;
}
static void o_le(uint8_t *o, uint32_t v) { o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)(v >> 16); o[3] = (uint8_t)(v >> 24); }
static void o_init(uint32_t *s, uint32_t ctr) { uint8_t t[16]; for (int i = 0; i < 4; i++) s[i] = o_le32(o_key + 4 * i); o_le(t, ctr); o_run(s, t, 4, 0); for (int i = 0; i < 4; i++) o_le(t + 4 * i, s[i]); o_run(s, t, 16, 1); }
static void o_final(const uint32_t *s, uint8_t *o) { uint32_t x = s[0] ^ s[1] ^ s[2] ^ s[3]; for (int i = 0; i < 4; i++) o_le(o + 4 * i, x ^ s[i]); }
static int o_frame(uint8_t *o, const uint8_t *d, uint32_t n, uint32_t ctr) {
    uint32_t pad = (0u - n) & 3u, s[4]; o[0] = 0x33; o[1] = (uint8_t)pad; o[2] = (uint8_t)(n >> 8); o[3] = (uint8_t)n;
    uint8_t *b = o + 0x14; std::memcpy(b, d, n); std::memset(b + n, 0, pad);
    o_init(s, ctr); o_run(s, b, n + pad, 0); o_final(s, o + 4);
    o_init(s, ctr); o_run(s, b, n + pad, 1); return (int)(0x14 + n + pad);
}
static int o_unframe(uint8_t *o, uint32_t total, uint32_t ctr) {
    if (total < 0x14 || o[0] != 0x33) return -1; uint32_t pad = o[1], n = (uint32_t)o[2] << 8 | o[3];
    uint32_t w = (n + pad + 3u) & ~3u; if (0x14 + w != total) return -1;
    uint32_t s[4]; uint8_t mac[16]; uint8_t *b = o + 0x14;
    o_init(s, ctr); o_run(s, b, w, 2); o_init(s, ctr); o_run(s, b, w, 0); o_final(s, mac);
    return std::memcmp(mac, o + 4, 16) ? -1 : (int)n;
}
static uint32_t o_b64(char *o, const uint8_t *s, uint32_t n) {
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; uint32_t k = 0, i = 0;
    for (; i + 3 <= n; i += 3) { o[k++] = a[s[i] >> 2]; o[k++] = a[(s[i] & 3) << 4 | s[i + 1] >> 4]; o[k++] = a[(s[i + 1] & 15) << 2 | s[i + 2] >> 6]; o[k++] = a[s[i + 2] & 63]; }
    if (n - i == 1) { o[k++] = a[s[i] >> 2]; o[k++] = a[(s[i] & 3) << 4]; o[k++] = '='; o[k++] = '='; }
    else if (n - i == 2) { o[k++] = a[s[i] >> 2]; o[k++] = a[(s[i] & 3) << 4 | s[i + 1] >> 4]; o[k++] = a[(s[i + 1] & 15) << 2]; o[k++] = '='; }
    o[k] = 0; return k;
}
static uint8_t o_b64v(char c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c - 'A') : c >= 'a' && c <= 'z' ? (uint8_t)(c - 'a' + 26) : c >= '0' && c <= '9' ? (uint8_t)(c - '0' + 52) : c == '+' || c == '*' ? 62 : c == '/' || c == '-' ? 63 : 0xffu; }

static void batch13() {
    static uint8_t data[0x900], f1[0x920], f2[0x920];
    for (int t = 0; t < 20000; t++) {
        for (auto &x : o_key) x = (uint8_t)rnd(); const uint32_t n = rnd() % 0x800, ctr = rnd() << 8 ^ rnd();
        for (uint32_t k = 0; k < n; k++) data[k] = (uint8_t)rnd();
        std::memset(f1, 0xaa, sizeof f1); std::memset(f2, 0xaa, sizeof f2);
        const int l1 = o_frame(f1, data, n, ctr), l2 = u3x_frame_seal(f2, data, n, ctr, o_key);
        CHECK(l1 == l2); CHECK(std::memcmp(f1, f2, sizeof f1) == 0);
        // open: the good frame, a damaged one, a short pad, a wrong length, a wrong counter
        const int mode = t % 5; uint32_t total = (uint32_t)l1, c2 = ctr;
        if (mode == 1) f1[0x14 + rnd() % (n + 1)] ^= 4;
        if (mode == 2 && f1[1]) f1[1]--;
        if (mode == 3) total -= 4;
        if (mode == 4) c2 ^= 1;
        std::memcpy(f2, f1, sizeof f1);
        CHECK(o_unframe(f1, total, c2) == u3x_frame_open(f2, total, c2, o_key)); CHECK(std::memcmp(f1, f2, sizeof f1) == 0);
    }
    uint8_t z[1] = {0x33}; CHECK(o_unframe(z, 3, 0) == u3x_frame_open(z, 3, 0, o_key));
    for (int t = 0; t < 20000; t++) {
        uint8_t s[80]; const uint32_t n = rnd() % 80; for (uint32_t k = 0; k < n; k++) s[k] = (uint8_t)rnd();
        char a[120], b[120]; std::memset(a, 7, sizeof a); std::memset(b, 7, sizeof b);
        CHECK(o_b64(a, s, n) == u3x_base64(b, s, n)); CHECK(std::memcmp(a, b, sizeof a) == 0);
    }
    for (int c = -128; c < 128; c++) CHECK(o_b64v((char)c) == u3x_base64_value((char)c));
}
