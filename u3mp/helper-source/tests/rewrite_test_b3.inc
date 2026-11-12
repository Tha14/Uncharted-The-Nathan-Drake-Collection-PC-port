// v7.115 batch: the old helper code (v7.114) for each rewritten section, and the old-versus-new checks.
static uint32_t old_page_tab(uint8_t *t, uint32_t page, uint32_t lan) {
    uint32_t n = *(uint32_t *)t; uint8_t *e = *(uint8_t **)(t + 8);
    for (uint32_t i = 0; e && i < n; i++, e += 0x20) {
        if (*(uint32_t *)e == page && *(uint32_t *)(e + 4) == lan) {
            uint8_t *tabs = *(uint8_t **)(e + 0x10); int32_t cur = *(int32_t *)(e + 0x18);
            return (tabs && cur >= 0) ? *(uint32_t *)(tabs + (size_t)cur * 0xc) : 0; }
    }
    return 0;
}
static int old_new_marker(int32_t *event, uint32_t flags) {
    if (!flags) return 0;
    event[2] = 0x2f; event[4] = 0; event[5] = 0; event[6] = 0; event[7] = 0; event[8] = 0;
    event[9] = -1; event[10] = 0; event[11] = -1; event[12] = 0x3f800000;
    return 1;
}
static int refreshed = 0;
static void fake_refresh(int32_t *) { refreshed++; }
static int old_ptr_ok(const void *p) { uintptr_t v = (uintptr_t)p; return v >= 0x10000u && v < (1ull << 47) && !(v & 7); }
static void old_custom_side(int32_t *cm, int side, int inplace, void *shown, uint32_t shown_id) {
    if ((uint32_t)side > 1) return; cm[0] = side;
    uint8_t **h = *(uint8_t ***)((uint8_t *)cm + 0xf60); uint32_t id = *(uint32_t *)((uint8_t *)cm + 0xf68);
    if (!inplace && id && id == shown_id && (void *)h == shown) return;
    if (h && id && old_ptr_ok(h) && old_ptr_ok(*h) && *(uint32_t *)(*h + 0x7c) == id) {
        fake_refresh(cm); *((uint8_t *)cm + 0x104c) = 1; }
}
static std::vector<uint64_t> calls;
static void fake_sound(int s) { calls.push_back(0x1000 + (uint64_t)s); }
static void fake_note(void *, int kind, uint32_t id) { calls.push_back(0x2000 + (uint64_t)kind * 0x100000000ull + id); }
static void fake_activate(void *page, int on) { calls.push_back(0x3000 + (uint64_t)(uintptr_t)page * 16 + (uint64_t)on); }
static void old_close_popup(uint8_t *m) {
    uint8_t *p = *(uint8_t **)(m + 0x898); if (!p) return;
    fake_sound(0xd);
    ((void (*)(void *, int))(*(uintptr_t **)p)[6])(p, 0);
    fake_note(m, 3, *(uint32_t *)(p + 0x10));
    uint8_t *top = *(uint8_t **)(m + 0x8b0 + (int64_t) * (int32_t *)(m + 0xdb0) * 8);
    ((void (*)(void *, int))(*(uintptr_t **)top)[6])(top, 1);
    *(uint8_t **)(m + 0x8a0) = *(uint8_t **)(m + 0x898); *(uint8_t **)(m + 0x898) = 0;
}
static int can_answer = 1; static int64_t clock_value = 0;
static uint8_t fake_can(uint8_t *, uint8_t *m, int) { return (uint8_t)(can_answer && (*(uint32_t *)m & 1) == 0); }
static int fake_done(uint8_t *pr, uint32_t id) { return pr && id < 0x400 && (pr[0xecc + (id >> 3)] >> (id & 7) & 1); }
static int64_t fake_clock(int64_t *) { return clock_value; }
static int old_take_mission(uint8_t *mo, uint8_t *pr, uint8_t *set, uint32_t row, int have_can, int have_clock) {
    uint32_t count = *(uint32_t *)(set + 8); uint8_t *list = *(uint8_t **)(set + 0x18);
    if (!list || row >= count) return 0; uint8_t *m = list + (size_t)row * 0x4c;
    if (!have_can || !fake_can(mo, m, 0) || fake_done(pr, *(uint32_t *)m)) return 0;
    *(uint32_t *)(pr + 0x1864) = *(uint32_t *)m; *(uint32_t *)(pr + 0xf4c) = 0;
    if (have_clock) *(uint32_t *)(pr + 0x1868) = (uint32_t)fake_clock(0);
    return 1;
}

