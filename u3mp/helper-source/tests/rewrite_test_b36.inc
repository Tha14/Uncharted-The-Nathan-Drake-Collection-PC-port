// third split: the old taunt package list helpers (taunt_pack, taunt_slot) against u3x_pkg_taunt_name / u3x_pkg_taunt_slot.
static const char o36_names[] =
    "carlton\0fffuuuuu-\0arm-wave\0mj-1\0throat-slit\0mj-2\0haka\0heisman\0air-guitar\0facepalm\0take-a-bow\0"
    "high-kick\0hillbilly-shuffle\0mic-drop\0school-fight\0come-at-me-bro\0backflip-squat\0jump-freeze\0"
    "river-dance\0cossack-dance\0six-flags\0carlton-2\0bolt\0freddie\0dougie\0cabbage-patch\0cheerleader\0gangnam\0"
    "top-model\0";
static uint32_t o36_taunt[16];
static const char *o36_pack(uint32_t sid, char out[64]) {
    if (!sid || sid == 0x0e69839du) return 0;
    for (int i = 12; i < 16; i++) if (o36_taunt[i] == sid) return 0;
    static const char head[] = "multi-celebrate-";
    for (const char *t = o36_names; *t;) { int n = 0; for (; head[n]; n++) out[n] = head[n]; int k = 0; for (; t[k] && n < 63; k++) out[n++] = t[k]; out[n] = 0;
        if (u3h_crc_sid(out) == sid) return out; t += k + 1; }
    o36_taunt[9]++; o36_taunt[10] = sid; return 0; }
static int o36_slot(uint8_t *m, uint32_t *slot, const char *n) {
    uint32_t sid = n ? u3h_crc_sid(n) : 0; int own = -1;
    if (slot[0] && slot[0] <= 50 && *(uint32_t *)(m + 4 + (slot[0] - 1) * 4) == slot[1]) own = (int)slot[0] - 1;
    if (own >= 0 && slot[1] == sid) return 0;
    int i = 50, free = -1;
    if (sid) for (i = 0; i < 50; i++) { uint32_t v = *(uint32_t *)(m + 4 + i * 4); if (v == sid && i != own) break; if (!v && free < 0) free = i; }
    int changed = 0;
    if (own >= 0) { *(uint32_t *)(m + 4 + own * 4) = 0; *(char *)(m + 0xcc + own * 0x40) = 0; changed = 1; if (free < 0 || own < free) free = own; }
    slot[0] = slot[1] = 0;
    if (!sid || i < 50 || free < 0) return changed;
    char *dst = (char *)(m + 0xcc + free * 0x40); int j = 0; for (; j < 63 && n[j]; j++) dst[j] = n[j]; dst[j] = 0;
    *(uint32_t *)(m + 4 + free * 4) = sid; slot[0] = (uint32_t)free + 1; slot[1] = sid; o36_taunt[8]++; return 1; }
static void batch36() {
    static uint8_t ma[0xd20], mb[0xd20];
    static const char *cands[6] = {"multi-celebrate-haka", "multi-celebrate-gangnam", "multi-celebrate-bolt", "kit-a", "kit-b", nullptr};
    uint32_t *ts = (uint32_t *)(u3h_ext() + 0xb80);
    for (int t = 0; t < 20000; t++) {
        for (int k = 0; k < 16; k++) o36_taunt[k] = ts[k] = 0;
        for (size_t k = 0; k < sizeof(ma); k++) ma[k] = mb[k] = 0;
        for (int i = 0; i < 50; i++) if (rnd() % 3 == 0) { const char *n = cands[rnd() % 5]; uint32_t sid = rnd() % 4 ? u3h_crc_sid(n) : rnd();
            *(uint32_t *)(ma + 4 + i * 4) = *(uint32_t *)(mb + 4 + i * 4) = sid; }
        uint32_t sa[2], sb[2];
        sa[0] = sb[0] = rnd() % 53; sa[1] = sb[1] = rnd() % 2 ? *(uint32_t *)(ma + 4 + ((sa[0] + 49) % 50) * 4) : rnd();
        const char *n = cands[rnd() % 6];
        CHECK(o36_slot(ma, sa, n) == u3x_pkg_taunt_slot(mb, sb, n));
        CHECK(sa[0] == sb[0] && sa[1] == sb[1] && memcmp(ma, mb, sizeof(ma)) == 0 && o36_taunt[8] == ts[8]);
        char oa[64], ob[64]; const uint32_t sid = rnd() % 3 ? u3h_crc_sid(cands[rnd() % 5]) : rnd();
        if (rnd() % 4 == 0) o36_taunt[13] = ts[13] = sid;
        const char *ra = o36_pack(sid, oa), *rb = u3x_pkg_taunt_name(sid, ob);
        CHECK((ra == nullptr) == (rb == nullptr)); if (ra && rb) CHECK(strcmp(ra, rb) == 0);
        CHECK(o36_taunt[9] == ts[9] && o36_taunt[10] == ts[10]);
    }
}
