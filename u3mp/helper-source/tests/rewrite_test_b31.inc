// third split: the old x79_num (a number put back in front of a text without digits) against u3x_x79_num.
static void o31_x79(char *o, int32_t v) {
    for (const char *c = o; *c; c++) if (*c >= '0' && *c <= '9') return;
    char t[64]; int n = 0; for (; o[n] && n < 40; n++) t[n] = o[n]; t[n] = 0;
    char *q = o29_fmt_int(o, o + 16, v, 0); *q++ = ' '; for (int i = 0; t[i]; i++) *q++ = t[i]; *q = 0; }
static void batch31() {
    for (int t = 0; t < 200000; t++) {
        char a[80], b[80];
        const int len = (int)(rnd() % 50);
        for (int k = 0; k < 80; k++) a[k] = b[k] = 'z';
        for (int k = 0; k < len; k++) { char ch = (char)('a' + rnd() % 26); if (rnd() % 30 == 0) ch = (char)('0' + rnd() % 10); a[k] = b[k] = ch; }
        a[len] = b[len] = 0;
        const int32_t v = t & 1 ? (int32_t)rnd() : (int32_t)(rnd() % 100);
        o31_x79(a, v);
        u3x_x79_num(b, v);
        for (int k = 0; k < 80; k++) CHECK(a[k] == b[k]);
    }
}
