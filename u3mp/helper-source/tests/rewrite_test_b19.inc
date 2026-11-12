// v7.132 part K: the old helper code (v7.131) for the lobby rows, icon lookup, hero looks and rank word, and the checks.
typedef struct { uint32_t text, icon, action; } OLobbyRow;
static const OLobbyRow o_lan[6] = {{0x66d63005u, 0x7e2c2757u, 0x7d593924u}, {0xebd77105u, 0x23e9be55u, 0x52f3d9f6u}, {0xa2aa79eau, 0xb675afdbu, 0xc4acc827u},
    {0xcb379354u, 0x12d18838u, 0xb85cb12au}, {0xfb08c743u, 0x2c68ff83u, 0xe7d7dbbbu}, {0xd7d4ccadu, 0x88c52b24u, 0xcd127c29u}};
static const OLobbyRow o_coop[4] = {{0x66d63005u, 0x7e2c2757u, 0x7d593924u}, {0xa2aa79eau, 0xb675afdbu, 0xc4acc827u}, {0xfb08c743u, 0x2c68ff83u, 0xe7d7dbbbu},
    {0xd7d4ccadu, 0x88c52b24u, 0xcd127c29u}};
static const OLobbyRow o_online[6] = {{0xbd1d2c53u, 0xb530fe7bu, 0x649e809au}, {0x66d63005u, 0x7e2c2757u, 0x7d593924u}, {0xa2aa79eau, 0xb675afdbu, 0xc4acc827u},
    {0xe62d7aebu, 0x2e9c6d07u, 0x8891e664u}, {0xfb08c743u, 0x2c68ff83u, 0xe7d7dbbbu}, {0xd7d4ccadu, 0x88c52b24u, 0xcd127c29u}};
static int32_t o_icon(uint8_t *t, uint32_t id) { if (!t) return -1; uint32_t n = *(uint16_t *)(t + 2); uint8_t *e = *(uint8_t **)(t + 8); for (uint32_t i = 0; e && i < n; i++) if (*(uint32_t *)(e + i * 40 + 8) == id) return (int32_t)i; return -1; }
static int o_fix(uint8_t *q) { if (q[0] || q[1] || q[2] || !q[3] || q[3] >= 0x40) return 0; q[0] = q[3]; q[3] = 0; return 1; }

static void batch19() {
    for (int online = 0; online < 2; online++) for (int coop = 0; coop < 2; coop++) {
        const OLobbyRow *want = online ? o_online : coop ? o_coop : o_lan; const uint32_t wn = online ? 6 : coop ? 4 : 6;
        uint32_t out[24] = {}; const uint32_t n = u3x_lobby_rows(online, coop, out);
        CHECK(n == wn); for (uint32_t k = 0; k < wn && k < n; k++) { CHECK(out[3 * k] == want[k].text); CHECK(out[3 * k + 1] == want[k].icon); CHECK(out[3 * k + 2] == want[k].action); }
    }
    for (int t = 0; t < 3000; t++) {
        static uint8_t e[40 * 70]; for (int k = 0; k < 70; k++) { uint32_t id = rnd() % 90; std::memcpy(e + k * 40 + 8, &id, 4); }
        alignas(8) uint8_t tab[16] = {}; uint16_t n = (uint16_t)(rnd() % 71); std::memcpy(tab + 2, &n, 2); uint8_t *ep = t % 11 ? e : nullptr; std::memcpy(tab + 8, &ep, 8);
        uint8_t *tp = t % 13 ? tab : nullptr;
        for (uint32_t id = 0; id < 92; id++) CHECK(o_icon(tp, id) == u3x_icon_index(tp, id));
    }
    for (uint32_t w = 0; w < 0x10000; w++) for (uint32_t hi : {0u, 1u, 0x3fu, 0x40u, 0xffu}) {
        uint8_t a[4] = {(uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 12 & 1), (uint8_t)hi}, b[4]; std::memcpy(b, a, 4);
        CHECK(o_fix(a) == u3x_fix_hero_look(b)); CHECK(std::memcmp(a, b, 4) == 0);
    }
    for (int t = 0; t < 100000; t++) {
        const int32_t lvl = (int32_t)(rnd() << 8 ^ rnd()); const uint32_t leg = rnd() << 8 ^ rnd(), bits = rnd() << 8 ^ rnd();
        CHECK((bits | ((uint32_t)lvl & 0xffffu) | ((leg & 0x3fffu) << 16)) == u3x_rank_word(lvl, leg, bits));
    }
}
