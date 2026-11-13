// fourth split: the old film tracker id and handle check (rp_trk_id, rp_h) against u3x_rp_trk_id / u3x_rp_h.
static uint32_t o39_id(const uint8_t *tr) { uint32_t lo = *(const uint32_t *)(tr + 0x1e4) & 0xfffu; return ((tr[0x1ec] && lo) ? lo + 0x800 : lo) + (*(const uint32_t *)(tr + 0x1e8) << 12); }
static uint8_t *o39_h(const uint8_t *h) { uint8_t **pp = *(uint8_t ***)h; uint32_t id = *(const uint32_t *)(h + 8); return (pp && id && *pp && *(uint32_t *)(*pp + 0x7c) == id) ? *pp : 0; }
static void batch39() {
    static uint8_t tr[0x200], obj[0x80], h[16]; static uint8_t *slot;
    for (int t = 0; t < 200000; t++) {
        *(uint32_t *)(tr + 0x1e4) = rnd(); *(uint32_t *)(tr + 0x1e8) = rnd() % 64; tr[0x1ec] = (uint8_t)(rnd() % 2);
        if (t % 5 == 0) *(uint32_t *)(tr + 0x1e4) &= 0xfffff000u;
        CHECK(o39_id(tr) == u3x_rp_trk_id(tr));
        *(uint32_t *)(obj + 0x7c) = rnd() % 4; slot = rnd() % 4 ? obj : nullptr;
        *(uint8_t ***)h = rnd() % 5 ? &slot : nullptr; *(uint32_t *)(h + 8) = rnd() % 4;
        CHECK(o39_h(h) == u3x_rp_h(h));
    }
}
