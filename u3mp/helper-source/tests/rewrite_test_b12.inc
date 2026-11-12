// v7.125 part D: the old helper code (v7.124) for the seal, and the old-versus-new checks (random keys; no game values).
static uint32_t t12_be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t o_bf[1042];
#define O_BF_F(x) ((((o_bf[18 + ((x) >> 24)] + o_bf[18 + 256 + (((x) >> 16) & 0xff)]) ^ o_bf[18 + 512 + (((x) >> 8) & 0xff)]) + o_bf[18 + 768 + ((x) & 0xff)]))
static void o_bf_dec(uint8_t *p) {
    uint32_t l = t12_be(p), r = t12_be(p + 4);
    for (int i = 17; i > 1; i--) { l ^= o_bf[i]; r ^= O_BF_F(l); uint32_t t = l; l = r; r = t; }
    uint32_t t = l; l = r; r = t; r ^= o_bf[1]; l ^= o_bf[0];
    p[0] = (uint8_t)(l >> 24); p[1] = (uint8_t)(l >> 16); p[2] = (uint8_t)(l >> 8); p[3] = (uint8_t)l; p[4] = (uint8_t)(r >> 24); p[5] = (uint8_t)(r >> 16); p[6] = (uint8_t)(r >> 8); p[7] = (uint8_t)r;
}
static void o_bf_enc(uint8_t *p) {
    uint32_t l = t12_be(p), r = t12_be(p + 4);
    for (int i = 0; i < 16; i++) { l ^= o_bf[i]; r ^= O_BF_F(l); uint32_t t = l; l = r; r = t; }
    uint32_t t = l; l = r; r = t; r ^= o_bf[16]; l ^= o_bf[17];
    p[0] = (uint8_t)(l >> 24); p[1] = (uint8_t)(l >> 16); p[2] = (uint8_t)(l >> 8); p[3] = (uint8_t)l; p[4] = (uint8_t)(r >> 24); p[5] = (uint8_t)(r >> 16); p[6] = (uint8_t)(r >> 8); p[7] = (uint8_t)r;
}
typedef struct { uint32_t h[5]; uint8_t buf[64]; uint32_t n; uint64_t len; } OSha1;
#define OROL(v, b) (((v) << (b)) | ((v) >> (32 - (b))))
static void o_sha1_block(uint32_t *h, const uint8_t *p) {
    uint32_t w[80]; for (int i = 0; i < 16; i++) w[i] = t12_be(p + i * 4); for (int i = 16; i < 80; i++) w[i] = OROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) { uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999u; } else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1u; } else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; } else { f = b ^ c ^ d; k = 0xca62c1d6u; }
        uint32_t t = OROL(a, 5) + f + e + k + w[i]; e = d; d = c; c = OROL(b, 30); b = a; a = t; }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
static void o_sha1_init(OSha1 *s) { s->h[0] = 0x67452301u; s->h[1] = 0xefcdab89u; s->h[2] = 0x98badcfeu; s->h[3] = 0x10325476u; s->h[4] = 0xc3d2e1f0u; s->n = 0; s->len = 0; }
static void o_sha1_upd(OSha1 *s, const uint8_t *p, uint32_t n) { s->len += n; while (n) { uint32_t k = 64 - s->n; if (k > n) k = n; std::memcpy(s->buf + s->n, p, k); s->n += k; p += k; n -= k; if (s->n == 64) { o_sha1_block(s->h, s->buf); s->n = 0; } } }
static void o_sha1_fin(OSha1 *s, uint8_t *out) {
    uint64_t bits = s->len * 8; uint8_t pad = 0x80; o_sha1_upd(s, &pad, 1); uint8_t z = 0; while (s->n != 56) o_sha1_upd(s, &z, 1);
    uint8_t l[8]; for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i)); o_sha1_upd(s, l, 8);
    for (int i = 0; i < 5; i++) { out[i * 4] = (uint8_t)(s->h[i] >> 24); out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16); out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8); out[i * 4 + 3] = (uint8_t)s->h[i]; }
}
static void o_hmac(const uint8_t *key, uint32_t kl, const uint8_t *p, uint32_t n, uint8_t *out) {
    uint8_t k[64]; std::memset(k, 0, 64); if (kl > 64) { OSha1 t; o_sha1_init(&t); o_sha1_upd(&t, key, kl); o_sha1_fin(&t, k); } else std::memcpy(k, key, kl);
    uint8_t ip[64], op[64]; for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    OSha1 s; o_sha1_init(&s); o_sha1_upd(&s, ip, 64); o_sha1_upd(&s, p, n); uint8_t in[20]; o_sha1_fin(&s, in);
    o_sha1_init(&s); o_sha1_upd(&s, op, 64); o_sha1_upd(&s, in, 20); o_sha1_fin(&s, out);
}
static int o_unseal(uint8_t *rec, const uint8_t *key) {
    if (t12_be(rec + 4) != 0x2818u) return 0;
    for (uint32_t o = 8; o < 0x2820; o += 8) o_bf_dec(rec + o);
    uint8_t mac[20]; o_hmac(key, 64, rec + 8, 0x2804, mac);
    for (int i = 0; i < 20; i++) if (mac[i] != rec[0x280c + i]) return 0;
    return 1;
}

