// v7.130 part I: the old helper code (v7.129) for the server list, LAN addresses and session messages, and the checks.
static uint32_t o_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
struct ONl { char srv_host[3][64]; uint32_t srv_port[3]; char iv_host[64]; uint32_t iv_port; };
static int o_nl_table(ONl *m, const uint8_t *d, uint32_t n) {
    if (n < 0x18 || d[0] != 'D' || d[1] != 'C' || d[2] != '0' || d[3] != '0') return 0;
    uint32_t ns = o_be32(d + 0x14); if (ns > 4096 || 0x18 + ns * 12 > n) return 0;
    uint32_t tab = 0;
    for (uint32_t i = 0; i < ns; i++) if (o_be32(d + 0x18 + i * 12) == 0x1d38f2f9u) { tab = o_be32(d + 0x18 + i * 12 + 8); break; }
    if (!tab || tab + 0xa0 > n) return 0;
    for (int k = 0; k < 3; k++) { uint32_t l = o_be32(d + tab + (k == 0 ? 0x7cu : k == 1 ? 0x78u : 0x5cu)); if (l + 8 > n || o_be32(d + l) < 1) return 0;
        uint32_t e = o_be32(d + l + 4); if (e + 8 > n) return 0; uint32_t h = o_be32(d + e), port = o_be32(d + e + 4); if (h >= n || port > 0xffff) return 0;
        int i = 0; for (; i < 63 && h + (uint32_t)i < n && d[h + i]; i++) m->srv_host[k][i] = (char)d[h + i]; m->srv_host[k][i] = 0; m->srv_port[k] = port; }
    m->iv_host[0] = 0; m->iv_port = 0;
    { uint32_t l = o_be32(d + tab + 0x40);
      if (l + 8 <= n && o_be32(d + l) >= 1) { uint32_t e = o_be32(d + l + 4);
          if (e + 8 <= n) { uint32_t h = o_be32(d + e), port = o_be32(d + e + 4);
              if (h < n && port <= 0xffff) { int i = 0; for (; i < 63 && h + (uint32_t)i < n && d[h + i]; i++) m->iv_host[i] = (char)d[h + i]; m->iv_host[i] = 0; m->iv_port = port; } } } }
    return 1;
}
static int n_nl_table(ONl *m, const uint8_t *d, uint32_t n) {   // the C wrapper as it is now
    uint32_t tab = u3x_server_list(d, n); if (!tab) return 0;
    for (int k = 0; k < 3; k++) if (!u3x_server_entry(d, n, tab + (k == 0 ? 0x7cu : k == 1 ? 0x78u : 0x5cu), m->srv_host[k], &m->srv_port[k])) return 0;
    m->iv_host[0] = 0; m->iv_port = 0;
    u3x_server_entry(d, n, tab + 0x40, m->iv_host, &m->iv_port);
    return 1;
}
static int o_lstrcmp(const char *a, const char *b) { for (int i = 0; i < 0x24; i++) { if (a[i] != b[i]) return 1; if (!a[i]) return 0; } return 0; }
static void o_lanaddr(uint8_t *m, const char *name, uint32_t ip) {
    uint32_t n = *(uint32_t *)(m + 0x488c);
    for (uint32_t i = 0; i < n && i < 32; i++) { uint8_t *e = m + 0x4890 + i * 0x28; if (!o_lstrcmp((const char *)e, name)) { *(uint32_t *)(e + 0x24) = ip; return; } }
    uint8_t *e = m + 0x4890 + (n & 0x1f) * 0x28; std::memcpy(e, name, 0x24); *(uint32_t *)(e + 0x24) = ip; *(uint32_t *)(m + 0x488c) = n + 1;
}
static const uint8_t o_map[] = {
 3,232,0,0, 0,4,8,4,12,4,16,4,20,4,24,2,26,2,28,2,32,2,34,2,36,2,0xff, 0xff, 4,88,0,0, 0,4,8,4,16,8,0xff, 0xff,
 5,160,160,104, 0,4,8,4,12,2,14,2,16,8,24,2,26,2,0xff, 36,2,0xff, 6,120,0,0, 0,4,8,8,52,2,0xff, 0xff, 7,16,0,0, 0,4,8,8,0xff, 0xff,
 8,24,0,0, 0,4,8,8,16,2,0xff, 0xff, 9,36,0,0, 0,4,8,4,12,4,16,4,20,2,22,2,24,2,28,2,30,2,32,4,0xff, 0xff,
 10,16,16,56, 0,4,8,4,12,4,0xff, 0,8,12,2,16,2,0xff, 11,16,0,0, 0,4,4,2,6,2,8,8,0xff, 0xff,
 12,16,0,0, 0,4,8,8,0xff, 0xff, 13,16,0,0, 0,4,8,8,0xff, 0xff, 14,80,0,0, 0,4,8,8,0xff, 0xff, 15,80,0,0, 0,4,4,2,8,8,0xff, 0xff,
 16,16,0,0, 0,4,4,2,8,8,0xff, 0xff, 17,16,0,0, 0,4,4,2,8,8,0xff, 0xff, 18,16,0,0, 0,4,8,8,0xff, 0xff, 19,16,0,0, 0,4,8,8,0xff, 0xff,
 20,16,0,0, 0,4,4,2,8,8,0xff, 0xff, 21,16,0,0, 0,4,4,2,8,8,0xff, 0xff, 22,16,16,2, 0,4,4,2,8,8,0xff, 0,2,0xff,
 23,144,0,0, 0,4,8,8,0xff, 0xff, 24,144,0,0, 0,4,8,8,0xff, 0xff, 25,4,0,0, 0,4,0xff, 0xff, 0};
