// v7.133 part L: the old helper code (v7.132) for the taunt names and slots and the rank image item, and the checks.
static const char *o_row_name(const char *p, uint32_t row) { for (uint32_t i = 0; *p; i++) { if (i == row) return p; while (*p) p++; p++; } return 0; }
struct OSlot { uint8_t *tc; uint32_t pid, prev; };
static uint32_t o_prev_get(OSlot *s, uint8_t *tc, uint32_t pid) { for (int i = 0; i < 8; i++) if (s[i].tc == tc && s[i].pid == pid) return s[i].prev; return 0; }
static int o_slot(OSlot *s, uint8_t *tc, uint32_t pid, uint32_t ec35) {
    int k = -1;
    for (int i = 0; i < 8; i++) if (s[i].tc == tc && s[i].pid == pid) { k = i; break; }
    if (k < 0) for (int i = 0; i < 8; i++) if (!s[i].tc) { k = i; break; }
    if (k < 0) { k = (int)(ec35 & 7); }
    return k;
}
static void o_item(uint32_t *o, void *tx) { for (int k = 1; k < 10; k++) o[k] = 0; o[7] = 0xffffffffu; o[9] = 0xffffffffu; o[10] = 0x3f800000u; o[0] = 2; *(void **)(o + 2) = tx; }

static void batch20() {
    static const char list[] = "anim-a\0anim-bb\0c\0\0";
    for (uint32_t r = 0; r < 6; r++) CHECK(o_row_name(list, r) == u3x_nth_name(list, r));
    static const char empty[] = "\0"; CHECK(o_row_name(empty, 0) == u3x_nth_name(empty, 0));
    static uint8_t ctl[4][8];
    for (int t = 0; t < 20000; t++) {
        OSlot s[8]; for (auto &x : s) { x.tc = rnd() % 3 ? ctl[rnd() % 4] : nullptr; x.pid = rnd() % 3; x.prev = rnd(); }
        uint8_t *tc = ctl[rnd() % 4]; const uint32_t pid = rnd() % 3, ec = rnd();
        CHECK(o_slot(s, tc, pid, ec) == u3x_taunt_slot((const TauntSlot *)s, tc, pid, ec)); CHECK(o_prev_get(s, tc, pid) == u3x_taunt_previous((const TauntSlot *)s, tc, pid));
    }
    { uint32_t a[12], b[12]; for (int k = 0; k < 12; k++) a[k] = b[k] = 0x77000000u + (uint32_t)k; static int tex; o_item(a, &tex); u3x_image_item(b, &tex); CHECK(std::memcmp(a, b, sizeof a) == 0); }
}
