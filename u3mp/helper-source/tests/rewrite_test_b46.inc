// row 5 (w004): medal list, records, tab filter, build and source text/icon paths.
struct T46List {
    uint16_t ids[256];
    uint16_t count, pad;
    uint32_t selected, tab, built;
};
static_assert(sizeof(T46List) == 0x210, "MedalList layout");

static uint8_t t46_base[0x100];
static uint8_t t46_tab[8 + 3 * 0x48], t46_records[3 * 0x48];
static uint8_t t46_pages[0x40], t46_entries[0x20], t46_tabs[0x30];
static uint8_t t46_source[0x40];
static uint8_t *t46_page_ptr = t46_pages;
static char t46_name[] = "net-medals/assistant";

static void *t46_lookup(uint32_t id) { return id == 0x980d6543u ? t46_tab : nullptr; }
static void *t46_page_lookup(uint32_t id) { return id == 0xf1514340u ? static_cast<void *>(&t46_page_ptr) : nullptr; }

extern "C" char *u3h_fmt_int(char *o, char *end, int64_t, int32_t) { if (o < end) *o++ = '7'; return o; }
extern "C" int u3h_native_gate_closed(uintptr_t, int32_t *) { return 0; }

static void *t46_vtex(uintptr_t, const char *) { return reinterpret_cast<void *>(static_cast<uintptr_t>(0x12345678)); }
static void *o46_medal_list(void *s) { return static_cast<uint8_t *>(s) + 0x2800; }
static uint8_t *o46_medal_record(void *s, uint32_t i) {
    const uintptr_t lookup = *reinterpret_cast<uintptr_t *>(static_cast<uint8_t *>(s) + 768 + 13 * 8);
    uint8_t *t = lookup ? reinterpret_cast<uint8_t *(*)(uint32_t)>(lookup)(0x980d6543u) : nullptr;
    if (!t || i >= *reinterpret_cast<uint32_t *>(t)) return nullptr;
    return *reinterpret_cast<uint8_t **>(t + 8) + static_cast<uintptr_t>(i) * 0x48;
}
static uint32_t o46_w04_tab(uintptr_t base, void *s) {
    uint32_t *eb = reinterpret_cast<uint32_t *>(t27_ext + 0x4050);
    if (eb[3] & 0x08000000u) return u3h_current_tab(base, s);
    const uintptr_t lookup = *reinterpret_cast<uintptr_t *>(static_cast<uint8_t *>(s) + 0x290);
    if (!lookup) return 0;
    uint8_t **sym = reinterpret_cast<uint8_t **(*)(uint32_t)>(lookup)(0xf1514340u);
    if (!sym || !*sym) return 0;
    uint8_t *t = *sym;
    const uint32_t n = *reinterpret_cast<uint32_t *>(t);
    uint8_t *e = *reinterpret_cast<uint8_t **>(t + 8);
    for (uint32_t i = 0; e && i < n && i < 1024; ++i, e += 0x20) {
        if (*reinterpret_cast<uint32_t *>(e) == 0xc98ee52au &&
            *reinterpret_cast<uint32_t *>(e + 4) == *reinterpret_cast<uint8_t *>(base + 0x17d5c60)) {
            uint8_t *tabs = *reinterpret_cast<uint8_t **>(e + 0x10);
            const int32_t cur = *reinterpret_cast<int32_t *>(e + 0x18);
            return (tabs && cur >= 0) ? *reinterpret_cast<uint32_t *>(tabs + static_cast<uintptr_t>(cur) * 0xc) : 0;
        }
    }
    return 0;
}
static void o46_medal_build(uintptr_t base, void *s) {
    T46List *l = reinterpret_cast<T46List *>(static_cast<uint8_t *>(s) + 0x2800);
    const uint32_t tab = o46_w04_tab(base, s);
    uint32_t mask = 0;
    switch (tab) {
    case 0x091026d1u: mask = 2; break;
    case 0xfb0dd522u: mask = 4; break;
    case 0xa13b68fbu: mask = 8; break;
    case 0xadbd5585u: mask = 0x10; break;
    }
    l->tab = tab;
    l->built = 1;
    l->count = 0;
    const uintptr_t lookup = *reinterpret_cast<uintptr_t *>(static_cast<uint8_t *>(s) + 768 + 13 * 8);
    uint8_t *t = lookup ? reinterpret_cast<uint8_t *(*)(uint32_t)>(lookup)(0x980d6543u) : nullptr;
    if (!t || !mask) return;
    const uint32_t n = *reinterpret_cast<uint32_t *>(t);
    int32_t old = -1, neu = -1;
    uint32_t *eb = reinterpret_cast<uint32_t *>(t27_ext + 0x4050);
    if (!(eb[3] & 0x08000000u))
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t *r = o46_medal_record(s, i);
            if (!r) continue;
            if (*reinterpret_cast<uint32_t *>(r) == 0xa8e90238u && old < 0) old = static_cast<int32_t>(i);
            else if (*reinterpret_cast<uint32_t *>(r) == 0x9befafdeu && neu < 0) neu = static_cast<int32_t>(i);
        }
    if (old < 0 || neu < 0 || neu < old) old = neu = -1;
    for (uint32_t i = 0; i < n && l->count < 256; ++i) {
        if (static_cast<int32_t>(i) == neu) continue;
        const uint32_t k = static_cast<int32_t>(i) == old ? static_cast<uint32_t>(neu) : i;
        uint8_t *r = o46_medal_record(s, k);
        if (r && (mask & *reinterpret_cast<uint32_t *>(r + 0x10)) == mask) l->ids[l->count++] = static_cast<uint16_t>(k);
    }
    if (l->selected >= l->count) l->selected = 0;
}
static int o46_medals_source(uintptr_t base, int32_t *event, void *s) {
    uint8_t *source = *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(event) + 0x98);
    if (!source || *reinterpret_cast<uint32_t *>(source + 0x1c) != 0x4c414ed3u) return 0;
    if (u3h_native_gate_closed(base, event)) return 1;
    T46List *l = reinterpret_cast<T46List *>(static_cast<uint8_t *>(s) + 0x2800);
    if (!l->built || l->tab != o46_w04_tab(base, s)) o46_medal_build(base, s);
    const uint32_t row = static_cast<uint32_t>(event[1]);
    switch (event[0]) {
    case 10: l->selected = row; [[fallthrough]];
    case 2: {
        if (row >= l->count) break;
        uint8_t *r = o46_medal_record(s, l->ids[row]);
        if (!r) break;
        const uint32_t sub = static_cast<uint32_t>(event[0x15]);
        if (sub == 0) *reinterpret_cast<const char **>(reinterpret_cast<uint8_t *>(event) + 0x40) = u3h_text(base, s, *reinterpret_cast<uint32_t *>(r));
        else if (sub == 0x80d4cc7bu) *reinterpret_cast<const char **>(reinterpret_cast<uint8_t *>(event) + 0x40) = u3h_text(base, s, *reinterpret_cast<uint32_t *>(r + 4));
        else if (sub == 0xb684d68bu) {
            uint32_t *ec = reinterpret_cast<uint32_t *>(t27_ext + 0x4450);
            ec[251] = ec[251] + 1;
            char num[24];
            char *e = u3h_fmt_int(num, num + 23, *reinterpret_cast<int32_t *>(r + 0x20), 1);
            *e = 0;
            char *o = reinterpret_cast<char *>(s) + 0x2b00;
            char *end = o + 120;
            for (const char *t = u3h_text(base, s, 0xb4b0d5c2u); *t && o < end; ++t) *o++ = *t;
            for (const char *t = " |@Cff7cce96|$"; *t && o < end; ++t) *o++ = *t;
            for (const char *t = num; *t && o < end; ++t) *o++ = *t;
            *o = 0;
            *reinterpret_cast<const char **>(reinterpret_cast<uint8_t *>(event) + 0x40) = reinterpret_cast<const char *>(s) + 0x2b00;
        }
        break;
    }
    case 4: {
        if (row >= l->count || static_cast<uint32_t>(event[0x14]) != 0xf5a294f3u) break;
        uint8_t *r = o46_medal_record(s, l->ids[row]);
        if (!r) break;
        const char *name = *reinterpret_cast<const char **>(r + 0x18);
        if (name) {
            unsigned k = 0;
            int slash = 0;
            for (; k < 48 && name[k]; ++k) {
                if (name[k] == '/') slash = 1;
                if (name[k] < 0x20 || name[k] > 0x7e) { name = nullptr; break; }
            }
            if (name && (!slash || k == 0 || k >= 48)) name = nullptr;
        }
        const uintptr_t vtex = *reinterpret_cast<uintptr_t *>(static_cast<uint8_t *>(s) + 768 + 20 * 8);
        void *tex = (vtex && name) ? reinterpret_cast<void *(*)(uintptr_t, const char *)>(vtex)(base + 0x1b31128, name) : nullptr;
        for (int k = 4; k <= 11; ++k) event[k] = 0;
        if (tex) {
            event[2] = 2;
            *reinterpret_cast<void **>(event + 4) = tex;
            event[9] = -1;
            event[11] = -1;
            event[12] = 0x3f800000;
        } else {
            event[2] = 0x10;
            event[9] = -1;
            event[11] = -1;
            event[12] = 0x40100000;
        }
        break;
    }
    case 5: event[1] = static_cast<int32_t>(l->selected); break;
    case 6: event[1] = static_cast<int32_t>(l->count) - 1; break;
    case 9: o46_medal_build(base, s); break;
    case 11: {
        uint32_t *ec = reinterpret_cast<uint32_t *>(t27_ext + 0x4450);
        ec[252] = ec[252] + 1;
        ec[253] = static_cast<uint32_t>(event[0x13]);
        if (static_cast<uint32_t>(event[0x13]) == 0x67a48eb9u) event[14] = 4;
        break;
    }
    }
    return 1;
}
static void t46_reset_list(void *s) {
    std::memset(static_cast<uint8_t *>(s) + 0x2800, 0, sizeof(T46List));
}
__attribute__((noinline)) static void batch46() {
    alignas(16) static uint8_t state[0x3000];
    const uintptr_t base = reinterpret_cast<uintptr_t>(t46_base) - 0x17d5c60;
    std::memset(state, 0, sizeof state);
    std::memset(t46_tab, 0, sizeof t46_tab);
    std::memset(t46_pages, 0, sizeof t46_pages);
    std::memset(t46_entries, 0, sizeof t46_entries);
    std::memset(t46_tabs, 0, sizeof t46_tabs);
    std::memset(t46_source, 0, sizeof t46_source);
    std::memset(t46_tab, 0, sizeof t46_tab);
        *reinterpret_cast<uint32_t *>(t46_tab) = 3;
    *reinterpret_cast<uint8_t **>(t46_tab + 8) = t46_records;
    for (int i = 0; i < 3; ++i) {
        uint8_t *r = t46_records + i * 0x48;
        *reinterpret_cast<uint32_t *>(r) = i == 1 ? 0xa8e90238u : i == 2 ? 0x9befafdeu : 0x11111111u;
        *reinterpret_cast<uint32_t *>(r + 0x10) = 2;
    }
    *reinterpret_cast<const char **>(t46_records + 0x18) = t46_name;
    *reinterpret_cast<uintptr_t *>(state + 768 + 13 * 8) = reinterpret_cast<uintptr_t>(t46_lookup);
    *reinterpret_cast<uintptr_t *>(state + 0x290) = reinterpret_cast<uintptr_t>(t46_page_lookup);
    *reinterpret_cast<uint32_t *>(t46_pages) = 1;
    *reinterpret_cast<uint8_t **>(t46_pages + 8) = t46_entries;
    *reinterpret_cast<uint32_t *>(t46_entries) = 0xc98ee52au;
    *reinterpret_cast<uint8_t **>(t46_entries + 0x10) = t46_tabs;
    *reinterpret_cast<int32_t *>(t46_entries + 0x18) = 1;
    *reinterpret_cast<uint32_t *>(t46_tabs + 0xc) = 0x091026d1u;
    *reinterpret_cast<uint32_t *>(t46_source + 0x1c) = 0x4c414ed3u;

    CHECK(o46_medal_list(state) == u3x_medal_list(state));
    for (uint32_t i = 0; i < 4; ++i) CHECK(o46_medal_record(state, i) == u3x_medal_record(state, i));
    CHECK(o46_w04_tab(base, state) == u3x_w04_tab(base, state));
    uint32_t *eb = reinterpret_cast<uint32_t *>(t27_ext + 0x4050);
    eb[3] |= 0x08000000u;
    CHECK(o46_w04_tab(base, state) == u3x_w04_tab(base, state));
    eb[3] &= ~0x08000000u;

    for (int pass = 0; pass < 2; ++pass) {
        if (pass) eb[3] |= 0x08000000u;
        else eb[3] &= ~0x08000000u;
        t46_reset_list(state);
        reinterpret_cast<T46List *>(state + 0x2800)->selected = 2;
        o46_medal_build(base, state);
        T46List old = *reinterpret_cast<T46List *>(state + 0x2800);
        t46_reset_list(state);
        reinterpret_cast<T46List *>(state + 0x2800)->selected = 2;
        u3x_medal_build(base, state);
        CHECK(std::memcmp(&old, state + 0x2800, sizeof old) == 0);
    }
    eb[3] &= ~0x08000000u;

    alignas(16) int32_t ev[0x30];
    std::memset(ev, 0, sizeof ev);
    *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(ev) + 0x98) = t46_source;
    T46List *l = reinterpret_cast<T46List *>(state + 0x2800);
    l->built = 1;
    l->tab = 0x091026d1u;
    l->count = 2;
    l->ids[0] = 0;
    l->ids[1] = 2;
    l->selected = 1;

    ev[0] = 5;
    CHECK(o46_medals_source(base, ev, state) == u3x_medals_source(base, ev, state));
    CHECK(ev[1] == 1);
    ev[0] = 6;
    CHECK(o46_medals_source(base, ev, state) == u3x_medals_source(base, ev, state));
    CHECK(ev[1] == 1);
    uint32_t *ec = reinterpret_cast<uint32_t *>(t27_ext + 0x4450);
    ev[0] = 11;
    ev[0x13] = 0x67a48eb9;
    ec[252] = ec[253] = 0;
    ev[14] = 0;
    CHECK(o46_medals_source(base, ev, state) == 1);
    const uint32_t old252 = ec[252], old253 = ec[253];
    const int32_t old14 = ev[14];
    ec[252] = ec[253] = 0;
    ev[14] = 0;
    CHECK(u3x_medals_source(base, ev, state) == 1);
    CHECK(ec[252] == old252 && ec[253] == old253 && ev[14] == old14);

    ev[0] = 2;
    ev[1] = 0;
    ev[0x15] = 0;
    *reinterpret_cast<const char **>(reinterpret_cast<uint8_t *>(ev) + 0x40) = nullptr;
    CHECK(o46_medals_source(base, ev, state) == 1);
    const char *old_text = *reinterpret_cast<const char **>(reinterpret_cast<uint8_t *>(ev) + 0x40);
    *reinterpret_cast<const char **>(reinterpret_cast<uint8_t *>(ev) + 0x40) = nullptr;
    CHECK(u3x_medals_source(base, ev, state) == 1);
    CHECK(*reinterpret_cast<const char **>(reinterpret_cast<uint8_t *>(ev) + 0x40) == old_text);

    ev[0] = 10;
    ev[1] = 1;
    ev[0x15] = 0;
    CHECK(o46_medals_source(base, ev, state) == 1);
    const uint32_t old_selected = l->selected;
    const char *old_ten = *reinterpret_cast<const char **>(reinterpret_cast<uint8_t *>(ev) + 0x40);
    l->selected = 1;
    *reinterpret_cast<const char **>(reinterpret_cast<uint8_t *>(ev) + 0x40) = nullptr;
    CHECK(u3x_medals_source(base, ev, state) == 1);
    CHECK(l->selected == old_selected && *reinterpret_cast<const char **>(reinterpret_cast<uint8_t *>(ev) + 0x40) == old_ten);

    ev[0] = 2;
    ev[1] = 0;
    ev[0x15] = 0xb684d68bu;
    ec[251] = 0;
    std::memset(state + 0x2b00, 0, 120);
    CHECK(o46_medals_source(base, ev, state) == 1);
    char old_buf[120];
    std::memcpy(old_buf, state + 0x2b00, sizeof old_buf);
    const uint32_t old_251 = ec[251];
    ec[251] = 0;
    std::memset(state + 0x2b00, 0, 120);
    CHECK(u3x_medals_source(base, ev, state) == 1);
    CHECK(ec[251] == old_251);
    CHECK(std::memcmp(old_buf, state + 0x2b00, sizeof old_buf) == 0);

    *reinterpret_cast<uintptr_t *>(state + 768 + 20 * 8) = 0;
    std::memset(ev, 0, sizeof ev);
    *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(ev) + 0x98) = t46_source;
    ev[0] = 4;
    ev[1] = 1;
    ev[0x14] = 0xf5a294f3;
    CHECK(o46_medals_source(base, ev, state) == 1);
    int32_t old_ev4[0x30];
    std::memcpy(old_ev4, ev, sizeof old_ev4);
    std::memset(ev, 0, sizeof ev);
    *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(ev) + 0x98) = t46_source;
    ev[0] = 4;
    ev[1] = 1;
    ev[0x14] = 0xf5a294f3;
    CHECK(u3x_medals_source(base, ev, state) == 1);
    CHECK(std::memcmp(old_ev4, ev, sizeof old_ev4) == 0);

    *reinterpret_cast<uintptr_t *>(state + 768 + 20 * 8) = reinterpret_cast<uintptr_t>(t46_vtex);
    std::memset(ev, 0, sizeof ev);
    *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(ev) + 0x98) = t46_source;
    ev[0] = 4;
    ev[1] = 0;
    ev[0x14] = 0xf5a294f3;
    CHECK(o46_medals_source(base, ev, state) == 1);
    std::memcpy(old_ev4, ev, sizeof old_ev4);
    std::memset(ev, 0, sizeof ev);
    *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(ev) + 0x98) = t46_source;
    ev[0] = 4;
    ev[1] = 0;
    ev[0x14] = 0xf5a294f3;
    CHECK(u3x_medals_source(base, ev, state) == 1);
    CHECK(std::memcmp(old_ev4, ev, sizeof old_ev4) == 0);

    ev[0] = 9;
    CHECK(o46_medals_source(base, ev, state) == u3x_medals_source(base, ev, state));
    *reinterpret_cast<uint32_t *>(t46_source + 0x1c) = 0;
    ev[0] = 5;
    CHECK(o46_medals_source(base, ev, state) == 0);
    CHECK(u3x_medals_source(base, ev, state) == 0);
}
