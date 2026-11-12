// v7.118: the session hello. The old code (v7.117) with its start value passed in, against the new one, on random values.
static uint32_t t6_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t old_sm_mix(uint32_t *st, const uint8_t *p, int n) {
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < n; i += 4) {
        d ^= 0x5b3aa654u; c += a; c += d; c = (c << 7) | (c >> 25); b += d; b += c; c ^= 0x75970a4du; b = (b << 11) | (b >> 21); a += c; a += b; a = (a << 17) | (a >> 15);
        d += (a & b) | (c & ~b); a = ~a; d ^= t6_le32(p + i); }
    st[0] = a; st[1] = b; st[2] = c; st[3] = d; return a + b + c + d;
}
static uint32_t old_sm_hash(const uint8_t *key, uint32_t chal, const uint8_t *npid) {
    uint32_t st[4]; uint8_t t[16];
    for (int i = 0; i < 4; i++) st[i] = t6_le32(key + 4 * i);
    t[0] = (uint8_t)chal; t[1] = (uint8_t)(chal >> 8); t[2] = (uint8_t)(chal >> 16); t[3] = (uint8_t)(chal >> 24); old_sm_mix(st, t, 4);
    for (int i = 0; i < 4; i++) { t[4 * i] = (uint8_t)st[i]; t[4 * i + 1] = (uint8_t)(st[i] >> 8); t[4 * i + 2] = (uint8_t)(st[i] >> 16); t[4 * i + 3] = (uint8_t)(st[i] >> 24); }
    old_sm_mix(st, t, 16);
    return old_sm_mix(st, npid, 36);
}
static void batch6() {
    for (int t = 0; t < 200000; t++) {
        uint8_t key[16], id[36]; for (auto &b : key) b = (uint8_t)rnd(); for (auto &b : id) b = (uint8_t)rnd();
        const uint32_t chal = rnd() << 8 ^ rnd();
        CHECK(old_sm_hash(key, chal, id) == u3x_session_answer(key, chal, id));
    }
    // key file text: good (both cases, with line ends), short, bad digit, trailing junk
    uint8_t k[16]; const uint8_t want[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
    CHECK(u3x_session_key_parse("0123456789abcdefFEDCBA9876543210", 32, k) == 1 && std::memcmp(k, want, 16) == 0);
    std::memset(k, 0, 16); CHECK(u3x_session_key_parse("0123456789abcdefFEDCBA9876543210\r\n", 34, k) == 1 && std::memcmp(k, want, 16) == 0);
    std::memset(k, 7, 16); CHECK(u3x_session_key_parse("0123456789abcdefFEDCBA987654321", 31, k) == 0 && k[0] == 7);
    CHECK(u3x_session_key_parse("0123456789abcdefFEDCBA987654321g", 32, k) == 0 && k[0] == 7);
    CHECK(u3x_session_key_parse("0123456789abcdefFEDCBA98765432100", 33, k) == 0 && k[0] == 7);
    CHECK(u3x_session_key_parse(nullptr, 0, k) == 0); CHECK(u3x_session_key_parse("", -1, k) == 0);
}