static const uint8_t *o_find(uint32_t id, uint32_t *hl, uint32_t *ro, uint32_t *rl) {
    const uint8_t *p = o_map; while (*p) { if (*p == id - 300) { *hl = p[1]; *ro = p[2]; *rl = p[3]; return p + 4; } p += 4; while (*p != 0xff) p += 2; p++; while (*p != 0xff) p += 2; p++; } return 0;
}
static void o_swapf(uint8_t *m, const uint8_t *p) { for (; *p != 0xff; p += 2) { uint8_t *q = m + p[0]; int w = p[1]; for (int i = 0; i < w / 2; i++) { uint8_t t = q[i]; q[i] = q[w - 1 - i]; q[w - 1 - i] = t; } } }
static uint32_t o_swap(uint8_t *m, uint32_t len) { uint32_t id = o_be32(m), hl, ro, rl; const uint8_t *p = o_find(id, &hl, &ro, &rl); if (!p) return id;
    o_swapf(m, p); while (*p != 0xff) p += 2; p++; if (rl) for (uint32_t o = ro; o + rl <= len; o += rl) o_swapf(m + o, p); return id; }
static uint32_t o_swap_out(uint8_t *m, uint32_t id, uint32_t len) { uint32_t hl, ro, rl; const uint8_t *p = o_find(id, &hl, &ro, &rl); if (!p) return 0;
    o_swapf(m, p); while (*p != 0xff) p += 2; p++; if (rl) for (uint32_t o = ro; o + rl <= len; o += rl) o_swapf(m + o, p); return 1; }
static uint32_t o_len(const uint8_t *m, uint32_t n) { uint32_t id = o_be32(m), hl, ro, rl; if (id < 301 || id > 326 || !o_find(id, &hl, &ro, &rl)) return 0;
    if (id == 305) return n < 28 ? 160u : 160u + (uint32_t)(((uint32_t)m[26] << 8) | m[27]) * 104u; if (id == 310) return n < 16 ? 16u : 16u + o_be32(m + 12) * 56u; return hl; }

