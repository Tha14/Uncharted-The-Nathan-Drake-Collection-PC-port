// next batch part 3: the old ring segment count (v7.153 t9_ring / k24_ring) against ring_segments.
static uint32_t o28_op(uint32_t a, uint32_t b, int k) { uint32_t r;
    if (k == 0) __asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\tsubss %%xmm1,%%xmm0\n\tmovd %%xmm0,%0" : "=r"(r) : "r"(a), "r"(b) : "xmm0", "xmm1");
    else __asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\tmulss %%xmm1,%%xmm0\n\tmovd %%xmm0,%0" : "=r"(r) : "r"(a), "r"(b) : "xmm0", "xmm1");
    return r; }
static int o28_lt(uint32_t a, uint32_t b) { int r; __asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\txorl %0,%0\n\tucomiss %%xmm0,%%xmm1\n\tseta %b0" : "=&r"(r) : "r"(a), "r"(b) : "xmm0", "xmm1", "cc"); return r; }
static uint32_t o28_fma(uint32_t a, uint32_t b, uint32_t c) { uint32_t r; __asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\tmovd %3,%%xmm2\n\t"
    "cvtss2sd %%xmm0,%%xmm0\n\tcvtss2sd %%xmm1,%%xmm1\n\tcvtss2sd %%xmm2,%%xmm2\n\tmulsd %%xmm1,%%xmm0\n\taddsd %%xmm2,%%xmm0\n\t"
    "cvtsd2ss %%xmm0,%%xmm0\n\tmovd %%xmm0,%0" : "=r"(r) : "r"(a), "r"(b), "r"(c) : "xmm0", "xmm1", "xmm2"); return r; }
static int32_t o28_trunc(uint32_t a) { int32_t r; __asm__("movd %1,%%xmm0\n\tcvttss2si %%xmm0,%0" : "=r"(r) : "r"(a) : "xmm0"); return r; }
static int32_t o28_segments(uint32_t s, uint32_t p) {
    const uint32_t F1 = 0x3f800000u; uint32_t pc = (p >> 31) ? 0 : p; if (p == 0x80000000u) pc = p;
    pc = o28_lt(o28_op(pc, F1, 0), 0) ? pc : F1;
    uint32_t cnt = o28_fma(o28_op(F1, pc, 0), 0x41200000u, o28_op(pc, 0x41f00000u, 1));
    uint32_t m = o28_lt(o28_op(F1, s, 0), 0) ? s : F1;
    return o28_trunc(o28_op(m, cnt, 1)); }
static void batch28() {
    const uint32_t special[] = {0, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x3f000000u, 0x7fc00000u, 0xffc00000u, 0x7f800000u, 0xff800000u, 1, 0x3ef33333u, 0x40000000u};
    for (uint32_t s : special) for (uint32_t p : special) CHECK(o28_segments(s, p) == ring_segments(s, p));
    for (int t = 0; t < 200000; t++) { const uint32_t s = t & 1 ? rnd() : (0x3e000000u + rnd() % 0x2000000u), p = t & 2 ? rnd() : (0x3c000000u + rnd() % 0x4000000u);
        CHECK(o28_segments(s, p) == ring_segments(s, p)); }
}
// v7.175: the old c009 bit reader against u3x_bits_read
struct O28Br { uint32_t *w; uint32_t pos, cap, z; uint32_t lim, err; };
static uint32_t o28_br(O28Br *r, uint32_t n) {
    uint32_t p = r->pos; if (!n) return 0;
    if (r->err || n > 32 || p + n > r->lim) { r->err = 1; return 0; }
    uint32_t sh = p & 31, room = 32 - sh; const uint32_t *w = r->w + (p >> 5);
    uint32_t v = ((uint32_t)((uint64_t)w[0] << sh)) >> (32 - n);
    if (room < n) v |= w[1] >> (32 - (n - room));
    r->pos = p + n; return v; }
static void batch28b() {
    for (int t = 0; t < 20000; t++) {
        uint32_t buf[80]; for (uint32_t &w : buf) w = rnd();
        const uint32_t nbytes = rnd() % 300;
        O28Br a{buf, 0, 2400, 0, nbytes * 8, 0}, b = a;
        for (int k = 0; k < 64; k++) { const uint32_t n = rnd() % 34; CHECK(o28_br(&a, n) == u3x_bits_read(&b, n)); CHECK(a.pos == b.pos && a.err == b.err); }
    }
}
