// v7.134 part M: the old helper code (v7.133) for the invite rows, quick actions and prompt text, and the checks.
static uint8_t o_b64v_(char c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c - 'A') : c >= 'a' && c <= 'z' ? (uint8_t)(c - 'a' + 26) : c >= '0' && c <= '9' ? (uint8_t)(c - '0' + 52) : c == '+' || c == '*' ? 62 : c == '/' || c == '-' ? 63 : 0xffu; }
static uint32_t o_iv_parse(char *t, uint8_t (*row)[0x30], uint32_t max) {
    uint32_t c = 0; char *p = t;
    while (*p && c < max) {
        char *l = p; while (*p && *p != '\n') p++; char *le = p; if (*p) p++;
        if (l == le || *l != '+') continue;
        char *q = l + 1; while (q < le && *q != ' ') q++; if (q >= le) break; q++;
        uint8_t *o = row[c]; std::memset(o, 0, 0x30); uint32_t st = 0, i = 0;
        for (; q < le && *q != '='; q++) { if (*q == '\r') continue; int8_t v = (int8_t)o_b64v_(*q); if (i >= 0x30) break;
            if (st == 0) { o[i] = (uint8_t)(v << 2); st = 1; }
            else if (st == 1) { o[i] |= (uint8_t)(v >> 4); if (++i < 0x30) o[i] = (uint8_t)(v << 4); st = 2; }
            else if (st == 2) { o[i] |= (uint8_t)(v >> 2); if (++i < 0x30) o[i] = (uint8_t)(v << 6); st = 3; }
            else { o[i] |= (uint8_t)v; i++; st = 0; } }
        c++; }
    return c;
}
struct OQa { uint32_t arg, page; int32_t n; uint32_t p[16]; };
static int o_qa_add(OQa *q, uint32_t t, uint32_t a) { if (q->n > 7) return 0; q->p[q->n * 2] = t; q->p[q->n * 2 + 1] = a; q->n++; return 1; }
static int o_qa_drop(OQa *q) { int d = 0; int32_t w = 0; for (int32_t i = 0; i < q->n; i++) { uint32_t a = q->p[i * 2 + 1]; if (a == 0x3fe6f70eu || a == 0xe1810202u) { d++; continue; } q->p[w * 2] = q->p[i * 2]; q->p[w * 2 + 1] = a; w++; } q->n = w; return d; }
static void o_ptxt(char *dst, const char *tok, const char *s) { int n = 0; while (*tok && n < 80) dst[n++] = *tok++; while (s && *s && n < 86) dst[n++] = *s++; dst[n] = 0; }

static void batch21() {
    static const char chars[] = "ABZabz09+/*-=\r \n+?";
    for (int t = 0; t < 20000; t++) {
        char text[700]; const int len = (int)(rnd() % 690);
        for (int k = 0; k < len; k++) text[k] = rnd() % 5 ? chars[rnd() % 18] : (rnd() % 2 ? '+' : '\n');
        text[len] = 0;
        uint8_t r1[12][0x30], r2[12][0x30]; std::memset(r1, 0x11, sizeof r1); std::memset(r2, 0x11, sizeof r2);
        const uint32_t max = rnd() % 13;
        CHECK(o_iv_parse(text, r1, max) == u3x_invite_rows(text, r2, max)); CHECK(std::memcmp(r1, r2, sizeof r1) == 0);
    }
    for (int t = 0; t < 20000; t++) {
        OQa a = {1, 2, 0, {}}, b = a; const int adds = (int)(rnd() % 12);
        for (int k = 0; k < adds; k++) { const uint32_t act = rnd() % 4 == 0 ? (rnd() % 2 ? 0x3fe6f70eu : 0xe1810202u) : rnd(); const uint32_t tx = rnd();
            CHECK(o_qa_add(&a, tx, act) == u3x_quick_action_add((QuickActions *)&b, tx, act)); }
        CHECK(o_qa_drop(&a) == u3x_quick_action_drop_profiles((QuickActions *)&b)); CHECK(std::memcmp(&a, &b, sizeof a) == 0);
    }
    for (int t = 0; t < 20000; t++) {
        char tok[100], lab[100]; const int lt = (int)(rnd() % 99), ll = (int)(rnd() % 99);
        for (int k = 0; k < lt; k++) tok[k] = (char)('a' + rnd() % 26); tok[lt] = 0; for (int k = 0; k < ll; k++) lab[k] = (char)('A' + rnd() % 26); lab[ll] = 0;
        char a[100], b[100]; std::memset(a, 3, sizeof a); std::memset(b, 3, sizeof b); const char *lp = t % 7 ? lab : nullptr;
        o_ptxt(a, tok, lp); u3x_prompt_text(b, tok, lp); CHECK(std::memcmp(a, b, sizeof a) == 0);
    }
}
