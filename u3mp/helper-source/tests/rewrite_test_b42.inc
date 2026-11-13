// row 1 (c008): has_eff119, char_np119 and player_fill119 against the old C logic.
static uint8_t t42_by_id[0x800];
static uint8_t *t42_by_id_value;

extern "C" {
uint8_t *u3h_np_by_id(uintptr_t, uint32_t id) { return id == 0x1234u ? t42_by_id_value : nullptr; }
static bool rd_np_code_memory = false;   // differential tests: the code is the word at np+4
uint32_t u3h_np_code(uint8_t *np) { if (rd_np_code_memory) { rd_log("np_code"); return np ? *reinterpret_cast<uint32_t *>(np + 4) : 0; } return np ? 0xabcdu : 0; }
void u3h_weapons_fill(uintptr_t, uint8_t *msg, uint8_t *) { rd_log("weapons_fill", msg ? msg[0x1dc] : 0); }
uint32_t u3h_anim_pack_full(uintptr_t, uint8_t *, uint8_t *bs, uint8_t *, uint8_t **, int32_t a, int32_t b) {
    rd_log("pack", *(uint32_t *)(bs + 0xc), (uint64_t)a << 8 | (uint64_t)b);
    if (rd_pack_err) *u3h_repl_error() = 1;
    return rd_pack_bits;
}
static int32_t rd_net_now = 0;
int32_t u3h_net_now(uintptr_t) { return rd_net_now; }
}

static int old_has_eff(uint8_t *c, uint32_t sid) {
    uint8_t **p = *(uint8_t ***)(c + 0x80);
    if (!p || !*p) return 0;
    return *(uint32_t *)(*(uint8_t **)(*p + 0x10) + 0x18) == sid;
}
static uint8_t *old_char_np(uintptr_t base, uint8_t *c) {
    if (!c || !*(uint8_t *)(base + 0x17d5c61u)) return 0;
    uint32_t ty = *(uint32_t *)(c + 0x30);
    if (ty == 0xa5a63e9bu) {
        int32_t s = *(int32_t *)(c + 0x1c68);
        return (s >= 0 && s < 2) ? *(uint8_t **)(base + 0x17cb060u + 0x370 + 8 * (size_t)s) : 0;
    }
    if (ty == 0xf2c86c22u && *(uint32_t *)(c + 0x3f8)) return u3h_np_by_id(base, *(uint32_t *)(c + 0x3f8));
    return 0;
}
static uint8_t t42_empty[1] = {0};
static uint8_t *t42_ret_empty(uint8_t *) { return t42_empty; }
static uint8_t *t42_ret_zero(uint8_t *) { return nullptr; }
static uint8_t *t42_ret_one(uint8_t *) { return (uint8_t *)(uintptr_t)1; }
static uint8_t *t42_ret_5678(uint8_t *) { return (uint8_t *)(uintptr_t)0x5678; }
static uint8_t *t42_ret_1234(uint8_t *) { return (uint8_t *)(uintptr_t)0x1234; }
static uint8_t t42_t[0xa00];
static uint8_t *t42_ret_t(uint8_t *) { return t42_t; }

// the game's on-screen value (0x689d20, SysV, a float in xmm0): 0.0, so the player is not "on"
__attribute__((sysv_abi)) static float t42_booster(uint64_t, uint64_t, uint64_t, uint64_t) { return 0.0f; }

