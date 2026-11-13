// fourth split: the old PPC fctiwz conversion (ppc_ctiwz119) against u3x_ppc_ctiwz.
static uint32_t o38_ctiwz(uint32_t f) { uint32_t e = (f >> 23) & 0xff;
    if (e == 0xff && (f & 0x7fffff)) return 0x80000000u;
    if (e >= 0x9e) { if (f >> 31) return 0x80000000u; return 0x7fffffffu; }
    return (uint32_t)o28_trunc(f); }
static void batch38() {
    for (int t = 0; t < 1000000; t++) { const uint32_t f = t & 1 ? rnd() : (rnd() & 0x80000000u) | (0x3f000000u + rnd() % 0x10000000u); CHECK(o38_ctiwz(f) == u3x_ppc_ctiwz(f)); }
    const uint32_t edge[] = {0, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0x4effffffu, 0x4f000000u, 0xcf000000u, 0xcf000001u};
    for (uint32_t f : edge) CHECK(o38_ctiwz(f) == u3x_ppc_ctiwz(f));
}
