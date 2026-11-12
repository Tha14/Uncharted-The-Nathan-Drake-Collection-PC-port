// v7.117 batch: the old helper code (v7.116) for each rewritten section, and the old-versus-new checks.
static int old_profile_name(const uint16_t *in, char out[64]) {
    unsigned n = 0; std::memset(out, 0, 64);
    for (unsigned i = 0; i < 15 && in[i]; i++) {
        uint32_t c = in[i];
        if (c < 32 || c == 127) return 0;
        if (c == '|' || c == '\\' || c == '.') c = '*';
        if (c >= 0xd800 && c <= 0xdbff) { if (i + 1 >= 15 || in[i + 1] < 0xdc00 || in[i + 1] > 0xdfff) return 0; c = 0x10000 + ((c - 0xd800) << 10) + (in[++i] - 0xdc00); }
        else if (c >= 0xdc00 && c <= 0xdfff) return 0;
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) { out[n++] = (char)(0xc0 | (c >> 6)); out[n++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { out[n++] = (char)(0xe0 | (c >> 12)); out[n++] = (char)(0x80 | ((c >> 6) & 63)); out[n++] = (char)(0x80 | (c & 63)); }
        else { out[n++] = (char)(0xf0 | (c >> 18)); out[n++] = (char)(0x80 | ((c >> 12) & 63)); out[n++] = (char)(0x80 | ((c >> 6) & 63)); out[n++] = (char)(0x80 | (c & 63)); }
    }
    unsigned first = 0; while (first < n && out[first] == ' ') first++;
    while (n > first && out[n - 1] == ' ') n--;
    if (first) { for (unsigned i = first; i < n; i++) out[i - first] = out[i]; n -= first; }
    out[n] = 0; return n != 0;
}
static int rebuilt = 0;
static void fake_rebuild(uint8_t *) { rebuilt++; }
static int old_preview(uint8_t **h, uint32_t id, uint32_t mask) {
    uint8_t *p = (h && id && old_ptr_ok(h) && *h && *(uint32_t *)(*h + 0x7c) == id) ? *h : 0;
    if (!p) return 0;
    *(uint16_t *)(p + 0x6f0) = (uint16_t)mask; fake_rebuild(p); return 1;
}
static int old_quit(uint8_t *r) {
    if (!*(uint32_t *)(r + 0x558)) return 0;
    *(uint32_t *)(r + 0x558) = 0; *(uint32_t *)(r + 0x5b0) += 1; *(uint32_t *)(r + 0x1f24) += 1; return 1;
}
static int old_guest(uint8_t *ps) { return *(uint64_t *)(ps + 0x18) && *(uint32_t *)(ps + 0x12e4) != *(uint32_t *)(ps + 0x12e8); }
// old lobby_buttons: label as (kind, id): 0 empty, 1 waiting, 2 text id; shown as the op-8 answer
static void old_buttons(int setup, int guest, int custom, uint32_t own, uint32_t *label, int *shown) {
    if (setup) { if (guest) { *label = 0; *shown = 0; return; } *label = own; *shown = 1; return; }
    if (guest) { *label = 1; *shown = 1; return; }
    *label = custom ? 0x92db2b96u : 0x63091cd0u; *shown = 1;
}
static int old_gate(uint8_t *l, uint32_t *st) {
    if (!old_ptr_ok(l)) return -1; *st = *(uint32_t *)(l + 0x30); if (*st == 8 || *st == 0xc) return 0; return 1;
}
static void old_empty(int32_t *event) {
    switch (event[0]) { case 5: event[1] = 0; break; case 6: event[1] = -1; break; case 7: event[1] = 0; break; case 11: event[14] = 0; break; case 21: event[1] = 0; break; }
}
static char *old_append(char *o, char *end, const char *t) { while (*t && o < end) *o++ = *t++; return o; }
static void old_last_game(char *buf, int32_t *ml, uint32_t mapid, uint32_t sub, int32_t gt, uint32_t d, const char *(*text)(uint32_t)) {
    const char *m = 0, *t = "type", *th = 0;
    if (ml) { uint8_t *e = *(uint8_t **)(ml + 2); for (int32_t i = 0; e && i < ml[0] && i < 128; i++, e += 0x98) if (*(uint32_t *)e == mapid) { m = text(*(uint32_t *)(e + 4)); break; } }
    if (gt == 4 || gt == 0xb) { static const uint32_t ids[5] = {0xc07f185fu, 0x461748d6u, 0xed7fc910u, 0xb0a09a11u, 0x36c8ca98u}; th = d < 5 ? text(ids[d]) : ""; }
    else if (sub && sub != 0x22a276ecu && sub != 0x6f96651fu) th = text(sub);
    char *o = buf, *end = o + 0x7f;
    if (!m) { if (th) { o = old_append(o, end, t); o = old_append(o, end, " - "); o = old_append(o, end, th); } else o = old_append(o, end, t); }
    else { o = old_append(o, end, m); o = old_append(o, end, " - "); o = old_append(o, end, t); if (th) { o = old_append(o, end, " - "); o = old_append(o, end, th); } }
    *o = 0;
}
static char text_pool[64][80];
static const char *fake_text(uint32_t id) {   // a different string per id; some long, to reach the end of the buffer
    char *s = text_pool[id % 64]; int n = 3 + (int)(id % 70); for (int i = 0; i < n; i++) s[i] = (char)('a' + (id + (uint32_t)i) % 26); s[n] = 0; return s;
}
static int old_page_saves(uint32_t page, uint32_t e) {
    int go = 0;
    if (page == 0xe0c0028bu || page == 0xe13ed766u || page == 0xdfdb36fau) go = e - 2u < 3u;
    else if (page == 0x35957e36u || page == 0xba7ae3b4u) go = e - 3u < 2u;
    return go;
}

static void batch5() {
    // names: random UTF-16 with spaces, forbidden characters, controls and (broken) surrogates
    const uint16_t pool[] = {' ', ' ', 'a', 'Z', '9', '|', '\\', '.', '*', 0xe9, 0x7ff, 0x800, 0x20ac, 0xffff, 0xd83d, 0xde00, 0xdbff, 0xdc00, 0x1f, 0x7f, 0};
    int usable = 0;
    for (int t = 0; t < 400000; t++) {
        uint16_t in[17] = {}; uint32_t n = rnd() % 17;
        for (uint32_t i = 0; i < n; i++) { uint32_t r = rnd() % 64; in[i] = r < 21 ? pool[r] : r < 40 ? (uint16_t)('a' + r % 26) : r < 44 ? 0xd83d : r < 48 ? 0xde00 : ' '; if (!in[i] && (rnd() % 8)) in[i] = 'q'; }
        char a[64], b[64]; std::memset(b, 0x33, sizeof b);
        const int ra = old_profile_name(in, a), rb = u3x_name_utf8(in, b, 64);
        CHECK(ra == rb);
        if (ra) { CHECK(std::strcmp(a, b) == 0); usable++; for (size_t k = std::strlen(b); k < 64; k++) if (b[k]) { CHECK(!"padding"); break; } }
        else for (int k = 0; k < 64; k++) if (b[k]) { CHECK(!"cleared"); break; }
    }
    CHECK(usable > 1000);
    // weapon row image
    { int32_t a[16], b[16]; for (int i = 0; i < 16; i++) a[i] = b[i] = 0x33330000 + i;
      a[2] = 0x21; a[4] = 0; a[5] = 0; a[6] = 0x1234567; a[7] = 0; a[8] = 0; a[9] = -1; a[10] = 0; a[11] = -1; a[12] = 0x3f800000;
      u3x_weapon_row_image(b, 0x1234567); CHECK(std::memcmp(a, b, sizeof a) == 0); }
    // preview mods: live, stale, no id, no handle, empty handle
    for (int t = 0; t < 40; t++) {
        alignas(8) static uint8_t o1[0x700], o2[0x700]; std::memset(o1, 0, sizeof o1); std::memset(o2, 0, sizeof o2);
        uint32_t oid = 0x800 + t % 2; std::memcpy(o1 + 0x7c, &oid, 4); std::memcpy(o2 + 0x7c, &oid, 4);
        uint8_t *s1 = (t % 5 == 0) ? nullptr : o1, *s2 = (t % 5 == 0) ? nullptr : o2;
        uint32_t id = (t % 7 == 0) ? 0 : 0x800 + (t / 2) % 2;
        rebuilt = 0; int r1 = old_preview(t % 11 == 0 ? nullptr : &s1, id, 0x12345 + t); int c1 = rebuilt;
        rebuilt = 0; int r2 = u3x_preview_mods(t % 11 == 0 ? nullptr : &s2, id, 0x12345 + t, fake_rebuild);
        CHECK(r1 == r2); CHECK(c1 == rebuilt); CHECK(std::memcmp(o1, o2, sizeof o1) == 0);
    }
    // quit count
    for (uint32_t flag : {0u, 1u, 0x100u}) {
        std::vector<uint8_t> a(0x2000, 0x11), b; std::memcpy(a.data() + 0x558, &flag, 4); b = a;
        CHECK(old_quit(a.data()) == u3x_count_quit(b.data())); CHECK(a == b);
    }
    // party guest and lobby buttons
    for (uint64_t party : {0ull, 5ull, 0x100000000ull}) for (uint32_t me : {0u, 1u}) for (uint32_t lead : {0u, 1u}) {
        std::vector<uint8_t> ps(0x1300, 0); std::memcpy(ps.data() + 0x18, &party, 8); std::memcpy(ps.data() + 0x12e4, &me, 4); std::memcpy(ps.data() + 0x12e8, &lead, 4);
        CHECK(old_guest(ps.data()) == u3x_party_guest(ps.data()));
    }
    for (int setup : {0, 1}) for (int guest : {0, 1}) for (int custom : {0, 1}) {
        uint32_t label; int shown; old_buttons(setup, guest, custom, 0xabcdef01u, &label, &shown);
        CHECK(label == u3x_lobby_button_label(setup, guest, custom, 0xabcdef01u)); CHECK(shown == u3x_lobby_button_shown(setup, guest));
    }
    // leave gate
    { alignas(8) uint8_t level[0x40] = {};
      for (uint32_t st = 0; st < 16; st++) { std::memcpy(level + 0x30, &st, 4); uint32_t s1 = 0, s2 = 0; CHECK(old_gate(level, &s1) == u3x_net_level_ready(level, &s2)); CHECK(s1 == s2); }
      uint32_t s1 = 0, s2 = 0; CHECK(old_gate(nullptr, &s1) == u3x_net_level_ready(nullptr, &s2)); CHECK(old_gate(level + 1, &s1) == u3x_net_level_ready(level + 1, &s2)); }
    // empty list
    for (int32_t op = 0; op < 24; op++) { int32_t a[16], b[16]; for (int i = 0; i < 16; i++) a[i] = b[i] = 0x44440000 + i; a[0] = b[0] = op; old_empty(a); u3x_empty_list_answer(b); CHECK(std::memcmp(a, b, sizeof a) == 0); }
    // last game line: maps found and not, table variants, game types, subtitles, difficulties, long texts
    static const uint32_t ids[5] = {0xc07f185fu, 0x461748d6u, 0xed7fc910u, 0xb0a09a11u, 0x36c8ca98u};
    for (int t = 0; t < 3000; t++) {
        static uint8_t ent[0x98 * 130]; int32_t count = (int32_t)(rnd() % 4 == 0 ? 130 : rnd() % 6);
        for (int k = 0; k < 130; k++) { uint32_t mid = 0x5000 + (uint32_t)k, tid = 0x100 + (uint32_t)k * 7 + (uint32_t)t; std::memcpy(ent + k * 0x98, &mid, 4); std::memcpy(ent + k * 0x98 + 4, &tid, 4); }
        alignas(8) int32_t ml[4] = {}; uint8_t *ep = (t % 17 == 0) ? nullptr : ent; ml[0] = (t % 23 == 0) ? -1 : count; std::memcpy(ml + 2, &ep, 8);
        int32_t *mp = (t % 19 == 0) ? nullptr : ml;
        uint32_t mapid = 0x5000 + rnd() % 131, sub = (rnd() % 4 == 0) ? 0 : (rnd() % 4 == 1) ? 0x22a276ecu : (rnd() % 4 == 2) ? 0x6f96651fu : 0x900 + rnd() % 60;
        int32_t gt = (int32_t)(rnd() % 14); uint32_t d = rnd() % 7;
        char a[0x90], b[0x90]; std::memset(a, 0x55, sizeof a); std::memset(b, 0x55, sizeof b);
        old_last_game(a, mp, mapid, sub, gt, d, fake_text);
        const char *m = 0, *th = 0; uint32_t id = 0;
        if (u3x_map_name_id(mp, mapid, &id)) { static char mbuf[80]; std::strcpy(mbuf, fake_text(id)); m = mbuf; }
        int k = u3x_last_game_third(gt, sub, d, ids, &id); if (k) th = k == 1 ? fake_text(id) : "";
        u3x_join_dash(b, b + 0x7f, m, "type", th);
        CHECK(std::strcmp(a, b) == 0); CHECK(std::memcmp(a, b, sizeof a) == 0);
    }
    // page saves
    for (uint32_t page : {0xe0c0028bu, 0xe13ed766u, 0xdfdb36fau, 0x35957e36u, 0xba7ae3b4u, 0u, 0xe0c0028cu}) for (uint32_t e = 0; e < 8; e++) CHECK(old_page_saves(page, e) == u3x_page_saves(page, e));
    CHECK(old_page_saves(0xe0c0028bu, 0xffffffffu) == u3x_page_saves(0xe0c0028bu, 0xffffffffu));
}
