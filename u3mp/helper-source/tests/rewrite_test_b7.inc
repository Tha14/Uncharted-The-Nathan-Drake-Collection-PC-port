// v7.119 batch: the old helper code (v7.118) for each rewritten section, and the old-versus-new checks.
static int old_k49_st(int st) { return (st == 0 || st == 1) ? 0 : 8; }
static int old_booster_already(uint32_t slot, uint32_t user, uint32_t id) { uint32_t eq = slot; if (!eq) eq = user; return eq == id; }
static int old_mod_id(uint8_t *mods, uint32_t n, int32_t row, uint32_t *id) {
    if (!mods || row < 0 || (uint32_t)row >= n) return 0; *id = *(uint32_t *)(mods + (size_t)row * 0x14); return 1;
}
static uint32_t old_retag(uint8_t *page) {
    uint32_t changed = 0;
    uint8_t *col = *(uint8_t **)(page + 0x48); if (!col) return 0; uint32_t n = *(uint32_t *)col; uint8_t *items = *(uint8_t **)(col + 8);
    if (!items || n > 64) return 0;
    for (uint32_t j = 0; j < n; j++) { uint8_t *it = items + j * 0x68;
        if (*(uint32_t *)(it + 0x30) == 21 && *(uint32_t *)(it + 0x34) == 0xa2 && *(uint32_t *)(it + 0x40) == 0) {
            *(uint32_t *)(it + 0x34) = 0xcf; *(uint32_t *)(it + 0x40) = 0x4c414ea2u; changed++; } }
    return changed;
}
static int old_comp_answer(uint32_t q, uint32_t id, int unlocked, int32_t *out) {
    int n = 0;
    if (q == 0xa1443de7u) { *out = !unlocked; n++; }
    if (q == 0x1720a1edu) { *out = (int32_t)id; n++; }
    return n;
}
static uint8_t *old_find_group(uint8_t *slots, uint32_t sid) {
    if (!slots) return 0;
    for (uint32_t i = 0; i < 0x15e; i++) { uint8_t *sl = slots + (size_t)i * 0x2c0;
        if (!*(uint32_t *)(sl + 0x20)) continue; uint8_t *pkg = *(uint8_t **)(sl + 0x18); if (!pkg) continue;
        int32_t n = *(int32_t *)(pkg + 0x2c); uint8_t *tab = *(uint8_t **)(pkg + 0x30); if (n <= 0 || !tab) continue;
        uint8_t *hit = 0; uint32_t h = sid % (uint32_t)n;
        for (uint32_t k = h; k < (uint32_t)n; k++) { uint32_t e = *(uint32_t *)(tab + (size_t)k * 16); if (!e) goto next; if (e == sid) { hit = tab + (size_t)k * 16; break; } }
        if (!hit) for (uint32_t k = 0; k < (uint32_t)n; k++) { uint32_t e = *(uint32_t *)(tab + (size_t)k * 16); if (!e) goto next; if (e == sid) { hit = tab + (size_t)k * 16; break; } }
        if (hit) { uint8_t *item = *(uint8_t **)(hit + 8); if (item) return item; }
    next:; }
    return 0;
}
static uint32_t fake_name_id(int, const char *name, int) { uint32_t h = 7; for (; *name; name++) h = h * 131 + (uint8_t)*name; return h % 97 + 1; }
static int old_is_film(uint32_t id) {
    switch (id) {
    case 0x38231b34u: case 0x3cf5deedu: case 0x43348860u: case 0x516bfbb8u: case 0x682c6868u: case 0x806c9824u: case 0x887c7d6eu:
    case 0x953b8f2au: case 0x9eb9632du: case 0xa65c61bau: case 0xbd8d7974u: case 0xc1f65aebu: case 0xe8184ce8u: case 0xff502160u: break;
    default: return 0; }
    return 1;
}
static uint8_t *old_live(uint8_t **h, uint32_t hid) { return (h && hid && *h && *(uint32_t *)(*h + 0x7c) == hid) ? *h : 0; }
static uint32_t old_ago(uint32_t sec, uint32_t *idp) {
    uint32_t id, v;
    if (sec < 0x78) { v = (int32_t)sec < 2 ? 2 : sec; id = 0x72cd40d5u; }
    else { v = sec / 0x3c; id = 0x6498bce2u; if (v > 0x77) { v /= 0x3c; id = 0x624258c1u; if (v > 0x2f) { v /= 0x18; id = 0x60f0bee3u; if (v >= 0xe) { id = 0xd250139cu; v /= 7; } } } }
    *idp = id; return v;
}
static int32_t old_mq_find(uint8_t *q, uint32_t *count, uint32_t type, uint32_t *idx, int remove) {
    uint32_t n = *count;
    for (uint32_t i = 0; i < n; i++) { if (*(uint32_t *)(q + (size_t)i * 0x40 + 0x28) != type) continue;
        if (!remove) { *idx = i; return 0; }
        for (uint32_t k = i; k + 1 < n; k++) std::memcpy(q + (size_t)k * 0x40, q + (size_t)(k + 1) * 0x40, 0x40);
        *count = n - 1; return 0; }
    return -1;
}

