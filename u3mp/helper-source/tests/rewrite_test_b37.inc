// fourth split: the old Advanced Settings "[A]" substitution (adv_subst) against u3x_adv_subst.
static void o37_subst(char *o, char *end, const char *t, int64_t v) {
    while (*t && o < end) { if (t[0] == '[' && t[1] == 'A' && t[2] == ']') { o = o29_fmt_int(o, end, v, 0); t += 3; } else *o++ = *t++; } *o = 0; }
static void batch37() {
    static const char *texts[5] = {"[A] seconds", "Respawn in [A]", "[A][A]", "no tag", "[A"};
    for (int t = 0; t < 100000; t++) {
        char a[140], b[140]; for (int k = 0; k < 140; k++) a[k] = b[k] = 'z';
        const char *tx = texts[rnd() % 5]; const int room = (int)(rnd() % 30); const int64_t v = (int64_t)(int32_t)rnd() % 1000;
        o37_subst(a, a + room, tx, v); u3x_adv_subst(b, b + room, tx, v);
        CHECK(memcmp(a, b, sizeof(a)) == 0);
    }
}