static void batch12() {
    // a known SHA-1 / HMAC value (RFC 2202 test case 2) to be sure both are the standard algorithm
    { uint8_t mac[20]; const char *key = "Jefe", *data = "what do ya want for nothing?";
      u3x_hmac_sha1((const uint8_t *)key, 4, (const uint8_t *)data, 28, mac);
      const uint8_t want[20] = {0xef, 0xfc, 0xdf, 0x6a, 0xe5, 0xeb, 0x2f, 0xa2, 0xd2, 0x74, 0x16, 0xd5, 0xf1, 0x84, 0xdf, 0x9c, 0x25, 0x9a, 0x7c, 0x79};
      CHECK(std::memcmp(mac, want, 20) == 0); }
    // HMAC: random keys (short, 64, long) and lengths
    for (int t = 0; t < 3000; t++) {
        static uint8_t key[200], data[700]; const uint32_t kl = rnd() % 200, n = rnd() % 700;
        for (auto &x : key) x = (uint8_t)rnd(); for (auto &x : data) x = (uint8_t)rnd();
        uint8_t a[20], b[20]; o_hmac(key, kl, data, n, a); u3x_hmac_sha1(key, kl, data, n, b); CHECK(std::memcmp(a, b, 20) == 0);
    }
    // Blowfish with random states: blocks both ways, and a whole record sealed by the new code unsealed by the old
    static uint8_t rec1[0x2828], rec2[0x2828], key[64];
    for (int t = 0; t < 20; t++) {
        for (auto &w : o_bf) w = rnd() << 8 ^ rnd(); for (auto &x : key) x = (uint8_t)rnd();
        for (int k = 0; k < 200; k++) { uint8_t b1[8], b2[8]; for (int i = 0; i < 8; i++) b1[i] = b2[i] = (uint8_t)rnd();
            o_bf_dec(b1); u3x_blowfish(o_bf, b2, 1); CHECK(std::memcmp(b1, b2, 8) == 0);
            o_bf_enc(b1); u3x_blowfish(o_bf, b2, 0); CHECK(std::memcmp(b1, b2, 8) == 0); }
        for (auto &x : rec1) x = (uint8_t)rnd(); rec1[4] = 0; rec1[5] = 0; rec1[6] = 0x28; rec1[7] = 0x18; std::memcpy(rec2, rec1, sizeof rec1);
        u3x_seal_record(rec2, o_bf, key, 64);
        uint8_t mac[20]; o_hmac(key, 64, rec1 + 8, 0x2804, mac); std::memcpy(rec1 + 0x280c, mac, 20); for (uint32_t o = 8; o < 0x2820; o += 8) o_bf_enc(rec1 + o);
        CHECK(std::memcmp(rec1, rec2, sizeof rec1) == 0);
        std::memcpy(rec2, rec1, sizeof rec1);
        CHECK(o_unseal(rec1, key) == u3x_unseal_record(rec2, o_bf, key, 64)); CHECK(std::memcmp(rec1, rec2, sizeof rec1) == 0);
        rec1[100] ^= 1; std::memcpy(rec2, rec1, sizeof rec1); o_bf_enc(rec1 + 96); o_bf_enc(rec2 + 96);   // a damaged record is refused by both
        CHECK(o_unseal(rec1, key) == u3x_unseal_record(rec2, o_bf, key, 64)); CHECK(std::memcmp(rec1, rec2, sizeof rec1) == 0);
        rec1[6] = 0x27; std::memcpy(rec2, rec1, sizeof rec1); CHECK(o_unseal(rec1, key) == u3x_unseal_record(rec2, o_bf, key, 64));
    }
}