static void batch3() {
    // page tabs: random tables; pages and play kinds inside and outside
    for (int t = 0; t < 400; t++) {
        alignas(8) uint8_t ent[0x20 * 6] = {}; alignas(8) uint8_t tabs[12 * 5] = {};
        for (uint32_t k = 0; k < 5; k++) { uint32_t id = 0x7000 + k + (uint32_t)t; std::memcpy(tabs + k * 12, &id, 4); }
        uint32_t n = rnd() % 7;
        for (uint32_t k = 0; k < 6; k++) {
            uint32_t page = 0x100 + rnd() % 3, lan = rnd() % 2; int32_t cur = (int32_t)(rnd() % 7) - 2;
            uint8_t *tp = (rnd() % 5) ? tabs : nullptr;
            std::memcpy(ent + k * 0x20, &page, 4); std::memcpy(ent + k * 0x20 + 4, &lan, 4);
            std::memcpy(ent + k * 0x20 + 0x10, &tp, 8); std::memcpy(ent + k * 0x20 + 0x18, &cur, 4);
            if (cur > 4) { cur = 4; std::memcpy(ent + k * 0x20 + 0x18, &cur, 4); }
        }
        alignas(8) uint8_t table[16] = {}; uint8_t *ep = (t % 19 == 0) ? nullptr : ent; if (n > 6) n = 6;
        std::memcpy(table, &n, 4); std::memcpy(table + 8, &ep, 8);
        for (uint32_t page = 0xff; page < 0x104; page++) for (uint32_t lan = 0; lan < 3; lan++)
            CHECK(old_page_tab(table, page, lan) == u3x_page_tab(table, page, lan));
    }
    // new marker: flags zero and non-zero
    for (uint32_t flags : {0u, 1u, 0x80000000u}) {
        int32_t a[16], b[16]; for (int i = 0; i < 16; i++) a[i] = b[i] = 0x11110000 + i;
        CHECK(old_new_marker(a, flags) == u3x_new_marker(b, flags)); CHECK(std::memcmp(a, b, sizeof a) == 0);
    }
    // custom side: sides, exit / in place, live / stale / missing preview, shown or not
    for (int t = 0; t < 600; t++) {
        alignas(8) static uint8_t obj[0x80]; uint8_t *slot = (t % 7 == 0) ? nullptr : obj; uint32_t oid = 0x900 + rnd() % 2;
        std::memcpy(obj + 0x7c, &oid, 4);
        std::vector<int32_t> a(0x1050 / 4 + 2, 0x55555555), b = a;
        uint8_t **h = (t % 5 == 0) ? nullptr : &slot; uint32_t id = (t % 11 == 0) ? 0 : 0x900 + rnd() % 2;
        for (auto *cm : {&a, &b}) { std::memcpy((uint8_t *)cm->data() + 0xf60, &h, 8); std::memcpy((uint8_t *)cm->data() + 0xf68, &id, 4); }
        int side = (int)(rnd() % 4) - 1, inplace = rnd() % 2; void *shown = (rnd() % 2) ? (void *)h : (void *)obj; uint32_t sid = 0x900 + rnd() % 2;
        refreshed = 0; old_custom_side(a.data(), side, inplace, shown, sid); int r1 = refreshed;
        refreshed = 0; u3x_custom_side(b.data(), side, inplace, shown, sid, fake_refresh);
        CHECK(r1 == refreshed); CHECK(a == b);
    }
    // close popup: with and without an open popup, several stack positions
    for (int t = 0; t < 12; t++) {
        static uintptr_t vt[8]; vt[6] = (uintptr_t)fake_activate;
        alignas(8) static uint8_t popup[0x20], pages[4][0x10];
        uintptr_t *vp = vt; std::memcpy(popup, &vp, 8); uint32_t pid = 0x471007a1u + t; std::memcpy(popup + 0x10, &pid, 4);
        for (auto &pg : pages) std::memcpy(pg, &vp, 8);
        std::vector<uint8_t> a(0xdc0, 0), b;
        uint8_t *pp = (t % 4 == 3) ? nullptr : popup; std::memcpy(a.data() + 0x898, &pp, 8);
        for (int k = 0; k < 4; k++) { uint8_t *pg = pages[k]; std::memcpy(a.data() + 0x8b0 + k * 8, &pg, 8); }
        int32_t idx = t % 4; std::memcpy(a.data() + 0xdb0, &idx, 4); b = a;
        calls.clear(); old_close_popup(a.data()); auto c1 = calls;
        calls.clear(); u3x_close_popup(b.data(), fake_sound, fake_note);
        // the note call carries the menu pointer in neither record, so the two logs compare directly
        CHECK(c1 == calls); CHECK(a == b); CHECK(pp ? calls.size() == 4 : calls.empty());
    }
    // take mission: rows in and out of range, refused, already done, with and without clock / check
    for (int t = 0; t < 500; t++) {
        alignas(8) uint8_t list[0x4c * 5] = {}; for (uint32_t k = 0; k < 5; k++) { uint32_t id = rnd() % 16; std::memcpy(list + k * 0x4c, &id, 4); }
        alignas(8) uint8_t set[0x20] = {}; uint32_t count = rnd() % 6; uint8_t *lp = (t % 13 == 0) ? nullptr : list;
        std::memcpy(set + 8, &count, 4); std::memcpy(set + 0x18, &lp, 8);
        std::vector<uint8_t> a(0x1870, 0), b; a[0xecc] = (uint8_t)rnd(); a[0xecd] = (uint8_t)rnd(); uint32_t junk = 0x7777; std::memcpy(a.data() + 0xf4c, &junk, 4); b = a;
        uint32_t row = rnd() % 7; int have_can = t % 17 != 0, have_clock = rnd() % 2; can_answer = rnd() % 4 != 0; clock_value = (int64_t)rnd();
        int r1 = old_take_mission(nullptr, a.data(), set, row, have_can, have_clock);
        int r2 = u3x_take_mission(nullptr, b.data(), set, row, have_can ? fake_can : nullptr, fake_done, have_clock ? fake_clock : nullptr);
        CHECK(r1 == r2); CHECK(a == b);
    }
}
