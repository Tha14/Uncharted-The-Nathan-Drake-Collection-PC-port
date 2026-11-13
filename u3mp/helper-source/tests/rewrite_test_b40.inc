// fourth split: the old emblem maths (sincos119, fq32_119, log2q8_119, h16q16_119, emb_blend119, emb_dxt119, emb_lod119)
// against the C++ versions.
static uint32_t o40_sincos(uint32_t x, uint32_t flag) {
    if ((x & 0x7fffffffu) > 0x7f800000u) return 0x7fc00000u;
    if ((x & 0x7fffffffu) == 0x7f800000u) return x;
    if (!(x & 0x7fffffffu)) return flag ? 0x3f800000u : x;
    uint32_t t = f_mul(x, 0x3f22f983u);
    t = f_less(0, t) ? f_add(t, 0x3f000000u) : f_sub(t, 0x3f000000u);
    int32_t n = f_to_int(t); uint32_t q = ((uint32_t)n & 3) + flag, nf = f_from_int(n);
    uint32_t r = f_madd(nf, 0xbfc90fdau, x); r = f_madd(nf, 0xb3a22169u, r);
    if (f_less(0xb9800000u, r) && f_less(r, 0x39800000u)) { if (q & 1) r = 0x3f800000u; }
    else { uint32_t r2 = f_mul(r, r);
        if (!(q & 1)) { uint32_t r3 = f_mul(r, r2), p = f_madd(r2, 0xb94c8c6eu, 0x3c088342u); p = f_madd(r2, p, 0xbe2aaaa1u); r = f_madd(r3, p, r); }
        else { uint32_t p = f_madd(r2, 0xbab24993u, 0x3d2aa036u); p = f_madd(r2, p, 0xbeffffdfu); r = f_madd(r2, p, 0x3f800000u); } }
    return (q & 2) ? r ^ 0x80000000u : r; }
static int64_t o40_fq32(uint32_t f) { int32_t e = (int32_t)((f >> 23) & 0xff); if (!e) return 0; uint64_t m = (f & 0x7fffffu) | 0x800000u; int32_t sh = e - 127 - 23 + 32; int64_t v;
    if (sh >= 0) { if (sh > 38) sh = 38; v = (int64_t)(m << sh); } else if (sh < -40) v = 0; else v = (int64_t)((m + ((1ull << (-sh)) >> 1)) >> (-sh));
    return (f >> 31) ? -v : v; }
static int32_t o40_log2q8(uint32_t f) { int32_t e = (int32_t)((f >> 23) & 0xff) - 127; uint64_t m = (f & 0x7fffffu) | 0x800000u; uint32_t r = 0;
    for (int i = 0; i < 9; i++) { m = (m * m) >> 23; r <<= 1; if (m >= (1ull << 24)) { m >>= 1; r |= 1; } }
    return e * 256 + (int32_t)((r + 1) >> 1); }
static uint32_t o40_h16(uint32_t f) { if ((f >> 31) || !(f & 0x7fffffffu)) return 0;
    int32_t e = (int32_t)((f >> 23) & 0xff) - 127; if (e < -25) return 0; if (e > 15) return 0x7fffffffu;
    uint32_t m = (f & 0x7fffffu) | 0x800000u; int32_t sh = 13; if (e < -14) sh += -14 - e;
    uint32_t q = m >> sh, rem = m & ((1u << sh) - 1), hf = 1u << (sh - 1); if (rem > hf || (rem == hf && (q & 1))) q++;
    int32_t p = e - 23 + sh + 16; uint64_t v = p >= 0 ? ((uint64_t)q << p) : ((uint64_t)q >> (-p)); return v > 0x7fffffffu ? 0x7fffffffu : (uint32_t)v; }
static void o40_blend(uint8_t *px, const uint32_t *c16, uint64_t a32) {
    if (a32 > (1ull << 32)) a32 = 1ull << 32; uint64_t ia = (1ull << 32) - a32;
    for (int k = 0; k < 3; k++) { uint64_t v = (255ull * c16[k] * a32 + (uint64_t)px[k] * 65536ull * ia + (1ull << 47)) >> 48; px[k] = (uint8_t)(v > 255 ? 255 : v); }
    uint64_t v = (255ull * 65536ull * a32 + (uint64_t)px[3] * 65536ull * ia + (1ull << 47)) >> 48; px[3] = (uint8_t)(v > 255 ? 255 : v); }
