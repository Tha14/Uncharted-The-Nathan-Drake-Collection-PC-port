// third split: the old XZ distance of the death camera (xz_dist2, SSE asm) against xz_distance2.
static uint32_t o35_xz(const uint32_t *a, const uint32_t *b) { uint32_t r;
    __asm__("movss (%1),%%xmm0\n\tsubss (%2),%%xmm0\n\tmulss %%xmm0,%%xmm0\n\tmovss 8(%1),%%xmm1\n\tsubss 8(%2),%%xmm1\n\tmulss %%xmm1,%%xmm1\n\taddss %%xmm1,%%xmm0\n\tmovd %%xmm0,%0"
            : "=r"(r) : "r"(a), "r"(b) : "xmm0", "xmm1", "memory"); return r & 0x7fffffffu; }
static void batch35() {
    for (int t = 0; t < 500000; t++) {
        uint32_t a[3], b[3];
        for (int k = 0; k < 3; k++) { a[k] = t & 1 ? rnd() : (rnd() & 0x80000000u) | (0x3c000000u + rnd() % 0x9000000u); b[k] = t & 2 ? rnd() : (rnd() & 0x80000000u) | (0x3c000000u + rnd() % 0x9000000u); }
        CHECK(o35_xz(a, b) == xz_distance2(a, b));
    }
}
