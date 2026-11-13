// row 15 (c008): bit writer, locator math, table lookups and the safe paths of the game-call wrappers.
static uint32_t old_fabs43(uint32_t f) { return f & 0x7fffffffu; }
static void old_bsw43(uint8_t *bs, uint32_t val, uint32_t n, uint32_t *err) {
    uint32_t pos = *(uint32_t *)(bs + 8);
    if (pos + n > *(uint32_t *)(bs + 0xc)) { *err = 1; return; }
    uint32_t *w = *(uint32_t **)bs;
    uint32_t v = (uint32_t)((uint64_t)val << (32 - n)), sh = pos & 31, room = 32 - sh;
    uint32_t *p = w + (pos >> 5);
    *p = (uint32_t)(*p & (uint32_t)((uint64_t)0xffffffffu << room)) | (uint32_t)(v >> sh);
    if (room < n) p[1] = (uint32_t)((uint64_t)v << room);
    *(uint32_t *)(bs + 8) = pos + n;
}
static void old_bsw_vec43(uint8_t *bs, const uint32_t *v, uint32_t *err) {
    if (f_less(old_fabs43(v[0]), 0x437f0000u) && f_less(old_fabs43(v[1]), 0x42fe0000u) &&
        f_less(old_fabs43(v[2]), 0x437f0000u)) {
        old_bsw43(bs, 0, 1, err);
        old_bsw43(bs, u3x_ppc_ctiwz(f_mul(v[0], 0x44000000u)), 18, err);
        old_bsw43(bs, u3x_ppc_ctiwz(f_mul(v[1], 0x43800000u)), 17, err);
        old_bsw43(bs, u3x_ppc_ctiwz(f_mul(v[2], 0x44000000u)), 18, err);
    } else {
        old_bsw43(bs, 1, 1, err);
        old_bsw43(bs, v[0], 32, err);
        old_bsw43(bs, v[1], 32, err);
        old_bsw43(bs, v[2], 32, err);
    }
}
static void old_bsw_frame43(uintptr_t, uint8_t *bs, uint8_t *, uint32_t *err) {
    if (*err) return;
    if (*(uint32_t *)(bs + 8) + 0x200 > *(uint32_t *)(bs + 0xc)) { *err = 1; return; }
}
static void old_q_mul43(uint32_t *o, const uint32_t *a, const uint32_t *b) {
    uint32_t x = f_add(f_add(f_mul(a[3], b[0]), f_mul(a[0], b[3])), f_sub(f_mul(a[1], b[2]), f_mul(a[2], b[1])));
    uint32_t y = f_add(f_add(f_mul(a[3], b[1]), f_mul(a[1], b[3])), f_sub(f_mul(a[2], b[0]), f_mul(a[0], b[2])));
    uint32_t z = f_add(f_add(f_mul(a[3], b[2]), f_mul(a[2], b[3])), f_sub(f_mul(a[0], b[1]), f_mul(a[1], b[0])));
    uint32_t w = f_sub(f_sub(f_mul(a[3], b[3]), f_mul(a[0], b[0])), f_add(f_mul(a[1], b[1]), f_mul(a[2], b[2])));
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
static void old_q_rot43(uint32_t *o, const uint32_t *q, const uint32_t *v) {
    uint32_t p[4] = {v[0], v[1], v[2], 0};
    uint32_t c[4] = {q[0] ^ 0x80000000u, q[1] ^ 0x80000000u, q[2] ^ 0x80000000u, q[3]}, t[4], r[4];
    old_q_mul43(t, q, p); old_q_mul43(r, t, c);
    o[0] = r[0]; o[1] = r[1]; o[2] = r[2]; o[3] = 0;
}
static void old_loc_mul43(uint32_t *o, const uint32_t *a, const uint32_t *b) {
    uint32_t r[4], q[4];
    old_q_rot43(r, a + 4, b); old_q_mul43(q, a + 4, b + 4);
    o[0] = f_add(a[0], r[0]); o[1] = f_add(a[1], r[1]); o[2] = f_add(a[2], r[2]); o[3] = 0;
    for (int i = 0; i < 4; ++i) o[4 + i] = q[i];
}
static void old_loc_rel43(uint32_t *o, const uint32_t *a, const uint32_t *b) {
    uint32_t c[4] = {a[4] ^ 0x80000000u, a[5] ^ 0x80000000u, a[6] ^ 0x80000000u, a[7]};
    uint32_t d[4] = {f_sub(b[0], a[0]), f_sub(b[1], a[1]), f_sub(b[2], a[2]), 0}, r[4], q[4];
    old_q_rot43(r, c, d); old_q_mul43(q, c, b + 4);
    for (int i = 0; i < 3; ++i) o[i] = r[i];
    o[3] = 0;
    for (int i = 0; i < 4; ++i) o[4 + i] = q[i];
}
static void old_bf_world43(uint8_t *f, uint32_t *o) {
    uint32_t par[8], loc[8];
    for (int i = 0; i < 8; ++i) {
        par[i] = *(uint32_t *)(f + 0x20 + 4 * i);
        loc[i] = *(uint32_t *)(f + 0x40 + 4 * i);
    }
    old_loc_mul43(o, par, loc);
}
static int32_t old_tab_find43(uint8_t *t, uint32_t key) {
    uint32_t n = *(uint32_t *)t;
    uint8_t *e = *(uint8_t **)(t + 8);
    uint64_t lo = 0, hi = (uint64_t)(uint32_t)(n - 1);
    for (int g = 0; g < 64; ++g) {
        uint64_t mid = (lo + hi) >> 1;
        if (mid >= n || !e) break;
        uint32_t v = *(uint32_t *)(e + mid * 16);
        if (key < v) hi = mid - 1;
        else { lo = mid + 1; if (key <= v) return (int32_t)mid; }
        if (lo > hi) break;
    }
    return -1;
}
static uint8_t *old_art_tab43(uint8_t *g) {
    uint8_t *a = g ? *(uint8_t **)(g + 0x30) : 0;
    return a ? *(uint8_t **)(a + 0x10) : 0;
}
static uint32_t old_sid43(uintptr_t base, const char *s) {
    if (!s || !*s) return 0;
    return ((uint32_t (*)(int, const char *, int))*(uintptr_t *)(base + 0x1702720u))(0, s, 1);
}
static int old_anim_lookup43(uintptr_t base, uint32_t *tab, int32_t *idx, uint32_t id, uint8_t *anim, uint8_t **tabs) {
    for (uint32_t i = 0; i < 8; ++i) {
        uint8_t *g = tabs[i];
        if (!g) continue;
        if (anim && *(uint32_t *)(g + 0x18) != old_sid43(base, *(const char **)(anim + 0x38))) continue;
        uint8_t *t = old_art_tab43(g);
        int32_t k = t ? old_tab_find43(t, id) : -1;
        if (k != -1) { *idx = k; *tab = i; return 1; }
    }
    return 0;
}
static const uint32_t t43_ex[] = {0x11111111u,0x22222222u,0x33333333u,0x44444444u,0x55555555u,0x66666666u,
                                  0x77777777u,0x88888888u,0x99999999u,0xaaaaaaaau,0xbbbbbbbbu,0xccccccccu};
static int old_layer43(uint32_t u) {
    if ((uint32_t)(u + 0x46d2949du) <= 4) return 0;
    if (u >= 0xb4e30afau && u <= 0xb4e30b08u) return 0;
    for (unsigned i = 0; i < sizeof(t43_ex) / 4; ++i) if (u == t43_ex[i]) return 0;
    return 1;
}
static uint8_t t43_binding[0x20];
static uint8_t *t43_ret_binding(uint8_t *, uint32_t) { return t43_binding; }
static uint8_t *old_np_by_id43(uintptr_t base, uint32_t id) {
    if (!id) return 0;
    for (int i = 0; i < 10; ++i) {
        uint8_t *q = (uint8_t *)(base + 0x17cb060u + 0x390) + (size_t)i * 0x9c0;
        if (u3h_np_code(q) == id) return q;
    }
    return 0;
}
static uint8_t *old_bind_root43(uint8_t *obj) {
    uint8_t *b = ((uint8_t *(*)(uint8_t *, uint32_t))(*(uintptr_t **)obj)[0x248 / 8])(obj, 0);
    if (*(uint16_t *)(obj + 0x128) & 0x800) return b;
    return 0;
}
static uint8_t *old_stream_root43(uintptr_t base) {
    int32_t n = *(int32_t *)(base + 0x1d46088u);
    for (int32_t i = 0; i < n && i < 0x32; ++i)
        if (*(uint8_t **)(base + 0x1d46090u + 8 * (size_t)i)) return (uint8_t *)(uintptr_t)1;
    return 0;
}
static uint32_t t43_sid(int, const char *s, int) {
    return s && s[0] == 'a' ? 0x1111u : 0x2222u;
}

static void batch43() {
    alignas(16) static uint8_t mem[0x800000];
    const uintptr_t base = (uintptr_t)mem - 0x1700000u;
    std::memset(mem, 0, sizeof mem);

    for (uint32_t f : {0u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x7fc00000u, 0xffffffffu})
        CHECK(old_fabs43(f) == u3x_fabs(f));

    alignas(8) uint32_t wo[64], wn[64];
    alignas(8) uint8_t bo[0x20], bn[0x20];
    const uint32_t vals[] = {0u, 1u, 0xffffffffu, 0x12345678u, 0x80000000u};
    const uint32_t nums[] = {0u, 1u, 3u, 17u, 18u, 32u};
    for (uint32_t pos : {0u, 1u, 5u, 31u, 32u, 33u, 63u}) {
        for (uint32_t n : nums) {
            for (uint32_t val : vals) {
                uint32_t eo = 0;
                std::memset(wo, 0x5a, sizeof wo); std::memset(wn, 0x5a, sizeof wn);
                std::memset(bo, 0, sizeof bo); std::memset(bn, 0, sizeof bn);
                *(uint32_t **)bo = wo; *(uint32_t *)(bo + 8) = pos; *(uint32_t *)(bo + 0xc) = 2048;
                *(uint32_t **)bn = wn; *(uint32_t *)(bn + 8) = pos; *(uint32_t *)(bn + 0xc) = 2048;
                *u3h_repl_error() = 0;
                old_bsw43(bo, val, n, &eo); u3x_bsw(bn, val, n);
                CHECK(eo == *u3h_repl_error());
                CHECK(*(uint32_t *)(bo + 8) == *(uint32_t *)(bn + 8));
                CHECK(std::memcmp(wo, wn, sizeof wo) == 0);
            }
        }
    }
    for (uint32_t pos : {0u, 31u, 32u, 63u}) {
        for (uint32_t n : {1u, 5u, 32u}) {
            uint32_t eo = 0;
            std::memset(wo, 0x33, sizeof wo); std::memset(wn, 0x33, sizeof wn);
            std::memset(bo, 0, sizeof bo); std::memset(bn, 0, sizeof bn);
            *(uint32_t **)bo = wo; *(uint32_t *)(bo + 8) = pos; *(uint32_t *)(bo + 0xc) = pos + n - 1;
            *(uint32_t **)bn = wn; *(uint32_t *)(bn + 8) = pos; *(uint32_t *)(bn + 0xc) = pos + n - 1;
            *u3h_repl_error() = 0;
            old_bsw43(bo, 0xabcdef01u, n, &eo); u3x_bsw(bn, 0xabcdef01u, n);
            CHECK(eo == *u3h_repl_error());
            CHECK(*(uint32_t *)(bo + 8) == *(uint32_t *)(bn + 8));
            CHECK(std::memcmp(wo, wn, sizeof wo) == 0);
        }
    }

    const uint32_t vecs[][3] = {{0,0,0}, {0x3f800000u,0xbf800000u,0x40000000u},
        {0x437effffu,0x42fdffffu,0x437effffu}, {0x437f0000u,0x42fe0000u,0x437f0000u},
        {0x7f800000u,0xff800000u,0x7fc00000u}, {0x501502f9u,0x501502f9u,0x501502f9u}};
    for (const auto &v : vecs) {
        uint32_t eo = 0;
        std::memset(wo, 0x44, sizeof wo); std::memset(wn, 0x44, sizeof wn);
        std::memset(bo, 0, sizeof bo); std::memset(bn, 0, sizeof bn);
        *(uint32_t **)bo = wo; *(uint32_t *)(bo + 8) = 0; *(uint32_t *)(bo + 0xc) = 2048;
        *(uint32_t **)bn = wn; *(uint32_t *)(bn + 8) = 0; *(uint32_t *)(bn + 0xc) = 2048;
        *u3h_repl_error() = 0;
        old_bsw_vec43(bo, v, &eo); u3x_bsw_vec(bn, v);
        CHECK(eo == *u3h_repl_error());
        CHECK(*(uint32_t *)(bo + 8) == *(uint32_t *)(bn + 8));
        CHECK(std::memcmp(wo, wn, sizeof wo) == 0);
    }

    {
        uint32_t eo = 1;
        std::memset(wo, 0x77, sizeof wo); std::memset(wn, 0x77, sizeof wn);
        std::memset(bo, 0, sizeof bo); std::memset(bn, 0, sizeof bn);
        *(uint32_t **)bo = wo; *(uint32_t *)(bo + 8) = 0; *(uint32_t *)(bo + 0xc) = 2048;
        *(uint32_t **)bn = wo; *(uint32_t *)(bn + 8) = 0; *(uint32_t *)(bn + 0xc) = 2048;
        *u3h_repl_error() = 1;
        old_bsw_frame43(0, bo, nullptr, &eo); u3x_bsw_frame(0, bn, nullptr);
        CHECK(eo == *u3h_repl_error());
        CHECK(std::memcmp(bo, bn, sizeof bo) == 0);
        CHECK(std::memcmp(wo, wn, sizeof wo) == 0);
    }
    {
        uint32_t eo = 0;
        std::memset(bo, 0, sizeof bo); std::memset(bn, 0, sizeof bn);
        *(uint32_t **)bo = wo; *(uint32_t *)(bo + 8) = 0x100; *(uint32_t *)(bo + 0xc) = 0x2ff;
        *(uint32_t **)bn = wo; *(uint32_t *)(bn + 8) = 0x100; *(uint32_t *)(bn + 0xc) = 0x2ff;
        *u3h_repl_error() = 0;
        old_bsw_frame43(0, bo, nullptr, &eo); u3x_bsw_frame(0, bn, nullptr);
        CHECK(eo == *u3h_repl_error());
        CHECK(eo == 1u);
        CHECK(std::memcmp(bo, bn, sizeof bo) == 0);
    }

    for (int round = 0; round < 16; ++round) {
        uint32_t a[8], b[8], oo[8] = {}, on[8] = {};
        for (int i = 0; i < 8; ++i) {
            a[i] = 0x3e000000u + (uint32_t)(round * 0x10001 + i * 0x101);
            b[i] = 0xbe800000u + (uint32_t)(round * 0x10003 + i * 0x103);
        }
        old_q_mul43(oo, a, b); u3x_q_mul(on, a, b);
        CHECK(std::memcmp(oo, on, 16) == 0);
        old_q_rot43(oo, a, b); u3x_q_rot(on, a, b);
        CHECK(std::memcmp(oo, on, 16) == 0);
        old_loc_mul43(oo, a, b); u3x_loc_mul(on, a, b);
        CHECK(std::memcmp(oo, on, 32) == 0);
        old_loc_rel43(oo, a, b); u3x_loc_rel(on, a, b);
        CHECK(std::memcmp(oo, on, 32) == 0);
    }

    alignas(8) uint8_t frame[0x60] = {};
    uint32_t world_o[8] = {}, world_n[8] = {};
    for (int i = 0; i < 8; ++i) {
        *(uint32_t *)(frame + 0x20 + 4 * i) = 0x3f000000u + (uint32_t)i;
        *(uint32_t *)(frame + 0x40 + 4 * i) = 0x3e800000u + (uint32_t)(i * 3);
    }
    old_bf_world43(frame, world_o);
    u3x_bf_world(base, frame, world_n);
    CHECK(std::memcmp(world_o, world_n, 32) == 0);

    alignas(8) uint8_t obj[0x200] = {};
    uintptr_t vt[0x60] = {};
    *(uintptr_t **)obj = vt;
    vt[0x248 / 8] = (uintptr_t)&t43_ret_binding;
    *(uint16_t *)(obj + 0x128) = 0x800;
    CHECK(old_bind_root43(obj) == u3x_bind_root(base, obj));
    CHECK(u3x_bind_root(base, obj) == t43_binding);

    uint8_t *nps = (uint8_t *)(base + 0x17cb3f0u);
    for (int i = 0; i < 10; ++i) *(uint8_t **)(nps + (size_t)i * 0x9c0) = (uint8_t *)(uintptr_t)(0x1000 + i);
    for (uint32_t id : {0u, 0xabcdu, 0x9999u})
        CHECK(old_np_by_id43(base, id) == u3x_np_by_id(base, id));

    alignas(8) uint8_t table[0x40] = {}, entries[0x60] = {};
    const uint32_t sids[] = {0x1000u, 0x1234u, 0x2000u};
    *(uint32_t *)table = 3;
    *(uint8_t **)(table + 8) = entries;
    for (int i = 0; i < 3; ++i) *(uint32_t *)(entries + 16 * i) = sids[i];
    for (uint32_t key : {0u, 0x0fffu, 0x1000u, 0x1001u, 0x1234u, 0x1fffu, 0x2000u, 0x2001u, 0xffffffffu})
        CHECK(old_tab_find43(table, key) == u3x_tab_find(table, key));
    *(uint32_t *)table = 0;
    CHECK(old_tab_find43(table, 0x1000u) == u3x_tab_find(table, 0x1000u));
    *(uint32_t *)table = 1;
    CHECK(old_tab_find43(table, 0x1000u) == u3x_tab_find(table, 0x1000u));
    CHECK(old_tab_find43(table, 0u) == u3x_tab_find(table, 0u));

    uint8_t g[0x40] = {}, a[0x20] = {}, t[0x40] = {};
    *(uint8_t **)(g + 0x30) = a;
    *(uint8_t **)(a + 0x10) = t;
    CHECK(old_art_tab43(g) == u3x_art_tab(g));
    CHECK(old_art_tab43(nullptr) == u3x_art_tab(nullptr));
    *(uint8_t **)(g + 0x30) = nullptr;
    CHECK(old_art_tab43(g) == u3x_art_tab(g));

    *(uintptr_t *)(base + 0x1702720u) = (uintptr_t)&t43_sid;
    CHECK(old_sid43(base, "a") == u3x_sid(base, "a"));
    CHECK(old_sid43(base, "b") == u3x_sid(base, "b"));
    CHECK(old_sid43(base, "") == u3x_sid(base, ""));
    CHECK(old_sid43(base, nullptr) == u3x_sid(base, nullptr));

    uint8_t *tabs[8] = {};
    uint8_t anim[0x40] = {};
    *(uint8_t **)(anim + 0x38) = (uint8_t *)"a";
    *(uint32_t *)(g + 0x18) = 0x1111u;
    *(uint8_t **)(g + 0x30) = a;
    *(uint8_t **)(a + 0x10) = t;
    *(uint32_t *)t = 3;
    *(uint8_t **)(t + 8) = entries;
    tabs[0] = g;
    for (uint32_t id : {0x1000u, 0x1234u, 0x2000u, 0x9999u}) {
        uint32_t to = 0xffffffffu, tn = 0xffffffffu;
        int32_t io = -1, in = -1;
        int ro = old_anim_lookup43(base, &to, &io, id, anim, tabs);
        int rn = u3x_anim_lookup(base, &tn, &in, id, anim, tabs);
        CHECK(ro == rn);
        CHECK(to == tn);
        CHECK(io == in);
    }
    *(uint32_t *)(g + 0x18) = 0x2222u;
    {
        uint32_t to = 0xffffffffu, tn = 0xffffffffu;
        int32_t io = -1, in = -1;
        int ro = old_anim_lookup43(base, &to, &io, 0x1234u, anim, tabs);
        int rn = u3x_anim_lookup(base, &tn, &in, 0x1234u, anim, tabs);
        CHECK(ro == rn);
        CHECK(to == tn);
        CHECK(io == in);
    }
    *(uint32_t *)(g + 0x18) = 0x1111u;
    {
        uint32_t to = 0xffffffffu, tn = 0xffffffffu;
        int32_t io = -1, in = -1;
        int ro = old_anim_lookup43(base, &to, &io, 0x1234u, nullptr, tabs);
        int rn = u3x_anim_lookup(base, &tn, &in, 0x1234u, nullptr, tabs);
        CHECK(ro == rn);
        CHECK(to == tn);
        CHECK(io == in);
    }

    for (uint32_t u = 0; u < 0x10000u; ++u)
        CHECK(old_layer43(u) == u3x_layer_counts(u, t43_ex, sizeof(t43_ex) / 4));
    for (uint32_t u : t43_ex)
        CHECK(old_layer43(u) == u3x_layer_counts(u, t43_ex, sizeof(t43_ex) / 4));
    for (uint32_t u : {0xb4e30afau,0xb4e30b08u,0xb4e30b09u,0x46d2949du,0x46d294a1u,0x46d294a2u})
        CHECK(old_layer43(u) == u3x_layer_counts(u, t43_ex, sizeof(t43_ex) / 4));

    *(int32_t *)(base + 0x1d46088u) = 0;
    CHECK(old_stream_root43(base) == u3x_stream_root(base, nullptr, nullptr));
    *(int32_t *)(base + 0x1d46088u) = 1;
    *(uint8_t **)(base + 0x1d46090u) = nullptr;
    CHECK(old_stream_root43(base) == u3x_stream_root(base, nullptr, nullptr));
}
