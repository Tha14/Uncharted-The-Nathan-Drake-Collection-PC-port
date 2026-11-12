// third split: the old pu01 LZ packer and base64 against u3x_pu_lz / u3x_pu_b64 (and the packed stream decodes back).
static uint32_t o34_lz(const uint8_t *s, uint32_t n, uint8_t *d, uint32_t cap, int32_t *head) {
    for (int i = 0; i < 4096; i++) head[i] = -1;
    uint32_t i = 0, o = 0, ls = 0;
    while (i < n) { uint32_t best = 0, boff = 0;
        if (i + 3 <= n) { uint32_t h = ((uint32_t)(s[i] | s[i + 1] << 8 | s[i + 2] << 16) * 2654435761u) >> 20;
            for (int k = 0; k < 2; k++) { int32_t c = k ? (int32_t)i - 1 : head[h]; if (c < 0 || (uint32_t)c >= i || i - (uint32_t)c > 8192u) continue;
                uint32_t L = 0; while (i + L < n && L < 264u && s[(uint32_t)c + L] == s[i + L]) L++; if (L > best) { best = L; boff = i - (uint32_t)c; } }
            head[h] = (int32_t)i; }
        if (best < 3) { i++; continue; }
        while (ls < i) { uint32_t k = i - ls; if (k > 32) k = 32; if (o + 1 + k > cap) return 0; d[o++] = (uint8_t)(k - 1); memcpy(d + o, s + ls, k); o += k; ls += k; }
        uint32_t l = best - 2, of = boff - 1; if (o + 3 > cap) return 0;
        if (l < 7) d[o++] = (uint8_t)((l << 5) | (of >> 8)); else { d[o++] = (uint8_t)(0xe0u | (of >> 8)); d[o++] = (uint8_t)(l - 7); }
        d[o++] = (uint8_t)of;
        for (uint32_t k = 1; k < best && i + k + 3 <= n; k++) { uint32_t j = i + k; head[((uint32_t)(s[j] | s[j + 1] << 8 | s[j + 2] << 16) * 2654435761u) >> 20] = (int32_t)j; }
        i += best; ls = i; }
    while (ls < n) { uint32_t k = n - ls; if (k > 32) k = 32; if (o + 1 + k > cap) return 0; d[o++] = (uint8_t)(k - 1); memcpy(d + o, s + ls, k); o += k; ls += k; }
    return o; }
static int o34_b64(char *o, const uint8_t *p, int n) { static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; int k = 0;
    for (int i = 0; i < n; i += 3) { uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o[k++] = a[v >> 18 & 63]; o[k++] = a[v >> 12 & 63]; o[k++] = i + 1 < n ? a[v >> 6 & 63] : '='; o[k++] = i + 2 < n ? a[v & 63] : '='; } return k; }
static void batch34() {
    static uint8_t src[0x2828], da[9000], db[9000], back[0x2828]; static int32_t ha[4096], hb[4096];
    for (int t = 0; t < 300; t++) {
        const uint32_t n = t % 5 == 0 ? 0x2828 : rnd() % 0x2828;
        const int mode = t % 4;
        for (uint32_t i = 0; i < n; i++) src[i] = (uint8_t)(mode == 0 ? rnd() : mode == 1 ? rnd() % 3 : mode == 2 ? (i / 7) & 0xff : (rnd() % 9 ? 0 : rnd()));
        const uint32_t cap = t % 7 == 0 ? rnd() % 3000 : 8096;
        const uint32_t a = o34_lz(src, n, da, cap, ha), b = u3x_pu_lz(src, n, db, cap, hb);
        CHECK(a == b); CHECK(memcmp(da, db, a) == 0);
        if (b && n) CHECK(u3x_lz_decode(db, b, back, n) == n && memcmp(back, src, n) == 0);
        char ca[40], cb[40]; const int bn = (int)(rnd() % 26);
        CHECK(o34_b64(ca, src, bn) == u3x_pu_b64(cb, src, bn)); CHECK(memcmp(ca, cb, (size_t)o34_b64(ca, src, bn)) == 0);
    }
}
