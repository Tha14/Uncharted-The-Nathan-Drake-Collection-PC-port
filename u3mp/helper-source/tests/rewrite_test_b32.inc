// third split: the old c009 state / command visitors (agc_port.inc verr119 .. vmark119, kinds 0 and 1) against u3x_v_*.
static uint32_t o32_ec[256];
static void o32_err(Visitor *v, int code) { if (!v->error) { v->error = code; if (code == 12) o32_ec[104]++; else o32_ec[82] = (uint32_t)code; } }
static uint32_t *o32_cw(Visitor *v, uint16_t type, uint16_t n) {
    uint8_t *cb = v->commands; uint32_t pos = *(uint32_t *)(cb + 0xc), cap = *(uint32_t *)(cb + 8);
    if (v->error) return 0; if (pos + n > cap) { o32_err(v, 7); return 0; }
    uint32_t *w = *(uint32_t **)cb + pos; *(uint16_t *)w = type; *((uint16_t *)w + 1) = n; *(uint32_t *)(cb + 0xc) = pos + n; return w; }
static void o32_max(Visitor *v, uint32_t i) { uint32_t *m = (uint32_t *)(v->commands + 0x10); if (*m <= i) *m = i; }
static void o32_c6(Visitor *v, uint32_t f, uint8_t *an, uint32_t i) { uint32_t *w = o32_cw(v, 8, 5); if (!w) return; *(uint8_t **)(w + 1) = an; w[3] = i; w[4] = f; o32_max(v, i); }
static void o32_c7(Visitor *v, uint32_t f, uint32_t a, uint32_t b, uint32_t c, uint32_t m) {
    uint32_t *w = o32_cw(v, 9, 6); if (!w) return; w[1] = a; w[2] = b; w[3] = c; w[4] = m; w[5] = f; o32_max(v, a); o32_max(v, b); o32_max(v, c); }
static void o32_c8(Visitor *v, uint32_t i) { uint32_t *w = o32_cw(v, 0xa, 2); if (!w) return; w[1] = i; o32_max(v, i); }
static void o32_c10(Visitor *v, uint32_t i) { uint32_t *w = o32_cw(v, 0xd, 2); if (!w) return; w[1] = i; o32_max(v, i); }
static void o32_c12(Visitor *v, uint32_t a, uint32_t b) { uint32_t *w = o32_cw(v, 0xf, 3); if (!w) return; w[1] = a; w[2] = b; o32_max(v, a); o32_max(v, b); }
static void o32_c9b(Visitor *v, uint16_t ty, uint32_t i, uint64_t p1, uint64_t p2, uint64_t p3) {
    if ((p1 | p2 | p3) & 0xf) { o32_err(v, 10); return; }
    uint32_t *w = o32_cw(v, ty, 8); if (!w) return; w[1] = i; *(uint64_t *)(w + 2) = p1; *(uint64_t *)(w + 4) = p2; *(uint64_t *)(w + 6) = p3; o32_max(v, i); }
static uint32_t o32_frames(uint8_t *an) { uint8_t *c = *(uint8_t **)(an + 0x28); return c ? *(uint16_t *)(c + 0x22) : 0; }
static void o32_push(Visitor *v, uint32_t ph, uint8_t *an) {
    v->key = 0; if (!an) return; uint8_t *el = *(uint8_t **)(an + 0x48); if (!el || !*(uint32_t *)(el + 4) || !*(uint64_t *)(an + 0x28)) return;
    v->key = *(uint32_t *)el;
    uint32_t *cnt = (uint32_t *)(v->entries + 0x210); if (*cnt > 21) { o32_ec[102]++; return; }
    uint8_t *e = v->entries + (size_t)*cnt * 0x18; (*cnt)++;
    *(uint32_t *)e = v->key; *(uint8_t **)(e + 8) = an; e[0x15] = 0; *(uint32_t *)(e + 0x10) = ph; e[0x14] = 1; }
static void o32_anim(Visitor *v, uint32_t ph, uint8_t *an, uint32_t dep) {
    if (v->kind == 0) o32_push(v, ph, an);
    else { if (dep + (uint32_t)v->v2 > 6) { o32_err(v, 6); return; }
        if (!an) o32_c10(v, dep + (uint32_t)v->v1); else o32_c6(v, f_mul(ph, f_sub(f_from_int((int32_t)o32_frames(an)), F_ONE)), an, dep + (uint32_t)v->v1); } }