static void batch7() {
    // click sounds, the paid slot, mod ids
    for (int st = -2; st < 12; st++) CHECK(old_k49_st(st) == u3x_click_sound(st));
    for (uint32_t slot : {0u, 5u, 9u}) for (uint32_t user : {0u, 5u, 9u}) for (uint32_t id : {0u, 5u, 9u}) CHECK(old_booster_already(slot, user, id) == u3x_booster_already(slot, user, id));
    { alignas(4) uint8_t mods[0x14 * 5]; for (uint32_t k = 0; k < sizeof mods; k++) mods[k] = (uint8_t)(k * 7 + 1);
      for (uint32_t n = 0; n < 6; n++) for (int32_t row = -2; row < 7; row++) for (int have = 0; have < 2; have++) {
          uint32_t a = 0x111, b = 0x111; CHECK(old_mod_id(have ? mods : nullptr, n, row, &a) == u3x_mod_id(have ? mods : nullptr, n, row, &b)); CHECK(a == b); } }
    // retag: random columns, counts up to and past 64, missing column or items
    for (int t = 0; t < 400; t++) {
        std::vector<uint8_t> items(0x68 * 70), items2;
        for (int k = 0; k < 70; k++) { uint32_t type = rnd() % 3 ? 21 : 20, en = rnd() % 3 ? 0xa2 : 0xa3, tag = rnd() % 4 ? 0 : 0x4c414e00u;
            std::memcpy(items.data() + k * 0x68 + 0x30, &type, 4); std::memcpy(items.data() + k * 0x68 + 0x34, &en, 4); std::memcpy(items.data() + k * 0x68 + 0x40, &tag, 4); }
        items2 = items;
        uint32_t n = t % 9 == 0 ? 65 + rnd() % 5 : rnd() % 65;
        alignas(8) uint8_t col1[16] = {}, col2[16] = {}, page1[0x50] = {}, page2[0x50] = {};
        uint8_t *i1 = t % 13 == 0 ? nullptr : items.data(), *i2 = t % 13 == 0 ? nullptr : items2.data();
        std::memcpy(col1, &n, 4); std::memcpy(col1 + 8, &i1, 8); std::memcpy(col2, &n, 4); std::memcpy(col2 + 8, &i2, 8);
        uint8_t *c1 = t % 17 == 0 ? nullptr : col1, *c2 = t % 17 == 0 ? nullptr : col2; std::memcpy(page1 + 0x48, &c1, 8); std::memcpy(page2 + 0x48, &c2, 8);
        CHECK(old_retag(page1) == u3x_retag_items(page2, 0xa2, 0x4c414ea2u)); CHECK(items == items2);
    }
    // weapon statistics answers
    for (uint32_t q : {0xa1443de7u, 0x1720a1edu, 0u, 0xa1443de8u}) for (int unlocked : {0, 1}) {
        int32_t a = 0x777, b = 0x777; CHECK(old_comp_answer(q, 0x4242, unlocked, &a) == u3x_comp_weapon_answer(q, 0x4242, !unlocked, &b)); CHECK(a == b);
    }
    // animation groups: random package tables (full, sparse, with empty entries, missing parts), every id
    static uint8_t slots[0x15e * 0x2c0];
    for (int t = 0; t < 200; t++) {
        std::memset(slots, 0, sizeof slots);
        static uint8_t pkgs[6][0x40]; static uint8_t tabs[6][16 * 24]; static uint8_t groups[40];
        std::memset(pkgs, 0, sizeof pkgs); std::memset(tabs, 0, sizeof tabs);
        for (int p = 0; p < 6; p++) {
            const uint32_t slot = rnd() % 0x15e; uint8_t *sl = slots + (size_t)slot * 0x2c0;
            uint32_t used = rnd() % 6 ? 1 : 0; uint8_t *pp = rnd() % 7 ? pkgs[p] : nullptr; std::memcpy(sl + 0x20, &used, 4); std::memcpy(sl + 0x18, &pp, 8);
            int32_t n = (int32_t)(rnd() % 26) - 1; if (n > 24) n = 24; uint8_t *tp = rnd() % 9 ? tabs[p] : nullptr;
            std::memcpy(pkgs[p] + 0x2c, &n, 4); std::memcpy(pkgs[p] + 0x30, &tp, 8);
            const uint32_t fill = rnd() % 3;   // 0 sparse, 1 half, 2 full
            for (int k = 0; k < 24; k++) { uint32_t id = (fill == 2 || rnd() % (fill ? 2 : 4) == 0) ? rnd() % 40 + 1 : 0; uint8_t *g = rnd() % 8 ? groups + rnd() % 40 : nullptr;
                std::memcpy(tabs[p] + k * 16, &id, 4); std::memcpy(tabs[p] + k * 16 + 8, &g, 8); }
        }
        for (uint32_t id = 0; id < 44; id++) CHECK(old_find_group(slots, id) == u3x_find_art_group(slots, id));
        static const char names[8][24] = {"proto", "anim-proto-multiplayer", "anim-soldier-mp", "anim-proto-shield", "", "", "", ""};
        uint8_t *a[8], *b[8]; uint32_t got = 0;
        for (int i = 0; i < 8; i++) { a[i] = names[i][0] ? old_find_group(slots, fake_name_id(0, names[i], 1)) : nullptr; if (a[i]) got |= 1u << i; b[i] = groups; }
        CHECK(got == u3x_player_net_anims(b, slots, fake_name_id, &names[0][0])); CHECK(std::memcmp(a, b, sizeof a) == 0);
    }
    CHECK(u3x_find_art_group(nullptr, 5) == nullptr);
    { uint8_t *b[8]; for (auto &p : b) p = slots; CHECK(u3x_player_net_anims(b, nullptr, fake_name_id, "x") == 0); for (auto *p : b) CHECK(p == nullptr); }
    // film actions: the 14 ids, their neighbours, random ids
    for (uint32_t id : {0x38231b34u, 0x3cf5deedu, 0x43348860u, 0x516bfbb8u, 0x682c6868u, 0x806c9824u, 0x887c7d6eu, 0x953b8f2au, 0x9eb9632du, 0xa65c61bau, 0xbd8d7974u, 0xc1f65aebu, 0xe8184ce8u, 0xff502160u})
        for (uint32_t d : {0u, 1u, 0xffffffffu}) CHECK(old_is_film(id + d) == u3x_is_film_action(id + d));
    for (int t = 0; t < 100000; t++) { const uint32_t id = rnd() << 8 ^ rnd(); CHECK(old_is_film(id) == u3x_is_film_action(id)); }
    // live object
    { alignas(8) static uint8_t obj[0x80]; uint32_t oid = 0x321; std::memcpy(obj + 0x7c, &oid, 4); uint8_t *full = obj, *empty = nullptr;
      for (uint8_t **h : {&full, &empty, (uint8_t **)nullptr}) for (uint32_t id : {0u, 0x321u, 0x322u}) CHECK(old_live(h, id) == u3x_live_object(h, id)); }
    // time ago: every second of the first 3 days, then steps up to the largest value, and the edges
    for (uint32_t sec = 0; sec < 3 * 86400; sec++) { uint32_t a = 0, b = 0; CHECK(old_ago(sec, &a) == u3x_time_ago(sec, &b)); CHECK(a == b); }
    for (uint64_t sec = 3 * 86400; sec <= 0xffffffffull; sec += 977) { uint32_t a = 0, b = 0; CHECK(old_ago((uint32_t)sec, &a) == u3x_time_ago((uint32_t)sec, &b)); CHECK(a == b); }
    for (uint32_t sec : {0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu, 14u * 86400u - 1, 14u * 86400u, 48u * 3600u - 1, 48u * 3600u, 7199u, 7200u})
        { uint32_t a = 0, b = 0; CHECK(old_ago(sec, &a) == u3x_time_ago(sec, &b)); CHECK(a == b); }
    // message queue: find and remove on random queues
    for (int t = 0; t < 20000; t++) {
        alignas(8) uint8_t q1[0x40 * 9], q2[0x40 * 9]; for (uint32_t k = 0; k < sizeof q1; k++) q1[k] = (uint8_t)rnd();
        uint32_t n = rnd() % 9; for (uint32_t i = 0; i < 8; i++) { uint32_t type = 0x10b + rnd() % 4; std::memcpy(q1 + i * 0x40 + 0x28, &type, 4); }
        std::memcpy(q2, q1, sizeof q1);
        uint32_t n1 = n, n2 = n, i1 = 0x99, i2 = 0x99; const uint32_t type = 0x10b + rnd() % 5; const int remove = rnd() % 2;
        CHECK(old_mq_find(q1, &n1, type, &i1, remove) == u3x_queue_find(q2, &n2, type, &i2, remove));
        CHECK(n1 == n2); CHECK(i1 == i2); CHECK(std::memcmp(q1, q2, (size_t)n1 * 0x40) == 0); CHECK(std::memcmp(q1 + 8 * 0x40, q2 + 8 * 0x40, 0x40) == 0);
    }
}
