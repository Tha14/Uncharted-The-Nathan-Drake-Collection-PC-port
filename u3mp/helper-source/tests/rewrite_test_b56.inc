// Row 29: the kill-port switch gate.
static void batch56() {
    uint8_t base[0x100] = {};
    uint8_t state[0x4000] = {};
    uint8_t info[0x80] = {};
    uint32_t *kr = reinterpret_cast<uint32_t *>(u3h_ext() + 0x2bc0);
    for (int i = 0; i < 16; ++i) kr[i] = 0;
    *reinterpret_cast<uint32_t *>(state + 0x3d78) = 0x200u;
    u3x_kr_port(reinterpret_cast<uintptr_t>(base), info, state);
    CHECK(kr[0] == 1 && kr[15] == 1);
}