struct O40Dds { uint8_t *file; uint32_t w, h, maxl, bs; uint32_t off[16]; };
static uint32_t o40_dxt(const O40Dds *t, uint32_t l, int32_t x, int32_t y) {
    int32_t w = (int32_t)(t->w >> l), h = (int32_t)(t->h >> l); if (w < 1) w = 1; if (h < 1) h = 1;
    if (x < 0) x = 0; else if (x >= w) x = w - 1; if (y < 0) y = 0; else if (y >= h) y = h - 1;
    const uint8_t *k = t->file + t->off[l] + ((uint32_t)(y >> 2) * (uint32_t)((w + 3) >> 2) + (uint32_t)(x >> 2)) * t->bs + (t->bs == 16 ? 8 : 0);
    uint32_t c0 = k[0] | (uint32_t)k[1] << 8, c1 = k[2] | (uint32_t)k[3] << 8;
    uint32_t ix = ((k[4] | (uint32_t)k[5] << 8 | (uint32_t)k[6] << 16 | (uint32_t)k[7] << 24) >> (((y & 3) * 4 + (x & 3)) * 2)) & 3;
    uint32_t r0 = c0 >> 11, r1 = c1 >> 11; r0 = (r0 << 3) | (r0 >> 2); r1 = (r1 << 3) | (r1 >> 2);
    if (ix == 0) return r0; if (ix == 1) return r1;
    if (c0 > c1 || t->bs == 16) return ix == 2 ? (2 * r0 + r1) / 3 : (r0 + 2 * r1) / 3;
    return ix == 2 ? (r0 + r1) / 2 : 0; }
static void o40_lod(const O40Dds *t, uint32_t rho2, uint32_t *L) {
    L[0] = L[1] = L[2] = 0; if (!(rho2 & 0x7fffffffu) || (rho2 >> 31)) return;
    int32_t lam = ((o40_log2q8(rho2) + 1) >> 1) - 256; if (lam <= 0) return;
    uint32_t d = (uint32_t)lam, mx = t->maxl << 8; if (d > mx) d = mx;
    L[0] = d >> 8; L[2] = d & 255; L[1] = L[0] + 1; if (L[1] > t->maxl) { L[1] = t->maxl; L[2] = 0; } }
static uint32_t o40_float(int t) { switch (t % 4) { case 0: return 0x3c000000u + rnd() % 0x6000000u; case 1: return (rnd() & 0x80000000u) | (0x30000000u + rnd() % 0x18000000u); case 2: return rnd(); default: return rnd() % 64; } }
static void batch40() {
    for (int t = 0; t < 400000; t++) {
        const uint32_t f = o40_float(t);
        CHECK(o40_sincos(f, 0) == u3x_sincos119(f, 0)); CHECK(o40_sincos(f, 1) == u3x_sincos119(f, 1));
        CHECK(o40_fq32(f) == u3x_fq32(f)); CHECK(o40_h16(f) == u3x_h16q16(f));
        if (!(f >> 31) && (f & 0x7f800000u)) CHECK(o40_log2q8(f) == u3x_log2q8(f));
    }
    static uint8_t file[4096];
    for (int t = 0; t < 4000; t++) {
        for (auto &b : file) b = (uint8_t)rnd();
        O40Dds a; a.file = file; a.w = 1u << (rnd() % 7); a.h = 1u << (rnd() % 7); a.bs = rnd() % 2 ? 8 : 16; a.maxl = rnd() % 5;
        for (int l = 0; l < 16; l++) a.off[l] = (uint32_t)(l * 64);
        uint8_t raw[88]; memcpy(raw, &a, sizeof a);
        for (int q = 0; q < 20; q++) { const uint32_t l = rnd() % 4; const int32_t x = (int32_t)(rnd() % 80) - 8, y = (int32_t)(rnd() % 80) - 8;
            CHECK(o40_dxt(&a, l, x, y) == u3x_emb_dxt(raw, l, x, y)); }
        uint32_t La[3], Lb[3]; const uint32_t rho = o40_float(t);
        o40_lod(&a, rho, La); u3x_emb_lod(raw, rho, Lb); CHECK(La[0] == Lb[0] && La[1] == Lb[1] && La[2] == Lb[2]);
        uint8_t pa[4], pb[4]; for (int k = 0; k < 4; k++) pa[k] = pb[k] = (uint8_t)rnd();
        uint32_t c16[3]; for (auto &c : c16) c = rnd() % 0x1000000u; const uint64_t a32 = t % 3 ? rnd() : ((uint64_t)rnd() << 1);
        o40_blend(pa, c16, a32); u3x_emb_blend(pb, c16, a32); CHECK(memcmp(pa, pb, 4) == 0);
    }
}
