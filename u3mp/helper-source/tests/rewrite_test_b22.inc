// v7.135 part N: the old helper code (v7.134) for the difficulty names, the two-line text and the picture answers.
static void o_two(char *out, const char *a, const char *b) { char *o = out, *end = o + 250; o = o_append(o, end, a); o = o_append(o, end, "\n"); o = o_append(o, end, b); *o = 0; }
static void o_pic(int32_t *event, void *t, uint32_t spin) { for (int k = 3; k <= 12; k++) event[k] = 0; event[9] = -1; event[11] = -1; if (t) { event[2] = 2; *(void **)(event + 4) = t; event[12] = 0x3f800000; } else { event[2] = 0x10; event[12] = (int32_t)spin; } }

static void batch22() {
    static const uint32_t old_ids[5] = {0xc07f185fu, 0x461748d6u, 0xed7fc910u, 0xb0a09a11u, 0x36c8ca98u};
    CHECK(std::memcmp(old_ids, u3x_difficulty_texts(), sizeof old_ids) == 0);
    for (int t = 0; t < 20000; t++) {
        char a[300], b[300]; const int la = (int)(rnd() % 290), lb = (int)(rnd() % 290);
        for (int k = 0; k < la; k++) a[k] = (char)('a' + rnd() % 26); a[la] = 0; for (int k = 0; k < lb; k++) b[k] = (char)('A' + rnd() % 26); b[lb] = 0;
        char x[300], y[300]; std::memset(x, 7, sizeof x); std::memset(y, 7, sizeof y);
        o_two(x, a, b); u3x_two_lines(y, y + 250, a, b); CHECK(std::memcmp(x, y, sizeof x) == 0);
    }
    static int tex;
    for (int v = 0; v < 2; v++) for (uint32_t spin : {0x3f800000u, 0x40100000u}) {
        int32_t a[16], b[16]; for (int i = 0; i < 16; i++) a[i] = b[i] = 0x12340000 + i; void *t = v ? &tex : nullptr;
        o_pic(a, t, spin); u3x_image_answer(b, t); if (!t) b[12] = (int32_t)spin; CHECK(std::memcmp(a, b, sizeof a) == 0);
    }
}
