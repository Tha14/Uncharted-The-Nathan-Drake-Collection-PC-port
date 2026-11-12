// third split: the old emblem layer corners and drip quads (emb_render119) against layer_uv / drip_quad.
static void o30_uv(uint32_t *uv, uint32_t s_, uint32_t cs, uint32_t sn) {
    uint32_t A = f_div(0x3f000000u, s_), Bq = f_div(0xbf000000u, s_);
    uint32_t f10 = f_mul(A, cs), f8 = f_mul(A, sn), f12 = f_add(f8, 0x3f000000u), f11 = f_mul(Bq, sn), f0 = f_mul(Bq, cs);
    uint32_t f13 = f_add(f10, 0x3f000000u), f7 = f_add(f11, 0x3f000000u), f9 = f_add(f0, 0x3f000000u);
    uv[0] = f_add(f8, f9); uv[1] = f_sub(F_ONE, f_add(f0, f7));
    uv[2] = f_add(f11, f9); uv[3] = f_sub(F_ONE, f_add(f10, f7));
    uv[4] = f_add(f11, f13); uv[5] = f_sub(F_ONE, f_add(f10, f12));
    uv[6] = f_add(f8, f13); uv[7] = f_sub(F_ONE, f_add(f0, f12)); }
static void o30_q(uint32_t *q, const uint8_t *en, uint32_t g, uint32_t s_, uint32_t cs, uint32_t sn, uint32_t h) {
    uint32_t p = *(const uint32_t *)en;
    uint32_t fy = f_from_int(*(const int32_t *)(en + 8)), fx = f_from_int(*(const int32_t *)(en + 4)), fpc = f_from_int((int32_t)(p & 3)), fpr = f_from_int((int32_t)(3 - (p >> 2)));
    uint32_t a12 = f_div(fy, g); a12 = f_add(a12, a12); uint32_t uc = f_madd(fpc, 0x3e800000u, 0x3e000000u);
    uint32_t a13 = f_div(fx, g); uint32_t vc = f_madd(fpr, 0x3e800000u, 0x3e000000u); a13 = f_add(a13, a13); a12 = f_sub(a12, F_ONE);
    q[5] = f_add(uc, 0x3e000000u); q[4] = f_sub(uc, 0x3e000000u); uint32_t vhi = f_add(vc, 0x3e000000u);
    a13 = f_sub(a13, F_ONE); uint32_t Y = f_mul(a12, s_); uint32_t vlo = f_sub(vc, 0x3e000000u); uint32_t Xx = f_mul(a13, s_);
    uint32_t t0 = f_mul(Y, cs), t1 = f_mul(Y, sn); uint32_t yr = f_madd(Xx, sn, t0), xr = f_madd(Xx, cs, t1 ^ 0x80000000u);
    yr = f_madd(yr, 0x3f000000u, 0x3f000000u); xr = f_madd(xr, 0x3f000000u, 0x3f000000u);
    q[2] = f_sub(yr, h); q[1] = f_add(xr, h); q[3] = f_add(yr, h); q[0] = f_sub(xr, h);
    q[6] = f_sub(F_ONE, vhi); q[7] = f_sub(F_ONE, vlo); }
static uint32_t o30_f(int t) {   // plausible and odd float inputs
    switch (t % 4) {
    case 0: return 0x3c000000u + rnd() % 0x4000000u;
    case 1: return (rnd() & 0x80000000u) | (0x3d000000u + rnd() % 0x2800000u);
    case 2: return rnd();
    default: return t & 4 ? 0 : 0x80000000u;
    }
}
static void batch30() {
    for (int t = 0; t < 300000; t++) {
        const uint32_t s = o30_f(t), cs = o30_f(t >> 2), sn = o30_f(t >> 4), h = o30_f(t >> 6);
        uint32_t a[8], b[8];
        o30_uv(a, s, cs, sn);
        layer_uv(b, s, cs, sn);
        for (int k = 0; k < 8; k++) CHECK(a[k] == b[k]);
        uint8_t en[12];
        *(uint32_t *)en = rnd() & 15;
        *(int32_t *)(en + 4) = (int32_t)(rnd() % 64) - 8;
        *(int32_t *)(en + 8) = (int32_t)(rnd() % 64) - 8;
        const uint32_t g = t & 8 ? f_from_int((int32_t)(rnd() % 40)) : o30_f(t >> 8);
        o30_q(a, en, g, s, cs, sn, h);
        drip_quad(b, en, g, s, cs, sn, h);
        for (int k = 0; k < 8; k++) CHECK(a[k] == b[k]);
    }
}
