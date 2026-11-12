// v7.139 part Q: the old helper code (v7.138) for the connection lookup, the replay clock, the context stack and the
// fixed camera slot, and the checks.
static uint8_t *o_by_addr(uint8_t *ent, uint32_t ip, uint32_t port) { for (int i = 0; i < 32; i++) { uint8_t *e = ent + i * 0x220; if (e[4] && *(uint32_t *)(e + 0x2c) == ip && *(uint16_t *)(e + 0x30) == (uint16_t)port) return e; } return 0; }
static void o_clock(uint8_t *c, uint32_t gt, int32_t t) {
    c[0] = 0; *(uint32_t *)(c + 8) = gt; *(uint32_t *)(c + 0x14) = 0x3c888889u; *(uint16_t *)(c + 0x1c) = 0;
    *(uint32_t *)(c + 0x24) = 0; *(uint32_t *)(c + 0x20) = 0; *(int32_t *)(c + 0x28) = t; *(int32_t *)(c + 0xc) = t; *(uint32_t *)(c + 0x18) = 0;
    *(uint32_t *)(c + 0x2c) = 0; *(uint32_t *)(c + 0x10) = 0; *(uint32_t *)(c + 4) = 0x3f800000u; *(uint64_t *)(c + 0x30) = 0; *(uint32_t *)(c + 0x38) = 0;
}
static int o_push(uint8_t *c, uintptr_t al, uintptr_t nm) { uint32_t n = *(uint32_t *)(c + 0x730); if (n >= 0x2e) return 0; uintptr_t *e = (uintptr_t *)(c + (size_t)n * 40); e[0] = al; e[1] = nm; e[2] = 0; e[3] = nm; e[4] = 1; *(uint32_t *)(c + 0x730) = n + 1; return 1; }
static void o_pop(uint8_t *c) { uint32_t n = *(uint32_t *)(c + 0x730) - 1; uintptr_t *e = (uintptr_t *)(c + (size_t)n * 40); e[0] = e[1] = e[2] = e[3] = e[4] = 0; *(uint32_t *)(c + 0x730) = n; }

static void batch26() {
    for (int t = 0; t < 3000; t++) {
        static uint8_t ent[32 * 0x220]; for (int i = 0; i < 32; i++) { uint8_t *e = ent + i * 0x220; e[4] = (uint8_t)(rnd() % 2); uint32_t ip = rnd() % 4; uint16_t pt = (uint16_t)(rnd() % 3); std::memcpy(e + 0x2c, &ip, 4); std::memcpy(e + 0x30, &pt, 2); }
        for (uint32_t ip = 0; ip < 5; ip++) for (uint32_t pt : {0u, 1u, 2u, 0x10001u}) CHECK(o_by_addr(ent, ip, pt) == u3x_entry_by_address(ent, ip, pt));
    }
    for (int t = 0; t < 1000; t++) { uint8_t a[0x40], b[0x40]; for (int k = 0; k < 0x40; k++) a[k] = b[k] = (uint8_t)rnd(); const uint32_t gt = rnd(); const int32_t tm = (int32_t)rnd();
        o_clock(a, gt, tm); u3x_clock_reset(b, gt, tm); CHECK(std::memcmp(a, b, sizeof a) == 0); }
    for (int t = 0; t < 2000; t++) {
        static uint8_t a[0x740], b[0x740]; for (uint32_t k = 0; k < sizeof a; k++) a[k] = b[k] = (uint8_t)rnd(); uint32_t n = rnd() % 0x2f; std::memcpy(a + 0x730, &n, 4); std::memcpy(b + 0x730, &n, 4);
        if (t % 3) { const uintptr_t al = rnd(), nm = rnd(); CHECK(o_push(a, al, nm) == u3x_context_push(b, al, nm)); }
        else if (n) { o_pop(a); u3x_context_pop(b); }
        CHECK(std::memcmp(a, b, sizeof a) == 0);
    }
    for (int32_t n = 1; n < 12; n++) for (int32_t idx = -40; idx < 40; idx++) {
        int32_t i1 = idx, i2 = idx; if (i1 < 0) i1 += n; if (i2 < 0) i2 += n;
        const int32_t k = i1 % n; CHECK((k < 0 ? -1 : k) == u3x_camera_slot(i2, n));
    }
}
