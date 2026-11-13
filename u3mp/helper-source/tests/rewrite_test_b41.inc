// fourth split, part 15: the online backend's room messages (onb_s5 / s8 / s10 / s12 / s13 / s14 / s15 / s16, the
// cookie and binding lookups, onb_strcpy, onb_name_time, onb_geo) as the C wrote them, against the C++ versions. The
// stand-in u3h_ol_frame keeps the frame the C++ sends; the old code builds its message into o41_m.
struct O41Be { void *vt; char name[0x24]; uint8_t *bind[4]; float next_ping; uint32_t rxlen; uint8_t rx[0x1000]; uintptr_t base; };
static uint8_t o41_m[0x100];
static uint32_t o41_len;
static int o41_bound(O41Be *be, uint8_t *ss) { for (int i = 0; i < 4; i++) if (be->bind[i] == ss) return i; return -1; }
static uint32_t o41_cookie(O41Be *be, uint8_t *ss) { int i = o41_bound(be, ss); return i < 0 ? 0 : (uint32_t)i + 1; }
static void o41_send(uint32_t n) { o41_len = n; }
static void o41_strcpy(char *d, const char *s, unsigned n) { unsigned i = 0; for (; i + 1 < n && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static void o41_name_time(uintptr_t base, char *out) {
    const char *id = (const char *)(base + 0x17d6010u); unsigned k = 0;
    for (; k < 0x24 && id[k]; k++) out[k] = id[k];
    out[k++] = '.';
    uint64_t t = (uint64_t)(uint32_t)*(int32_t *)(base + 0x17b4ba4u); char d[24]; int nd = 0;
    do { d[nd++] = (char)('0' + t % 10); t /= 10; } while (t);
    while (nd) out[k++] = d[--nd];
    out[k] = 0; }
static void o41_geo(uintptr_t base, uint16_t *a, uint16_t *b) {
    float f124, f112; memcpy(&f124, (void *)(base + 0x17d5c58u + 0x30), 4); memcpy(&f112, (void *)(base + 0x17d5c58u + 0x34), 4);
    *a = (uint16_t)(int32_t)f112; *b = (uint16_t)(int32_t)f124; }
static int64_t o41_s5(O41Be *be, uint8_t *ss, uint32_t a3, int32_t a5, int32_t a6, int32_t a7, uint32_t a8) {
    uint8_t *m = o41_m; memset(m, 0, 36);
    *(uint32_t *)m = 309; *(uint32_t *)(m + 8) = o41_cookie(be, ss); *(uint32_t *)(m + 12) = *(uint32_t *)(ss + 0x14);
    *(uint32_t *)(ss + 0x218) = 0;
    *(uint32_t *)(m + 16) = *(uint32_t *)(ss + 0xf8) | (a8 ? 0x8000u : 0);
    o41_geo(be->base, (uint16_t *)(m + 20), (uint16_t *)(m + 22));
    *(uint16_t *)(m + 24) = (uint16_t)a3; *(uint32_t *)(m + 32) = 0;
    if (a5 >= 0) { int32_t lo = a5 - a6; if (lo < 0) lo = 0; *(uint16_t *)(m + 28) = (uint16_t)lo; *(uint16_t *)(m + 30) = (uint16_t)(a5 + a7); }
    o41_send(36); ss[0xb0] = 0; *(uint32_t *)(ss + 0xb4) = 0; return 0; }
static int64_t o41_s8(O41Be *be, uint8_t *ss, uint32_t v) {
    if (o41_bound(be, ss) < 0) return -1;
    uint8_t cur = ss[0x12ec]; v &= 0xff;
    if (v ? cur != 0 : cur == 0) return -1;
    ss[0x12ec] = (uint8_t)v;
    uint64_t room = *(uint64_t *)(ss + 0x18); if (!room) return 0;
    uint8_t *m = o41_m; memset(m, 0, 16); *(uint32_t *)m = 318; *(uint64_t *)(m + 8) = room; m[5] = 3;
    m[4] = *(uint32_t *)(be->base + 0x17d603cu) == 2 ? (uint8_t)v : (v ? 3 : 0);
    o41_send(16); return 0; }
static int64_t o41_s10(O41Be *be, uint8_t *ss, uint32_t id) {
    if (o41_bound(be, ss) < 0) return -1;
    uint64_t room = *(uint64_t *)(ss + 0x18); if (!room) return 0;
    uint8_t *m = o41_m; memset(m, 0, 16); *(uint32_t *)m = 316; *(uint16_t *)(m + 4) = (uint16_t)id; *(uint64_t *)(m + 8) = room; o41_send(16); return 0; }
static int64_t o41_s12(O41Be *be, uint8_t *ss, const uint8_t *data, uint32_t len) {
    if (len > 64) return -1;
    *(uint32_t *)(ss + 0x12f0) = len; memcpy(ss + 0x12f4, data, len);
    if (o41_bound(be, ss) < 0) return -1;
    uint64_t room = *(uint64_t *)(ss + 0x18); if (!room) return 0;
    uint8_t *m = o41_m; memset(m, 0, 80); *(uint32_t *)m = 314; m[4] = (uint8_t)len; *(uint64_t *)(m + 8) = room; memcpy(m + 16, data, len);
    o41_send(80); return 0; }
static int64_t o41_s13(O41Be *be, uint8_t *ss, uint32_t v) {
    if (o41_bound(be, ss) < 0) return -1;
    uint64_t room = *(uint64_t *)(ss + 0x18); if (!room) return 0;
    uint8_t *m = o41_m; memset(m, 0, 16); *(uint32_t *)m = 318; m[4] = (v & 0xff) ? 4 : 0; m[5] = 4; *(uint64_t *)(m + 8) = room; o41_send(16); return 0; }
static int64_t o41_s14(O41Be *be, uint8_t *ss, const uint16_t *rank, uint32_t n) {
    if (ss == (uint8_t *)(be->base + 0x17d6420u) && *(uint32_t *)(be->base + 0x1b68810u) == 0x30u && !ss[0x1338] && *(uint32_t *)(ss + 0x12e4) == *(uint32_t *)(ss + 0x12e8)) ss[0x1338] = 1;
    if (o41_bound(be, ss) < 0) return -1;
    uint64_t room = *(uint64_t *)(ss + 0x18);
    if (!room) { ss[0xe0] = 1; return 0; }
    if (n > (0x100 - 16) / 2) n = (0x100 - 16) / 2;
    uint8_t *m = o41_m; memset(m, 0, 16 + 2 * n); *(uint32_t *)m = 322; *(uint16_t *)(m + 4) = (uint16_t)n; *(uint64_t *)(m + 8) = room;
    memcpy(m + 16, rank, 2 * n); o41_send(16 + 2 * n); return 0; }
static int64_t o41_s16(uint8_t *ss) {
    ss[0xe8] = 1;
    uint64_t room = *(uint64_t *)(ss + 0x18); if (!room) return 0;
    uint8_t *m = o41_m; memset(m, 0, 144); *(uint32_t *)m = 323; *(uint64_t *)(m + 8) = room; o41_strcpy((char *)(m + 16), (const char *)(ss + 0x20), 128);
    o41_send(144); return 0; }
static void batch41() {
    static std::vector<uint8_t> mem(0x1c00000);
    const uintptr_t base = (uintptr_t)mem.data();
    static uint8_t sb[0x1400], other[0x1400], keep[0x1400], after[0x1400], frame[0x100];
    O41Be *be = (O41Be *)t41_object;
    for (int t = 0; t < 40000; t++) {
        memset(t41_object, 0, sizeof t41_object); be->base = base;
        uint8_t *ss = t % 5 == 0 ? (uint8_t *)(base + 0x17d6420u) : sb;   // the match session sometimes (slot 14 tests it)
        for (int i = 0; i < 0x1400; i += 4) *(uint32_t *)(ss + i) = rnd() % 5 ? rnd() : 0;
        ss[0x12ec] = rnd() % 2 ? (uint8_t)rnd() : 0; ss[0x1338] = rnd() % 2 ? 1 : 0;
        if (rnd() % 2) *(uint32_t *)(ss + 0x12e8) = *(uint32_t *)(ss + 0x12e4);
        if (rnd() % 4 == 0) *(uint64_t *)(ss + 0x18) = 0;
        ss[0x20 + rnd() % 160] = 0;
        const int slot = rnd() % 4;
        if (rnd() % 6) be->bind[slot] = ss;
        if (rnd() % 2) be->bind[(slot + 1) % 4] = other;
        *(uint32_t *)(base + 0x17d603cu) = rnd() % 3;
        *(uint32_t *)(base + 0x1b68810u) = rnd() % 2 ? 0x30u : rnd();
        *(int32_t *)(base + 0x17b4ba4u) = (int32_t)rnd();
        const float g1 = (float)((int32_t)(rnd() % 200000) - 100000) / 7.0f, g2 = (float)(rnd() % 70000);
        memcpy((void *)(base + 0x17d5c58u + 0x30), &g1, 4); memcpy((void *)(base + 0x17d5c58u + 0x34), &g2, 4);
        const int idn = (int)(rnd() % 0x26);
        for (int k = 0; k < 0x24; k++) ((char *)(base + 0x17d6010u))[k] = k < idn ? (char)('a' + rnd() % 26) : 0;
        uint8_t data[64]; uint16_t rank[200]; for (auto &d : data) d = (uint8_t)rnd(); for (auto &r : rank) r = (uint16_t)rnd();
        const uint32_t a = rnd(), b = rnd(), a8 = rnd() % 2, len = rnd() % 70, n = rnd() % 200, v = rnd() % 3 ? rnd() % 4 : rnd();
        const int32_t a5 = (int32_t)(rnd() % 40) - 5, a6 = (int32_t)(rnd() % 20), a7 = (int32_t)(rnd() % 20);
        // the new code, then the old code on the same session restored
        memcpy(keep, ss, sizeof keep);
        t41_frames = 0; int64_t r_new = 0, r_old = 0;
        switch (t % 8) {
        case 0: r_new = u3x_onb_s5(be, ss, a, b, a5, a6, a7, a8); break;
        case 1: r_new = u3x_onb_s8(be, ss, v); break;
        case 2: r_new = u3x_onb_s10(be, ss, a); break;
        case 3: r_new = u3x_onb_s12(be, ss, data, len); break;
        case 4: r_new = u3x_onb_s13(be, ss, a); break;
        case 5: r_new = u3x_onb_s14(be, ss, rank, n); break;
        case 6: r_new = u3x_onb_s16(be, ss); break;
        default: r_new = u3x_onb_s15(be, ss); break;
        }
        const uint32_t frames = t41_frames; memcpy(frame, t41_frame, sizeof frame);
        memcpy(after, ss, sizeof after); memcpy(ss, keep, sizeof keep);
        o41_len = 0;
        switch (t % 8) {
        case 0: r_old = o41_s5(be, ss, a, a5, a6, a7, a8); break;
        case 1: r_old = o41_s8(be, ss, v); break;
        case 2: r_old = o41_s10(be, ss, a); break;
        case 3: r_old = o41_s12(be, ss, data, len); break;
        case 4: r_old = o41_s13(be, ss, a); break;
        case 5: r_old = o41_s14(be, ss, rank, n); break;
        case 6: r_old = o41_s16(ss); break;
        default: o41_name_time(base, (char *)(ss + 0x20)); r_old = o41_s16(ss); break;
        }
        CHECK(r_new == r_old);
        CHECK(frames == (o41_len ? 1u : 0u));
        if (frames && o41_len) { CHECK(t41_len == o41_len && memcmp(o41_m, frame, o41_len) == 0); CHECK(t41_type == 0x400 + (*(uint32_t *)o41_m - 300)); }
        CHECK(memcmp(ss, after, sizeof after) == 0);
    }
    // the lookups, the copy and the location words
    for (int t = 0; t < 20000; t++) {
        memset(t41_object, 0, sizeof t41_object);
        uint8_t *ses[6] = {sb, other, sb + 8, other + 8, nullptr, sb + 16};
        for (auto &p : be->bind) p = ses[rnd() % 6];
        for (int i = 0; i < 6; i++) if (ses[i]) *(uint64_t *)(ses[i] + 0x18) = rnd() % 4;
        uint8_t *q = ses[rnd() % 6];
        CHECK(u3x_onb_bound(be, q) == o41_bound(be, q) && u3x_onb_cookie(be, q) == o41_cookie(be, q));
        const uint64_t room = rnd() % 4; uint8_t *want = nullptr;
        for (auto p : be->bind) if (p && *(uint64_t *)(p + 0x18) == room) { want = p; break; }
        CHECK(u3x_onb_by_room(be, room) == want);
        const uint32_t c = rnd() % 6;
        CHECK(u3x_onb_by_cookie(be, c) == (c >= 1 && c <= 4 ? be->bind[c - 1] : nullptr));
        char src[40], d1[40], d2[40]; for (auto &ch : src) ch = rnd() % 5 ? (char)('a' + rnd() % 26) : 0;
        const uint32_t n = 1 + rnd() % 40; memset(d1, 7, sizeof d1); memset(d2, 7, sizeof d2);
        u3x_onb_strcpy(d1, src, n); o41_strcpy(d2, src, n); CHECK(memcmp(d1, d2, sizeof d1) == 0);
        const float g1 = (float)((int32_t)(rnd() % 200000) - 100000) / 3.0f, g2 = (float)rnd() / 65536.0f;
        memcpy((void *)(base + 0x17d5c58u + 0x30), &g1, 4); memcpy((void *)(base + 0x17d5c58u + 0x34), &g2, 4);
        uint16_t g[4]; u3x_onb_geo(base, &g[0], &g[1]); o41_geo(base, &g[2], &g[3]); CHECK(g[0] == g[2] && g[1] == g[3]);
    }
}
