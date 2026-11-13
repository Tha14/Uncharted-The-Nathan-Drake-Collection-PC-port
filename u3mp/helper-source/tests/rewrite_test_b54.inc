// Row 25: film HUD draw helpers and early draw gates.
static uint8_t t54_base[0x1d00000];
static void batch54() {
    CHECK(u3x_k26_fsel(0x3f800000u, 1, 2) == 1);
    CHECK(u3x_k26_fsel(0xbf800000u, 1, 2) == 2);
    CHECK(u3x_k26_fsel(0x7fc00000u, 1, 2) == 2);
    uint32_t c[4] = {F_ONE, F_ONE, F_ONE, F_ONE};
    CHECK(u3x_k26_col(c) == 0xffffffffu);
    uint32_t z[4] = {0, 0, 0, 0};
    CHECK(u3x_k26_col(z) == 0);
    uint32_t P[48];
    u3x_k26_tp(P);
    CHECK(P[5] == 0xbf800000u && P[7] == 0xbf800000u && P[0x2c / 4] == F_ONE && P[0x90 / 4] == F_ONE);
    CHECK(u3x_k26_f2d(F_ONE) == 0x3ff0000000000000ull);
    uint32_t q[4] = {};
    u3x_k26_c4(q, 1, 2, 3, 4);
    CHECK(q[0] == 1 && q[1] == 2 && q[2] == 3 && q[3] == 4);
    uint32_t v = 0x12345678;
    CHECK(u3x_k26_rd(&v) == v);

    uint8_t *bp = t54_base;
    for (uint32_t off : {0x11b8dd0u, 0x11b89b0u, 0xe42310u, 0xf1d9f0u, 0xf32ae0u, 0x640ac0u, 0x63d6f0u, 0x6a2700u, 0x139cab0u})
        *reinterpret_cast<uint32_t *>(bp + off) = off == 0x6a2700u ? 0x009cbf80u : off == 0x139cab0u ? 0x6aca25ffu : 0xe5894855u;
    CHECK(u3x_k26_callees(reinterpret_cast<uintptr_t>(bp)) == 1);

    *reinterpret_cast<uint8_t *>(bp + 0x1acab60u + 0x13) = 1;
    CHECK(u3x_k26_draw(reinterpret_cast<uintptr_t>(bp), nullptr, nullptr) == 0);
    CHECK(reinterpret_cast<uint32_t *>(t27_ext + 0x4050)[229] == 1);
}
