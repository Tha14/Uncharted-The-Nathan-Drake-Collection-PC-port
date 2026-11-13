// row 3 (s016/s017): character lookup, page list, look apply, refresh and poll early paths.
static int o45_ok(const void *p) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000u && v < (1ull << 47) && !(v & 7);
}
static uint8_t *old_s16_char45(uintptr_t, uint8_t *cm) {
    uint8_t **h = *reinterpret_cast<uint8_t ***>(cm + 0xf60);
    const uint32_t id = *reinterpret_cast<uint32_t *>(cm + 0xf68);
    if (!h || !id || !o45_ok(h)) return nullptr;
    uint8_t *ch = *h;
    if (!ch || !o45_ok(ch) || *reinterpret_cast<uint32_t *>(ch + 0x7c) != id) return nullptr;
    return ch;
}
static int old_s17_page45(uint32_t pg) {
    return pg == 0x81bfb972u || pg == 0xd7e00396u || pg == 0x1f707657u || pg == 0x211019f2u ||
           pg == 0x812c7d24u || pg == 0xea656124u || pg == 0x11201ffdu || pg == 0xcbe59a09u ||
           pg == 0xe13ed766u;
}
static int old_s17_apply45(uintptr_t base, uint8_t *cm, int own_kill) {
    volatile uint32_t *d = u3h_s16_diag();
    uint8_t *ch = old_s16_char45(base, cm);
    if (!ch) { d[4] = d[4] + 1; return 0; }
    if (*reinterpret_cast<uint32_t *>(ch + 0x30) != 0xa5a63e9bu) {
        if ((d[11] & 0xffu) != 0xffu) d[11] = d[11] + 1;
        return 0;
    }
    return 0;
}
static int old_s16_refresh45(uintptr_t base, uint64_t *r) {
    volatile uint32_t *d = u3h_s16_diag();
    uint8_t *cm = reinterpret_cast<uint8_t *>(r[4]);
    d[0] = d[0] + 1;
    uint32_t *eb = reinterpret_cast<uint32_t *>(t27_ext + 0x4050);
    uint32_t *ec = reinterpret_cast<uint32_t *>(t27_ext + 0x4450);
    if (eb[2] & 0x02000000u) return 0;
    if (!(ec[1] & 1u)) return 0;
    if (cm != reinterpret_cast<uint8_t *>(base + 0x17f9938)) return 0;
    return old_s17_apply45(base, cm, 0);
}
static int32_t o45_last74;
static uint8_t o45_on;
static void o45_page_refresh(uint8_t *) {}
static void old_s16_tick45(uintptr_t base) {
    volatile uint32_t *d = u3h_s16_diag();
    const uint32_t pg = u3h_active_page(base);
    uint8_t *cm = reinterpret_cast<uint8_t *>(base + 0x17f9938);
    if (pg == 0x81bfb972u) {
        d[8] = d[8] + 1;
        const int32_t v = *reinterpret_cast<int32_t *>(cm + 0xf74);
        if (v != o45_last74) { o45_last74 = v; d[9] = d[9] + 1; }
        d[10] = static_cast<uint32_t>(cm[0xfc8]) | static_cast<uint32_t>(cm[0xf70]) << 8 |
                static_cast<uint32_t>(*reinterpret_cast<uint8_t *>(base + 0x17bba90)) << 16 |
                static_cast<uint32_t>(old_s16_char45(base, cm) != nullptr) << 24;
    }
    u3x_h01_tick(base, cm);
    uint32_t *eb = reinterpret_cast<uint32_t *>(t27_ext + 0x4050);
    uint32_t *ec = reinterpret_cast<uint32_t *>(t27_ext + 0x4450);
    if (eb[2] & 0x04000000u) return;
    const int ok = old_s17_page45(pg) && *reinterpret_cast<volatile uint8_t *>(base + 0x17d5c61) &&
                   *reinterpret_cast<uint32_t *>(base + 0x17bba90) == 2 &&
                   *reinterpret_cast<uint32_t *>(base + 0x17d1868) != 2;
    if (!ok) {
        if (o45_on) {
            o45_on = 0;
            if ((ec[1] & 1u) && *reinterpret_cast<uint32_t *>(base + 0x17bba90) == 2 && cm[0xf70])
                old_s17_apply45(base, cm, 1);
        }
        return;
    }
    o45_on = 1;
    uint32_t w = d[11], cur = w >> 24;
    if (!old_s16_char45(base, cm)) {
        if (d[13] % 15u == 0 && cur < 255u) cur++;
        w = (w & 0x00ffffffu) | cur << 24;
    } else if (cur) {
        w = (w & 0x0000ffffu) | cur << 16;
    }
    d[11] = w;
    if (!cm[0xf70]) { o45_page_refresh(cm); d[12] = d[12] + 1; }
    d[13] = d[13] + 1;
    if (ec[1] & 1u) old_s17_apply45(base, cm, 1);
}
static void batch45() {
    alignas(8) static uint8_t ch[0x2000], cm[0x1100];
    static uint64_t regs[8];
    std::memset(ch, 0, sizeof ch);
    std::memset(cm, 0, sizeof cm);
    uint8_t *h = nullptr;
    *reinterpret_cast<uint8_t ***>(cm + 0xf60) = &h;

    CHECK(*(uint32_t *)(cm + 0xf68) == 0);
    CHECK(old_s16_char45(0, cm) == nullptr);
    CHECK(u3x_s16_char(0, cm) == nullptr);

    *reinterpret_cast<uint32_t *>(cm + 0xf68) = 0x1234u;
    CHECK(old_s16_char45(0, cm) == nullptr);
    CHECK(u3x_s16_char(0, cm) == nullptr);

    h = reinterpret_cast<uint8_t *>(0x10);
    CHECK(old_s16_char45(0, cm) == nullptr);
    CHECK(u3x_s16_char(0, cm) == nullptr);

    h = ch;
    *reinterpret_cast<uint32_t *>(ch + 0x7c) = 0x1235u;
    CHECK(old_s16_char45(0, cm) == nullptr);
    CHECK(u3x_s16_char(0, cm) == nullptr);

    *reinterpret_cast<uint32_t *>(ch + 0x7c) = 0x1234u;
    CHECK(old_s16_char45(0, cm) == ch);
    CHECK(u3x_s16_char(0, cm) == ch);

    const uint32_t pages[] = {0x81bfb972u, 0xd7e00396u, 0x1f707657u, 0x211019f2u, 0x812c7d24u,
                              0xea656124u, 0x11201ffdu, 0xcbe59a09u, 0xe13ed766u, 0, 0x81bfb973u};
    for (uint32_t pg : pages) CHECK(old_s17_page45(pg) == u3x_s17_page(pg));

    volatile uint32_t *d = t27_s16;
    volatile uint32_t *vd = u3h_s16_diag();
    h = nullptr;
    *reinterpret_cast<uint8_t ***>(cm + 0xf60) = &h;
    for (uint32_t i = 0; i < 16; ++i) d[i] = 0;
    CHECK(old_s17_apply45(0, cm, 0) == 0);
    const uint32_t old_d4 = d[4];
    for (uint32_t i = 0; i < 16; ++i) d[i] = 0;
    CHECK(u3x_s17_apply(0, cm, 0) == 0);
    CHECK(d[4] == old_d4);

    h = ch;
    *reinterpret_cast<uint32_t *>(ch + 0x7c) = 0x1234u;
    *reinterpret_cast<uint32_t *>(ch + 0x30) = 0;
    for (uint32_t i = 0; i < 16; ++i) d[i] = 0;
    CHECK(old_s17_apply45(0, cm, 0) == 0);
    const uint32_t old_d11 = d[11];
    for (uint32_t i = 0; i < 16; ++i) d[i] = 0;
    CHECK(u3x_s17_apply(0, cm, 0) == 0);
    CHECK(d[11] == old_d11);

    uint32_t *eb = reinterpret_cast<uint32_t *>(t27_ext + 0x4050);
    uint32_t *ec = reinterpret_cast<uint32_t *>(t27_ext + 0x4450);
    h = nullptr;
    *reinterpret_cast<uint8_t ***>(cm + 0xf60) = &h;
    regs[4] = reinterpret_cast<uintptr_t>(cm);
    const uintptr_t base = reinterpret_cast<uintptr_t>(cm) - 0x17f9938u;

    ec[1] |= 1u;
    eb[2] &= ~0x02000000u;
    for (uint32_t i = 0; i < 16; ++i) d[i] = 0;
    CHECK(old_s16_refresh45(base, regs) == 0);
    const uint32_t old_r0 = d[0], old_r4 = d[4];
    for (uint32_t i = 0; i < 16; ++i) d[i] = 0;
    CHECK(u3x_s16_refresh(base, regs) == 0);
    CHECK(d[0] == old_r0 && d[4] == old_r4);

    eb[2] |= 0x02000000u;
    for (uint32_t i = 0; i < 16; ++i) d[i] = 0;
    CHECK(old_s16_refresh45(base, regs) == 0);
    const uint32_t old_g0 = d[0], old_g4 = d[4];
    for (uint32_t i = 0; i < 16; ++i) d[i] = 0;
    CHECK(u3x_s16_refresh(base, regs) == 0);
    CHECK(d[0] == old_g0 && d[4] == old_g4);

    eb[2] &= ~0x02000000u;
    ec[1] &= ~1u;
    for (uint32_t i = 0; i < 16; ++i) d[i] = 0;
    CHECK(old_s16_refresh45(base, regs) == 0);
    const uint32_t old_e0 = d[0], old_e4 = d[4];
    for (uint32_t i = 0; i < 16; ++i) d[i] = 0;
    CHECK(u3x_s16_refresh(base, regs) == 0);
    CHECK(d[0] == old_e0 && d[4] == old_e4);

    ec[1] |= 1u;
    regs[4] += 8;
    for (uint32_t i = 0; i < 16; ++i) d[i] = 0;
    CHECK(old_s16_refresh45(base, regs) == 0);
    const uint32_t old_m0 = d[0], old_m4 = d[4];
    for (uint32_t i = 0; i < 16; ++i) d[i] = 0;
    CHECK(u3x_s16_refresh(base, regs) == 0);
    CHECK(d[0] == old_m0 && d[4] == old_m4);

    regs[4] = reinterpret_cast<uintptr_t>(cm);
    const uintptr_t tick_base = 0x100000u;
    eb[2] |= 0x04000000u;
    for (uint32_t i = 0; i < 16; ++i) d[i] = 0;
    old_s16_tick45(tick_base);
    const uint32_t old_t0 = d[0], old_t8 = d[8], old_t9 = d[9];
    for (uint32_t i = 0; i < 16; ++i) d[i] = 0;
    u3x_s16_tick(tick_base);
    CHECK(d[0] == old_t0 && d[8] == old_t8 && d[9] == old_t9);

    eb[2] &= ~0x04000000u;
    for (uint32_t i = 0; i < 16; ++i) d[i] = 0;
    old_s16_tick45(tick_base);
    const uint32_t old_u0 = d[0], old_u8 = d[8], old_u9 = d[9];
    for (uint32_t i = 0; i < 16; ++i) d[i] = 0;
    u3x_s16_tick(tick_base);
    CHECK(d[0] == old_u0 && d[8] == old_u8 && d[9] == old_u9);
    (void)vd;
}