static void batch42() {
    // an executable image from the game function at 0x689d20 to the data at 0x17c0000..0x1bc0000
    static uint8_t *image = static_cast<uint8_t *>(test_exec_alloc(0x1bc0000));
    uint8_t *const mem = image + 0x17c0000;
    const uintptr_t base = reinterpret_cast<uintptr_t>(image);
    std::memset(mem, 0, 0x400000);
    {
        uint8_t *p = image + 0x689d20;
        const uint64_t a = reinterpret_cast<uint64_t>(&t42_booster);
        p[0] = 0x48;
        p[1] = 0xb8;
        std::memcpy(p + 2, &a, 8);
        p[10] = 0xff;
        p[11] = 0xe0;
    }

    alignas(8) uint8_t hc[0x100] = {}, hr[0x20] = {};
    uint8_t *q = hr;
    uint8_t **p = &q;
    *(uint8_t ***)(hc + 0x80) = p;
    *(uint8_t **)(hr + 0x10) = hr;
    const uint32_t sid = 0x1234u;
    std::memcpy(hr + 0x18, &sid, 4);
    for (uint32_t v : {sid, 0u, 0xffffffffu}) {
        std::memcpy(hr + 0x18, &v, 4);
        CHECK(old_has_eff(hc, v) == u3x_has_eff(hc, v));
    }
    *(uint8_t ***)(hc + 0x80) = nullptr;
    CHECK(old_has_eff(hc, sid) == u3x_has_eff(hc, sid));
    *(uint8_t ***)(hc + 0x80) = p;
    q = nullptr;
    CHECK(old_has_eff(hc, sid) == u3x_has_eff(hc, sid));

    *(uint8_t *)(base + 0x17d5c61u) = 1;
    uint8_t np0[0x800] = {}, np1[0x800] = {};
    *(uint8_t **)(base + 0x17cb060u + 0x370) = np0;
    *(uint8_t **)(base + 0x17cb060u + 0x378) = np1;
    t42_by_id_value = np1;
    alignas(8) uint8_t cc[0x2000] = {};
    const uint32_t player_type = 0xa5a63e9bu, net_type = 0xf2c86c22u;
    for (int32_t s : {-1, 0, 1, 2}) {
        std::memcpy(cc + 0x30, &player_type, 4);
        std::memcpy(cc + 0x1c68, &s, 4);
        CHECK(old_char_np(base, cc) == u3x_char_np(base, cc));
    }
    std::memcpy(cc + 0x30, &net_type, 4);
    for (uint32_t id : {0u, 0x1234u, 0x9999u}) {
        std::memcpy(cc + 0x3f8, &id, 4);
        CHECK(old_char_np(base, cc) == u3x_char_np(base, cc));
    }
    *(uint8_t *)(base + 0x17d5c61u) = 0;
    std::memcpy(cc + 0x30, &player_type, 4);
    CHECK(old_char_np(base, cc) == u3x_char_np(base, cc));

    alignas(16) uint8_t msg[0x240], pl[0x2100] = {}, copy[0x1a0];
    std::memset(msg, 0xcd, sizeof msg);
    *(uint32_t *)(msg + 0x144) = 0xdead;
    *(uint32_t *)(msg + 0x148) = 0;
    *(uint32_t *)(msg + 0x1f0) = 0xbeef;
    for (int i = 0; i < 0x60; ++i) copy[0x140 + i] = (uint8_t)(i + 1);
    *(uint8_t **)(pl + 0x538) = copy;
    *(uint32_t *)(pl + 0x3c0) = 0x11111111u;
    *(uint32_t *)(pl + 0x3a8) = 0x22222222u;
    for (int i = 0; i < 4; ++i) *(uint32_t *)(pl + 0x3b0 + 4 * i) = 0x30000000u + (uint32_t)i;
    *(uint32_t *)(pl + 0x39c) = 0x33333333u;
    *(int32_t *)(pl + 0x1c68) = -1;
    uint8_t c550[0x20] = {}, c548[0x20] = {}, c310[0x20] = {}, c668[0x20] = {}, c520[0x20] = {};
    uintptr_t vt550[0x20] = {}, vt548[0x20] = {}, vt310[0x20] = {}, vt668[0x20] = {}, vt520[0x20] = {};
    *(uintptr_t **)c550 = vt550;
    *(uintptr_t **)c548 = vt548;
    *(uintptr_t **)c310 = vt310;
    *(uintptr_t **)c668 = vt668;
    *(uintptr_t **)c520 = vt520;
    vt550[0x80 / 8] = (uintptr_t)&t42_ret_one;
    vt548[0x98 / 8] = (uintptr_t)&t42_ret_5678;
    vt310[0x70 / 8] = (uintptr_t)&t42_ret_1234;
    vt668[0x60 / 8] = (uintptr_t)&t42_ret_t;
    vt520[0x78 / 8] = (uintptr_t)&t42_ret_empty;
    *(uint8_t **)(pl + 0x550) = c550;
    *(uint8_t **)(pl + 0x548) = c548;
    *(uint8_t **)(pl + 0x310) = c310;
    *(uint8_t **)(pl + 0x668) = c668;
    *(uint8_t **)(pl + 0x520) = c520;
    *(uint32_t *)(t42_t + 0x98) = 0xaaaau;
    *(uint32_t *)(t42_t + 0x9ac) = 0xbbbbu;
    *(uint32_t *)(t42_t + 0x9b0) = 0xccccu;

    CHECK(u3x_player_fill(base, nullptr, msg, pl) == 0);
    CHECK(*(uint32_t *)(msg + 0x148) == 3u);
    CHECK(*(uint32_t *)(msg + 0x14c) == 0);
    CHECK(*(uint32_t *)(msg + 0x150) == 0x11111111u);
    CHECK(*(uint32_t *)(msg + 0x14) == 0);
    CHECK(*(uint32_t *)(msg + 0x154) == 0x22222222u);
    for (int i = 0; i < 4; ++i) CHECK(*(uint32_t *)(msg + 0x160 + 4 * i) == 0x30000000u + (uint32_t)i);
    CHECK(*(uint32_t *)(msg + 0x158) == 0x5678u);
    CHECK(std::memcmp(msg + 0x170, copy + 0x140, 0x60) == 0);
    CHECK(msg[0x1de] == 0);
    CHECK(*(uint16_t *)(msg + 0x1dc) == 0x1234u);
    CHECK(msg[0x1e0] == 1);
    CHECK(*(uint32_t *)(msg + 0x1e4) == 0xaaaau);
    CHECK(*(uint32_t *)(msg + 0x1e8) == 0xbbbbu);
    CHECK(*(uint32_t *)(msg + 0x1ec) == 0xccccu);
    CHECK(*(uint32_t *)(msg + 0x144) == 0);
    CHECK(*(uint32_t *)(msg + 0x1d0) == 0x33333333u);
    CHECK(*(uint32_t *)(msg + 0x1d4) == 0);
    CHECK(*(uint32_t *)(msg + 0x1d8) == 0);
    CHECK(*(uint32_t *)(msg + 0x210) == 0xcdcdcdcdu);
    CHECK(*(uint32_t *)(msg + 0x1f0) == 0xbeefu);

    uint8_t t198[0x60] = {}, c598[0x20] = {};
    uintptr_t vt598[0x20] = {};
    *(uintptr_t **)c598 = vt598;
    vt598[0xa8 / 8] = (uintptr_t)&t42_ret_zero;
    *(uint8_t **)(pl + 0x198) = t198;
    *(uint8_t **)(pl + 0x598) = c598;
    *(uint32_t *)(t198 + 0x50) = 3;
    *(uint64_t *)(base + 0x1b24ee0u + 0x3450) = 1;
    pl[0x1338] = 1;
    *(uint32_t *)(msg + 0x148) = 0;
    CHECK(u3x_player_fill(base, nullptr, msg, pl) == 0);
    CHECK(*(uint32_t *)(msg + 0x148) == 0x4fu);
}