static void o32_sub0(Visitor *v, uint32_t x) {
    if (v->kind == 0) { if (!v->key) return; uint32_t n = *(uint32_t *)(v->entries + 0x210);
        for (int32_t i = (int32_t)n - 1; i >= 0; i--) { uint8_t *e = v->entries + (size_t)i * 0x18; if (*(uint32_t *)e == v->key) { e[0x15] = 1; return; } } }
    else o32_c8(v, x + (uint32_t)v->v1); }
static void o32_sub3(Visitor *v, uint32_t dep) { if (v->kind != 1) return; if (dep + (uint32_t)v->v2 > 6) { o32_err(v, 6); return; } o32_c10(v, dep + (uint32_t)v->v1); }
static void o32_sub4(Visitor *v) { if (v->kind == 1) o32_c12(v, (uint32_t)v->v1 + 1, (uint32_t)v->v1); }
static void batch32() {
    static uint8_t anims[4][0x60], clips[4][0x30], elems[4][8];
    for (int k = 0; k < 4; k++) {
        for (auto &b : anims[k]) b = 0;
        *(uint8_t **)(anims[k] + 0x28) = k == 3 ? nullptr : clips[k];
        *(uint16_t *)(clips[k] + 0x22) = (uint16_t)(rnd() % 90);
        *(uint32_t *)elems[k] = 100 + k % 2; *(uint32_t *)(elems[k] + 4) = k == 2 ? 0 : 1;
        *(uint8_t **)(anims[k] + 0x48) = elems[k];
    }
    for (int t = 0; t < 20000; t++) {
        static Visitor a, b; static uint8_t ea[0x220], eb2[0x220], cba[0x18], cbb[0x18]; static uint32_t wa[64], wb[64];
        for (auto &x : ea) x = 0; for (auto &x : eb2) x = 0; for (auto &x : wa) x = 0; for (auto &x : wb) x = 0;
        for (auto &x : cba) x = 0; for (auto &x : cbb) x = 0;
        *(uint32_t **)cba = wa; *(uint32_t **)cbb = wb; *(uint32_t *)(cba + 8) = *(uint32_t *)(cbb + 8) = rnd() % 64;
        uint8_t *pa = (uint8_t *)&a, *pb = (uint8_t *)&b; for (size_t k = 0; k < sizeof(Visitor); k++) pa[k] = pb[k] = 0;
        a.kind = b.kind = (int)(rnd() % 2); a.v1 = b.v1 = (int32_t)(rnd() % 4); a.v2 = b.v2 = (int32_t)(rnd() % 5);
        a.entries = ea; b.entries = eb2; a.commands = cba; b.commands = cbb;
        for (auto &x : o32_ec) x = 0; uint32_t *ec = ext_ec(); for (int k = 0; k < 256; k++) ec[k] = 0;
        for (int step = 0; step < 40; step++) {
            const uint32_t op = rnd() % 8, x = rnd() % 8, ph = rnd(); uint8_t *an = rnd() % 5 ? anims[rnd() % 4] : nullptr;
            switch (op) {
            case 0: o32_anim(&a, ph, an, x); u3x_v_anim(&b, ph, an, x); break;
            case 1: o32_sub0(&a, x); u3x_v_sub0(&b, x); break;
            case 2: o32_sub3(&a, x); u3x_v_sub3(&b, x); break;
            case 3: o32_sub4(&a); u3x_v_sub4(&b); break;
            case 4: if (a.kind == 1) { o32_c7(&a, ph, x, x + 1, x + 2, 1); u3x_v_blend(&b, ph, x - (uint32_t)b.v1, x + 1 - (uint32_t)b.v1, x + 2 - (uint32_t)b.v1, 1); } break;
            case 5: { const uint64_t p = (uint64_t)(rnd() % 4) << 2; o32_c9b(&a, 0xc, x, p, 0x100, 0x200); u3x_v_ice(&b, 0xc, x, p, 0x100, 0x200); break; }
            case 6: o32_c12(&a, x, x + 1); u3x_v_copy(&b, x, x + 1); break;
            default: o32_err(&a, (int)x + 5); u3x_v_error(&b, (int)x + 5); break;
            }
        }
        CHECK(a.error == b.error && a.key == b.key);
        for (int k = 0; k < 0x220; k++) CHECK(ea[k] == eb2[k]);
        for (int k = 8; k < 0x18; k++) CHECK(cba[k] == cbb[k]);
        for (int k = 0; k < 64; k++) CHECK(wa[k] == wb[k]);
        CHECK(o32_ec[82] == ec[82] && o32_ec[102] == ec[102] && o32_ec[104] == ec[104]);
    }
}