static void batch17() {
    // server lists: valid files with random server entries, then random damage
    static uint8_t f[2048];
    for (int t = 0; t < 30000; t++) {
        for (auto &x : f) x = (uint8_t)rnd();
        uint32_t n = 256 + rnd() % 1700; f[0] = 'D'; f[1] = 'C'; f[2] = '0'; f[3] = t % 13 ? '0' : '1';
        uint32_t ns = rnd() % 6; auto put = [&](uint32_t at, uint32_t v) { if (at + 4 <= sizeof f) { f[at] = (uint8_t)(v >> 24); f[at + 1] = (uint8_t)(v >> 16); f[at + 2] = (uint8_t)(v >> 8); f[at + 3] = (uint8_t)v; } };
        put(0x14, t % 17 ? ns : 5000); uint32_t tab = 0x60 + rnd() % 64;
        for (uint32_t k = 0; k < ns; k++) { put(0x18 + k * 12, k == ns - 1 || rnd() % 3 == 0 ? 0x1d38f2f9u : rnd()); put(0x18 + k * 12 + 8, rnd() % 9 ? tab : n - 0x50); }
        for (uint32_t field : {0x40u, 0x5cu, 0x78u, 0x7cu}) { uint32_t ref = 0x120 + rnd() % 64 * 4, entry = 0x200 + rnd() % 64 * 4, host = 0x300 + rnd() % 200;
            put(tab + field, rnd() % 15 ? ref : n + 4); put(ref, rnd() % 12 ? 1 : 0); put(ref + 4, entry); put(entry, rnd() % 15 ? host : n); put(entry + 4, rnd() % 15 ? rnd() % 0x10000 : 0x10000);
            for (int c = 0; c < 70; c++) if (host + c < sizeof f) f[host + c] = (uint8_t)(rnd() % 4 ? 'a' + rnd() % 26 : 0); }
        ONl a, b; std::memset(&a, 0x5a, sizeof a); std::memset(&b, 0x5a, sizeof b);
        CHECK(o_nl_table(&a, f, n) == n_nl_table(&b, f, n)); CHECK(std::memcmp(&a, &b, sizeof a) == 0);
    }
    // LAN addresses
    for (int t = 0; t < 3000; t++) {
        static uint8_t m1[0x4890 + 33 * 0x28], m2[sizeof m1]; for (auto &x : m1) x = (uint8_t)rnd(); uint32_t n = rnd() % 40; std::memcpy(m1 + 0x488c, &n, 4);
        for (int k = 0; k < 32; k++) { char *nm = (char *)m1 + 0x4890 + k * 0x28; nm[0] = (char)('a' + rnd() % 4); nm[1] = rnd() % 2 ? 0 : 'x'; nm[2] = 0; }
        std::memcpy(m2, m1, sizeof m1);
        char name[0x24]; for (auto &c : name) c = (char)rnd(); name[0] = (char)('a' + rnd() % 5); name[1] = rnd() % 2 ? 0 : 'x'; name[2] = 0;
        const uint32_t ip = rnd(); o_lanaddr(m1, name, ip); u3x_lan_address((uint32_t *)(m2 + 0x488c), m2 + 0x4890, name, ip);
        CHECK(std::memcmp(m1, m2, sizeof m1) == 0);
    }
    // messages: every id around the range, random contents and lengths
    for (int t = 0; t < 50000; t++) {
        uint8_t a[0x800], b[0x800]; for (auto &x : a) x = (uint8_t)rnd(); const uint32_t id = 290 + rnd() % 300 + (t % 50 == 0 ? 256 : 0);
        a[0] = (uint8_t)(id >> 24); a[1] = (uint8_t)(id >> 16); a[2] = (uint8_t)(id >> 8); a[3] = (uint8_t)id; if (rnd() % 2) { a[26] = 0; a[27] = (uint8_t)(rnd() % 8); a[12] = a[13] = a[14] = 0; a[15] = (uint8_t)(rnd() % 8); }
        const uint32_t have = rnd() % 0x400, len = rnd() % 0x780;
        CHECK(o_len(a, have) == u3x_message_length(a, have));
        std::memcpy(b, a, sizeof a); { const uint32_t before = o_be32(b); u3x_message_swap(b, before, len); CHECK(o_swap(a, len) == before); } CHECK(std::memcmp(a, b, sizeof a) == 0);
        std::memcpy(b, a, sizeof a); CHECK(o_swap_out(a, id, len) == (uint32_t)u3x_message_swap(b, id, len)); CHECK(std::memcmp(a, b, sizeof a) == 0);
    }
}
